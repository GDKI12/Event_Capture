#include "ui.h"

UI::UI(QWidget* parent) : QMainWindow(parent)
{
    WorkManager manager;

    QWidget* centralWidget = new QWidget(this);

    setCentralWidget(centralWidget);

    resize(1280, 720);
}
