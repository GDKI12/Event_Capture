#ifndef MISSIONCONTROLLER_H
#define MISSIONCONTROLLER_H

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>

class QNetworkReply;
class QNetworkRequest;

class MissionController : public QObject
{
    Q_OBJECT
public:
    explicit MissionController(QObject* parent = nullptr);
    ~MissionController() override;

public slots:
    void start(const QString& configPath);
    void stop();
    void pollMission();
    void heartbeat();

signals:
    // An empty object means the server successfully returned no missions.
    void missionUpdated(const QJsonObject& mission);
    void missionCheckFailed(const QString& message);
    void heartbeatStatusChanged(bool success);
    void logMessage(const QString& message);

private:
    QNetworkRequest request(const QString& path) const;
    void setTimeout(QNetworkReply* reply);
    QString requestError(QNetworkReply* reply) const;
    void reportError(const QString& message, QString& previousError);

    QNetworkAccessManager network;
    QTimer missionTimer;
    QTimer heartbeatTimer;
    QNetworkReply* missionReply = nullptr;
    QNetworkReply* heartbeatReply = nullptr;
    QString baseUrl;
    QByteArray authId;
    QByteArray secretKey;
    QString missionError;
    QString heartbeatError;
    int timeoutMs = 30000;
    bool running = false;
    bool hasMission = false;
};

#endif // MISSIONCONTROLLER_H
