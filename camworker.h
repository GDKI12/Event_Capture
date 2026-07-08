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


private:
    QString camId;
    QString dstIp;
    int dstPort;
    int metaPort;

    QQueue<QString> rawFiles;

};

#endif // CAMWORKER_H
