#include "apicontroller.h"
#include "define.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonObject>
APIController::APIController(const QString& baseURL, const QString& id
                             , const QString& pwd, QObject* parent): QObject(parent)
{
    start = false;

    networkManager = new QNetworkAccessManager(this);

    authId = id;
    secretKey = pwd;
    this->baseURL = baseURL;
    missionRequestURL = baseURL + "/api/devices/missions/pull";
    heartbeatURL = baseURL + "/api/devices/heartbeat";

    connect(networkManager, &QNetworkAccessManager::finished,
            this, &APIController::onAPIFinished);

    heartbeat();
}

void APIController::pullingMission()
{
    QUrl url(missionRequestURL);
    QNetworkRequest request(url);

    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Device-Id", authId.toUtf8());
    request.setRawHeader("X-Device-Auth-Key", secretKey.toUtf8());

    QNetworkReply* reply = networkManager->post(request, QByteArray());

    reply->setProperty("apiType", "mission");
    Writter::info("Request pull mission");
}

void APIController::heartbeat()
{
    QUrl url(heartbeatURL);

    QNetworkRequest request(url);

    QJsonObject obj;
    QJsonObject pos;
    pos["lat"] = 36.5;
    pos["lng"] = 127.25;

    obj["status"] = "online";
    obj["batteryPercent"] = 88;
    obj["location"] = pos;

    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);


    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("X-Device-Id", authId.toUtf8());
    request.setRawHeader("X-Device-Auth-Key", secretKey.toUtf8());

    QNetworkReply* reply = networkManager->post(request, body);
    reply->setProperty("apiType", "heartbeat");

    Writter::info("Request heartbeat");
}

void APIController::processStatus(const QString& id, double rate)
{
    QString urlPath = baseURL + "/api/devices/missions/" + id + "/progress";
    QUrl url(urlPath);

    QJsonObject object;
    object["progressPercent"] = rate;
    object["status"] = "running";
    object["message"] = "test";

    QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("X-Device-Id", authId.toUtf8());
    request.setRawHeader("X-Device-Auth-Key", secretKey.toUtf8());

    QNetworkReply* reply = networkManager->post(request, body);
    reply->setProperty("apiType", "progress");

}
void APIController::finishMission(const QString& id)
{
    QString urlPath = baseURL + "/api/devices/missions/" + id + "/collected";
    QUrl url(urlPath);

    QJsonObject obj;
    obj["eventId"] = "test_event";
    obj["message"] = "complete";

    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QNetworkRequest request(url);

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("X-Device-Id", authId.toUtf8());
    request.setRawHeader("X-Device-Auth-Key", secretKey.toUtf8());

    QNetworkReply* reply = networkManager->post(request, body);
    reply->setProperty("apiType", "collected");
}

void APIController::onAPIFinished(QNetworkReply *reply)
{
    const QString apiType = reply->property("apiType").toString();

    if(reply->error() == QNetworkReply::NoError)
    {
        if(apiType == "mission")
        {
            Mission mission;

            QByteArray response = reply->readAll();

            QJsonParseError error;
            QJsonDocument doc = QJsonDocument::fromJson(response, &error);

            if(error.error != QJsonParseError::NoError)
            {
                Writter::error("Mission json response parsing error: " + error.errorString());
            }

            QJsonArray missions = doc.array();

            if(missions.isEmpty())
            {
                Writter::info("No mission");
                return;
            }
            QJsonObject missionObj = missions[0].toObject();
            QJsonObject conditions = missionObj["conditions"].toObject();

            QJsonArray weatherArr = conditions["weatherConditions"].toArray();
            QJsonArray timeArr = conditions["timeConditions"].toArray();
            QJsonArray roadArr = conditions["roadEnvironments"].toArray();
            QJsonArray scenarioArr = conditions["scenarios"].toArray();

            QJsonObject volume = missionObj["volume"].toObject();

            QJsonObject storageTargetObj = missionObj["storageTarget"].toObject();
            QJsonArray scenes = storageTargetObj["scenes"].toArray();

            mission.id = missionObj["dispatchId"].toString();
            mission.deviceType = missionObj["deviceType"].toString();
            mission.clipLengthSec = volume["clipLengthSec"].toInt();
            mission.targetScenes = volume["targetSceneCount"].toInt();

            for(const QJsonValue& value : scenes)
                mission.saveFolders.enqueue(value["relativePath"].toString());

            for(const QJsonValue& value : weatherArr)
                mission.weather.append(value.toString());

            for(const QJsonValue& value : timeArr)
                mission.time.append(value.toString());

            for(const QJsonValue& value : roadArr)
                mission.roadEnv.append(value.toString());

            for(const QJsonValue& value : scenarioArr)
                mission.scenario.append(value.toString());

            Writter::info("Get Mission");
            emit getMission(mission);


        }else if(apiType == "heartbeat")
        {
            Writter::info("Success to heartbeat");

            if(!start)
            {
                start = true;
                pullingMission();
            }
        }else if(apiType == "progress")
        {

        }else if(apiType == "collected")
        {
            Writter::info("Finish mission successfully");
        }
    }else
    {
        QString errorLog = reply->errorString();
        Writter::warn("Fail to call mission pulling api: " + reply->errorString());
    }

    reply->deleteLater();
}
