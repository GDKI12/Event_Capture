#include "missioncontroller.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>

MissionController::MissionController(QObject* parent) : QObject(parent)
{
    connect(&missionTimer, &QTimer::timeout, this, &MissionController::pollMission);
    connect(&heartbeatTimer, &QTimer::timeout, this, &MissionController::heartbeat);
}

MissionController::~MissionController()
{
    stop();
}

void MissionController::start(const QString& configPath)
{
    if (running)
        return;

    if (configPath.isEmpty() || !QFileInfo(configPath).isFile()) {
        emit logMessage(tr("[ERROR] 미션 API 설정 파일을 찾을 수 없습니다."));
        emit missionCheckFailed(tr("설정 파일을 확인하고 프로그램을 다시 실행하세요."));
        return;
    }

    QSettings settings(configPath, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    baseUrl = settings.value("setting/base_url").toString().trimmed();
    while (baseUrl.endsWith('/'))
        baseUrl.chop(1);
    authId = settings.value("setting/auth_id").toString().trimmed().toUtf8();
    secretKey = settings.value("setting/secret_key").toString().trimmed().toUtf8();
    const int missionInterval = settings.value("setting/mission_poll_interval_ms", 60000).toInt();
    const int heartbeatInterval = settings.value("setting/heartbeat_interval_ms", 120000).toInt();
    timeoutMs = settings.value("setting/api_timeout_ms", 30000).toInt();
    const QUrl url(baseUrl);
    if (settings.status() != QSettings::NoError || !url.isValid() || url.host().isEmpty()
        || (url.scheme() != "http" && url.scheme() != "https")
        || url.hasQuery() || url.hasFragment() || !url.userInfo().isEmpty()
        || authId.isEmpty() || secretKey.isEmpty()
        || missionInterval <= 0 || heartbeatInterval <= 0 || timeoutMs <= 0) {
        emit logMessage(tr("[ERROR] 미션 API 설정을 확인하세요: base_url, 인증 정보, 호출 주기."));
        emit missionCheckFailed(tr("서버 주소, 인증 정보와 호출 주기를 확인하고 프로그램을 다시 실행하세요."));
        return;
    }

    hasMission = false;
    promptsLoaded = false;
    activeMissionId.clear();
    pendingCompletionId.clear();
    initialHeartbeatSucceeded = false;
    missionError.clear();
    heartbeatError.clear();
    running = true;
    missionTimer.setInterval(missionInterval);
    heartbeatTimer.start(heartbeatInterval);
    emit logMessage(tr("[INFO] 미션 API 시작: 미션 조회 %1초, heartbeat %2초")
                    .arg(missionInterval / 1000.0).arg(heartbeatInterval / 1000.0));
    heartbeat();
}

void MissionController::stop()
{
    running = false;
    initialHeartbeatSucceeded = false;
    missionTimer.stop();
    heartbeatTimer.stop();
    for (QNetworkReply* reply : {missionReply, heartbeatReply, promptsReply, completionReply}) {
        if (!reply)
            continue;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    missionReply = nullptr;
    heartbeatReply = nullptr;
    promptsReply = nullptr;
    completionReply = nullptr;
}

QNetworkRequest MissionController::request(const QString& path) const
{
    QNetworkRequest result{QUrl(baseUrl + path)};
    result.setRawHeader("Accept", "application/json");
    result.setRawHeader("X-Device-Id", authId);
    result.setRawHeader("X-Device-Auth-Key", secretKey);
    // Keep device credentials on the configured API endpoint.
    result.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    return result;
}

void MissionController::setTimeout(QNetworkReply* reply)
{
    QTimer::singleShot(timeoutMs, reply, [reply] {
        if (reply->isRunning()) {
            reply->setProperty("timedOut", true);
            reply->abort();
        }
    });
}

QString MissionController::requestError(QNetworkReply* reply) const
{
    if (reply->property("timedOut").toBool())
        return tr("요청 시간 초과");
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
        return tr("HTTP %1: %2").arg(status).arg(reply->errorString());
    return {};
}

void MissionController::reportError(const QString& message, QString& previousError)
{
    if (previousError != message)
        emit logMessage(message);
    previousError = message;
}

void MissionController::pollMission()
{
    if (!running || !initialHeartbeatSucceeded || hasMission || missionReply)
        return;
    auto* reply = manager.post(request("/api/devices/missions/pull"), QByteArray());
    missionReply = reply;
    setTimeout(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        missionReply = nullptr;
        reply->deleteLater();
        QString error = requestError(reply);
        QJsonArray missions;
        if (error.isEmpty() && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 204) {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(reply->readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isArray())
                error = tr("미션 응답 JSON 형식 오류");
            else
                missions = document.array();
        }
        if (error.isEmpty() && !missions.isEmpty()
            && (!missions.first().isObject()
                || missions.first().toObject().value("dispatchId").toString().trimmed().isEmpty()))
            error = tr("미션 응답에 dispatchId가 없습니다.");

        if (!error.isEmpty()) {
            reportError(tr("[ERROR] 미션 조회 실패: %1").arg(error), missionError);
            emit missionCheckFailed(tr("%1\n다음 조회 주기에 다시 시도합니다.").arg(error));
            return;
        }
        if (!missionError.isEmpty())
            emit logMessage(tr("[INFO] 미션 API 연결 복구"));
        missionError.clear();
        if (missions.isEmpty()) {
            emit missionUpdated(QJsonObject());
            return;
        }

        // pull dispatches a mission, rather than listing current missions.
        // Retain the accepted mission and do not consume another dispatch.
        hasMission = true;
        missionTimer.stop();
        const auto mission = missions.first().toObject();
        activeMissionId = mission.value("dispatchId").toString();
        emit logMessage(tr("[INFO] 미션 수신: dispatchId=%1").arg(mission.value("dispatchId").toString()));
        emit missionUpdated(mission);
    });
}

void MissionController::heartbeat()
{
    if (!running || heartbeatReply)
        return;
    auto heartbeatRequest = request("/api/devices/heartbeat");
    heartbeatRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    const QJsonObject body{{"status", "online"}};
    auto* reply = manager.post(heartbeatRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));
    heartbeatReply = reply;
    setTimeout(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        heartbeatReply = nullptr;
        reply->deleteLater();
        const QString error = requestError(reply);
        if (!error.isEmpty()) {
            reportError(tr("[ERROR] heartbeat 실패: %1").arg(error), heartbeatError);
            emit heartbeatStatusChanged(false);
            return;
        }
        if (!heartbeatError.isEmpty())
            emit logMessage(tr("[INFO] heartbeat 연결 복구"));
        heartbeatError.clear();
        if (!promptsLoaded)
            pullPrompts();
        if (!pendingCompletionId.isEmpty())
            completeMission(pendingCompletionId);
        // Start mission polling only after the first successful heartbeat.
        if (running && !initialHeartbeatSucceeded) {
            initialHeartbeatSucceeded = true;
            missionTimer.start();
            pollMission();
        }
        emit heartbeatStatusChanged(true);
    });
}

