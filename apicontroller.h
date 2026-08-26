#ifndef APICONTROLLER_H
#define APICONTROLLER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QDateTime>
#include "define.h"

class APIController : public QObject
{
    Q_OBJECT
public:
    APIController(const QString& baseURL, const QString& id
                  , const QString& pwd, QObject* parent = nullptr);

public slots:
    void pullingMission();
    void heartbeat();
    void processStatus(const QString& id, double rate);
    void finishMission(const QString& id);
    void onAPIFinished(QNetworkReply *reply);
signals:
    void getMission(const Mission& mission);
private:
    bool start;
    QNetworkAccessManager *networkManager;

    QString baseURL;
    QString missionRequestURL;
    QString heartbeatURL;
    QString collectedURL;

    QString authId;
    QString secretKey;
};
#endif // APICONTROLLER_H
