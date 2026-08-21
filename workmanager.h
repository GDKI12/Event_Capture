#ifndef WORKMANAGER_H
#define WORKMANAGER_H

#include <QMap>
#include <QObject>
#include <QTcpServer>
#include <iostream>
#include <memory>
#include "camworker.h"
#include "config.h"
#include "define.h"

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
    void processReceiveData();
    void sendClip(const QVector<QString>& clips, CamWorker* camWorker);
    void sendToServer(int channel, const Mission& mission, int fps = 10);
    bool ensureFfmpegRunning(CamWorker* camWorker);
    void stopFfmpeg();
    void closeMetaSocket(int timeoutMs = 3000);
    void closeVssSocket(int timeoutMs = 3000);
    void getVssInfos(const QByteArray&);
    void onProcessSensor(const QString&);
    void initMissionServer();
    void saveSensorData(const QString&);
signals:
    void requestToProcessSensor(const QString&);
    void receivedMission(const Mission& mission);
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

    // Mission Server
    QTcpServer server;
    QByteArray receiveBuffer;

    Mission mission;


    QTcpSocket* metaSocket;
    QTcpSocket* vssSocket;

    QProcess* ffmpeg;

    QString currDir;

    int camN;

    // setting params
    QString rootPath;
    QString savePath;
    QString dstIp;
    int dstPort;
    int metaPort;
    int initPort;
    int timeInterval;
    int videoLength;
    bool mode;
    int width;
    int height;
    int frames;

    // 처리된 파일들 SC001_xxxx
    QQueue<QString> saveQueue;
};

#endif // WORKMANAGER_H
