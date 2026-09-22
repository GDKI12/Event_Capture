#ifndef WORKMANAGER_H
#define WORKMANAGER_H

#include <QMap>
#include <QSet>
#include <QObject>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTimer>
#include <QUuid>
#include <iostream>
#include <memory>
#include "camworker.h"
#include "apicontroller.h"
#include "config.h"
#include "define.h"
#include "vsslogger.h"

const QString PORMPT_FILE_PATH = "../config/prompt.json";

class WorkManager : public QObject
{
    Q_OBJECT
public:
    explicit WorkManager(QObject* parent = nullptr);
    ~WorkManager();
    void stop();

private:
    bool decideToSave(QStringList answers);
    void processSensor(const QString& camId , QStringList text);
    void missionFinish(const QString&);
    QString createPrompt();
    void createVideo(const QString& camId, const QVector<QString>& clips);
    void createVideo(const QString& camId, std::function<QVector<QString>(int)>);
    bool isVLMAlive();
    void infer(const QString& camId, const QString& videoPath);
    void nextClip(const QString& camId);
    void cancelPendingInferences();
signals:
    // 클립 추론 요청
    void requestInfer(const QString& camId, const QString& videoPath);

    // 클립 추론 종료
    void finishInfer(const QString& camId);

    void requestToProcessSensor(const QString& camId, QStringList text);

public slots:
    void startFileMode();
    void startLiveMode();
    void init(const Mission& mission);
    void onFileSystemChanged(const QString& path);



private:
    QString prompt;
    QList<QString> camIds;
    QNetworkAccessManager *manager;
    APIController* apiController;
    VssLogger* logger;
    QFileSystemWatcher watcher;
    QHash<QString, std::shared_ptr<CamWorker>> camWorkers;
    QSet<QNetworkReply*> inferenceReplies;

    Config config;

    QString currDir;
    QString workingDir;

    QMap<QString, VssInfo> vssInfos;

    Mission mission;
    QTimer* healthyTimer;
    QTimer* missionTimer;

    int camN;

    // setting params
    QString rootPath;
    QString savePath;
    int videoLength;
    bool mode;
    int width;
    int height;

    // 처리된 파일들 SC001_xxxx
    QQueue<QString> saveQueue;

    bool stopping = false;
    bool restart = false;
    bool nextBatchScheduled = false;

    // 미션개수
    int missionCnt;
};

#endif // WORKMANAGER_H
