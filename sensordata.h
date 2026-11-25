#ifndef SENSORDATA_H
#define SENSORDATA_H

#include "Define.h"
#include <QByteArray>
#include <QImage>
#include <QString>

namespace SensorData {
QByteArray lidarBin(const CustomLidarDataType& frame, QString& error);
double timestampMs(double timestamp);
QImage cameraImage(const CustomCamDataType& frame, const QString& pixelFormat, QString& error);
QByteArray bgrBytes(const QImage& image);
}

#endif
