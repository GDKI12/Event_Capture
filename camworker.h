#ifndef CAMWORKER_H
#define CAMWORKER_H

#include <QObject>
#include <QTimer>
#include <QSet>
#include <QQueue>
#include <QVector>
#include <QFileInfoList>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QTcpSocket>
#include <QDateTime>

#include "config.h"
#include "define.h"
class CamWorker : public QObject
{
    Q_OBJECT
public:
    explicit CamWorker(const QString& camId, QObject* parent = nullptr);
    void setSensorDirs(const QVector<QString>& dirList);

    void setRawFiles(const QString& dirPath);
    void addRawFile(const QString&);

    bool sensorDirIsEmpty();
    QString getCamId();
    int rawFileSize();
    QVector<QString> getRawFiles(int);

public slots:
    void processClip(bool isSave, QString rootPath);
    void changeDir();


signals:
    void finishCreateClip();
//    void requestCreateClip(const QString& camId, const QVector<QString>& clips);

private:
    QString camId;
    QQueue<QString> sensorDirs;
    QQueue<QString> rawFiles;
    QVector<QString> trashList;
};

#endif // CAMWORKER_H
