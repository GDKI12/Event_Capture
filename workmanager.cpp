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

void WorkManager::infer(const QString& camId, const QString& videoPath)
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
    textObj["text"] = prompt;

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

    // 추론시간 측정시작
    QElapsedTimer inferTimer;
    inferTimer.start();


    QNetworkReply* reply = manager->post(request, body);

    connect(reply, &QNetworkReply::finished, this, [this, reply, camId, inferTimer](){
        if(reply->error() == QNetworkReply::NoError)
        {
            // 측정 결과
            const qint64 elapsedMs = inferTimer.elapsed();
            const double elapsedSec =
                          static_cast<double>(elapsedMs) / 1000.0;

            Writter::info(QString("[%1] VLM inference finished: %2 ms (%3 sec)")
                          .arg(camId)
                          .arg(elapsedMs)
                          .arg(elapsedSec, 0, 'f', 3));
            // 측정 끝



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
                Writter::info(QString("[%1]").arg(camId));
                qDebug().noquote() << reasoning;
                qDebug() << "";

                if(!stopping)
                {
                    emit finishInfer(camId);
                    emit requestToProcessSensor(camId, reasoning.split("\n\n"));
                }
            }

        }else
        {
            Writter::error("Fail to VLM infer");
            camWorkers[camId]->setStatus(false);
            return;
        }

        reply->deleteLater();

    });
}

void WorkManager::init(const Mission& mission)
{
    missionCnt = 0;
    this->mission = mission;
    videoLength = mission.clipLengthSec * 10;

    prompt.clear();

    prompt = createPrompt();
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
            {
                for(const QString& camId : config.camList)
                    watcher.addPath(f + "/" + camId);
            }
        }

    }else if(baseName.startsWith("Sensor_Data"))
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

        if(fiList.isEmpty())
        {
            Writter::info("No raw file");
            return;
        }

        const QFileInfo latestFile = fiList.constLast();
        const QString rawPath = latestFile.absoluteFilePath();

        if(!camWorkers[baseName]->getStatus())
        {
            camWorkers[baseName]->addRawFile(rawPath);
//            Writter::info(QString("Insert %1 in %2").arg(rawPath, baseName));

            if(camWorkers[baseName]->rawFileSize() >= videoLength)
            {
                if(!camWorkers[baseName]->getStatus())
                {
                    camWorkers[baseName]->setStatus(true);
                    createVideo(baseName, camWorkers[baseName]->getRawFiles(videoLength));
                }
            }
        }
    }

}

