#include "missionprocessor.h"
#include "missionlogic.h"
#include "sensorarchive.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <cmath>
#include <limits>

MissionProcessor::MissionProcessor(QObject* parent) : QObject(parent)
{
    connect(&writer, &ArchiveWriter::failed, this, [this](quint64 token, const QString& message) {
        if (token == clipToken && !stopped)
            fail(message);
    }, Qt::QueuedConnection);
    connect(&writer, &ArchiveWriter::waitingForLidar, this, [this](quint64 token, const QString& reason) {
        if (token != clipToken || stopped || !commitPending)
            return;
        lastSyncError = reason;
        if (!lidarWaitTimer.isActive())
            lidarWaitTimer.start();
        emit statusChanged(QStringLiteral("라이다 시간 동기화 대기"));
    }, Qt::QueuedConnection);
    connect(&writer, &ArchiveWriter::saved, this, &MissionProcessor::clipSaved, Qt::QueuedConnection);
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
            emit logMessage(QStringLiteral("[INFO] %1 / Data / Image 수집: %2 / %3프레임").arg(cameraName).arg(count).arg(required));
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
    writer.setQueueLimit(qint64(qBound(16, settings.value("capture/write_queue_mb", 256).toInt(), 2048)) * 1024 * 1024);
    prompts = {};
    vlm.configure(configPath);
}

void MissionProcessor::clearClip()
{
    recorder.cancel();
    vlm.cancel();
    lidarWaitTimer.stop();
    // Transfer our last directory reference before waking the cleanup worker.
    writer.cancel(std::move(clipDirectory));
    clipToken = 0;
    capturedFrames = 0;
    commitPending = false;
    acceptedVideo.clear();
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
    pendingScenes.clear();
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
    QVector<int> remaining;
    int existingScenes = 0;
    for (int i = 0; i < targetScenes; ++i) {
        QString error;
        const auto destination = MissionLogic::scenePath(saveRoot, storage, i, error);
        if (destination.isEmpty() || destinations.contains(destination)) {
            fail(error.isEmpty() ? QStringLiteral("미션 저장 폴더 경로가 중복됩니다: %1").arg(destination) : error);
            return;
        }
        destinations.insert(destination);
        if (SensorArchive::emptyScene(destination)) {
            remaining.append(i);
        } else if (SensorArchive::completedScene(destination, requiredFrames, lidarToleranceMs, maxGapMs, error)) {
            ++existingScenes;
            emit logMessage(QStringLiteral("[INFO] 기존 수집 완료 장면 확인: %1").arg(destination));
        } else {
            fail(QStringLiteral("기존 장면이 미완성이거나 잘못되었습니다: %1 (%2)").arg(destination, error));
            return;
        }
    }
    const auto roadConditions = mission.value("conditions").toObject().value("roadEnvironments").toArray();
    if (!remaining.isEmpty() && mission.value("deviceType").toString() != "infra" && !roadConditions.isEmpty()
        && !roadConditions.contains(QJsonValue("any"))) {
        fail(QStringLiteral("차량 미션의 도로 조건 검사에는 GPS 정보가 필요합니다."));
        return;
    }
    stopped = false;
    pendingScenes = remaining;
    savedScenes = existingScenes;
    lastPromptError.clear();
    emit progressChanged(savedScenes, targetScenes);
    emit logMessage(QStringLiteral("[INFO] 미션 수집 현황: 기존 완료 %1 / %2장면, 추가 수집 %3장면")
                    .arg(savedScenes).arg(targetScenes).arg(pendingScenes.size()));
    if (pendingScenes.isEmpty()) {
        requestCompletion();
        return;
    }
    preparePrompt();
}

void MissionProcessor::setPrompts(const QJsonObject& value)
{
    prompts = value;
    if (!stopped && !waitingCompletion && prompt.isEmpty())
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
    if (commitPending) {
        // An accepted scene may already be crossing the final rename boundary.
        // Finish and count it before applying the new camera to the next clip.
        cameraName = camera;
        emit statusChanged(QStringLiteral("RAW·라이다 저장 마무리 중 · 다음 카메라: %1")
                           .arg(camera.isEmpty() ? QStringLiteral("선택 대기") : camera));
        return;
    }
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

void MissionProcessor::receiveFrame(const QString& camera, const QImage& image, double timestamp, const QByteArray& packedBgr)
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
        clipToken = writer.begin(clipDirectory, lidarToleranceMs, requiredFrames);
        emit logMessage(QStringLiteral("[INFO] %1 / Data / Image 클립 수집 시작: %2초, 목표 %3프레임")
                        .arg(cameraName).arg(requiredFrames / 10).arg(requiredFrames));
    }
    recorder.append(image, timestamp);
    // append() may reject a discontinuous frame and clear the current clip.
    if (!clipDirectory || !recorder.isActive())
        return;
    if (!writer.image(clipToken, packedBgr, image.width(), image.height(), timestamp)) {
        fail(QStringLiteral("파일 저장 큐가 가득 찼습니다. 디스크 속도와 capture/write_queue_mb를 확인하세요."));
        return;
    }
    if (++capturedFrames == 1) {
        for (const auto& lidar : recentLidar) {
            if (!writer.lidar(clipToken, lidar.second, lidar.first)) {
                fail(QStringLiteral("라이다 파일 저장 큐가 가득 찼습니다."));
                return;
            }
        }
    }
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
    if (!writer.lidar(clipToken, xyzi, timestamp))
        fail(QStringLiteral("라이다 파일 저장 큐가 가득 찼습니다. 디스크 속도와 capture/write_queue_mb를 확인하세요."));
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
    if (acceptedVideo.isEmpty() || stopped || commitPending)
        return;
    if (pendingScenes.isEmpty()) {
        fail(QStringLiteral("추가 수집할 장면이 없습니다."));
        return;
    }
    QString error;
    const auto destination = MissionLogic::scenePath(saveRoot, mission.value("storageTarget").toObject(), pendingScenes.first(), error);
    if (destination.isEmpty()) {
        fail(error);
        return;
    }
    commitPending = true;
    emit statusChanged(QStringLiteral("RAW·라이다 저장 마무리 중"));
    if (!writer.commit(clipToken, destination))
        fail(QStringLiteral("장면 저장 요청 큐가 가득 찼습니다."));
}

void MissionProcessor::clipSaved(quint64 token, const QString& destination)
{
    if (token != clipToken || stopped || !commitPending)
        return;
    emit logMessage(QStringLiteral("[INFO] 조건 일치 클립 저장: %1").arg(destination));
    pendingScenes.removeFirst();
    ++savedScenes;
    emit progressChanged(savedScenes, targetScenes);
    clearClip();
    if (savedScenes == targetScenes) {
        requestCompletion();
    } else {
        emit statusChanged(QStringLiteral("새 클립 수집 대기"));
    }
}

void MissionProcessor::requestCompletion()
{
    waitingCompletion = true;
    emit statusChanged(QStringLiteral("저장 완료 · 서버 완료 확인 대기"));
    emit completionRequested(mission.value("dispatchId").toString());
}
