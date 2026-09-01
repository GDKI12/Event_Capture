#include "workmanager.h"

#include <QDataStream>
#include <QElapsedTimer>
#include <QCoreApplication>

WorkManager::WorkManager(QVector<QString> cams, QObject* parent) : QObject(parent)
{
    manager = new QNetworkAccessManager(this);
    ffmpeg = new QProcess(this);
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

    dstIp = config.ip;

    dstPort = config.port;

    mode = config.mode;
    width = config.width;
    height = config.height;

    frames = timeInterval / 100;
    int idx = 0;

    receivedN = 0;

    watcher.addPath(rootPath);

    for(QString name : cams)
    {
        camWorkers[name] = std::make_shared<CamWorker>(name, dstPort + idx, this);

        QTcpSocket* socket = new QTcpSocket(this);
        videoSockets.insert(name, socket);

        QTimer* resultTimer = new QTimer(this);
        resultTimer->setSingleShot(true);
        resultTimers.insert(name, resultTimer);

        connect(resultTimer, &QTimer::timeout,
                this, [this, name]() { onResultTimeout(name); });

        connect(socket, &QTcpSocket::readyRead,
                this, [this, name]() { readServerResult(name); });

        connect(socket,
                static_cast<void(QTcpSocket::*)(QAbstractSocket::SocketError)>
                    (&QTcpSocket::error),
                this,
                [socket, name](QAbstractSocket::SocketError) {
            Writter::error(QString("Video socket error (%1): %2")
                           .arg(name, socket->errorString()));
        });

        connect(socket, &QTcpSocket::disconnected,
                this, [this, name]() { onVideoDisconnected(name); });
        idx++;
    }
    // 2분에 한번씩 hearbeat 호출
    connect(healthyTimer, &QTimer::timeout, apiController, &APIController::heartbeat);

    // 1분에 한번씩 mission 풀링
    connect(missionTimer, &QTimer::timeout, [&](){
        apiController->pullingMission();
    });

    // 풀릴 완료시 타이머 stop
    connect(apiController, &APIController::stopPullingMission, this,[this](){
        missionTimer->stop();
    });

    connect(apiController, &APIController::getMission, this, &WorkManager::init);
    connect(&watcher, &QFileSystemWatcher::directoryChanged, this, &WorkManager::onFileSystemChanged);

    // summarize된 결과로 해당 파일들을 어떠게 할건지 처리
    connect(this, &WorkManager::requestToProcessSensor, this, &WorkManager::processSensor);

    QUrl url(config.vssHealthyURL);

    QNetworkRequest request(url);
    QNetworkReply* reply = manager->get(request);

    connect(reply, &QNetworkReply::finished, [reply](){
        if(reply->error() == QNetworkReply::NoError)
        {
            QByteArray data = reply->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(data);
            QJsonObject obj = doc.object();
            bool vssAgentStatus = obj["success"].toBool();

            reply->deleteLater();

            if(!vssAgentStatus)
            {
                Writter::error("Not alive vss agent");
                QCoreApplication::exit(EXIT_FAILURE);
                return;
            }
        }else{
            qDebug() << "VSS agent heathy api error: " << reply->errorString();
        }

        reply->deleteLater();
    });

}

WorkManager::~WorkManager()
{
    stop();
}


