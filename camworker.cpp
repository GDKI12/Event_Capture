#include "camworker.h"

#include <QElapsedTimer>
#include <QThread>
#include <QAbstractSocket>
#include <QRegularExpression>
#include <QtGlobal>
#include <algorithm>
#include <QStringList>
#include <QList>
#include "define.h"


namespace VSS {
    void removeDir(const QList<QString>& files
                   , int startIndex, int len)
    {
        int endIndex = qMin(startIndex + len, files.size());

        for(int i = startIndex; i < endIndex; i++)
        {
            const QString& f = files[i];

            if(QFile::exists(f))
                QFile::remove(f);
        }
    }
}

CamWorker::CamWorker(const QString& camId,int port, QObject* parent)
    : QObject(parent), camId(camId), dstPort(port)
{
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

    trashList = result;

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

void CamWorker::processClip(bool isSave)
{
    if(!isSave)
    {
        for(const QString& filePath : std::as_const(trashList))
        {
            if(QFile::exists(filePath))
            {
                QFile::remove(filePath);
            }
        }

        Writter::info("Success to remove file");
    }

    trashList.clear();
}
