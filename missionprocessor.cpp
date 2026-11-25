#include "missionprocessor.h"
#include "missionlogic.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <cmath>
#include <limits>

MissionProcessor::MissionProcessor(QObject* parent) : QObject(parent)
{
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
        if (count % 10 == 0 || count == 1)
            emit statusChanged(QStringLiteral("%1 수집 중: %2 / %3프레임").arg(cameraName).arg(count).arg(required));
        if (count == required) {
            processing = true;
            emit statusChanged(QStringLiteral("영상 인코딩 마무리 중"));
        }
    });
    connect(&recorder, &ClipRecorder::ready, this, [this](const QString& path, const QJsonObject& metadata) {
        if (stopped || waitingCompletion)
            return;
        clipMetadata = metadata;
        clipMetadata.insert("camera", cameraName);
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
    prompts = {};
    vlm.configure(configPath);
}

void MissionProcessor::clearClip()
{
    recorder.cancel();
    vlm.cancel();
    clipDirectory.reset();
    clipMetadata = {};
    processing = false;
}

void MissionProcessor::stop()
{
    stopped = true;
    clearClip();
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
        if (destination.isEmpty() || destinations.contains(destination) || QFileInfo::exists(destination)) {
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

void MissionProcessor::receiveFrame(const QString& camera, const QImage& image, double timestamp)
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
    }
    recorder.append(image, timestamp);
}

bool MissionProcessor::saveClip(const QString& videoPath, const QString& answer, double seconds, QString& error)
{
    const auto destination = MissionLogic::scenePath(saveRoot, mission.value("storageTarget").toObject(), savedScenes, error);
    if (destination.isEmpty())
        return false;
    const auto parent = QFileInfo(destination).absolutePath();
    if (QFileInfo::exists(destination) || !QDir().mkpath(parent)) {
        error = QStringLiteral("저장 폴더가 이미 존재하거나 생성할 수 없습니다: %1").arg(destination);
        return false;
    }
    QTemporaryDir staging(QDir(parent).filePath(".scene-XXXXXX"));
    if (!staging.isValid() || !QFile::copy(videoPath, staging.filePath("video.mp4"))) {
        error = QStringLiteral("클립 영상 저장 실패");
        return false;
    }
    auto metadata = clipMetadata;
    metadata.insert("mission", mission);
    metadata.insert("prompt", prompt);
    metadata.insert("answer", answer);
    metadata.insert("inferenceSeconds", seconds);
    QSaveFile result(staging.filePath("inference.json"));
    const auto json = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    if (!result.open(QIODevice::WriteOnly) || result.write(json) != json.size() || !result.commit()) {
        error = QStringLiteral("추론 결과 저장 실패");
        return false;
    }
    QByteArray csv("frame,timestamp_ms\n");
    const auto timestamps = clipMetadata.value("timestamps").toArray();
    for (int i = 0; i < timestamps.size(); ++i)
        csv += QByteArray::number(i) + ',' + QByteArray::number(timestamps.at(i).toDouble(), 'f', 3) + '\n';
    QSaveFile frames(staging.filePath("frames.csv"));
    if (!frames.open(QIODevice::WriteOnly) || frames.write(csv) != csv.size() || !frames.commit()
        || !QDir().rename(staging.path(), destination)) {
        error = QStringLiteral("프레임 정보 저장 또는 장면 폴더 확정 실패");
        return false;
    }
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
        QString error;
        if (!saveClip(videoPath, answer, seconds, error)) {
            fail(error);
            return;
        }
        ++savedScenes;
        emit progressChanged(savedScenes, targetScenes);
    } else {
        emit logMessage(QStringLiteral("[INFO] 조건 불일치 클립 제외: %1").arg(reason));
    }
    clearClip();
    if (savedScenes == targetScenes) {
        waitingCompletion = true;
        emit statusChanged(QStringLiteral("저장 완료 · 서버 완료 확인 대기"));
        emit completionRequested(mission.value("dispatchId").toString());
    } else {
        emit statusChanged(QStringLiteral("새 클립 수집 대기"));
    }
}
