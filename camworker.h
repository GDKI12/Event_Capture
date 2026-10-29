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

    void addRawFile(const QString&);
    QVector<QString> discoverRawFiles(const QFileInfoList& files);

    bool sensorDirIsEmpty();
    QString getCamId();
    int rawFileSize();
    QVector<QString> getRawFiles(int);
    bool getStatus();
    void setStatus(bool);
    void cancelCurrentBatch();

public slots:
    bool processClip(bool isSave, QString rootPath);


signals:
    void finishCreateClip();
//    void requestCreateClip(const QString& camId, const QVector<QString>& clips);

private:
    QString camId;
    QString curDir;
    QQueue<QString> sensorDirs;
    QQueue<QString> rawFiles;
    QVector<QString> trashList;
    QSet<QString> knownRawFiles;

    bool inferStatus;
};

#endif // CAMWORKER_H
