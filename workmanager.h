#ifndef WORKMANAGER_H
#define WORKMANAGER_H

#include <QElapsedTimer>
#include <QImage>
#include <QMap>
#include <QObject>
#include <QStringList>
#include "Define.h"

class QTimer;

class WorkManager : public QObject
{
    Q_OBJECT
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

signals:
    void configurationLoaded(const QString& configPath);
    void cameraNamesChanged(const QStringList& cameraNames);
    void frameReady(const QString& cameraName, const QImage& image);
    void streamUnavailable(const QString& cameraName);
    void logMessage(const QString& message);

private:
    void setStreamStatus(const QString& status);

    PoshRuntime* _runtime = nullptr;
    QString currCamName;
    QVector<QString> camNameList;
    QMap<QString, Subscriber<CustomCamDataType>*> camSubscribers;
    QTimer* receiveTimer = nullptr;
    QTimer* connectionTimer = nullptr;
    QElapsedTimer lastFrame;
    QString streamStatus;
    QString pixelFormat;
    bool framePending = false;
    bool waitingForRouDi = false;
    bool configValid = false;

    QString baseURL;
    QString authId;
    QString secretKey;
    QString rootPath;
    QString savePath;
};

#endif // WORKMANAGER_H
