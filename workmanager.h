#ifndef WORKMANAGER_H
#define WORKMANAGER_H

#include <QMap>
#include <QObject>
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
    void init();
    bool isSensorDirReady(const QString&);

    void start();
    void stop();

private:
    void sendClip(const QVector<QString>& clips, CamWorker* camWorker);
    void sendToServer(int camN, int videoL, int fps = 10);
    bool ensureFfmpegRunning(CamWorker* camWorker);
    void stopFfmpeg();
    void closeMetaSocket(int timeoutMs = 3000);
    void closeVssSocket(int timeoutMs = 3000);
    void getVssInfos(const QByteArray&);
    void onProcessSensor(const QString&);

signals:
    void requestToProcessSensor(const QString&);

public slots:
    void onFileSystemChanged(const QString& path);



private:
    QFileSystemWatcher watcher;
    QHash<QString, std::shared_ptr<CamWorker>> camWorkers;

    Config config;

    QQueue<QString> sensorDirs;
    QSet<QString> queuedSensors;

    QSet<QString> preSensors;

    QMap<QString, VssInfo> vssInfos;

    QTcpSocket* socket;

    QTcpSocket* vssSocket;

    QProcess* ffmpeg;

    QString currDir;

    int camN;

    // setting params
    QString rootPath;
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
};

#endif // WORKMANAGER_H
