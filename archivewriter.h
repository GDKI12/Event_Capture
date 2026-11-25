#ifndef ARCHIVEWRITER_H
#define ARCHIVEWRITER_H

#include <QByteArray>
#include <QMutex>
#include <QTemporaryDir>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <deque>
#include <memory>
#ifdef CAPTURE_TESTING
#include <functional>
#endif

// Only this thread touches the in-progress SensorArchive and its files.
// Calls from the receiver copy borrowed iceoryx bytes into a bounded FIFO.
class ArchiveWriter : public QThread
{
    Q_OBJECT
public:
    explicit ArchiveWriter(QObject* parent = nullptr);
    ~ArchiveWriter() override;
    void setQueueLimit(qint64 bytes);
    quint64 begin(const std::shared_ptr<QTemporaryDir>& directory, int toleranceMs, int frames);
    void cancel(std::shared_ptr<QTemporaryDir> retiredDirectory = {});
    bool image(quint64 token, const QByteArray& bytes, int width, int height, double timestamp);
    bool lidar(quint64 token, const QByteArray& bytes, double timestamp);
    bool commit(quint64 token, const QString& destination);

signals:
    void failed(quint64 token, const QString& message);
    void waitingForLidar(quint64 token, const QString& reason);
    void saved(quint64 token, const QString& destination);

protected:
    void run() override;

private:
#ifdef CAPTURE_TESTING
    friend class ArchiveWriterTests;
    std::function<void()> beforeWrite;
#endif
    enum class Kind { Begin, Image, Lidar, Commit, Reset };
    struct Job {
        Kind kind = Kind::Reset;
        quint64 token = 0;
        std::shared_ptr<QTemporaryDir> directory;
        QByteArray bytes;
        QString destination;
        int width = 0, height = 0, tolerance = 0, frames = 0;
        double timestamp = 0;
        qint64 queuedAt = 0;
    };
    bool enqueue(Job job);
    QMutex mutex;
    QWaitCondition available;
    std::deque<Job> jobs;
    std::atomic<quint64> generation{0};
    qint64 queuedBytes = 0;
    qint64 byteLimit = 256LL * 1024 * 1024;
    bool stopping = false;
};

#endif
