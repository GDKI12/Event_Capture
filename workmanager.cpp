#include "workmanager.h"

#include <QDataStream>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <limits>

namespace {
struct ProcessCleanup
{
    QProcess& process;
    ~ProcessCleanup()
    {
        if(process.state() != QProcess::NotRunning)
        {
            process.kill();
            process.waitForFinished(30000);
        }
    }
};
}


WorkManager::WorkManager(QVector<QString> cams, QObject* parent) : QObject(parent)
{
    manager = new QNetworkAccessManager(this);
    healthyTimer = new QTimer(this);
    missionTimer = new QTimer(this);


    // 2 minute
    healthyTimer->setInterval(120000);
    healthyTimer->setTimerType(Qt::PreciseTimer);

    missionTimer->setInterval(60000);
    missionTimer->setTimerType(Qt::PreciseTimer);

    healthyTimer->start();
    missionTimer->start();
    config.loadConfig();

    apiController = new APIController(config.baseURL, config.authId, config.secretKey);

    // Get config setting params
    rootPath = config.rootPath;
    savePath = config.savePath;

    // 로그 설정
    QString logPath = config.logPath;
    logger = new VssLogger(logPath, this);


    mode = config.mode;
    width = config.width;
    height = config.height;

    for(QString name : cams)
    {
        camWorkers[name] = std::make_shared<CamWorker>(name, this);

    }
    // 2분에 한번씩 hearbeat 호출
    connect(healthyTimer, &QTimer::timeout, apiController, &APIController::heartbeat);

    // 1분에 한번씩 mission 풀링
    connect(missionTimer, &QTimer::timeout, [&](){
        apiController->pullingMission();
    });

    // 풀링 완료시 타이머 stop
    connect(apiController, &APIController::stopPullingMission, this,[this](){
        missionTimer->stop();
    });

    connect(apiController, &APIController::getMission, this, &WorkManager::init);
    connect(&watcher, &QFileSystemWatcher::directoryChanged, this, &WorkManager::onFileSystemChanged);

    // summarize된 결과로 해당 파일들을 어떠게 할건지 처리
    connect(this, &WorkManager::requestToProcessSensor, this, &WorkManager::processSensor);

    // cosmos-reason2 추론이벤트 처리
    connect(this, &WorkManager::requestInfer, this, &WorkManager::infer);

    // cosmos-reason2 추론 종료 다음 동영상요청
    connect(this, &WorkManager::finishInfer, this, &WorkManager::nextClip);

    isVLMAlive();
}

WorkManager::~WorkManager()
{
    stop();
}

bool WorkManager::isVLMAlive()
{
    bool isAlive = false;

    QNetworkRequest request(QUrl("http://127.0.0.1:8000/health"));
    QNetworkReply* reply = manager->get(request);

    connect(reply, &QNetworkReply::finished, [reply](){
        if(reply->error() == QNetworkReply::NoError)
        {
            int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

            qDebug() << "status: " << statusCode;
        }
        else{
            Writter::warn("Cosmos-reason2 is not alive");
        }

        reply->deleteLater();
    });

    return isAlive;
}

QString WorkManager::infer(const QString& camId, const QString& videoPath)
{
    QString result;
    QString fileURL = "file://" + videoPath;

    QNetworkRequest request(QUrl("http://127.0.0.1:8000/v1/chat/completions"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject videoURLObj;
    videoURLObj["url"] = fileURL;

    QJsonObject videoObj;
    videoObj["type"] = "video_url";
    videoObj["video_url"] = videoURLObj;

    QJsonObject textObj;
    textObj["type"] = "text";
    textObj["text"] = R"(Analyze the entire driving video and determine whether the following event occurs:

            Event: lane_keep

            Determine whether the ego vehicle maintains its current lane while driving.

            Consider this event to have occurred only if the ego vehicle continuously travels within the same lane without changing lanes or merging into another lane.

            Use the temporal sequence of the video rather than a single frame. Do not classify the event based only on the presence of lane markings.

            If the event clearly occurs, answer YES and briefly describe the visible evidence.
            Otherwise, answer NO.

            Output:
            Event: lane_keep
            Occurred: YES or NO
            Evidence: <brief explanation>)";

    QJsonArray contentArr;
    contentArr.append(videoObj);
    contentArr.append(textObj);


    QJsonObject msgObj;
    msgObj["role"] ="user";
    msgObj["content"] = contentArr;

    QJsonArray messagesArr;
    messagesArr.append(msgObj);

    QJsonObject root;
    root["model"] = "cosmos-reason2";
    root["messages"] = messagesArr;
    root["max_tokens"] = 1024;
    root["stream"] = false;

    QJsonDocument doc(root);
    QByteArray body = doc.toJson(QJsonDocument::Compact);

    QNetworkReply* reply = manager->post(request, body);

    connect(reply, &QNetworkReply::finished, this, [this, reply, camId](){
        if(reply->error() == QNetworkReply::NoError)
        {

            QByteArray response = reply->readAll();

            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);

            QJsonObject root = doc.object();
            QJsonArray choices = root["choices"].toArray();

            if (!choices.isEmpty())
            {
                QJsonObject choice = choices[0].toObject();

                QJsonObject message =
                    choice["message"].toObject();

                QString reasoning =
                    message["reasoning"].toString();

                qDebug().noquote() << reasoning;
                qDebug() << "";
                emit finishInfer(camId);
            }

        }else
        {
            Writter::error("Fail to VLM infer");
            return;
        }

        reply->deleteLater();

    });


    return result;
}

