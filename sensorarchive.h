#ifndef SENSORARCHIVE_H
#define SENSORARCHIVE_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

class SensorArchive
{
public:
    void begin(const QString& directory, int toleranceMs);
    void clear();
    bool addImage(const QByteArray& bayer, int width, int height, double timestamp, QString& error);
    bool addLidar(const QByteArray& xyzi, double timestamp, QString& error);
    void finishImages() { imagesFinished = true; }
    bool match(QJsonArray& pairs, QString& error) const;
    bool stage(const QString& destination, const QJsonArray& pairs, QString& error);
    int imageCount() const { return images.size(); }
    static bool emptyScene(const QString& path);
    static bool commitScene(const QString& staging, const QString& destination, QString& error);

private:
    struct Image { double timestamp; QString file; int width; int height; };
    QString root;
    QVector<Image> images;
    QMap<double, QString> lidars;
    int tolerance = 100;
    bool imagesFinished = false;
};

#endif
