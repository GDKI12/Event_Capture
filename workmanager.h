#ifndef WORKMANAGER_H
#define WORKMANAGER_H

#include <QMap>
#include <QObject>
#include <QTcpServer>
#include <QTimer>
#include <QUuid>
#include <iostream>
#include <memory>
#include "camworker.h"
#include "config.h"
#include "define.h"
#include "vssProtocol.h"

class WorkManager : public QObject
{
    Q_OBJECT
public:
    explicit WorkManager(QVector<QString> cams, QObject* parent = nullptr);
    ~WorkManager();
    bool isSensorDirReady(const QString&);

    void start();
    void stop();

private:
    void sendClip(const QVector<QString>& clips, CamWorker* camWorker);
    void sendToServer(int channel, const Mission& mission, int fps = 10);
    bool ensureFfmpegRunning();
    bool drainFfmpegOutput(QTcpSocket* socket);
    bool sendFramedPacket(QTcpSocket* socket,
                          VssProtocol::PacketType type,
                          const QByteArray& payload,
                          int timeoutMs = 30000);
    void stopFfmpeg();
    QTcpSocket* ensureVideoSocket(CamWorker* camWorker);
    void readServerResult(const QString& camId);
    void onVideoDisconnected(const QString& camId);
    void onResultTimeout(const QString& camId);
    void completeCameraRequest(const QString& camId,
                               const QString& requestId,
                               bool success,
                               const QString& reason = QString());
    void scheduleNextBatch();
    void closeVideoSockets(int timeoutMs = 3000);
    void closeVssSocket(int timeoutMs = 3000);
    void getVssInfos(const QByteArray&);
    void onProcessSensor(const QString&);
    void saveSensorData(const QString&);
signals:
    void requestToProcessSensor(const QString&);
    void requestToSave(const QString&);

public slots:
    void init(const Mission& mission);
    void onFileSystemChanged(const QString& path);



private:
    QFileSystemWatcher watcher;
    QHash<QString, std::shared_ptr<CamWorker>> camWorkers;

    Config config;

    QQueue<QString> sensorDirs;
    QSet<QString> queuedSensors;

    QSet<QString> preSensors;

    QMap<QString, VssInfo> vssInfos;

    Mission mission;


    QHash<QString, QTcpSocket*> videoSockets;
    QHash<QString, QByteArray> socketBuffers;
    QSet<QString> receivedCams;
    QHash<QString, QString> pendingRequestIds;
    QHash<QString, int> timeoutCounts;
    QHash<QString, QTimer*> resultTimers;
    QTcpSocket* vssSocket;

    QProcess* ffmpeg;

    QString currDir;

    int camN;

    // setting params
    QString rootPath;
    QString savePath;
    QString dstIp;
    int dstPort;
    int initPort;
    int timeInterval;
    int videoLength;
    bool mode;
    int width;
    int height;
    int frames;

    // 처리된 파일들 SC001_xxxx
    QQueue<QString> saveQueue;

    int receivedN;
    bool stopping = false;
    bool nextBatchScheduled = false;

    static constexpr int RESULT_TIMEOUT_MS = 120000;
    static constexpr int RETRY_WAIT_MS = 30000;
    static constexpr int MAX_TIMEOUT_RETRIES = 3;
    static constexpr int RECONNECT_DELAY_MS = 1000;
};

#endif // WORKMANAGER_H
