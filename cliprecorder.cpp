#include "cliprecorder.h"
#include "capturediagnostics.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>
#include <cmath>

ClipRecorder::ClipRecorder(QObject* parent) : QObject(parent)
{
    connect(&encoder, &QProcess::readyReadStandardError, this, [this] {
        errors += encoder.readAllStandardError();
        errors = errors.right(4096);
    });
    connect(&encoder, &QProcess::started, this, [this] {
        if (!active)
            return;
        if (!startingFrames.isEmpty()) {
            if (encoder.write(startingFrames) != startingFrames.size()) {
                fail(QStringLiteral("FFmpeg 초기 프레임 쓰기 실패"));
                return;
            }
            startingFrames.clear();
        }
        if (finishing)
            encoder.closeWriteChannel();
    });
    connect(&encoder, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (active && error == QProcess::FailedToStart)
            fail(QStringLiteral("FFmpeg 실행 실패: %1").arg(encoder.errorString()));
    });
    connect(&encoder, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
        if (!active)
            return;
        if (!finishing || code != 0 || status != QProcess::NormalExit || QFileInfo(outputPath).size() <= 0) {
            fail(QStringLiteral("FFmpeg 클립 생성 실패: %1").arg(QString::fromLocal8Bit(errors).trimmed()));
            return;
        }
        active = false;
        QJsonArray times;
        for (double timestamp : timestamps)
            times.append(timestamp);
        const QJsonObject metadata{{"width", frameSize.width()}, {"height", frameSize.height()},
                                   {"fps", framesPerSecond}, {"frameCount", timestamps.size()},
                                   {"timestamps", times}};
        emit ready(outputPath, metadata);
    });
}

ClipRecorder::~ClipRecorder() { cancel(); }

void ClipRecorder::begin(const QString& path, int frameCount, int fps, int maxGapMs)
{
    cancel();
    outputPath = path;
    required = frameCount;
    framesPerSecond = fps;
    gapMs = maxGapMs;
    active = frameCount > 0 && fps > 0;
}

void ClipRecorder::cancel()
{
    active = false;
    finishing = false;
    ++generation;
    if (encoder.state() != QProcess::NotRunning) {
        encoder.kill();
        encoder.waitForFinished(1000);
    }
    startingFrames.clear();
    timestamps.clear();
    frameSize = QSize();
    errors.clear();
}

void ClipRecorder::fail(const QString& message, bool retryable)
{
    cancel();
    emit failed(message, retryable);
}

void ClipRecorder::append(const QImage& image, double timestamp)
{
    if (!active || finishing)
        return;
    if (image.isNull() || !std::isfinite(timestamp) || timestamp <= 0) {
        fail(QStringLiteral("수신 프레임 또는 타임스탬프가 잘못되었습니다."), true);
        return;
    }
    if (!timestamps.isEmpty()
        && (timestamp <= timestamps.last() || timestamp - timestamps.last() > gapMs)) {
        CAPTURE_TRACE(QStringLiteral("frame_gap previous_ms=%1 current_ms=%2 delta_ms=%3 limit_ms=%4 received=%5/%6")
                .arg(timestamps.last(), 0, 'f', 3).arg(timestamp, 0, 'f', 3)
                .arg(timestamp - timestamps.last(), 0, 'f', 3).arg(gapMs).arg(timestamps.size()).arg(required));
        fail(QStringLiteral("프레임 누락 또는 시간 역전: 미완성 클립을 다시 수집합니다."), true);
        return;
    }
    if (timestamps.isEmpty()) {
        frameSize = image.size();
        encoder.setProcessChannelMode(QProcess::SeparateChannels);
        encoder.start("ffmpeg", {"-nostdin", "-loglevel", "error", "-f", "rawvideo",
                      "-pixel_format", "rgb24", "-video_size", QString("%1x%2").arg(image.width()).arg(image.height()),
                      "-framerate", QString::number(framesPerSecond), "-i", "pipe:0",
                      "-an", "-c:v", "libx264", "-preset", "veryfast", "-threads", "2",
                      "-crf", "23", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-pix_fmt", "yuv420p",
                      "-movflags", "+faststart", "-y", outputPath});
    } else if (image.size() != frameSize) {
        fail(QStringLiteral("수집 도중 카메라 해상도가 변경되었습니다."), true);
        return;
    }
    const auto rgb = image.convertToFormat(QImage::Format_RGB888);
    const qint64 frameBytes = qint64(rgb.width()) * rgb.height() * 3;
    if (frameBytes > 2147483647 || encoder.bytesToWrite() + startingFrames.size() + frameBytes > frameBytes * 4) {
        fail(QStringLiteral("인코더 처리 지연: 미완성 클립을 다시 수집합니다."), true);
        return;
    }
    QByteArray pixels;
    pixels.reserve(int(frameBytes));
    for (int row = 0; row < rgb.height(); ++row)
        pixels.append(reinterpret_cast<const char*>(rgb.constScanLine(row)), rgb.width() * 3);
    if (encoder.state() == QProcess::Starting)
        startingFrames.append(pixels);
    else if (encoder.write(pixels) != pixels.size()) {
        fail(QStringLiteral("FFmpeg 프레임 쓰기 실패"));
        return;
    }
    timestamps.append(timestamp);
    CAPTURE_TRACE(QStringLiteral("encoder_input count=%1/%2 timestamp_ms=%3 pending_bytes=%4")
                  .arg(timestamps.size()).arg(required).arg(timestamp, 0, 'f', 3)
                  .arg(encoder.bytesToWrite() + startingFrames.size()));
    emit progress(timestamps.size(), required);
    if (timestamps.size() == required)
        finishInput();
}

void ClipRecorder::finishInput()
{
    finishing = true;
    if (encoder.state() == QProcess::Running)
        encoder.closeWriteChannel();
    const auto token = generation;
    QTimer::singleShot(30000, this, [this, token] {
        if (active && finishing && token == generation)
            fail(QStringLiteral("FFmpeg 클립 마무리 시간 초과"));
    });
}
