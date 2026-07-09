#include "workmanager.h"

#include <QElapsedTimer>
WorkManager::WorkManager(QVector<QString> cams, QObject* parent) : QObject(parent)
{
    vssSocket = new QTcpSocket(this);

    socket = new QTcpSocket(this);
    ffmpeg = new QProcess(this);

    config.loadConfig();

    // Get config setting params
    rootPath = config.rootPath;
    dstIp = config.ip;

    dstPort = config.port;
    metaPort = dstPort + 100;
    initPort = 4303;
    timeInterval = config.timeInterval;

    videoLength = config.videoLength;

    mode = config.mode;
    width = config.width;
    height = config.height;

    frames = timeInterval / 100;
    int idx = 0;
    for(QString name : cams)
    {
        camWorkers[name] = std::make_shared<CamWorker>(name, dstPort + idx, this);
        idx++;
    }
    connect(this, &WorkManager::requestToProcessSensor, this, &WorkManager::onProcessSensor);
    connect(&watcher, &QFileSystemWatcher::directoryChanged, this, &WorkManager::onFileSystemChanged);

    connect(vssSocket, &QTcpSocket::connected, this, [=](){
        Writter::info("Connected to VSS server");
    });

    connect(vssSocket, static_cast<void(QTcpSocket::*)(QAbstractSocket::SocketError)>(&QTcpSocket::error), this,
            [=](QAbstractSocket::SocketError){
        Writter::error(QString("VSS socket error: %1").arg(vssSocket->errorString()));
    });
    connect(socket, &QTcpSocket::readyRead, this, [=]()
    {
        // TODO
        Writter::info("GET DATA");
    });

    connect(vssSocket, &QTcpSocket::readyRead, this, [=](){
        QByteArray data = vssSocket->readAll();

        getVssInfos(data);
        Writter::info(QString("Receive replies from VSS_Server: %1").arg(QString::fromUtf8(data)));

        start();
    });

    vssSocket->connectToHost(dstIp, initPort);

    init();

    start();
}

WorkManager::~WorkManager()
{
    closeMetaSocket();

    socket->deleteLater();
    ffmpeg->deleteLater();
}

void WorkManager::init()
{
    Writter::info("Start to initialize");

    QDir dir(rootPath);
    QFileInfoList infos = dir.entryInfoList({"Sensor_Data*"}, QDir::Dirs | QDir::NoDotAndDotDot,
                                            QDir::Name);

    for(const QFileInfo& fi : infos)
        sensorDirs.enqueue(fi.absoluteFilePath());

    QString firstSensorDirPath = sensorDirs.first();
    QDir firstSensorDir(firstSensorDirPath);
    QStringList camList = firstSensorDir.entryList({"cam*"}, QDir::Dirs | QDir::NoDotAndDotDot);

    camN = camList.size();

    sendToServer(camList.size(), videoLength);

    Writter::info(QString("Load sensor folder from %1").arg(rootPath));
    Writter::info(QString("Initial sensor folder list : %1").arg(QStringList(sensorDirs.begin(), sensorDirs.end()).join(",")));

    currDir = sensorDirs.dequeue();

    for(int i = 1; i < camN+1; i++)
    {
        QString camName = QString("cam%1").arg(i);
        camWorkers[camName].get()->addRawFiles(currDir);

        Writter::info(QString("%1 is working...").arg(camName));
    }


    Writter::info("Success to initialize");
}


bool WorkManager::isSensorDirReady(const QString& dir)
{
    for(int i = 1; i < camN + 1; i++)
    {
        QDir camDir(QString("%1/cam%2").arg(dir, i));

        if(!camDir.exists())
            return false;

        QFileInfoList rawList = camDir.entryInfoList({"*.raw"},
                                                     QDir::Files | QDir::NoDotAndDotDot);
        if(rawList.size() < 3000)
            return false;
    }

    return true;
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

        QSet<QString> curSensors;

        for(const QFileInfo& fi : sensorList)
            curSensors.insert(fi.absoluteFilePath());

        QSet<QString> added = curSensors - preSensors;

        for(const QString& f : added)
        {
            QString sensorDir = QString("%1/%2").arg(f);

            Writter::info(QString("Added folder : %1").arg(sensorDir));
            preSensors.insert(f);
            watcher.addPath(sensorDir);
        }

    }else if(baseName.startsWith("Sensor_Data_"))
    {
        for(int i = 0; i < camN; i++)
        {
            QString camId = QString("cam%1").arg(i+1);
            QString camPath = QString("%1/%2").arg(path, camId);

            watcher.addPath(camPath);
        }
    }else if(baseName.startsWith("cam"))
    {
        dir.cdUp();

        QString sensorPath = dir.absolutePath();

        if(isSensorDirReady(dir.absolutePath()))
        {
            if(!queuedSensors.contains(sensorPath))
            {
                sensorDirs.enqueue(sensorPath);
                queuedSensors.insert(sensorPath);
            }
        }
    }

}

