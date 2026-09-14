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

    QJsonArray arr;
    QJsonDocument doc(arr);

    file.write(doc.toJson(QJsonDocument::Indented));

    file.close();
}

void VssLogger::addLog(const QString& rootPath, QStringList text)
{
    QFile file(filePath);
    QJsonObject obj;

    // 저장된파일의 경로를 저장하기위한 처리
    obj["save_path"] = rootPath;

    // summarize 결과를 로그에 넣기위해서 데이터 가공
    for(QString& s : text)
    {
        if(s.isEmpty())
            continue;

        const int separator = s.indexOf(':');
        if(separator < 0)
            continue;

        QString key = s.left(separator).trimmed();
        QString value = s.mid(separator+1).trimmed();

        if(!key.isEmpty())
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
