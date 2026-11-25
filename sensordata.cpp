#include "sensordata.h"

#include <QtEndian>
#include <cmath>
#include <cstring>

QByteArray SensorData::lidarBin(const CustomLidarDataType& frame, QString& error)
{
    error.clear();
    const auto count = frame.points.size();
    // With a publisher-side point filter, frame.size may contain the unfiltered count.
    if (count == 0 || count > frame.points.capacity() || frame.intensitys.size() != count
        || frame.intensitys.size() > frame.intensitys.capacity()) {
        error = QStringLiteral("라이다 points/intensity 배열 크기가 잘못되었습니다.");
        return {};
    }
    QByteArray bytes;
    bytes.resize(int(count * 16));
    char* output = bytes.data();
    for (uint64_t i = 0; i < count; ++i) {
        const auto& point = frame.points[i];
        const float values[] = {float(point.x), float(point.y), float(point.z), frame.intensitys[i]};
        for (float value : values) {
            quint32 bits;
            static_assert(sizeof(bits) == sizeof(value), "BIN requires 32-bit float");
            std::memcpy(&bits, &value, sizeof(bits));
            qToLittleEndian(bits, reinterpret_cast<uchar*>(output));
            output += sizeof(bits);
        }
    }
    return bytes;
}

double SensorData::timestampMs(double timestamp)
{
    if (!std::isfinite(timestamp) || timestamp <= 0)
        return 0;
    // Capture publishers use SDK epoch seconds, microseconds, or epoch milliseconds.
    if (timestamp >= 1e17)
        return timestamp / 1e6;
    if (timestamp >= 1e14)
        return timestamp / 1000;
    if (timestamp >= 1e8 && timestamp < 1e11)
        return timestamp * 1000;
    return timestamp;
}

QImage SensorData::cameraImage(const CustomCamDataType& frame, const QString& pixelFormat, QString& error)
{
    error.clear();
    const bool threeChannels = pixelFormat == "bgr8" || pixelFormat == "rgb8";
    const quint64 bytes = quint64(frame.width) * frame.height * (threeChannels ? 3U : 1U);
    if (frame.width == 0 || frame.height == 0 || frame.width > 2048 || frame.height > 1536
        || bytes > sizeof(frame.data)) {
        error = QStringLiteral("영상 해상도 또는 공유 메모리 데이터 크기가 잘못되었습니다.");
        return {};
    }
    try {
        const cv::Mat raw(int(frame.height), int(frame.width), threeChannels ? CV_8UC3 : CV_8UC1,
                          const_cast<uint8_t*>(frame.data));
        cv::Mat rgb;
        if (pixelFormat == "bgr8")
            cv::cvtColor(raw, rgb, cv::COLOR_BGR2RGB);
        else if (pixelFormat == "rgb8")
            rgb = raw;
        else if (pixelFormat == "mono8" || pixelFormat == "gray8")
            cv::cvtColor(raw, rgb, cv::COLOR_GRAY2RGB);
        else if (pixelFormat == "bayer_rggb8")
            cv::cvtColor(raw, rgb, cv::COLOR_BayerBG2RGB);
        else if (pixelFormat == "bayer_bggr8")
            cv::cvtColor(raw, rgb, cv::COLOR_BayerRG2RGB);
        else if (pixelFormat == "bayer_grbg8")
            cv::cvtColor(raw, rgb, cv::COLOR_BayerGB2RGB);
        else if (pixelFormat == "bayer_gbrg8")
            cv::cvtColor(raw, rgb, cv::COLOR_BayerGR2RGB);
        else {
            error = QStringLiteral("지원하지 않는 Image 픽셀 형식: %1").arg(pixelFormat);
            return {};
        }
        return QImage(rgb.data, rgb.cols, rgb.rows, int(rgb.step), QImage::Format_RGB888).copy();
    } catch (const cv::Exception& exception) {
        error = QString::fromUtf8(exception.what());
        return {};
    }
}

QByteArray SensorData::bgrBytes(const QImage& image)
{
    if (image.isNull() || quint64(image.width()) * image.height() * 3 > 2147483647U)
        return {};
    const auto rgb = image.convertToFormat(QImage::Format_RGB888);
    QByteArray result;
    result.resize(rgb.width() * rgb.height() * 3);
    auto* destination = reinterpret_cast<uchar*>(result.data());
    for (int row = 0; row < rgb.height(); ++row) {
        const auto* source = rgb.constScanLine(row);
        for (int x = 0; x < rgb.width(); ++x) {
            *destination++ = source[x * 3 + 2];
            *destination++ = source[x * 3 + 1];
            *destination++ = source[x * 3];
        }
    }
    return result;
}
