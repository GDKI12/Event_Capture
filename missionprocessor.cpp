#include "missionprocessor.h"
#include "missionlogic.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <cmath>
#include <limits>

MissionProcessor::MissionProcessor(QObject* parent) : QObject(parent)
{
    lidarWaitTimer.setSingleShot(true);
    connect(&lidarWaitTimer, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("라이다 시간 동기화 실패: %1").arg(lastSyncError));
    });
    connect(&recorder, &ClipRecorder::failed, this, [this](const QString& message, bool retryable) {
        if (!retryable) {
            fail(message);
            return;
        }
        clearClip();
        emit logMessage(QStringLiteral("[WARN] %1").arg(message));
        emit statusChanged(QStringLiteral("수신 대기 · 클립 재수집"));
    });
    connect(&recorder, &ClipRecorder::progress, this, [this](int count, int required) {
        if (count % 10 == 0 || count == 1) {
            emit statusChanged(QStringLiteral("%1 수집 중: %2 / %3프레임").arg(cameraName).arg(count).arg(required));
            emit logMessage(QStringLiteral("[INFO] %1 RAW 수집: %2 / %3프레임").arg(cameraName).arg(count).arg(required));
        }
        if (count == required) {
            processing = true;
            emit statusChanged(QStringLiteral("영상 인코딩 마무리 중"));
        }
    });
    connect(&recorder, &ClipRecorder::ready, this, [this](const QString& path, const QJsonObject& metadata) {
        if (stopped || waitingCompletion)
            return;
        emit logMessage(QStringLiteral("[INFO] MP4 인코딩 완료: %1 (%2프레임)")
                        .arg(path).arg(metadata.value("frameCount").toInt()));
        emit statusChanged(QStringLiteral("VLM 상태 확인 및 추론 중"));
        vlm.isVLMAlive(path, prompt);
    });
    connect(&vlm, &VLMController::logMessage, this, &MissionProcessor::logMessage);
    connect(&vlm, &VLMController::failed, this, &MissionProcessor::fail);
    connect(&vlm, &VLMController::finished, this, &MissionProcessor::processAnswer);
}

