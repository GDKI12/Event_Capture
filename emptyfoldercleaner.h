#ifndef EMPTYFOLDERCLEANER_H
#define EMPTYFOLDERCLEANER_H

#include <QDir>
#include <QObject>

class EmptyFolderCleaner : public QObject
{
    Q_OBJECT
public:
    EmptyFolderCleaner(QObject* parent = nullptr);
    ~EmptyFolderCleaner();
public slots:
    void deleteEmptyFolder(QDir dir);
};

#endif // EMPTYFOLDERCLEANER_H