void WorkManager::start()
{
    bool isChange = true;

    QVector<bool> camValid;
    camValid.resize(camN);

    for(int i = 0; i < camN; i++)
    {
        int rawLen = camWorkers[QString("cam%1").arg(i+1)]->rawFileSize();

        if(rawLen == 0 || rawLen < videoLength)
            camValid[i] = false;
        else
            camValid[i] = true;
    }

    for(int i = 0; i < camN; i++)
        isChange &= !camValid[i];

    if(sensorDirs.isEmpty() && !camValid[0])
    {
        emit requestToProcessSensor(currDir);
        Writter::info("No sensor dir in queue");
        return;
    }else
    {
        if(isChange)
        {
            emit requestToProcessSensor(currDir);

            currDir = sensorDirs.dequeue();
            for(int i = 1 ; i < camN+1; i++)
                camWorkers[QString("cam%1").arg(i)]->addRawFiles(currDir);
        }
    }


    for(int i = 0; i < camN; i++)
    {
        QVector<QString> clips
                = camWorkers[QString("cam%1").arg(i+1)]->getRawFiles(timeInterval, videoLength);

        int n = camWorkers[QString("cam%1").arg(i+1)]->rawFileSize() / videoLength;
        Writter::info(QString("Current Dir : %1, Left : %2").arg(QString("%1/cam%2").arg(currDir, QString::number(i+1)), QString::number(n)));

        sendClip(clips, camWorkers[QString("cam%1").arg(i+1)].get());
    }


}

void WorkManager::stop()
{
    closeMetaSocket(-1);
    socket->deleteLater();

    closeVssSocket(-1);
    vssSocket->deleteLater();

    stopFfmpeg();
    ffmpeg->deleteLater();

    watcher.removePaths(watcher.directories());
}


void WorkManager::sendClip(const QVector<QString> &clips, CamWorker* camWorker)
{
    int metaPort = camWorker->getPort() + 100;
    QString metaLog = QString("Send %1 meta port : %2").arg(camWorker->getCamId()).arg(metaPort);
    Writter::info(metaLog);

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhMMss");

    closeMetaSocket();
    socket->connectToHost(dstIp, metaPort);

    if(!socket->waitForConnected(3000))
        return;

    // Send Meta Info
    QJsonObject obj;
    QDir dir(currDir);

    obj["videoName"] = QString("%1_%2_%3.mp4").arg(dir.dirName(), camWorker->getCamId(), timestamp);

    QByteArray header = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    header.append("\n");

    socket->write(header);

    if (!socket->waitForBytesWritten(3000))
        closeMetaSocket();

    closeMetaSocket();

    if(!ensureFfmpegRunning(camWorker))
        return;

    qint64 rawBytes = (qint64) width * height;
    QByteArray frame(rawBytes, Qt::Uninitialized);
    QElapsedTimer timer;
    timer.start();

    for(const QString& file : clips)
    {
        QFile in(file);
        if(!in.open(QIODevice::ReadOnly)) continue;

        qint64 readBytes = in.read(frame.data(), rawBytes);

        in.close();

        if(readBytes != rawBytes)
        {
            QString log = QString("Fail to read raw file : %1, read = %2, expected = %3")
                    .arg(in.fileName()).arg(readBytes).arg(rawBytes);

            Writter::error(log);

            stopFfmpeg();
            return;
        }

        qint64 written = 0;

        while(written < frame.size())
        {
            qint64 chunk = ffmpeg->write(frame.constData() + written, frame.size() - written);
            if(chunk < 0)
            {
                stopFfmpeg();
                return;
            }

            written += chunk;

            if(!ffmpeg->waitForBytesWritten(-1))
            {
                qCritical() << "[ERROR] Failt to flush ffmpeg stdin flush:" << in.fileName();
                qCritical() << "[ERROR] state =" << ffmpeg->state();
                qCritical() << "[ERROR] exitCode =" << ffmpeg->exitCode();
                qCritical() << "[ERROR] error =" << ffmpeg->errorString();

                stopFfmpeg();
                return;
            }

        }
    }

    if (ffmpeg->state() != QProcess::Running)
        return;

    stopFfmpeg();

    Writter::info(QString("Success to send clip of %1").arg(camWorker->getCamId()));
    return;
}