void MissionController::pullPrompts()
{
    if (!running || promptsReply)
        return;
    auto* reply = manager.get(request("/api/devices/common-codes/requirements"));
    promptsReply = reply;
    setTimeout(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        promptsReply = nullptr;
        reply->deleteLater();
        QString error = requestError(reply);
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(error.isEmpty() ? reply->readAll() : QByteArray(), &parseError);
        if (error.isEmpty() && (parseError.error != QJsonParseError::NoError || !document.isObject()
                               || !document.object().value("groups").isArray()))
            error = tr("시나리오 프롬프트 응답 JSON 형식 오류");
        if (!error.isEmpty()) {
            emit logMessage(tr("[ERROR] 프롬프트 조회 실패: %1 (다음 heartbeat 후 재시도)").arg(error));
            return;
        }
        QJsonObject prompts;
        for (const auto& group : document.object().value("groups").toArray()) {
            if (group.toObject().value("code").toString() != "scenario")
                continue;
            for (const auto& item : group.toObject().value("items").toArray()) {
                const auto object = item.toObject();
                const auto code = object.value("code").toString();
                if (!code.isEmpty())
                    prompts.insert(code, object.value("collectionPrompt").toString());
            }
        }
        promptsLoaded = true;
        emit logMessage(tr("[INFO] 서버 시나리오 프롬프트 수신: %1개").arg(prompts.size()));
        emit promptsUpdated(prompts);
    });
}

void MissionController::completeMission(const QString& dispatchId)
{
    if (!running || !hasMission || dispatchId.isEmpty() || dispatchId != activeMissionId || completionReply)
        return;
    pendingCompletionId = dispatchId;
    auto completionRequest = request("/api/devices/missions/"
                                     + QString::fromUtf8(QUrl::toPercentEncoding(dispatchId)) + "/collected");
    completionRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto* reply = manager.post(completionRequest, QByteArray());
    completionReply = reply;
    setTimeout(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, dispatchId] {
        completionReply = nullptr;
        reply->deleteLater();
        const auto error = requestError(reply);
        if (!error.isEmpty()) {
            emit logMessage(tr("[ERROR] 미션 완료 통보 실패: %1 (다음 heartbeat 후 재시도)").arg(error));
            return;
        }
        if (dispatchId != activeMissionId)
            return;
        pendingCompletionId.clear();
        activeMissionId.clear();
        hasMission = false;
        emit logMessage(tr("[INFO] 서버 미션 완료 확인: %1").arg(dispatchId));
        emit missionCompletionConfirmed(dispatchId);
        emit missionUpdated(QJsonObject());
        missionTimer.start();
    });
}
