#ifndef MISSIONPROCESSOR_H
#define MISSIONPROCESSOR_H

#include "cliprecorder.h"
#include "vlmcontroller.h"
#include "sensorarchive.h"
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

class MissionProcessor : public QObject
{
    Q_OBJECT
public:
    explicit MissionProcessor(QObject* parent = nullptr);
    ~MissionProcessor() override { stop(); }
    void configure(const QString& configPath);
    void acceptMission(const QJsonObject& value);
    void setPrompts(const QJsonObject& value);
    void selectCamera(const QString& camera);
    void receiveFrame(const QString& camera, const QImage& image, double timestamp, const QByteArray& packedBgr);
    void receiveLidar(const QByteArray& xyzi, double timestamp);
    bool wantsFrames() const { return !stopped && !waitingCompletion && !prompt.isEmpty() && !processing && !cameraName.isEmpty(); }
    void streamInterrupted();
    void stop();

signals:
    void logMessage(const QString& message);
    void statusChanged(const QString& message);
    void progressChanged(int saved, int target);
    void inferenceResult(const QString& answer);
    void completionRequested(const QString& dispatchId);

private:
    void preparePrompt();
    void clearClip();
    void fail(const QString& message);
    void processAnswer(const QString& videoPath, const QString& answer, double seconds);
    void finishAcceptedClip();
    void requestCompletion();
    bool saveClip(QString& error);

    ClipRecorder recorder;
    VLMController vlm;
    SensorArchive archive;
    QTimer lidarWaitTimer;
    QVector<QPair<double, QByteArray>> recentLidar;
    QJsonArray sensorPairs;
    QString acceptedVideo;
    QString lastSyncError;
    int lidarToleranceMs = 100;
    QJsonObject mission;
    QJsonObject prompts;
    QString cameraName;
    QString saveRoot;
    QString cacheRoot;
    QString templatePath;
    QString prompt;
    QString lastPromptError;
    std::unique_ptr<QTemporaryDir> clipDirectory;
    int requiredFrames = 0;
    int targetScenes = 0;
    int savedScenes = 0;
    QVector<int> pendingScenes;
    int maxGapMs = 180;
    bool processing = false;
    bool stopped = true;
    bool waitingCompletion = false;
};

#endif
