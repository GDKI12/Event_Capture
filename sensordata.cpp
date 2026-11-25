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

QImage SensorData::bayerImage(const CustomCamDataType& frame, QString& error)
{
    error.clear();
    if (frame.width < 2 || frame.height < 2 || frame.width > 2048 || frame.height > 1536
        || quint64(frame.width) * frame.height > sizeof(frame.data)) {
        error = QStringLiteral("원본 BayerRG8 해상도가 잘못되었습니다.");
        return {};
    }
    try {
        const cv::Mat raw(int(frame.height), int(frame.width), CV_8UC1, const_cast<uint8_t*>(frame.data));
        cv::Mat rgb;
        cv::cvtColor(raw, rgb, cv::COLOR_BayerBG2RGB);
        return QImage(rgb.data, rgb.cols, rgb.rows, int(rgb.step), QImage::Format_RGB888).copy();
    } catch (const cv::Exception& exception) {
        error = QString::fromUtf8(exception.what());
        return {};
    }
}
