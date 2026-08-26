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
    explicit CamWorker(const QString& camId, int port, QObject* parent = nullptr);
    void addRawFiles(const QString&);
    int getPort();
    QString getCamId();
    int rawFileSize();
    QVector<QString> getRawFiles(int, int);

public slots:
    void processClip(bool isSave, QString rootPath);

private:
    QString camId;
    QString dstIp;
    int dstPort;

    QQueue<QString> rawFiles;
    QVector<QString> trashList;
};

#endif // CAMWORKER_H