void WorkManager::init(const Mission& mission)
{
    missionCnt = 0;
    this->mission = mission;
    videoLength = mission.clipLengthSec * 10;
    timeInterval = videoLength;

    Writter::info("Start to initialize");

    QDir dir(rootPath);
    QFileInfoList infos = dir.entryInfoList({"Sensor_Data*"}, QDir::Dirs | QDir::NoDotAndDotDot,
                                            QDir::Name);

    if(infos.isEmpty())
    {
        Writter::warn("There is no sensor data");
        return;
    }

    if(!mode)
    {
        if(restart)
        {
            for(const QFileInfo& fi : infos)
                sensorDirs.enqueue(fi.absoluteFilePath());


        }
    }

    QString firstSensorDirPath = sensorDirs.first();
    QDir firstSensorDir(firstSensorDirPath);

    QString cameraDirPath = firstSensorDirPath + "/camera";
    QDir cameraDir(cameraDirPath);

    QStringList camList = cameraDir.entryList({"cam?"}, QDir::Dirs | QDir::NoDotAndDotDot);

    camN = camList.size();

    Writter::info(QString("Load sensor folder from %1").arg(rootPath));
    Writter::info(QString("Initial sensor folder list : %1").arg(QStringList(sensorDirs.begin(), sensorDirs.end()).join(",")));

    currDir = sensorDirs.dequeue();

    for(int i = 1; i < camN+1; i++)
    {
        QString camName = QString("cam%1").arg(i);
        camWorkers[camName].get()->addRawFiles(currDir);

        Writter::info(QString("%1 is working...").arg(camName));
    }

    Writter::info("Success to initialize, start to send video");
    stopping = false;
    startFileMode();
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
        QList<QString> camDirs;
        QFileInfoList subDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);

        QString cameraPath;
        for(const QFileInfo &fi: std::as_const(subDirs))
        {
            QString folderName = fi.baseName();
            if(folderName == "camera")
                cameraPath = fi.absoluteFilePath();
        }

        QDir cameraDir(cameraPath);
        QFileInfoList camFiList = cameraDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);

        for(const QFileInfo& fi : std::as_const(camFiList))
        {
            if(fi.baseName().startsWith("cam"))
            {
                if(!watcher.directories().contains(fi.absoluteFilePath()))
                    watcher.addPath(fi.absoluteFilePath());
            }
        }

    }else if(baseName.startsWith("cam") && baseName.length() == 4)
    {
        dir.cdUp();
        dir.cdUp();


        if(dir.absolutePath().startsWith("Sensor_Data_"))
        {
            if(!sensorDirs.contains(dir.absolutePath()))
                sensorDirs.enqueue(dir.absolutePath());
        }
    }

}

