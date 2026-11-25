#ifndef WORKMANAGER_H
#define WORKMANAGER_H

#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QStringList>
#include "Define.h"

class QTimer;
class MissionProcessor;

class WorkManager : public QObject
{
    Q_OBJECT
    friend class ImageInputTests;
public:
    explicit WorkManager(QObject* parent = nullptr);
    ~WorkManager() override;
    bool init();
    void initConfig();

public slots:
    void start();
    void stop();
    void selectCamera(const QString& cameraName);
    void receiveGrabFrame();
    void acknowledgeFrame();
    void acceptMission(const QJsonObject& mission);
    void setPrompts(const QJsonObject& prompts);

signals:
    void configurationLoaded(const QString& configPath);
    void cameraNamesChanged(const QStringList& cameraNames);
    void frameReady(const QString& cameraName, const QImage& image);
    void streamUnavailable(const QString& cameraName);
    void logMessage(const QString& message);
    void processingStatusChanged(const QString& status);
    void missionProgressChanged(int saved, int target);
    void inferenceResult(const QString& answer);
    void missionCompletionRequested(const QString& dispatchId);

private:
    void setStreamStatus(const QString& status);
    void consumeImageFrame(const CustomCamDataType& frame);
    void receiveLidarFrames();

    PoshRuntime* _runtime = nullptr;
    QString currCamName;
    QVector<QString> camNameList;
    QMap<QString, Subscriber<CustomCamDataType>*> camSubscribers;
    Subscriber<CustomLidarDataType>* lidarSubscriber = nullptr;
    QString lidarStatus;
    QTimer* receiveTimer = nullptr;
    QTimer* connectionTimer = nullptr;
    QElapsedTimer lastFrame;
    QString streamStatus;
    QString pixelFormat;
    bool framePending = false;
    bool waitingForRouDi = false;
    bool configValid = false;
    MissionProcessor* processor = nullptr;
#ifndef QT_NO_DEBUG
    double debugCameraTimestamp = 0;
    double debugLidarTimestamp = 0;
    QElapsedTimer debugCameraArrival;
    QElapsedTimer debugReceivePoll;
#endif

    QString baseURL;
    QString authId;
    QString secretKey;
    QString rootPath;
    QString savePath;
};

#endif // WORKMANAGER_H