void WorkManager::init(const Mission& mission)
{
    missionCnt = 0;
    this->mission = mission;
    videoLength = mission.clipLengthSec * 10;

    Writter::info("Success to initialize");

    stopping = false;

    if(mode)
        startLiveMode();
    else
        startFileMode();
}

void WorkManager::onFileSystemChanged(const QString& path)
{
    QFileInfo info(path);

    QString baseName = info.baseName();

    QDir dir(path);

    if(path == rootPath)
    {
        QFileInfoList sensorList = dir.entryInfoList({"Sensor_Data*"}, QDir::Dirs | QDir::NoDotAndDotDot,
                                                     QDir::Name);

        QList<QString> curSensors;

        for(const QFileInfo& fi : sensorList)
            curSensors.append(fi.absoluteFilePath());


        for(const QString& f : std::as_const(curSensors))
        {
            if(!watcher.directories().contains(f))
                watcher.addPath(f);
        }

    }else if(baseName.startsWith("Sensor_Data_"))
    {
        QFileInfoList camDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);


        for(const QFileInfo& fi : std::as_const(camDirs))
        {
            if(fi.baseName().startsWith("cam"))
            {
                if(!watcher.directories().contains(fi.absoluteFilePath()))
                    watcher.addPath(fi.absoluteFilePath());
            }
        }

    }else if(baseName.startsWith("cam") && baseName.length() == 4)
    {

        QFileInfoList fiList = dir.entryInfoList({"*raw"}, QDir::Files | QDir::NoDotAndDotDot);

        QFileInfo latestFile;
        QDateTime latestTime;

        for(const QFileInfo& fi : fiList)
        {
            QDateTime createdTime = fi.birthTime();

            if(createdTime.isValid() && (!latestTime.isValid() || createdTime > latestTime))
            {
                latestTime = createdTime;
                latestFile = fi;
            }
        }

        camWorkers[baseName]->addRawFile(latestFile.absoluteFilePath());

        if(camWorkers[baseName]->rawFileSize() >= videoLength)
        {
            createVideo(baseName, camWorkers[baseName]->getRawFiles(videoLength));
        }
    }

}

void WorkManager::startLiveMode()
{
    watcher.addPath(rootPath);
}
void WorkManager::startFileMode()
{
    QVector<QString> sensorDirs;

    camIds.clear();
    QDir dir(rootPath);
    QFileInfoList infos = dir.entryInfoList({"Sensor_Data*"}, QDir::Dirs | QDir::NoDotAndDotDot,
                                            QDir::Name);

    if(infos.isEmpty())
    {
        Writter::warn("There is no sensor data");
        return;
    }

    for(const QFileInfo& fi : infos)
        sensorDirs.push_back(fi.absoluteFilePath());

    QString firstSensorDirPath = sensorDirs.first();
    QDir firstSensorDir(firstSensorDirPath);

    QStringList camList = firstSensorDir.entryList({"cam?"}, QDir::Dirs | QDir::NoDotAndDotDot);



    for(const QString& camId : camList)
    {
        camIds.append(camId);
        camWorkers[camId] = std::make_shared<CamWorker>(camId, this);
        camWorkers[camId]->setSensorDirs(sensorDirs);
        camWorkers[camId]->changeDir();
//        connect(camWorkers[camId].get(), &CamWorker::requestCreateClip, this, &WorkManager::createVideo);


        createVideo(camId, camWorkers[camId]->getRawFiles(videoLength));
    }


}


