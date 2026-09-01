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
#include "apicontroller.h"
#include "config.h"
#include "define.h"
#include "vssProtocol.h"
#include "vsslogger.h"

class WorkManager : public QObject
{
    Q_OBJECT
public:
    explicit WorkManager(QVector<QString> cams, QObject* parent = nullptr);
    ~WorkManager();
    bool isSensorDirReady(const QString&);

    void stop();

private:
    void sendClip(const QVector<QString>& clips, CamWorker* camWorker);
    bool ensureFfmpegRunning();
    bool drainFfmpegOutput(QTcpSocket* socket);
    bool sendFramedPacket(QTcpSocket* socket,
                          VssProtocol::PacketType type,
                          const QByteArray& payload,
                          int timeoutMs = 30000);
    void stopFfmpeg();
    void pauseVideoSending();
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
    bool decideToSave(QStringList answers);
    void processSensor(const QString& camId, const QString& rootPath , QStringList text);
    void missionFinish(const QString&);
signals:
    void requestToProcessSensor(const QString& camId, const QString& rootPath, QStringList text);

public slots:
    void startFileMode();
    void startLiveMode();
    void init(const Mission& mission);
    void onFileSystemChanged(const QString& path);



private:
    // vss api manager
    QNetworkAccessManager* manager;

    APIController* apiController;
    VssLogger* logger;
    QFileSystemWatcher watcher;
    QHash<QString, std::shared_ptr<CamWorker>> camWorkers;

    Config config;

    QString currDir;
    QString workingDir;
    QQueue<QString> sensorDirs;
    QSet<QString> queuedSensors;

    QMap<QString, VssInfo> vssInfos;

    Mission mission;
    QTimer* healthyTimer;
    QTimer* missionTimer;

    QHash<QString, QTcpSocket*> videoSockets;
    QHash<QString, QByteArray> socketBuffers;
    QSet<QString> receivedCams;
    QHash<QString, QString> pendingRequestIds;
    QHash<QString, int> timeoutCounts;
    QHash<QString, QTimer*> resultTimers;

    QProcess* ffmpeg;


    int camN;

    // setting params
    QString rootPath;
    QString savePath;
    QString dstIp;
    int dstPort;
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
    bool restart = false;
    bool nextBatchScheduled = false;

    static constexpr int RESULT_TIMEOUT_MS = 120000;
    static constexpr int RETRY_WAIT_MS = 30000;
    static constexpr int MAX_TIMEOUT_RETRIES = 3;
    static constexpr int RECONNECT_DELAY_MS = 1000;

    // 미션개수
    int missionCnt;
};

#endif // WORKMANAGER_H