void WorkManager::startLiveMode()
{

}
void WorkManager::startFileMode()
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

        for(int i = 1 ; i < camN+1; i++)
        {
            // 파일을 지울지말지 판단하는 부분
            QString camId = QString("cam%1").arg(i);
        }
        Writter::info("No sensor dir in queue");
        return;
    }else
    {
        if(isChange)
        {

            for(int i = 1 ; i < camN+1; i++)
            {
                // 파일을 지울지말지 판단하는 부분
                QString camId = QString("cam%1").arg(i);
            }

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
    stopping = true;
    for (QTimer* timer : resultTimers)
        timer->stop();

    closeVideoSockets(-1);

    stopFfmpeg();

    const QStringList paths = watcher.directories();
    if(!paths.isEmpty())
        watcher.removePaths(watcher.directories());

}


void WorkManager::sendClip(const QVector<QString> &clips, CamWorker* camWorker)
{
    if (!camWorker)
        return;

    const QString camId = camWorker->getCamId();
    const QString requestId =
            QUuid::createUuid().toString(QUuid::WithoutBraces);

    pendingRequestIds[camId] = requestId;
    timeoutCounts[camId] = 0;
    receivedCams.remove(camId);

    QTcpSocket* activeSocket = nullptr;
    const auto failRequest =
            [this, camId, requestId, &activeSocket](const QString& reason) {
        // VIDEO_END 전에 실패하면 연결을 끊어 서버 FFmpeg도 즉시 정리시킨다.
        if (activeSocket &&
            activeSocket->state() != QAbstractSocket::UnconnectedState)
            activeSocket->abort();

        completeCameraRequest(camId, requestId, false, reason);
    };

    if (clips.isEmpty()) {
        failRequest("Clip list is empty");
        return;
    }

    Writter::info(QString("Send %1 on single socket port %2")
                  .arg(camId)
                  .arg(camWorker->getPort()));

    QTcpSocket* socket = ensureVideoSocket(camWorker);
    activeSocket = socket;
    if (!socket) {
        failRequest("Video socket connection failed");
        return;
    }

    // 동일한 영상 소켓에서 META -> VIDEO_CHUNK -> VIDEO_END 순서로 보낸다.
    QJsonObject obj;
    QDir dir(currDir);

    obj["sensorName"] = dir.dirName();
    obj["camId"] = camId;
    obj["requestId"] = requestId;

    const QByteArray metadata =
            QJsonDocument(obj).toJson(QJsonDocument::Compact);

    if (!sendFramedPacket(socket,
                          VssProtocol::PacketType::Meta,
                          metadata)) {
        failRequest("Failed to send meta info");
        return;
    }

    if(!ensureFfmpegRunning()) {
        failRequest("Failed to start video sender");
        return;
    }

    qint64 rawBytes = (qint64) width * height;
    QByteArray frame(rawBytes, Qt::Uninitialized);
    QElapsedTimer timer;
    timer.start();

    for(const QString& file : clips)
    {
        QFile in(file);
        if(!in.open(QIODevice::ReadOnly)) {
            stopFfmpeg();
            failRequest(QString("Failed to open raw file: %1").arg(file));
            return;
        }

        qint64 readBytes = in.read(frame.data(), rawBytes);

        in.close();

        if(readBytes != rawBytes)
        {
            QString log = QString("Fail to read raw file : %1, read = %2, expected = %3")
                    .arg(in.fileName()).arg(readBytes).arg(rawBytes);

            Writter::error(log);

            stopFfmpeg();
            failRequest(log);
            return;
        }

        qint64 written = 0;

        while(written < frame.size())
        {
            qint64 chunk = ffmpeg->write(frame.constData() + written, frame.size() - written);
            if(chunk < 0)
            {
                const QString error = ffmpeg->errorString();
                stopFfmpeg();
                failRequest(QString("FFmpeg write failed: %1").arg(error));
                return;
            }

            written += chunk;

            if (!drainFfmpegOutput(socket)) {
                stopFfmpeg();
                failRequest("Failed to send encoded video chunk");
                return;
            }

            if(!ffmpeg->waitForBytesWritten(30000))
            {
                qCritical() << "[ERROR] Failt to flush ffmpeg stdin flush:" << in.fileName();
                qCritical() << "[ERROR] state =" << ffmpeg->state();
                qCritical() << "[ERROR] exitCode =" << ffmpeg->exitCode();
                qCritical() << "[ERROR] error =" << ffmpeg->errorString();

                const QString error = ffmpeg->errorString();
                stopFfmpeg();
                failRequest(QString("FFmpeg write timeout: %1").arg(error));
                return;
            }

            if (!drainFfmpegOutput(socket)) {
                stopFfmpeg();
                failRequest("Failed to send encoded video chunk");
                return;
            }

        }
        // End to send clip
    }

    if (ffmpeg->state() != QProcess::Running) {
        failRequest("FFmpeg stopped before clip completion");
        return;
    }

    ffmpeg->closeWriteChannel();

    QElapsedTimer finishTimer;
    finishTimer.start();

    while (ffmpeg->state() != QProcess::NotRunning)
    {
        ffmpeg->waitForReadyRead(100);

        if (!drainFfmpegOutput(socket)) {
            stopFfmpeg();
            failRequest("Failed to flush encoded video");
            return;
        }

        if (finishTimer.elapsed() > 60000) {
            stopFfmpeg();
            failRequest("FFmpeg finish timeout");
            return;
        }
    }

    if (!drainFfmpegOutput(socket)) {
        failRequest("Failed to send final encoded video chunk");
        return;
    }

    if (ffmpeg->exitStatus() != QProcess::NormalExit ||
        ffmpeg->exitCode() != 0) {
        failRequest(QString("FFmpeg exited with code %1: %2")
                    .arg(ffmpeg->exitCode())
                    .arg(QString::fromUtf8(ffmpeg->readAllStandardError())));
        return;
    }

    if (!sendFramedPacket(socket,
                          VssProtocol::PacketType::VideoEnd,
                          QByteArray())) {
        failRequest("Failed to send video end packet");
        return;
    }

    Writter::info(QString("Success to send clip of %1, requestId=%2")
                  .arg(camId, requestId));

    QTimer* resultTimer = resultTimers.value(camId, nullptr);
    if (resultTimer)
        resultTimer->start(RESULT_TIMEOUT_MS);

    return;
}


bool WorkManager::ensureFfmpegRunning()
{
    if(ffmpeg->state() == QProcess::Running)
        return true;

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
        "pipe:1"
    };

    ffmpeg->setProgram("ffmpeg");
    ffmpeg->setArguments(args);
    ffmpeg->setProcessChannelMode(QProcess::SeparateChannels);
    ffmpeg->start();

    if(!ffmpeg->waitForStarted(30000))
    {
      return false;
    }

    return true;
}

bool WorkManager::drainFfmpegOutput(QTcpSocket* socket)
{
    QByteArray output = ffmpeg->readAllStandardOutput();
    int offset = 0;

    while (offset < output.size())
    {
        const int size = qMin(VssProtocol::VideoChunkSize,
                              output.size() - offset);
        const QByteArray chunk = output.mid(offset, size);

        if (!sendFramedPacket(socket,
                              VssProtocol::PacketType::VideoChunk,
                              chunk))
            return false;

        offset += size;
    }

    return true;
}

