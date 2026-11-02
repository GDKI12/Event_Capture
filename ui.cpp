#include "ui.h"
#include <QHBoxLayout>
#include <QVBoxLayout>

UI::UI(QWidget* parent) : QMainWindow(parent)
{
    WorkManager manager;

    QWidget* centralWidget = new QWidget(this);
    progressBar = new QProgressBar();

    logDisplay = new QPlainTextEdit();
    logDisplay->setReadOnly(true);




    setCentralWidget(centralWidget);

    resize(1280, 720);
}

UI::~UI(){}

void UI::appendLog(const QString& line)
{
    logDisplay->appendPlainText(line);
}

void UI::clearLog()
{
    logDisplay->clear();
}

void UI::initProgressBar(int maxValue)
{
    progressBar->setRange(0, maxValue);
}

void UI::setValueProgressBar(int value)
{
    progressBar->setValue(value);
}
