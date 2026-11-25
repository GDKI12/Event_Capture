#include "workmanager.h"
#include "missionprocessor.h"
#include "sensordata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

#include <limits>

// Probe RouDi before initRuntime(), which otherwise blocks and eventually
// terminates the process when the daemon is absent (iceoryx 2.90).
#include "iceoryx_posh/internal/runtime/ipc_interface_user.hpp"

WorkManager::WorkManager(QObject* parent) : QObject(parent)
{
}

WorkManager::~WorkManager()
{
    stop();
}

void WorkManager::initConfig()
{
    camNameList.clear();
    configValid = false;
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("config/config.ini"),
        appDir.filePath(CONFIG_PATH),
        QDir::current().filePath("config/config.ini"),
        QString::fromUtf8(CONFIG_PATH)
    };
    QString configPath;
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate).isFile()) {
            configPath = QFileInfo(candidate).absoluteFilePath();
            break;
        }
    }
    if (configPath.isEmpty()) {
        emit configurationLoaded(QString());
        emit logMessage(tr("[ERROR] config/config.ini 파일을 찾을 수 없습니다."));
        emit cameraNamesChanged({});
        return;
    }

    QSettings settings(configPath, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    baseURL = settings.value("setting/base_url").toString();
    authId = settings.value("setting/auth_id").toString();
    secretKey = settings.value("setting/secret_key").toString();
    rootPath = settings.value("setting/root_path").toString();
    savePath = settings.value("setting/save_path").toString();
    if (processor)
        processor->configure(configPath);
    emit configurationLoaded(configPath);

    // cam_list is the current config key; retain support for older files.
    const auto configuredCameras = settings.value("setting/cam_list",
                                                 settings.value("setting/cams"));
    QStringList cameras;
    for (const QString& entry : configuredCameras.toStringList()) {
        for (const QString& name : entry.split(',')) {
            const QString cameraName = name.trimmed();
            if (cameraName.isEmpty() || cameras.contains(cameraName))
                continue;
            if (cameraName.toUtf8().size() > int(iox::capro::IdString_t::capacity())) {
                emit logMessage(tr("[WARN] 카메라 이름이 너무 깁니다: %1").arg(cameraName));
                continue;
            }
            cameras.append(cameraName);
            camNameList.append(cameraName);
        }
    }
    // EdgeInfra Capture publishes converted BGR8 images. The saved RAW files
    // contain Bayer pixels and have a separate pixel format in the config.
    pixelFormat = settings.value("shared_memory/pixel_format", "bgr8").toString().trimmed().toLower();
    const QStringList supportedFormats = {"bgr8", "rgb8", "mono8", "gray8",
                                         "bayer_rggb8", "bayer_bggr8",
                                         "bayer_grbg8", "bayer_gbrg8"};
    configValid = settings.status() == QSettings::NoError && !cameras.isEmpty()
                  && supportedFormats.contains(pixelFormat);
    emit cameraNamesChanged(cameras);
    emit logMessage(tr("[INFO] 설정 로드: %1 (카메라 %2개, %3)")
                    .arg(configPath).arg(cameras.size()).arg(pixelFormat));
    if (settings.status() != QSettings::NoError)
        emit logMessage(tr("[ERROR] 설정 파일을 읽을 수 없습니다."));
    if (cameras.isEmpty())
        emit logMessage(tr("[WARN] setting/cam_list에 등록된 카메라가 없습니다."));
    if (!supportedFormats.contains(pixelFormat))
        emit logMessage(tr("[ERROR] 지원하지 않는 shared_memory/pixel_format: %1").arg(pixelFormat));
}

