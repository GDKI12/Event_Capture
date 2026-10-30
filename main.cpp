#include <QApplication>
#include "workmanager.h"
#include "ui.h"
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    WorkManager manager;

    UI ui;
    ui.show();
    return a.exec();
}