void WorkManager::stop()
{
    stopping = true;

    const QStringList paths = watcher.directories();
    if(!paths.isEmpty())
        watcher.removePaths(watcher.directories());

}


void WorkManager::createVideo(const QString& camId, const QVector<QString>& clips)
{
    if(stopping || clips.isEmpty())
        return;

    const qint64 rawBytes = qint64(width) * height;
    if(width <= 0 || height <= 0 || rawBytes > std::numeric_limits<int>::max())
    {
        Writter::error("Invalid raw frame dimensions");
        return;
    }
    if(!QDir().mkpath(savePath))
    {
        Writter::error(QString("Failed to create video output directory: %1").arg(savePath));
        return;
    }

    QString randomName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    randomName += ".mp4";

    QString clipPath = QDir(savePath).absoluteFilePath(randomName);

    QProcess ffmpeg;
    ProcessCleanup cleanup{ffmpeg};

    const QStringList args = {
        "-f", "rawvideo",
        "-loglevel", "error",
        "-pixel_format", "bayer_rggb8",
        "-video_size", QString("%1x%2").arg(width).arg(height),
        "-framerate", "10",
        "-i", "pipe:0",
        "-c:v", "libx264",
        "-preset", "veryfast",
        "-crf", "23",
        "-pix_fmt", "yuv420p",
        "-movflags", "+faststart",
        "-y",
        clipPath
    };

    ffmpeg.setProgram("ffmpeg");
    ffmpeg.setArguments(args);
    ffmpeg.setProcessChannelMode(QProcess::SeparateChannels);
    ffmpeg.start();

    if(!ffmpeg.waitForStarted(30000))
    {
        Writter::error("Failt to start ffmpeg");
        return;
    }

    QByteArray frame(rawBytes, Qt::Uninitialized);

    for(const QString& file : clips)
    {
        QFile in(file);

        if(!in.open(QIODevice::ReadOnly)) {
            Writter::error(QString("Failed to open raw file: %1").arg(file));
            return;
        }

        qint64 readBytes = in.read(frame.data(), rawBytes);
        in.close();

        if(readBytes != rawBytes)
        {
            QString log = QString("Fail to read raw file : %1, read = %2, expected = %3")
                    .arg(in.fileName()).arg(readBytes).arg(rawBytes);

            Writter::error(log);
            return;
        }

        qint64 written = 0;

        while(written < frame.size())
        {
            qint64 chunk = ffmpeg.write(frame.constData() + written, frame.size() - written);
            if(chunk < 0)
            {
                QString error = ffmpeg.errorString();
                Writter::error(QString("FFmpeg write failed: %1").arg(error));
                return;
            }

            written += chunk;

            if(ffmpeg.bytesToWrite() > 0 && !ffmpeg.waitForBytesWritten(300000))
            {
                Writter::error(QString("FFmpeg write failed: %1; stderr: %2")
                              .arg(ffmpeg.errorString(), QString::fromLocal8Bit(ffmpeg.readAllStandardError())));
                return;
            }
        }

    }

    ffmpeg.closeWriteChannel();

    if(!ffmpeg.waitForFinished(30000))
    {
        Writter::error("FFmpeg finish timeout");

        ffmpeg.kill();
        ffmpeg.waitForFinished();

        return;
    }

    if(ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0)
    {
        Writter::error(QString("FFmpeg failed: %1").arg(QString::fromLocal8Bit(ffmpeg.readAllStandardError())));

        return;
    }

    Writter::info(QString("Video created: %1").arg(clipPath));

    // 해당영상 추론 요청
    emit requestInfer(camId, clipPath);
}



void WorkManager::createVideo(const QString& camId, std::function<QVector<QString>(int)> callback)
{
    createVideo(camId, callback(videoLength));
}

