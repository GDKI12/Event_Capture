#include "vlmcontroller.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSettings>
#include <QTimer>

VLMController::VLMController(QObject* parent) : QObject(parent) {}
VLMController::~VLMController() { cancel(); }

void VLMController::configure(const QString& configPath)
{
    cancel();
    QSettings settings(configPath, QSettings::IniFormat);
    baseUrl = settings.value("vlm/base_url", "http://127.0.0.1:8000").toString().trimmed();
    while (baseUrl.endsWith('/'))
        baseUrl.chop(1);
    model = settings.value("vlm/model", "cosmos-reason2").toString();
    healthTimeoutMs = qMax(1, settings.value("vlm/health_timeout_ms", 30000).toInt());
    inferTimeoutMs = qMax(1, settings.value("vlm/infer_timeout_ms", 600000).toInt());
}

void VLMController::cancel()
{
    ++generation;
    busy = false;
    for (auto* reply : replies) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    replies.clear();
}

void VLMController::setTimeout(QNetworkReply* reply, int timeoutMs)
{
    QTimer::singleShot(timeoutMs, reply, [reply] {
        if (reply->isRunning()) {
            reply->setProperty("timedOut", true);
            reply->abort();
        }
    });
}

QString VLMController::error(QNetworkReply* reply) const
{
    if (reply->property("timedOut").toBool())
        return QStringLiteral("요청 시간 초과");
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
        return QStringLiteral("HTTP %1: %2").arg(status).arg(reply->errorString());
    return {};
}

void VLMController::isVLMAlive(const QString& videoPath, const QString& prompt)
{
    if (busy)
        return;
    const QUrl url(baseUrl);
    if (!QFileInfo(videoPath).isFile() || prompt.trimmed().isEmpty() || url.host().isEmpty()
        || (url.scheme() != "http" && url.scheme() != "https")) {
        emit failed(QStringLiteral("추론 영상, 프롬프트 또는 VLM 서버 주소가 잘못되었습니다."));
        return;
    }
    busy = true;
    const auto token = generation;
    emit logMessage(QStringLiteral("[INFO] VLM 상태 확인 요청: %1/health").arg(baseUrl));
    auto* reply = manager.get(QNetworkRequest(QUrl(baseUrl + "/health")));
    replies.insert(reply);
    setTimeout(reply, healthTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this, reply, videoPath, prompt, token] {
        replies.remove(reply);
        reply->deleteLater();
        if (token != generation)
            return;
        const auto message = error(reply);
        if (!message.isEmpty()) {
            busy = false;
            emit failed(QStringLiteral("VLM 상태 확인 실패: %1").arg(message));
            return;
        }
        emit logMessage(QStringLiteral("[INFO] VLM 상태 확인 성공"));
        infer(videoPath, prompt);
    });
}

void VLMController::infer(const QString& videoPath, const QString& prompt)
{
    const auto token = generation;
    QNetworkRequest request(QUrl(baseUrl + "/v1/chat/completions"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    const QJsonObject videoUrl{{"url", QUrl::fromLocalFile(videoPath).toString()}};
    const QJsonArray content{
        QJsonObject{{"type", "video_url"}, {"video_url", videoUrl}},
        QJsonObject{{"type", "text"}, {"text", prompt}}
    };
    const QJsonObject body{
        {"model", model}, {"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", content}}}},
        {"max_tokens", 1024}, {"stream", false}
    };
    QElapsedTimer timer;
    timer.start();
    auto* reply = manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    replies.insert(reply);
    setTimeout(reply, inferTimeoutMs);
    emit logMessage(QStringLiteral("[INFO] VLM 추론 요청: %1").arg(QFileInfo(videoPath).fileName()));
    connect(reply, &QNetworkReply::finished, this, [this, reply, videoPath, timer, token] {
        replies.remove(reply);
        reply->deleteLater();
        if (token != generation)
            return;
        busy = false;
        QString message = error(reply);
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(message.isEmpty() ? reply->readAll() : QByteArray(), &parseError);
        const auto choices = document.object().value("choices").toArray();
        QString answer;
        if (!choices.isEmpty()) {
            const auto result = choices.first().toObject().value("message").toObject();
            answer = result.value("content").toString();
            if (answer.trimmed().isEmpty())
                answer = result.value("reasoning").toString();
        }
        if (message.isEmpty() && (parseError.error != QJsonParseError::NoError || answer.trimmed().isEmpty()))
            message = QStringLiteral("추론 응답 JSON 또는 choices.message 내용이 비어 있습니다.");
        if (!message.isEmpty()) {
            emit failed(QStringLiteral("VLM 추론 실패: %1").arg(message));
            return;
        }
        emit finished(videoPath, answer, timer.elapsed() / 1000.0);
    });
}