void MissionProcessor::configure(const QString& configPath)
{
    stop();
    QSettings settings(configPath, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    saveRoot = settings.value("setting/save_path").toString().trimmed();
    if (!saveRoot.isEmpty())
        saveRoot = QFileInfo(saveRoot).absoluteFilePath();
    cacheRoot = QDir(saveRoot).filePath(".event-capture-cache");
    templatePath = QDir(QFileInfo(configPath).absolutePath()).filePath("prompt.json");
    maxGapMs = qMax(101, settings.value("shared_memory/max_frame_gap_ms", 180).toInt());
    lidarToleranceMs = qMax(1, settings.value("lidar/max_time_diff_ms", 100).toInt());
    lidarWaitTimer.setInterval(qMax(1, settings.value("lidar/wait_timeout_ms", 2000).toInt()));
    prompts = {};
    vlm.configure(configPath);
}

void MissionProcessor::clearClip()
{
    recorder.cancel();
    vlm.cancel();
    lidarWaitTimer.stop();
    archive.clear();
    sensorPairs = {};
    acceptedVideo.clear();
    clipDirectory.reset();
    processing = false;
}

void MissionProcessor::stop()
{
    stopped = true;
    clearClip();
    recentLidar.clear();
    mission = {};
    prompt.clear();
    waitingCompletion = false;
}

void MissionProcessor::fail(const QString& message)
{
    stopped = true;
    clearClip();
    emit logMessage(QStringLiteral("[ERROR] 미션 처리 중단: %1").arg(message));
    emit statusChanged(QStringLiteral("처리 중단: %1").arg(message));
}

void MissionProcessor::acceptMission(const QJsonObject& value)
{
    if (!value.isEmpty() && value.value("dispatchId") == mission.value("dispatchId"))
        return;
    stop();
    savedScenes = 0;
    targetScenes = 0;
    if (value.isEmpty()) {
        emit statusChanged(QString());
        emit progressChanged(0, 0);
        return;
    }
    mission = value;
    const auto volume = mission.value("volume").toObject();
    const double seconds = volume.value("clipLengthSec").toDouble();
    const double target = volume.value("targetSceneCount").toDouble();
    if (mission.value("dispatchId").toString().isEmpty() || seconds < 1 || seconds != std::floor(seconds)
        || seconds > std::numeric_limits<int>::max() / 10 || target < 1 || target != std::floor(target)
        || target > std::numeric_limits<int>::max() || saveRoot.isEmpty()) {
        fail(QStringLiteral("미션 ID, 클립 길이, 목표 장면 수 또는 save_path를 확인하세요."));
        return;
    }
    requiredFrames = int(seconds) * 10;
    targetScenes = int(target);
    const auto storage = mission.value("storageTarget").toObject();
    if (storage.value("scenes").toArray().size() < targetScenes) {
        fail(QStringLiteral("목표 장면 수보다 미션 저장 폴더 수가 적습니다."));
        return;
    }
    QSet<QString> destinations;
    for (int i = 0; i < targetScenes; ++i) {
        QString error;
        const auto destination = MissionLogic::scenePath(saveRoot, storage, i, error);
        if (destination.isEmpty() || destinations.contains(destination) || !SensorArchive::emptyScene(destination)) {
            fail(error.isEmpty() ? QStringLiteral("미션 저장 폴더가 중복되거나 이미 존재합니다.") : error);
            return;
        }
        destinations.insert(destination);
    }
    const auto roadConditions = mission.value("conditions").toObject().value("roadEnvironments").toArray();
    if (mission.value("deviceType").toString() != "infra" && !roadConditions.isEmpty()
        && !roadConditions.contains(QJsonValue("any"))) {
        fail(QStringLiteral("차량 미션의 도로 조건 검사에는 GPS 정보가 필요합니다."));
        return;
    }
    stopped = false;
    lastPromptError.clear();
    emit progressChanged(0, targetScenes);
    preparePrompt();
}

void MissionProcessor::setPrompts(const QJsonObject& value)
{
    prompts = value;
    if (!stopped && prompt.isEmpty())
        preparePrompt();
}

void MissionProcessor::preparePrompt()
{
    QString error;
    prompt = MissionLogic::createPrompt(mission, prompts, templatePath, error);
    if (prompt.isEmpty()) {
        if (lastPromptError != error)
            emit logMessage(QStringLiteral("[WARN] 프롬프트 준비 대기: %1").arg(error));
        lastPromptError = error;
        emit statusChanged(QStringLiteral("프롬프트 준비 대기: %1").arg(error));
        return;
    }
    emit logMessage(QStringLiteral("[INFO] 미션 프롬프트 생성 완료: %1자, 클립 %2초 (%3프레임)")
                    .arg(prompt.size()).arg(requiredFrames / 10).arg(requiredFrames));
    emit statusChanged(cameraName.isEmpty() ? QStringLiteral("추론할 카메라 선택 대기") : QStringLiteral("새 클립 수집 대기"));
}

void MissionProcessor::selectCamera(const QString& camera)
{
    if (cameraName == camera)
        return;
    clearClip();
    cameraName = camera;
    if (!stopped && !waitingCompletion)
        emit statusChanged(camera.isEmpty() ? QStringLiteral("추론할 카메라 선택 대기") : QStringLiteral("카메라 변경 · 새 클립 수집 대기"));
}

void MissionProcessor::streamInterrupted()
{
    if (recorder.isActive() && !processing) {
        clearClip();
        emit logMessage(QStringLiteral("[WARN] 카메라 수신 중단: 미완성 클립 폐기"));
        emit statusChanged(QStringLiteral("카메라 수신 대기"));
    }
}

void MissionProcessor::receiveFrame(const QString& camera, const QImage& image, double timestamp, const QByteArray& originalBayer)
{
    if (stopped || waitingCompletion || prompt.isEmpty() || processing || camera != cameraName || camera.isEmpty())
        return;
    if (!recorder.isActive()) {
        if (!QDir().mkpath(cacheRoot)) {
            fail(QStringLiteral("임시 영상 폴더 생성 실패: %1").arg(cacheRoot));
            return;
        }
        clipDirectory.reset(new QTemporaryDir(QDir(cacheRoot).filePath("clip-XXXXXX")));
        if (!clipDirectory->isValid()) {
            fail(QStringLiteral("임시 클립 생성 실패"));
            return;
        }
        recorder.begin(clipDirectory->filePath("clip.mp4"), requiredFrames, 10, maxGapMs);
        archive.begin(clipDirectory->path(), lidarToleranceMs);
        emit logMessage(QStringLiteral("[INFO] %1 원본 RAW 클립 수집 시작: %2초, 목표 %3프레임")
                        .arg(cameraName).arg(requiredFrames / 10).arg(requiredFrames));
    }
    recorder.append(image, timestamp);
    // append() may reject a discontinuous frame and clear the current clip.
    if (!clipDirectory || !recorder.isActive())
        return;
    QString error;
    if (!archive.addImage(originalBayer, image.width(), image.height(), timestamp, error)) {
        fail(error);
        return;
    }
    if (archive.imageCount() == 1) {
        for (const auto& lidar : recentLidar) {
            if (!archive.addLidar(lidar.second, lidar.first, error)) {
                fail(error);
                return;
            }
        }
    }
    if (archive.imageCount() == requiredFrames)
        archive.finishImages();
}

void MissionProcessor::receiveLidar(const QByteArray& xyzi, double timestamp)
{
    if (stopped || waitingCompletion)
        return;
    recentLidar.append({timestamp, xyzi});
    while (recentLidar.size() > 4)
        recentLidar.removeFirst();
    if (!clipDirectory)
        return;
    QString error;
    if (!archive.addLidar(xyzi, timestamp, error)) {
        fail(error);
        return;
    }
    if (!acceptedVideo.isEmpty())
        finishAcceptedClip();
}

bool MissionProcessor::saveClip(QString& error)
{
    const auto destination = MissionLogic::scenePath(saveRoot, mission.value("storageTarget").toObject(), savedScenes, error);
    if (destination.isEmpty())
        return false;
    const auto parent = QFileInfo(destination).absolutePath();
    if (!SensorArchive::emptyScene(destination) || !QDir().mkpath(parent)) {
        error = QStringLiteral("저장 폴더가 이미 존재하거나 생성할 수 없습니다: %1").arg(destination);
        return false;
    }
    QTemporaryDir staging(QDir(parent).filePath(".scene-XXXXXX"));
    if (!staging.isValid()) {
        error = QStringLiteral("센서 파일 저장용 임시 폴더 생성 실패");
        return false;
    }
    if (!archive.stage(staging.path(), sensorPairs, error)
        || !SensorArchive::commitScene(staging.path(), destination, error))
        return false;
    emit logMessage(QStringLiteral("[INFO] 조건 일치 클립 저장: %1").arg(destination));
    return true;
}

void MissionProcessor::processAnswer(const QString& videoPath, const QString& answer, double seconds)
{
    if (stopped || !processing)
        return;
    emit inferenceResult(answer);
    emit logMessage(QStringLiteral("[INFO] 추론 결과 (%1초):\n%2").arg(seconds, 0, 'f', 1).arg(answer));
    QString reason;
    if (MissionLogic::matches(mission, answer, reason)) {
        acceptedVideo = videoPath;
        finishAcceptedClip();
        return;
    } else {
        emit logMessage(QStringLiteral("[INFO] 조건 불일치 클립 제외: %1").arg(reason));
    }
    clearClip();
    emit statusChanged(QStringLiteral("새 클립 수집 대기"));
}

void MissionProcessor::finishAcceptedClip()
{
    if (acceptedVideo.isEmpty() || stopped)
        return;
    if (!archive.match(sensorPairs, lastSyncError)) {
        if (!lidarWaitTimer.isActive())
            lidarWaitTimer.start();
        emit statusChanged(QStringLiteral("라이다 시간 동기화 대기"));
        return;
    }
    lidarWaitTimer.stop();
    QString error;
    if (!saveClip(error)) {
        fail(error);
        return;
    }
    ++savedScenes;
    emit progressChanged(savedScenes, targetScenes);
    clearClip();
    if (savedScenes == targetScenes) {
        waitingCompletion = true;
        emit statusChanged(QStringLiteral("저장 완료 · 서버 완료 확인 대기"));
        emit completionRequested(mission.value("dispatchId").toString());
    } else {
        emit statusChanged(QStringLiteral("새 클립 수집 대기"));
    }
}
