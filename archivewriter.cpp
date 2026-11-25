#include "archivewriter.h"
#include "capturediagnostics.h"
#include "sensorarchive.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutexLocker>
#include <chrono>

namespace {
qint64 monotonicNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

ArchiveWriter::ArchiveWriter(QObject* parent) : QThread(parent)
{
    setObjectName(QStringLiteral("sensor-archive-writer"));
}

ArchiveWriter::~ArchiveWriter()
{
    {
        QMutexLocker lock(&mutex);
        stopping = true;
        ++generation;
        jobs.clear();
        queuedBytes = 0;
        available.wakeOne();
    }
    wait(); // Shutdown only. Disk writes never block the receive loop.
}

void ArchiveWriter::setQueueLimit(qint64 bytes)
{
    QMutexLocker lock(&mutex);
    byteLimit = qMax(qint64(1), bytes);
}

quint64 ArchiveWriter::begin(const std::shared_ptr<QTemporaryDir>& directory, int toleranceMs, int frames)
{
    QMutexLocker lock(&mutex);
    const auto token = ++generation;
    jobs.clear();
    queuedBytes = 0;
    Job job;
    job.kind = Kind::Begin;
    job.token = token;
    job.directory = directory;
    job.tolerance = toleranceMs;
    job.frames = frames;
    jobs.push_back(std::move(job));
    if (!isRunning())
        start();
    available.wakeOne();
    return token;
}

void ArchiveWriter::cancel(std::shared_ptr<QTemporaryDir> retiredDirectory)
{
    QMutexLocker lock(&mutex);
    const auto token = ++generation;
    jobs.clear();
    queuedBytes = 0;
    Job job;
    job.token = token;
    job.directory = std::move(retiredDirectory);
    jobs.push_back(std::move(job));
    available.wakeOne();
}

bool ArchiveWriter::enqueue(Job job)
{
    QMutexLocker lock(&mutex);
    if (stopping || job.token != generation.load())
        return false;
    if (jobs.size() >= 1024 || job.bytes.size() > byteLimit - queuedBytes) {
        CAPTURE_TRACE(QStringLiteral("writer_queue_overflow clip=%1 queued_bytes=%2 incoming_bytes=%3 limit_bytes=%4 jobs=%5")
                      .arg(job.token).arg(queuedBytes).arg(job.bytes.size()).arg(byteLimit).arg(jobs.size()));
        return false;
    }
    // fromRawData() is borrowed; a shallow QByteArray copy would outlive the loan.
    job.bytes = QByteArray(job.bytes.constData(), job.bytes.size());
    job.queuedAt = monotonicNs();
    queuedBytes += job.bytes.size();
    CAPTURE_TRACE(QStringLiteral("writer_enqueue clip=%1 bytes=%2 queued_bytes=%3 jobs=%4")
                  .arg(job.token).arg(job.bytes.size()).arg(queuedBytes).arg(jobs.size() + 1));
    jobs.push_back(std::move(job));
    available.wakeOne();
    return true;
}

bool ArchiveWriter::image(quint64 token, const QByteArray& bytes, int width, int height, double timestamp)
{
    Job job;
    job.kind = Kind::Image;
    job.token = token;
    job.bytes = bytes;
    job.width = width;
    job.height = height;
    job.timestamp = timestamp;
    return enqueue(std::move(job));
}

bool ArchiveWriter::lidar(quint64 token, const QByteArray& bytes, double timestamp)
{
    Job job;
    job.kind = Kind::Lidar;
    job.token = token;
    job.bytes = bytes;
    job.timestamp = timestamp;
    return enqueue(std::move(job));
}

bool ArchiveWriter::commit(quint64 token, const QString& destination)
{
    Job job;
    job.kind = Kind::Commit;
    job.token = token;
    job.destination = destination;
    return enqueue(std::move(job));
}

void ArchiveWriter::run()
{
    SensorArchive archive;
    std::shared_ptr<QTemporaryDir> directory;
    QString destination;
    quint64 token = 0;
    int required = 0;
    bool failedClip = false, committed = false;
    for (;;) {
        Job job;
        {
            QMutexLocker lock(&mutex);
            while (!stopping && jobs.empty())
                available.wait(&mutex);
            if (stopping)
                break;
            job = std::move(jobs.front());
            jobs.pop_front();
            queuedBytes -= job.bytes.size();
        }
        if (job.token != generation.load())
            continue;
        if (job.kind == Kind::Begin || job.kind == Kind::Reset) {
            archive.clear();
            directory.reset(); // Old temporary files are removed on the writer thread.
            destination.clear();
            token = job.token;
            failedClip = committed = false;
            if (job.kind == Kind::Begin) {
                directory = job.directory;
                required = job.frames;
                archive.begin(directory->path(), job.tolerance);
            }
            continue;
        }
        if (!directory || token != job.token || failedClip || committed)
            continue;
#ifdef CAPTURE_TESTING
        if (beforeWrite)
            beforeWrite();
        if (token != generation.load())
            continue;
#endif
        QString error;
        QElapsedTimer elapsed;
        elapsed.start();
        const double waitMs = job.queuedAt ? (monotonicNs() - job.queuedAt) / 1e6 : 0;
        Q_UNUSED(waitMs)
        bool ok = true;
        if (job.kind == Kind::Image) {
            ok = archive.addImage(job.bytes, job.width, job.height, job.timestamp, error);
            if (ok && archive.imageCount() == required)
                archive.finishImages();
        } else if (job.kind == Kind::Lidar) {
            ok = archive.addLidar(job.bytes, job.timestamp, error);
        } else if (job.kind == Kind::Commit) {
            destination = job.destination;
        }
        CAPTURE_TRACE(QStringLiteral("%1_write clip=%2 timestamp_ms=%3 bytes=%4 queue_wait_ms=%5 write_ms=%6 ok=%7")
                      .arg(job.kind == Kind::Image ? "RAW" : job.kind == Kind::Lidar ? "LIDAR" : "commit_request")
                      .arg(token).arg(job.timestamp, 0, 'f', 3).arg(job.bytes.size())
                      .arg(waitMs, 0, 'f', 3).arg(elapsed.nsecsElapsed() / 1e6, 0, 'f', 3).arg(ok));
        if (!ok) {
            failedClip = true;
            emit failed(token, error);
            continue;
        }
        if (destination.isEmpty() || token != generation.load())
            continue;
        QJsonArray pairs;
        if (!archive.match(pairs, error)) {
            emit waitingForLidar(token, error);
            continue;
        }
        elapsed.restart();
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
            error = QStringLiteral("저장 폴더 생성 실패: %1").arg(destination);
            ok = false;
        } else {
            QTemporaryDir staging(QDir(QFileInfo(destination).absolutePath()).filePath(".scene-XXXXXX"));
            ok = staging.isValid() && archive.stage(staging.path(), pairs, error);
            if (ok && token == generation.load())
                ok = SensorArchive::commitScene(staging.path(), destination, error);
            else if (token != generation.load())
                continue;
            if (!staging.isValid())
                error = QStringLiteral("센서 파일 저장용 임시 폴더 생성 실패");
        }
        CAPTURE_TRACE(QStringLiteral("scene_commit clip=%1 commit_ms=%2 ok=%3 path=%4")
                      .arg(token).arg(elapsed.nsecsElapsed() / 1e6, 0, 'f', 3).arg(ok).arg(destination));
        if (ok) {
            committed = true;
            emit saved(token, destination);
        } else {
            failedClip = true;
            emit failed(token, error);
        }
    }
    // Local archive and directory die here, after the last in-flight write.
}
