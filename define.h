#ifndef DEFINE_H
#define DEFINE_H

#include <QObject>
#include <QDateTime>
#include <QDebug>
#include <QQueue>

enum class LogLevel{
    INFO, WARN, ERROR
};

struct InitConfig
{
    int channel;
    int fps;
    int clipLengthSec;
    int targetScenes;

    QString deviceType;

    QList<QString> weather;
    QList<QString> time;
    QList<QString> roadEnv;
    QList<QString> scenario;
};

struct VssInfo
{
    QVector<quint8> isEvent;
    QVector<quint8> weather;
    QVector<quint8> eventType;
};

struct Mission{
    QString id;
    QString deviceType;
    QList<QString> weather;
    QList<QString> time;
    QList<QString> roadEnv;
    QList<QString> scenario;
    QQueue<QString> saveFolders;
    int clipLengthSec;
    int targetScenes;
};

struct RawFileInfo{
    int index;
    QString fileName;
};

Q_DECLARE_METATYPE(LogLevel)

class Writter
{
public:
  static void write(const QString& content, LogLevel level = LogLevel::INFO)
  {
      QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss.zzz");

      if(level == LogLevel::INFO)
          qDebug().noquote() << timestamp << "[INFO] " << content;
      else if(level == LogLevel::WARN)
          qWarning().noquote() << "\033[33m" << timestamp << "[WARN] " << content;
      else if(level == LogLevel::ERROR)
          qCritical().noquote() << "\033[31m" << timestamp << "[ERROR] " << content;
  }

  static void info(const QString& content)
  {
      write(content, LogLevel::INFO);
  }

  static void warn(const QString& content)
  {
      write(content, LogLevel::WARN);
  }

  static void error(const QString& content)
  {
      write(content, LogLevel::ERROR);
  }
};
#endif // DEFINE_H
