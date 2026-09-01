#include "vsslogger.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "define.h"

VssLogger::VssLogger(const QString& rootPath, QObject* parent) : QObject(parent)
{
    QString date = QDateTime::currentDateTime().toString("yyyyMMdd");
    filePath = rootPath + "/" +date + ".json";

    QFile file(filePath);

    if(!file.open(QIODevice::WriteOnly))
    {
        Writter::info(QString("Fail to open log file %1").arg(filePath));
    }

    file.close();
}

void VssLogger::addLog(QVector<QString> processedFiles, QStringList text)
{
    QFile file(filePath);
    QJsonObject obj;
    QJsonArray fileArr;

    // 저장된파일의 경로를 저장하기위한 처리
    for(const QString& s : processedFiles)
    {
        fileArr.append(s);
    }

    obj["files_path"] = fileArr;

    // summarize 결과를 로그에 넣기위해서 데이터 가공
    for(QString& s : text)
    {
        QStringList syntex = s.split(':');
        QString key = syntex[0].trimmed();
        QString value = syntex[1].trimmed();

        obj[key] = value;
    }


    // 로그파일 안열리면 에러처리더 할까??
    if(!file.open(QIODevice::ReadOnly))
    {
        Writter::warn("Fail to open log file: " + filePath);
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if(parseError.error != QJsonParseError::NoError)
    {
        Writter::error("Fail to parse log file");
        return;
    }


    QJsonArray docArr = doc.array();
    docArr.append(obj);

    if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        Writter::error("Fail to open log file for writting");
        return;
    }

    QJsonDocument newDoc(docArr);

    file.write(newDoc.toJson(QJsonDocument::Indented));
    file.close();
}
