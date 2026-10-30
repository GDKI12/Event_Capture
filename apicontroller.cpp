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
    commonPromptURL = baseURL + "/api/devices/common-codes/requirements";

    heartbeat();
    pullPrompt();
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

    connect(reply, &QNetworkReply::finished, [reply, this](){
        reply->deleteLater();

        if(reply->error() != QNetworkReply::NoError)
        {
            return;
        }

        Mission mission;
        QByteArray response = reply->readAll();

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(response, &error);

        if(error.error != QJsonParseError::NoError)
            Writter::error("Mission response parsing error: " + error.errorString());

        QJsonArray missions = doc.array();

        if(missions.isEmpty())
        {
            Writter::info("Can not find mission");
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
        QString parentDir = storageTargetObj["writePath"].toString();
        mission.id = missionObj["dispatchId"].toString();
        mission.deviceType = missionObj["deviceType"].toString();
        mission.clipLengthSec = volume["clipLengthSec"].toInt();
        mission.targetScenes = volume["targetSceneCount"].toInt();

        for(const QJsonValue& value : scenes)
        {
            QString path = parentDir + "/" + value["relativePath"].toString();
            mission.saveFolders.enqueue(path);
        }

        for(const QJsonValue& value : weatherArr)
            mission.weather.append(value.toString());

        for(const QJsonValue& value : timeArr)
            mission.time.append(value.toString());

        for(const QJsonValue& value : roadArr)
            mission.roadEnv.append(value.toString());

        for(const QJsonValue& value : scenarioArr)
        {
//                if(value.toString())
            mission.scenario.append(value.toString());
        }

        Writter::info("Get Mission");
        emit stopPullingMission();
        emit getMission(mission);
    });
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

    connect(reply, &QNetworkReply::finished, this, [this, reply](){
        reply->deleteLater();

        if(reply->error() != QNetworkReply::NoError)
        {
            return;
        }

        Writter::info("Success to heartbeat");
        emit hasPendingMissions();

        if(!start)
        {
            start = true;
            pullingMission();
        }
    });
}

void APIController::pullPrompt()
{
    QUrl url(commonPromptURL);

    QNetworkRequest request(url);

    request.setRawHeader("X-Device-Id", authId.toUtf8());
    request.setRawHeader("X-Device-Auth-Key", secretKey.toUtf8());

    QNetworkReply* reply = networkManager->get(request);
    reply->setProperty("apiType", "prompt");

    Writter::info("Request commont prompts");

    connect(reply, &QNetworkReply::finished, [reply, this](){
        reply->deleteLater();

        if(reply->error() != QNetworkReply::NoError)
        {
            return;
        }

        QMap<QString, QString> promptMap;

        QByteArray response = reply->readAll();
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(response, &error);

        if(error.error != QJsonParseError::NoError)
            Writter::error("Pulling prompt parsing error: " + error.errorString());

        QJsonObject root = doc.object();
        QJsonArray groupsArr = root["groups"].toArray();
        QJsonArray scenarioArr;

        for(const QJsonValue& value : groupsArr)
        {
            if(value["code"].toString() == "scenario")
                scenarioArr = value["items"].toArray();
        }

        for(const QJsonValue& value : scenarioArr)
        {
            QJsonObject obj = value.toObject();
            promptMap[obj["code"].toString()] = obj["collectionPrompt"].toString();
        }

        emit getPrompts(promptMap);

    });
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

    connect(reply, &QNetworkReply::finished, [reply, this](){
        reply->deleteLater();

        if(reply->error() != QNetworkReply::NoError)
        {
            return;
        }
    });

}
void APIController::finishMission(const QString& id)
{
    QString urlPath = baseURL + "/api/devices/missions/" + id + "/collected";
    QUrl url(urlPath);


    QNetworkRequest request(url);

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("X-Device-Id", authId.toUtf8());
    request.setRawHeader("X-Device-Auth-Key", secretKey.toUtf8());

    QNetworkReply* reply = networkManager->post(request, QByteArray());
    reply->setProperty("apiType", "collected");

    connect(reply, &QNetworkReply::finished, [reply, this](){
        reply->deleteLater();

        if(reply->error() != QNetworkReply::NoError)
        {
            const QString failedUrl = reply->url().toString();
            Writter::warn("Fail to send Finished mission");
            return;
        }

        const QString url = reply->url().toString();
        QStringList splitedUrl = url.split('/');
        QString id = splitedUrl[splitedUrl.size() - 2];

        emit successToFinished(id);
    });
}