void WorkManager::startLiveMode()
{
    Writter::info("Start to live mode");


    for(const QString& camId : config.camList)
    {
        camWorkers[camId] = std::make_shared<CamWorker>(camId, this);
    }

    watcher.addPath(rootPath);

    QDir dir(rootPath);
    QFileInfoList fiList = dir.entryInfoList({"Sensor_Data*"}, QDir::Dirs | QDir::NoDotAndDotDot,
                                             QDir::Name);

    if(fiList.isEmpty())
    {
        Writter::warn("No Working capture program, check it!!");
        return;
    }

    const QFileInfo lastSensorFi = fiList.constLast();
    QString lastSensorDirPath = lastSensorFi.absoluteFilePath();
    QDir lastSensorDir(lastSensorDirPath);

    QStringList subList = lastSensorDir.entryList({"cam*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    for(const QString& sub : subList)
    {
        QString currentSensorCamDir = lastSensorDirPath + "/" + sub;
        watcher.addPath(currentSensorCamDir);
    }
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

QString WorkManager::createPrompt()
{
    QFile file(PORMPT_FILE_PATH);

    if(!file.open(QIODevice::ReadOnly))
    {
        Writter::error("Fail to open prompt file");
        return "";
    }

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject root = doc.object();

    QJsonObject events = root["events"].toObject();
    QJsonObject composition = root["composition"].toObject();

    QStringList eventPrompts;
    QStringList outputEventName;

    for(const QString &key : mission.scenario)
    {
        if(events.contains("any"))
            return "";

        if(!events.contains(key))
            continue;

        QJsonObject event = events[key].toObject();
        eventPrompts.append(event["prompt"].toString());
        outputEventName.append(event["output_event"].toString());

    }

    QString header = composition["selected_event_header_template"].toString();
    header.replace("{{SELECTED_EVENT_NAMES}}",
                   outputEventName.join(composition["selected_event_names_separator"].toString()));

    QString finalPrompt = QStringList{
            root["common_prompt"].toString(),
            header,
            eventPrompts.join(composition["event_separator"].toString()),
            root["output_prompt"].toString()}.join("\n\n");

    return finalPrompt;
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
        camWorkers[camId]->setStatus(false);
        return;
    }
    if(!QDir().mkpath(savePath))
    {
        Writter::error(QString("Failed to create video output directory: %1").arg(savePath));
        camWorkers[camId]->setStatus(false);
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
        camWorkers[camId]->setStatus(false);
        return;
    }

    QByteArray frame(rawBytes, Qt::Uninitialized);

    for(const QString& file : clips)
    {
        QFile in(file);

        if(!in.open(QIODevice::ReadOnly)) {
            Writter::error(QString("Failed to open raw file: %1").arg(file));
            camWorkers[camId]->setStatus(false);
            return;
        }

        qint64 readBytes = in.read(frame.data(), rawBytes);
        in.close();

        if(readBytes != rawBytes)
        {
            QString log = QString("Fail to read raw file : %1, read = %2, expected = %3")
                    .arg(in.fileName()).arg(readBytes).arg(rawBytes);

            Writter::error(log);
            camWorkers[camId]->setStatus(false);
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
                camWorkers[camId]->setStatus(false);
                return;
            }

            written += chunk;

            if(ffmpeg.bytesToWrite() > 0 && !ffmpeg.waitForBytesWritten(300000))
            {
                Writter::error(QString("FFmpeg write failed: %1; stderr: %2")
                              .arg(ffmpeg.errorString(), QString::fromLocal8Bit(ffmpeg.readAllStandardError())));
                camWorkers[camId]->setStatus(false);
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

        camWorkers[camId]->setStatus(false);
        return;
    }

    if(ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0)
    {
        Writter::error(QString("FFmpeg failed: %1").arg(QString::fromLocal8Bit(ffmpeg.readAllStandardError())));
        camWorkers[camId]->setStatus(false);
        return;
    }

    Writter::info(QString("Video created: %1").arg(clipPath));

    // 해당영상 추론 요청
    if(!prompt.isEmpty())
    {
        emit requestInfer(camId, clipPath);
    }
    else
    {
        QString path = savePath + "";
        camWorkers[camId]->processClip(true, path);
    }

}



void WorkManager::createVideo(const QString& camId, std::function<QVector<QString>(int)> callback)
{
    createVideo(camId, callback(videoLength));
}

void WorkManager::nextClip(const QString& camId)
{
    const auto worker = camWorkers.value(camId);
    if(!mode)
    {
        if(stopping || !worker || videoLength <= 0 || (worker->rawFileSize() < videoLength)
                || ((worker->rawFileSize() < videoLength) && worker->sensorDirIsEmpty()) )
        {
            if(worker->rawFileSize() < videoLength && !mode)
            {
                // 여기서 index 에러발생
                if(!worker->sensorDirIsEmpty())
                {
                    worker->changeDir();
                    createVideo(camId, worker->getRawFiles(videoLength));
                }
                else
                {
                    Writter::info("Finish process");
                    return;
                }
            }
            return;
        }

        createVideo(camId, worker->getRawFiles(videoLength));

    }else{
        worker->setStatus(false);
    }
}
bool WorkManager::decideToSave(QStringList answers)
{
    return true;
}

void WorkManager::processSensor(const QString& camId, QStringList text)
{
    if(stopping)
    {
        return;
    }
    bool isSave = decideToSave(text);

    if(mission.saveFolders.isEmpty())
        return;
    QString path = savePath + mission.saveFolders.first();
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
