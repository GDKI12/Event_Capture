#include "apicontroller.h"
#include "define.h"
APIController::APIController(QString requestURL, int interval, QObject* parent): QObject(parent)
{
    networkManager = new QNetworkAccessManager(this);

    timer = new QTimer(this);
    timer->setInterval(interval);
    timer->setTimerType(Qt::PreciseTimer);

    missionRequestURL = requestURL;
    connect(timer, &QTimer::timeout, this, &APIController::pullingMission);
    connect(networkManager, &QNetworkAccessManager::finished,
            this, &APIController::onAPIFinished);
}

void APIController::pullingMission()
{
    QUrl url(missionRequestURL);
    QNetworkRequest request(url);

    request.setRawHeader("Accept",
                      "application/json");

    QNetworkReply* reply = networkManager->get(request);
    reply->setProperty("apiType", "mission");
    Writter::info("Request pull mission");
}

void APIController::sendHeartbeat(QString requestURL)
{
    QUrl url(requestURL);

    QNetworkRequest request(url);

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");

    QNetworkReply* reply = networkManager->get(request);
    reply->setProperty("apiType", "heartbeat");

    Writter::info("Request heartbeat");
}

void APIController::onAPIFinished(QNetworkReply *reply)
{
    const QString apiType = reply->property("apiType").toString();

    if(reply->error() == QNetworkReply::NoError)
    {
        if(apiType == "mission")
        {
            QByteArray response = reply->readAll();
            Writter::info("Get Mission");

        }else if(apiType == "heartbeat")
        {

        }
    }else
    {
        Writter::warn("Fail to call mission pulling api: " + reply->errorString());
    }

    reply->deleteLater();
}