void WorkManager::sendToServer(int camN, int videoL, int fps)
{
    InitConfig initConfig;

    initConfig.camSize = camN;
    initConfig.videoLength = videoL;
    initConfig.fps = fps;

    if(vssSocket->state() == QAbstractSocket::UnconnectedState)
    {
        vssSocket->connectToHost(dstIp, initPort);

        if(!vssSocket->waitForConnected(3000))
        {
            Writter::error(QString("Write timeout: %1").arg(vssSocket->errorString()));
            // TODO
            // STOP logic
        }
    }

    qint64  writeBytes = vssSocket->write(reinterpret_cast<const char*>(&initConfig), sizeof(initConfig));

    if (writeBytes == -1)
    {
        Writter::error("Fail to send to server");
        return;

        // STOP logic
        stop();
    }

    if(!vssSocket->waitForBytesWritten(3000))
    {
        Writter::error("Time out send to server");

        // STOP logic
        stop();
    }

    Writter::info("Success to send init config params");
}
bool WorkManager::ensureFfmpegRunning(CamWorker* camWorker)
{
    if(ffmpeg->state() == QProcess::Running)
        return true;
    const QString url = QString("tcp://%1:%2").arg(dstIp).arg(camWorker->getPort());
          const QStringList args = {
              "-f", "rawvideo",
              "-loglevel", "error",
              "-pixel_format", "bayer_rggb8",
              "-video_size", QString("%1x%2").arg(width).arg(height),
              "-framerate", "10",
              "-i", "pipe:0",
              "-vf", "format=bgr24",
              "-c:v", "libx264",
              "-preset", "veryfast",
              "-tune", "zerolatency",
              "-pix_fmt", "yuv420p",
              "-f", "mpegts",
              url
          };

    ffmpeg->setProgram("ffmpeg");
    ffmpeg->setArguments(args);
    ffmpeg->setProcessChannelMode(QProcess::SeparateChannels);
    ffmpeg->start();

    if(!ffmpeg->waitForStarted(-1))
    {
      return false;
    }

    return true;
}

void WorkManager::stopFfmpeg()
{
    if(ffmpeg->state() == QProcess::NotRunning)
        return;

    ffmpeg->closeWriteChannel();
    if(ffmpeg->waitForFinished(60000))
        return;

    ffmpeg->kill();
    ffmpeg->waitForFinished();
}

void WorkManager::closeMetaSocket(int timeoutMs)
{
    if(!socket || socket->state() == QAbstractSocket::UnconnectedState)
        return;

    socket->flush();
    socket->disconnectFromHost();

    if(socket->state() != QAbstractSocket::UnconnectedState)
        socket->waitForDisconnected(timeoutMs);
}

void WorkManager::closeVssSocket(int timeoutMs)
{
    if(!socket || socket->state() == QAbstractSocket::UnconnectedState)
        return;

    socket->flush();
    socket->disconnectFromHost();

    if(socket->state() != QAbstractSocket::UnconnectedState)
        socket->waitForDisconnected(timeoutMs);
}

void WorkManager::getVssInfos(const QByteArray& data)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);

    if (err.error != QJsonParseError::NoError) {
        qDebug() << "Parse Error:" << err.errorString();
        return;
    }

    QJsonObject obj = doc.object();

    VssInfo &vssInfo = vssInfos[currDir];
    (vssInfo.isEvent).append(static_cast<quint8>(obj.value("isEvent").toInt()));
    (vssInfo.weather).append(static_cast<quint8>(obj.value("weather").toInt()));
    (vssInfo.eventType).append(static_cast<quint8>(obj.value("eventType").toInt()));

}

void WorkManager::onProcessSensor(const QString& preDir)
{
    if(!vssInfos.contains(preDir))
    {
        Writter::warn(QString("There is no crrespond path, %1").arg(preDir));
        return;
    }

    Writter::info(QString("Process Sensor: %1").arg(preDir));
    bool eventHappened = false;

    VssInfo vInfo = vssInfos[preDir];
    QVector<quint8> isEventedList = vInfo.isEvent;

    for(int i = 0; i < isEventedList.size(); i++)
    {
        bool eventValue = false;

        if(isEventedList[i] == 0x00)
            eventValue = false;
        else if(isEventedList[i] == 0x01)
            eventValue = true;
        else{
            // TODO
        }

        eventHappened = eventHappened || eventValue;
    }

    if(!eventHappened)
    {
        // TODO
        QDir removeDir(preDir);
        if(removeDir.exists())
        {
            if(removeDir.removeRecursively())
            {
                Writter::info(QString("Success to remove folder : %1").arg(preDir));
            }
            else
            {
                Writter::warn(QString("Failed to remove folder : %1").arg(preDir));
            }
        }
    }
}
