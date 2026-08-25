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
    APIController(QString requestURL, int interval, QObject* parent = nullptr);

public slots:
    void pullingMission();
    void onAPIFinished(QNetworkReply *reply);
    void sendHeartbeat(QString requestURL);
private:
    QTimer *timer;
    QNetworkAccessManager *networkManager;

    QString missionRequestURL;
};
#endif // APICONTROLLER_H
