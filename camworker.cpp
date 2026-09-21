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

CamWorker::CamWorker(const QString& camId, QObject* parent)
    : QObject(parent), camId(camId), inferStatus(false)
{
    Writter::info(QString("Worker(%1) is working").arg(camId));
}

bool CamWorker::getStatus(){return inferStatus;}

void CamWorker::setStatus(bool status){this->inferStatus = status;}

QVector<QString> CamWorker::getRawFiles(int videoL)
{
    Writter::info(QString("Process raw files of %1 %2/%3").arg(camId).arg(videoL).arg(rawFileSize()));

    QVector<QString> result;

    if(videoL <= 0 || rawFiles.size() < videoL)
    {
        Writter::warn(QString("Worker(%1): insufficient raw files (%2 requested, %3 available)")
                      .arg(camId).arg(videoL).arg(rawFiles.size()));
        return result;
    }

    for(int i = 0; i < videoL; i++)
        result.push_back(rawFiles.dequeue());

    trashList = result;

    return result;
}

void CamWorker::addRawFile(const QString& rawFile)
{
//    Writter::info(QString("Insert %1 to queue of %2  %3").arg(rawFile, camId, QString::number(rawFileSize())));
    rawFiles.enqueue(rawFile);
}

// 파일모드시 처리할 디렉토리들저장
void CamWorker::setSensorDirs(const QVector<QString>& dirList)
{
    for(const QString& dir : dirList)
        sensorDirs.enqueue(dir);
}

//
void CamWorker::setRawFiles(const QString& dirPath)
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

QString CamWorker::getCamId(){return camId;}

bool CamWorker::sensorDirIsEmpty(){ return sensorDirs.isEmpty(); }
void CamWorker::processClip(bool isSave, QString rootPath)
{
    QDir().mkpath(rootPath);

    if(isSave)
    {
        for(const QString& filePath : std::as_const(trashList))
        {
            QString filename = filePath.split('/').last();
            QString dstPath = rootPath + "/" + filename;

            if(QFile::exists(filePath))
            {
//                if(!QFile::rename(filePath, dstPath))
                if(!QFile::copy(filePath, dstPath))
                    Writter::error(QString("Fail to move %1 to %2").arg(filePath, dstPath));
            }
        }
        Writter::info("Success to save file");
    }else
    {
        for(const QString& filePath : std::as_const(trashList))
        {
            if(QFile::exists(filePath))
            {
//                QFile::remove(filePath);
            }
        }

        Writter::info("Success to remove file");

    }

    trashList.clear();
}

void CamWorker::changeDir()
{
    QString workDir = sensorDirs.dequeue();
    Writter::info("Current Working directory is " + workDir);
    setRawFiles(workDir);
}