void WorkManager::start()
{
    if (connectionTimer || receiveTimer)
        return;
    if (!processor) {
        processor = new MissionProcessor(this);
        connect(processor, &MissionProcessor::logMessage, this, &WorkManager::logMessage);
        connect(processor, &MissionProcessor::statusChanged, this, &WorkManager::processingStatusChanged);
        connect(processor, &MissionProcessor::progressChanged, this, &WorkManager::missionProgressChanged);
        connect(processor, &MissionProcessor::inferenceResult, this, &WorkManager::inferenceResult);
        connect(processor, &MissionProcessor::completionRequested, this, &WorkManager::missionCompletionRequested);
    }
    initConfig();
    if (!configValid)
        return;

    connectionTimer = new QTimer(this);
    connectionTimer->setInterval(1000);
    const auto tryConnect = [this] {
        if (init())
            connectionTimer->stop();
    };
    connect(connectionTimer, &QTimer::timeout, this, tryConnect);
    connectionTimer->start();
    tryConnect();
}

bool WorkManager::init()
{
    if (!camSubscribers.isEmpty())
        return true;
    if (!configValid)
        return false;
    if (!_runtime) {
        iox::runtime::IpcInterfaceUser daemon(iox::roudi::IPC_CHANNEL_ROUDI_NAME);
        if (!daemon.isInitialized()) {
            if (!waitingForRouDi) {
                emit logMessage(tr("[WARN] iceoryx RouDi 연결 대기 중입니다. iox-roudi 실행을 확인하세요."));
                waitingForRouDi = true;
            }
            return false;
        }
        _runtime = &PoshRuntime::initRuntime(APP_NAME);
        emit logMessage(tr("[INFO] iceoryx 런타임 연결: %1").arg(APP_NAME));
    }

    iox::popo::SubscriberOptions options;
    options.queueCapacity = 1U;
    options.queueFullPolicy = iox::popo::QueueFullPolicy::DISCARD_OLDEST_DATA;
    for (const QString& name : camNameList) {
        const auto encodedName = name.toUtf8();
        const iox::capro::IdString_t service{iox::cxx::TruncateToCapacity, encodedName.constData()};
        camSubscribers.insert(name, new Subscriber<CustomCamDataType>({service, "Data", "Image"}, options));
        rawSubscribers.insert(name, new Subscriber<CustomCamDataType>({service, "Data", "RawImage"}, options));
        emit logMessage(tr("[INFO] 카메라 구독: %1 / Data / Image").arg(name));
    }
    options.queueCapacity = 8U;
    lidarSubscriber = new Subscriber<CustomLidarDataType>({"lidar", "Data", "Points"}, options);
    emit logMessage(tr("[INFO] 원본 BayerRG8 구독: cam / Data / RawImage, 라이다 구독: lidar / Data / Points"));
    receiveTimer = new QTimer(this);
    receiveTimer->setTimerType(Qt::PreciseTimer);
    receiveTimer->setInterval(33);
    connect(receiveTimer, &QTimer::timeout, this, &WorkManager::receiveGrabFrame);
    receiveTimer->start();
    return true;
}

void WorkManager::stop()
{
    if (processor)
        processor->stop();
    if (connectionTimer)
        connectionTimer->stop();
    if (receiveTimer)
        receiveTimer->stop();
    qDeleteAll(camSubscribers);
    camSubscribers.clear();
    qDeleteAll(rawSubscribers);
    rawSubscribers.clear();
    delete lidarSubscriber;
    lidarSubscriber = nullptr;
}

void WorkManager::selectCamera(const QString& cameraName)
{
    if (!cameraName.isEmpty() && !camNameList.contains(cameraName)) {
        emit logMessage(tr("[WARN] 등록되지 않은 카메라: %1").arg(cameraName));
        return;
    }
    currCamName = cameraName;
    rawStatus.clear();
    lastRawFrame.invalidate();
    if (processor)
        processor->selectCamera(cameraName);
    framePending = false;
    streamStatus.clear();
    lastFrame.invalidate();
    if (!cameraName.isEmpty()) {
        auto* subscriber = camSubscribers.value(cameraName, nullptr);
        if (subscriber)
            subscriber->releaseQueuedData();
        auto* raw = rawSubscribers.value(cameraName, nullptr);
        if (raw)
            raw->releaseQueuedData();
        emit logMessage(tr("[INFO] 카메라 선택: %1").arg(cameraName));
    }
}

void WorkManager::acknowledgeFrame()
{
    framePending = false;
}

