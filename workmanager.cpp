#include "workmanager.h"
#include "missionprocessor.h"
#include "sensordata.h"
#include "capturediagnostics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

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
    // Image is the single camera input for preview, inference and BGR8 RAW storage.
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
        emit logMessage(tr("[INFO] 카메라 구독: %1 / Data / Image").arg(name));
    }
    options.queueCapacity = 8U;
    lidarSubscriber = new Subscriber<CustomLidarDataType>({"lidar", "Data", "Points"}, options);
    emit logMessage(tr("[INFO] 라이다 구독: lidar / Data / Points"));
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
    if (processor)
        processor->selectCamera(cameraName);
    framePending = false;
    streamStatus.clear();
    lastFrame.invalidate();
#ifndef QT_NO_DEBUG
    debugCameraTimestamp = 0;
    debugCameraArrival.invalidate();
#endif
    if (!cameraName.isEmpty()) {
        auto* subscriber = camSubscribers.value(cameraName, nullptr);
        if (subscriber)
            subscriber->releaseQueuedData();
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
#ifndef QT_NO_DEBUG
    if (debugReceivePoll.isValid() && debugReceivePoll.elapsed() > 100)
        CAPTURE_TRACE(QStringLiteral("receive_poll_delay poll_gap_ms=%1").arg(debugReceivePoll.elapsed()));
    debugReceivePoll.start();
#endif
    receiveLidarFrames();
    auto* subscriber = camSubscribers.value(currCamName, nullptr);
    if (!subscriber)
        return;

    if (subscriber->getSubscriptionState() != iox::SubscribeState::SUBSCRIBED) {
        if (processor)
            processor->streamInterrupted();
        const QString status = tr("[WARN] %1: 카메라 publisher 연결 대기 중").arg(currCamName);
        if (streamStatus != status)
            emit streamUnavailable(currCamName);
        setStreamStatus(status);
        return;
    }

    if (subscriber->hasMissedData())
        CAPTURE_TRACE(QStringLiteral("iceoryx_queue_overflow channel=%1/Data/Image capacity=1 discarded_old_frames=true").arg(currCamName));
    auto result = subscriber->take();
    if (result.has_error()) {
        if (result.get_error() != iox::popo::ChunkReceiveResult::NO_CHUNK_AVAILABLE) {
            setStreamStatus(tr("[ERROR] %1: iceoryx 프레임 수신 오류").arg(currCamName));
        } else if (!lastFrame.isValid() || lastFrame.elapsed() > 2000) {
            if (processor)
                processor->streamInterrupted();
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
    consumeImageFrame(*sample);
}

void WorkManager::consumeImageFrame(const CustomCamDataType& frame)
{
#ifndef QT_NO_DEBUG
    QElapsedTimer elapsed;
    elapsed.start();
    const auto arrivalMs = debugCameraArrival.isValid() ? debugCameraArrival.elapsed() : 0;
    debugCameraArrival.start();
    CAPTURE_TRACE(QStringLiteral("image_receive camera=%1 previous_ms=%2 current_ms=%3 delta_ms=%4 arrival_gap_ms=%5 collecting=%6")
                  .arg(currCamName).arg(debugCameraTimestamp, 0, 'f', 3).arg(frame.timestamp, 0, 'f', 3)
                  .arg(debugCameraTimestamp > 0 ? frame.timestamp - debugCameraTimestamp : 0, 0, 'f', 3)
                  .arg(arrivalMs).arg(processor && processor->wantsFrames()));
    debugCameraTimestamp = frame.timestamp;
#endif
    QString error;
    const auto image = SensorData::cameraImage(frame, pixelFormat, error);
    if (image.isNull()) {
        setStreamStatus(tr("[ERROR] %1: 영상 변환 실패: %2").arg(currCamName, error));
        return;
    }
    lastFrame.start();
#ifndef QT_NO_DEBUG
    const auto convertNs = elapsed.nsecsElapsed();
#endif
    // The selected Image sample drives inference independently of preview painting.
    if (processor && processor->wantsFrames()) {
        const auto bgr = pixelFormat == "bgr8"
            ? QByteArray::fromRawData(reinterpret_cast<const char*>(frame.data), int(frame.width * frame.height * 3))
            : SensorData::bgrBytes(image);
        processor->receiveFrame(currCamName, image, frame.timestamp, bgr);
    }
    CAPTURE_TRACE(QStringLiteral("image_receive_cost camera=%1 convert_ms=%2 enqueue_encode_ms=%3 total_ms=%4")
                  .arg(currCamName).arg(convertNs / 1e6, 0, 'f', 3)
                  .arg((elapsed.nsecsElapsed() - convertNs) / 1e6, 0, 'f', 3).arg(elapsed.nsecsElapsed() / 1e6, 0, 'f', 3));
    setStreamStatus(tr("[INFO] %1: 영상 수신 중 (%2 × %3)")
                    .arg(currCamName).arg(frame.width).arg(frame.height));
    if (!framePending) {
        framePending = true;
        emit frameReady(currCamName, image);
    }
}

void WorkManager::acceptMission(const QJsonObject& mission)
{
    if (!mission.isEmpty()) {
        auto* subscriber = camSubscribers.value(currCamName, nullptr);
        if (subscriber)
            subscriber->releaseQueuedData();
    }
    if (processor)
        processor->acceptMission(mission);
}

void WorkManager::setPrompts(const QJsonObject& prompts)
{
    if (processor)
        processor->setPrompts(prompts);
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
    if (lidarSubscriber->hasMissedData())
        CAPTURE_TRACE(QStringLiteral("iceoryx_queue_overflow channel=lidar/Data/Points capacity=8 discarded_old_frames=true"));
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
#ifndef QT_NO_DEBUG
        QElapsedTimer elapsed;
        elapsed.start();
#endif
        const auto bytes = SensorData::lidarBin(*sample, error);
        const double timestamp = SensorData::timestampMs(sample->timestamp);
        if (bytes.isEmpty() || timestamp <= 0) {
            report(tr("[ERROR] 라이다 수신 데이터 오류: %1").arg(error));
            continue;
        }
        processor->receiveLidar(bytes, timestamp);
        CAPTURE_TRACE(QStringLiteral("lidar_receive previous_ms=%1 current_ms=%2 delta_ms=%3 bytes=%4 serialize_enqueue_ms=%5")
                      .arg(debugLidarTimestamp, 0, 'f', 3).arg(timestamp, 0, 'f', 3)
                      .arg(debugLidarTimestamp > 0 ? timestamp - debugLidarTimestamp : 0, 0, 'f', 3)
                      .arg(bytes.size()).arg(elapsed.nsecsElapsed() / 1e6, 0, 'f', 3));
#ifndef QT_NO_DEBUG
        debugLidarTimestamp = timestamp;
#endif
        lidarStatus.clear();
    }
}
