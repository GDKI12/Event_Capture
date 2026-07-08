#include "camworker.h"

#include <QElapsedTimer>
#include <QThread>
#include <QAbstractSocket>
#include <QRegularExpression>
#include <QtGlobal>
#include <algorithm>
#include <QStringList>
#include "define.h"
CamWorker::CamWorker(const QString& camId,int port, QObject* parent)
    : QObject(parent), camId(camId), dstPort(port)
{
    metaPort = dstPort + 100;

    Writter::info(QString("Worker(%1) is working on %2 port").arg(camId).arg(port));
}


QVector<QString> CamWorker::getRawFiles(int timeInterval, int videoL)
{
    QVector<QString> result;
    for(int i = 0; i < timeInterval; i++)
    {
        if(i < videoL)
            result.push_back(rawFiles.dequeue());
        else
            rawFiles.dequeue();
    }

    return result;
}


void CamWorker::addRawFiles(const QString& dirPath)
{
    // delete raw files
    rawFiles.clear();

    QString rawDirPath = QString("%1/%2").arg(dirPath, camId);

    QDir dir(rawDirPath);

    QFileInfoList infos = dir.entryInfoList({"*.raw"}, QDir::Files | QDir::NoDotAndDotDot,
                                            QDir::Name);

    for(const QFileInfo& info : infos)
        rawFiles.enqueue(info.absoluteFilePath());
}

int CamWorker::rawFileSize(){ return rawFiles.size(); }

int CamWorker::getPort() {return dstPort;}

QString CamWorker::getCamId(){return camId;}