void WorkManager::setStreamStatus(const QString& status)
{
    if (streamStatus == status)
        return;
    streamStatus = status;
    emit logMessage(status);
}

void WorkManager::receiveGrabFrame()
{
    receiveLidarFrames();
    receiveRawFrame();
    auto* subscriber = camSubscribers.value(currCamName, nullptr);
    if (!subscriber)
        return;

    if (subscriber->getSubscriptionState() != iox::SubscribeState::SUBSCRIBED) {
        const QString status = tr("[WARN] %1: 카메라 publisher 연결 대기 중").arg(currCamName);
        if (streamStatus != status)
            emit streamUnavailable(currCamName);
        setStreamStatus(status);
        return;
    }

    auto result = subscriber->take();
    if (result.has_error()) {
        if (result.get_error() != iox::popo::ChunkReceiveResult::NO_CHUNK_AVAILABLE) {
            setStreamStatus(tr("[ERROR] %1: iceoryx 프레임 수신 오류").arg(currCamName));
        } else if (!lastFrame.isValid() || lastFrame.elapsed() > 2000) {
            const QString status = tr("[WARN] %1: 영상 프레임 수신 대기 중").arg(currCamName);
            if (streamStatus != status)
                emit streamUnavailable(currCamName);
            setStreamStatus(status);
        }
        return;
    }

    const auto& sample = result.value();
    if (sample.getChunkHeader()->userPayloadSize() < sizeof(CustomCamDataType)) {
        setStreamStatus(tr("[ERROR] %1: CustomCamDataType과 다른 프레임 크기입니다.").arg(currCamName));
        return;
    }
    const auto& frame = *sample;
    const bool threeChannels = pixelFormat == "bgr8" || pixelFormat == "rgb8";
    const quint64 bytes = quint64(frame.width) * frame.height * (threeChannels ? 3U : 1U);
    if (frame.width == 0 || frame.height == 0
        || frame.width > quint32(std::numeric_limits<int>::max() / 3)
        || frame.height > quint32(std::numeric_limits<int>::max())
        || bytes > sizeof(frame.data)) {
        setStreamStatus(tr("[ERROR] %1: 잘못된 프레임 해상도 (%2 × %3)")
                        .arg(currCamName).arg(frame.width).arg(frame.height));
        return;
    }

    try {
        const cv::Mat raw(int(frame.height), int(frame.width), threeChannels ? CV_8UC3 : CV_8UC1,
                          const_cast<uint8_t*>(frame.data));
        cv::Mat rgb;
        if (pixelFormat == "rgb8")
            rgb = raw;
        else if (pixelFormat == "bgr8")
            cv::cvtColor(raw, rgb, cv::COLOR_BGR2RGB);
        else if (pixelFormat == "mono8" || pixelFormat == "gray8")
            cv::cvtColor(raw, rgb, cv::COLOR_GRAY2RGB);
        // OpenCV 4.5 uses two-letter names: BG corresponds to an RGGB mosaic.
        else if (pixelFormat == "bayer_rggb8")
            cv::cvtColor(raw, rgb, cv::COLOR_BayerBG2RGB);
        else if (pixelFormat == "bayer_bggr8")
            cv::cvtColor(raw, rgb, cv::COLOR_BayerRG2RGB);
        else if (pixelFormat == "bayer_grbg8")
            cv::cvtColor(raw, rgb, cv::COLOR_BayerGB2RGB);
        else
            cv::cvtColor(raw, rgb, cv::COLOR_BayerGR2RGB);

        // The iceoryx sample is released on return; the UI must own its pixels.
        const QImage image = QImage(rgb.data, rgb.cols, rgb.rows, int(rgb.step), QImage::Format_RGB888).copy();
        if (image.isNull()) {
            setStreamStatus(tr("[ERROR] %1: 영상 이미지 생성 실패").arg(currCamName));
            return;
        }
        lastFrame.start();
        setStreamStatus(tr("[INFO] %1: 영상 수신 중 (%2 × %3)")
                        .arg(currCamName).arg(frame.width).arg(frame.height));
        // Only one queued image may be in flight, even if painting is slower.
        if (!framePending) {
            framePending = true;
            emit frameReady(currCamName, image);
        }
    } catch (const cv::Exception& error) {
        setStreamStatus(tr("[ERROR] %1: 영상 변환 실패: %2")
                        .arg(currCamName, QString::fromUtf8(error.what())));
    }
}

