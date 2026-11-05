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
        QDir subDir(fi.absoluteFilePath());
        deleteEmptyFolder(subDir);

        QFileInfoList remainingList = subDir.entryInfoList(
                QDir::AllEntries |
                QDir::NoDotAndDotDot |
                QDir::Hidden |
                QDir::System);

        // Only remove children so the directory passed by the caller is preserved.
        if(remainingList.isEmpty())
        {
            if(subDir.rmdir(subDir.absolutePath()))
                Writter::info("Success to remove folder("+subDir.absolutePath()+")");
            else
                Writter::info("Fail to remove folder("+subDir.absolutePath()+")");
        }
    }

}
