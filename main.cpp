#include <QApplication>
#include "workmanager.h"
#include "ui.h"
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    UI ui;
    WorkManager manager;
    ui.show();
    return a.exec();
}
