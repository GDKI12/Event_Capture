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

void CamWorker::cancelCurrentBatch()
{
    trashList.clear();
    inferStatus = false;
}

QVector<QString> CamWorker::getRawFiles(int videoL)
{
    // 저장/삭제 전에는 다음 배치를 꺼내 현재 배치와 섞지 않는다.
    if(!trashList.isEmpty())
    {
        Writter::warn(QString("Worker(%1): previous raw batch is still pending").arg(camId));
        return {};
    }
    QVector<QString> result;

    if(videoL <= 0 || (sensorDirs.isEmpty() && (rawFiles.size() <= 0)))
    {
        Writter::warn(QString("Worker(%1): insufficient raw files (%2 requested, %3 available)")
                      .arg(camId).arg(videoL).arg(rawFiles.size()));
        Writter::info("There is no Data in root folder");
        trashList.clear();
        return {};
        // error
    }


    while(!sensorDirs.isEmpty())
    {
        if(rawFiles.size() >= videoL)
            break;
        curDir = sensorDirs.dequeue();
        QString rawDirPath = QString("%1/%2").arg(curDir, camId);

        QDir rawDir(rawDirPath);
        QFileInfoList infos = rawDir.entryInfoList({"*.raw"}, QDir::Files | QDir::NoDotAndDotDot,
                                                   QDir::Name);
        if(infos.isEmpty())
            continue;

        for(const QFileInfo& info : infos)
            rawFiles.enqueue(info.absoluteFilePath());
    }

    if(sensorDirs.isEmpty() && rawFiles.size() < videoL)
    {
        Writter::info("Insufficient data");
        trashList.clear();
        return {};
    }

    for(int i = 0; i < videoL; i++)
    {
        QString rawFilePath = rawFiles.dequeue();
        result.push_back(rawFilePath);
    }

    trashList = result;
    return result;
}

void CamWorker::addRawFile(const QString& rawFile)
{
//    Writter::info(QString("Insert %1 to queue of %2  %3").arg(rawFile, camId, QString::number(rawFileSize())));
    if(!rawFiles.contains(rawFile) && !trashList.contains(rawFile))
        rawFiles.enqueue(rawFile);
}

QVector<QString> CamWorker::discoverRawFiles(const QFileInfoList& files)
{
    QSet<QString> currentFiles;
    QVector<QString> newFiles;
    for(const QFileInfo& file : files)
    {
        const QString path = file.absoluteFilePath();
        currentFiles.insert(path);
        if(!knownRawFiles.contains(path)
                && !rawFiles.contains(path) && !trashList.contains(path))
            newFiles.append(path);
    }
    // 삭제 이벤트에서는 남아 있는 원본을 새 파일로 다시 처리하지 않는다.
    knownRawFiles = currentFiles;
    return newFiles;
}

// 파일모드시 처리할 디렉토리들저장
void CamWorker::setSensorDirs(const QVector<QString>& dirList)
{
    for(const QString& dir : dirList)
        sensorDirs.enqueue(dir);
}

int CamWorker::rawFileSize(){ return rawFiles.size(); }

QString CamWorker::getCamId(){return camId;}

bool CamWorker::sensorDirIsEmpty(){ return sensorDirs.isEmpty(); }
bool CamWorker::processClip(bool isSave, QString rootPath)
{
    if(isSave)
    {
        if(trashList.isEmpty())
        {
            Writter::error(QString("[%1] Save failed: empty batch").arg(camId));
            return false;
        }
        if(!QDir().mkpath(rootPath))
        {
            Writter::error(QString("[%1] Save failed: cannot create %2").arg(camId, rootPath));
            return false;
        }

        // 전체 복사가 성공하기 전에는 원본을 삭제하지 않는다.
        QStringList copiedFiles;
        for(const QString& filePath : std::as_const(trashList))
        {
            const QString dstPath = QDir(rootPath).filePath(QFileInfo(filePath).fileName());
            QFile source(filePath);
            if(!source.copy(dstPath))
            {
                Writter::error(QString("[%1] Save failed: %2 -> %3: %4 (%5/%6 copied)")
                              .arg(camId, filePath, dstPath, source.errorString())
                              .arg(copiedFiles.size()).arg(trashList.size()));
                for(const QString& copied : std::as_const(copiedFiles))
                    if(!QFile::remove(copied))
                        Writter::error(QString("Failed to roll back saved file: %1").arg(copied));
                return false;
            }
            copiedFiles.append(dstPath);
        }
        for(const QString& filePath : std::as_const(trashList))
            if(!QFile::remove(filePath))
                Writter::warn(QString("Saved but failed to remove source: %1").arg(filePath));

        Writter::info(QString("[%1] Saved %2/%2 raw files to %3")
                      .arg(camId).arg(trashList.size()).arg(rootPath));
    }else
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
    return true;
}
