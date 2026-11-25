#ifndef CLIPRECORDER_H
#define CLIPRECORDER_H

#include <QImage>
#include <QJsonObject>
#include <QProcess>
#include <QSize>
#include <QVector>

// Streams frames to FFmpeg without retaining a raw clip in RAM.
class ClipRecorder : public QObject
{
    Q_OBJECT
public:
    explicit ClipRecorder(QObject* parent = nullptr);
    ~ClipRecorder() override;
    void begin(const QString& path, int frameCount, int fps, int maxGapMs);
    void append(const QImage& image, double timestamp);
    void cancel();
    bool isActive() const { return active; }

signals:
    void ready(const QString& path, const QJsonObject& metadata);
    void failed(const QString& message, bool retryable);
    void progress(int received, int required);

private:
    void fail(const QString& message, bool retryable = false);
    void finishInput();
    QProcess encoder;
    QString outputPath;
    QByteArray errors;
    QByteArray startingFrames;
    QVector<double> timestamps;
    QSize frameSize;
    int required = 0;
    int framesPerSecond = 10;
    int gapMs = 180;
    bool active = false;
    bool finishing = false;
    quint64 generation = 0;
};

#endif
