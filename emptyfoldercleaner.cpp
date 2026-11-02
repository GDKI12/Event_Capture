#include "emptyfoldercleaner.h"

#include <QFileInfoList>
#include "define.h"
EmptyFolderCleaner::EmptyFolderCleaner( QObject* parent)
    : QObject(parent)
{}

EmptyFolderCleaner::~EmptyFolderCleaner(){};

void EmptyFolderCleaner::deleteEmptyFolder(QDir dir)
{
    QFileInfoList subDirList = dir.entryInfoList(
                QDir::Dirs |
                QDir::NoDotAndDotDot,
                QDir::Name);

    for(QFileInfo fi : subDirList)
    {
        deleteEmptyFolder(QDir(fi.absoluteFilePath()));
    }

    QFileInfoList remainingList = dir.entryInfoList(
                QDir::AllEntries |
                QDir::NoDotAndDotDot |
                QDir::Hidden |
                QDir::System);

    if(remainingList.isEmpty())
    {
        if(dir.rmdir(dir.absolutePath()))
            Writter::info("Success to remove folder("+dir.absolutePath()+")");
        else
            Writter::info("Fail to remove folder("+dir.absolutePath()+")");
    }

}
