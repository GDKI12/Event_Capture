#include <QCoreApplication>
#include "workmanager.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);


    QVector<QString> list;
    list.push_back("cam1");
    list.push_back("cam2");
    list.push_back("cam3");

    WorkManager manager(list);

    return a.exec();
}
