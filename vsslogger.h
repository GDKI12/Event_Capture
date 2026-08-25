#ifndef VSSLOGGER_H
#define VSSLOGGER_H

#include <QObject>

class VssLogger: public QObject
{
    Q_OBJECT
public:
    VssLogger(const QString& rootPath, QObject* parent = nullptr);

public slots:
    void addLog();
private:
    QString filePath;
};

#endif // VSSLOGGER_H
