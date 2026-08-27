#include "vsslogger.h"

#include <QDateTime>
#include <QFile>

#include "define.h"

VssLogger::VssLogger(const QString& rootPath, QObject* parent) : QObject(parent)
{
    QString date = QDateTime::currentDateTime().toString("yyyyMMdd");
    filePath = rootPath + "/" +date + ".json";
}

void VssLogger::addLog()
{
    QFile file(filePath);

    if(file.open(QIODevice::WriteOnly))
        file.close();

    // 로그파일 안열리면 에러처리더 할까??
    if(!file.open(QIODevice::ReadOnly))
    {
        Writter::warn("Fail to open file: " + filePath);
        return;
    }

    QByteArray data = file.readAll();
    file.close();

}