void WorkManager::acceptMission(const QJsonObject& mission)
{
    if (!mission.isEmpty()) {
        auto* subscriber = camSubscribers.value(currCamName, nullptr);
        if (subscriber)
            subscriber->releaseQueuedData();
        auto* raw = rawSubscribers.value(currCamName, nullptr);
        if (raw)
            raw->releaseQueuedData();
    }
    if (processor)
        processor->acceptMission(mission);
}

void WorkManager::setPrompts(const QJsonObject& prompts)
{
    if (processor)
        processor->setPrompts(prompts);
}

void WorkManager::receiveRawFrame()
{
    auto* subscriber = rawSubscribers.value(currCamName, nullptr);
    if (!processor || !processor->wantsFrames() || !subscriber)
        return;
    auto report = [this](const QString& status) {
        if (rawStatus != status) {
            rawStatus = status;
            emit logMessage(status);
        }
    };
    if (subscriber->getSubscriptionState() != iox::SubscribeState::SUBSCRIBED) {
        report(tr("[WARN] %1: 원본 RAW publisher 대기 (Data / RawImage). 송신 프로그램의 RAW 채널을 확인하세요.").arg(currCamName));
        processor->streamInterrupted();
        emit processingStatusChanged(tr("원본 Bayer RAW 수신 대기"));
        return;
    }
    auto result = subscriber->take();
    if (result.has_error()) {
        if (lastRawFrame.isValid() && lastRawFrame.elapsed() > 1000) {
            processor->streamInterrupted();
            report(tr("[WARN] %1: 원본 RAW 프레임 수신 중단").arg(currCamName));
        }
        return;
    }
    const auto& sample = result.value();
    if (sample.getChunkHeader()->userPayloadSize() < sizeof(CustomCamDataType)) {
        report(tr("[ERROR] 원본 RAW 공유 메모리 구조 크기가 다릅니다."));
        return;
    }
    QString error;
    const auto image = SensorData::bayerImage(*sample, error);
    if (image.isNull()) {
        report(tr("[ERROR] 원본 RAW 변환 실패: %1").arg(error));
        return;
    }
    const auto& frame = *sample;
    lastRawFrame.start();
    // The archive writes this borrowed payload synchronously before the sample is released.
    const auto bayer = QByteArray::fromRawData(reinterpret_cast<const char*>(frame.data), int(frame.width * frame.height));
    processor->receiveFrame(currCamName, image, frame.timestamp, bayer);
    rawStatus.clear();
}

void WorkManager::receiveLidarFrames()
{
    if (!processor || !lidarSubscriber)
        return;
    auto report = [this](const QString& status) {
        if (lidarStatus != status) {
            lidarStatus = status;
            emit logMessage(status);
        }
    };
    if (lidarSubscriber->getSubscriptionState() != iox::SubscribeState::SUBSCRIBED) {
        if (processor->wantsFrames())
            report(tr("[WARN] lidar / Data / Points 연결 대기 중"));
        return;
    }
    for (int i = 0; i < 8; ++i) {
        auto result = lidarSubscriber->take();
        if (result.has_error())
            break;
        const auto& sample = result.value();
        if (sample.getChunkHeader()->userPayloadSize() < sizeof(CustomLidarDataType)) {
            report(tr("[ERROR] 라이다 공유 메모리 구조 크기가 다릅니다. intensity 배열 용량을 확인하세요."));
            continue;
        }
        QString error;
        const auto bytes = SensorData::lidarBin(*sample, error);
        const double timestamp = SensorData::timestampMs(sample->timestamp);
        if (bytes.isEmpty() || timestamp <= 0) {
            report(tr("[ERROR] 라이다 수신 데이터 오류: %1").arg(error));
            continue;
        }
        processor->receiveLidar(bytes, timestamp);
        lidarStatus.clear();
    }
}