void WorkManager::nextClip(const QString& camId)
{
    const auto worker = camWorkers.value(camId);
    if(stopping || !worker || videoLength <= 0
            || ((worker->rawFileSize() < videoLength) && worker->sensorDirIsEmpty()) )
    {
        if(worker->rawFileSize() < videoLength && !mode)
        {
            worker->changeDir();
            createVideo(camId, worker->getRawFiles(videoLength));
        }
        return;
    }
    createVideo(camId, worker->getRawFiles(videoLength));
}
bool WorkManager::decideToSave(QStringList answers)
{
    bool isSave = false;
    QString weatherInfo;
    QString timeInfo;
    QString roadInfo;
    QString eventInfo;

    bool isWeather = false;
    bool isTime = false;
    bool isRoad = false;
    bool isEvent = false;


    for(const QString& s : answers)
    {
        QStringList row = s.split(':');

        if(row[0].trimmed() == "Weather")
            weatherInfo = row[1].trimmed();
        else if(row[0].trimmed() == "Time")
            timeInfo = row[1].trimmed();
        else if(row[0].trimmed() == "Road")
            roadInfo = row[1].trimmed();
        else if(row[0].trimmed() == "Event")
        {
            if(eventInfo.isEmpty())
                eventInfo = row[1].trimmed();
            else
                eventInfo.append(QString(" %1").arg(row[1].trimmed()));
        }
        else if(row[0].trimmed() == "Road Features")
        {
            if(eventInfo.isEmpty())
                eventInfo = row[1].trimmed();
            else
                eventInfo.append(QString(" %1").arg(row[1].trimmed()));
        }
    }



    if(mission.weather.isEmpty())
    {
        isWeather = true;
    }else
    {
        for(const QString& s : std::as_const(mission.weather))
        {
            if(weatherInfo.contains(s, Qt::CaseInsensitive) || s == "any")
            {
                isWeather = true;
                break;
            }
        }
    }

    if(mission.time.isEmpty())
    {
        isTime = true;
    }else
    {
        for(const QString& s : std::as_const(mission.time))
        {
            if(timeInfo.contains(s, Qt::CaseInsensitive) || s == "any")
            {
                isTime = true;
                break;
            }
        }
    }

    if(mission.roadEnv.isEmpty())
    {
        isRoad = true;
    }else
    {
        for(const QString& s : std::as_const(mission.roadEnv))
        {
            if(roadInfo.contains(s, Qt::CaseInsensitive) || s == "any")
            {
                isRoad = true;
                break;
            }
        }
    }

    if(mission.scenario.isEmpty())
    {
        isEvent = true;
    }else
    {

        for(const QString& s : std::as_const(mission.scenario))
        {
            if(eventInfo.contains(s, Qt::CaseInsensitive) || s == "any")
            {
                isEvent = true;
                break;
            }
        }
    }

    isSave = isWeather && isTime && isRoad && isEvent;

    qDebug() << "[mission]";
    qDebug() << "weather: " << mission.weather;
    qDebug() << "time: " << mission.time;
    qDebug() << "road: " << mission.roadEnv;
    qDebug() << "event: " << mission.scenario;

    Writter::info(QString("weather: %1").arg(weatherInfo));
    Writter::info(QString("time: %1").arg(timeInfo));
    Writter::info(QString("road: %1").arg(roadInfo));
    Writter::info(QString("event: %1").arg(eventInfo));

    Writter::info(QString("weather: %1, time: %2, road: %3, evnet: %4")
                  .arg(isWeather).arg(isTime).arg(isRoad).arg(isEvent));
    return isSave;
}

void WorkManager::processSensor(const QString& camId, const QString& rootPath, QStringList text)
{
    bool isSave = decideToSave(text);
    QString path = savePath + rootPath;
    Writter::info(QString("Request process file to %1").arg(path));
    camWorkers[camId]->processClip(isSave, path);

    if(isSave)
    {
        missionCnt++;
        mission.saveFolders.dequeue();
        logger->addLog(path, text);

        if(missionCnt == mission.targetScenes)
            missionFinish(mission.id);
    }

}


void WorkManager::missionFinish(const QString& id)
{
    qDebug() << "Start to pulling mission";
    missionTimer->start();
    apiController->finishMission(id);

    stopping = true;
    mission.id.clear();
    mission.deviceType.clear();
    mission.weather.clear();
    mission.time.clear();
    mission.roadEnv.clear();
    mission.scenario.clear();
    mission.clipLengthSec = 0;
    mission.targetScenes = 0;

    missionCnt = 0;
}