bool WorkManager::sendFramedPacket(QTcpSocket* socket,
                                   VssProtocol::PacketType type,
                                   const QByteArray& payload,
                                   int timeoutMs)
{
    if (!socket ||
        socket->state() != QAbstractSocket::ConnectedState ||
        payload.size() > static_cast<int>(VssProtocol::MaxPayloadSize))
        return false;

    const QByteArray packet = VssProtocol::makePacket(type, payload);
    if (socket->write(packet) != packet.size())
        return false;

    const qint64 queueLimit =
            type == VssProtocol::PacketType::VideoChunk
            ? 4 * 1024 * 1024
            : 0;

    QElapsedTimer timer;
    timer.start();

    while (socket->bytesToWrite() > queueLimit)
    {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !socket->waitForBytesWritten(remaining))
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

QTcpSocket* WorkManager::ensureVideoSocket(CamWorker* camWorker)
{
    if (!camWorker)
        return nullptr;

    const QString camId = camWorker->getCamId();
    QTcpSocket* socket = videoSockets.value(camId, nullptr);
    if (!socket)
        return nullptr;

    if (socket->state() == QAbstractSocket::ConnectedState)
        return socket;

    if (socket->state() != QAbstractSocket::UnconnectedState)
        socket->abort();

    const quint16 port = static_cast<quint16>(camWorker->getPort());
    socket->connectToHost(dstIp, port);

    if (!socket->waitForConnected(3000)) {
        Writter::error(QString("Video connect failed (%1:%2): %3")
                       .arg(camId)
                       .arg(port)
                       .arg(socket->errorString()));
        return nullptr;
    }

    Writter::info(QString("Persistent video socket connected: %1, port %2")
                  .arg(camId)
                  .arg(port));

    const QString requestId = pendingRequestIds.value(camId);
    if (!requestId.isEmpty())
    {
        QJsonObject resume;
        resume["requestId"] = requestId;
        sendFramedPacket(
            socket,
            VssProtocol::PacketType::Resume,
            QJsonDocument(resume).toJson(QJsonDocument::Compact));
    }

    return socket;
}

void WorkManager::readServerResult(const QString& camId)
{
    QTcpSocket* socket = videoSockets.value(camId, nullptr);
    if (!socket)
        return;

    QByteArray& buffer = socketBuffers[camId];
    buffer.append(socket->readAll());

    while (true) {
        if (buffer.size() < VssProtocol::HeaderSize)
            return;

        const QByteArray header = buffer.left(VssProtocol::HeaderSize);
        QDataStream stream(header);
        stream.setByteOrder(QDataStream::BigEndian);

        quint32 magic = 0;
        quint8 rawType = 0;
        quint32 payloadSize = 0;
        stream >> magic >> rawType >> payloadSize;

        if (magic != VssProtocol::Magic ||
            payloadSize > VssProtocol::MaxPayloadSize) {
            Writter::error(QString("Invalid packet header from %1")
                           .arg(camId));
            buffer.clear();
            socket->abort();
            return;
        }

        const int packetSize = VssProtocol::HeaderSize
                + static_cast<int>(payloadSize);
        if (buffer.size() < packetSize)
            return;

        const QByteArray payload =
                buffer.mid(VssProtocol::HeaderSize, payloadSize);
        buffer.remove(0, packetSize);

        const auto packetType =
                static_cast<VssProtocol::PacketType>(rawType);
        if (packetType != VssProtocol::PacketType::Result &&
            packetType != VssProtocol::PacketType::Error) {
            Writter::warn(QString("Unexpected server packet type %1 (%2)")
                          .arg(rawType)
                          .arg(camId));
            continue;
        }

        QJsonParseError parseError;
        const QJsonDocument document =
                QJsonDocument::fromJson(payload, &parseError);

        if (parseError.error != QJsonParseError::NoError ||
            !document.isObject()) {
            Writter::error(QString("Invalid summarize result (%1): %2")
                           .arg(camId, parseError.errorString()));
            continue;
        }

        const QJsonObject result = document.object();
        QString answer = result["answer"].toString();
        const QString requestId = result.value("requestId").toString();
        const QString pendingId = pendingRequestIds.value(camId);

        if (requestId.isEmpty() || requestId != pendingId) {
            Writter::warn(
                QString("Ignore stale/unknown result (%1): received=%2 pending=%3")
                    .arg(camId, requestId, pendingId));
            continue;
        }

        if (!result.value("success").toBool()) {
            const QString error = result.value("error").toString();
            Writter::error(QString("Summarize failed (%1): %2")
                           .arg(camId, error));
            completeCameraRequest(camId, requestId, false, error);
        } else {
            Writter::info(QString("Summarize result (%1, %2): %3")
                          .arg(camId,
                               result.value("fileName").toString(),
                               result.value("answer").toString()));
            completeCameraRequest(camId, requestId, true);

            // TODO
            QStringList vssInfo = answer.split("\n");

            emit requestToProcessSensor(camId, mission.saveFolders.head(), vssInfo);

        }
    }
}

void WorkManager::onVideoDisconnected(const QString& camId)
{
    socketBuffers[camId].clear();

    if (stopping || !pendingRequestIds.contains(camId))
        return;

    const QString requestId = pendingRequestIds.value(camId);
    Writter::warn(QString("Video socket disconnected (%1); reconnect scheduled")
                  .arg(camId));

    QTimer::singleShot(RECONNECT_DELAY_MS, this,
                       [this, camId, requestId]() {
        if (stopping || pendingRequestIds.value(camId) != requestId)
            return;

        auto worker = camWorkers.value(camId);
        if (!worker || !ensureVideoSocket(worker.get())) {
            Writter::warn(QString("Video reconnect failed (%1)").arg(camId));
            return;
        }

        Writter::info(QString("Video socket reconnected (%1)").arg(camId));
    });
}

void WorkManager::onResultTimeout(const QString& camId)
{
    if (stopping || !pendingRequestIds.contains(camId))
        return;

    const QString requestId = pendingRequestIds.value(camId);
    const int attempt = timeoutCounts.value(camId) + 1;
    timeoutCounts[camId] = attempt;

    if (attempt > MAX_TIMEOUT_RETRIES) {
        completeCameraRequest(
            camId,
            requestId,
            false,
            QString("Summarize response timeout after %1 retries")
                .arg(MAX_TIMEOUT_RETRIES));
        return;
    }

    Writter::warn(QString("Summarize timeout (%1), reconnect retry %2/%3")
                  .arg(camId)
                  .arg(attempt)
                  .arg(MAX_TIMEOUT_RETRIES));

    QTcpSocket* socket = videoSockets.value(camId, nullptr);
    if (socket && socket->state() != QAbstractSocket::UnconnectedState)
        socket->abort();

    auto worker = camWorkers.value(camId);
    if (worker)
        ensureVideoSocket(worker.get());

    QTimer* timer = resultTimers.value(camId, nullptr);
    if (timer)
        timer->start(RETRY_WAIT_MS);
}

void WorkManager::completeCameraRequest(const QString& camId,
                                        const QString& requestId,
                                        bool success,
                                        const QString& reason)
{
    if (pendingRequestIds.value(camId) != requestId)
        return;

    QTimer* timer = resultTimers.value(camId, nullptr);
    if (timer)
        timer->stop();

    pendingRequestIds.remove(camId);
    timeoutCounts.remove(camId);
    receivedCams.insert(camId);
    receivedN = receivedCams.size();

    if (!success) {
        Writter::error(QString("Camera request completed as failure (%1, %2): %3")
                       .arg(camId, requestId, reason));
    }

    if (receivedN >= camN)
        scheduleNextBatch();
}

void WorkManager::scheduleNextBatch()
{
    if (stopping || nextBatchScheduled)
        return;

    nextBatchScheduled = true;
    receivedCams.clear();
    receivedN = 0;

    // readyRead 또는 전송 실패 처리 중 start()가 중첩 호출되지 않도록
    // 현재 이벤트가 끝난 뒤 다음 묶음을 시작한다.
    QTimer::singleShot(0, this, [this]() {
        nextBatchScheduled = false;
        if (!stopping)
            startFileMode();
    });
}

void WorkManager::closeVideoSockets(int timeoutMs)
{
    for (QTcpSocket* socket : videoSockets) {
        if (!socket ||
            socket->state() == QAbstractSocket::UnconnectedState)
            continue;

        socket->flush();
        socket->disconnectFromHost();

        if (timeoutMs >= 0 &&
            socket->state() != QAbstractSocket::UnconnectedState)
            socket->waitForDisconnected(timeoutMs);
    }
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
    QVector<QString> processedFiles = camWorkers[camId]->processClip(isSave, path);

    if(isSave)
    {
        missionCnt++;
        mission.saveFolders.dequeue();
        logger->addLog(processedFiles, text);
    }

    if(missionCnt == mission.targetScenes)
        missionFinish(mission.id);
}

void WorkManager::pauseVideoSending()
{
    stopping = true;
    for(QTimer* timer : resultTimers)
        timer->stop();

    pendingRequestIds.clear();
    timeoutCounts.clear();
    receivedCams.clear();
    receivedN = 0;
    nextBatchScheduled = false;

    stopFfmpeg();
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
