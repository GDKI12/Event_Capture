#include <QCoreApplication>
#include "workmanager.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    WorkManager manager;

    return a.exec();
}
