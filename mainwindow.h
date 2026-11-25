#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QComboBox>
#include <QPlainTextEdit>
#include "vlmcontroller.h"
#include "missioncontroller.h"
#include "workmanager.h"
#include "videowidget.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

signals:
    void cameraSelected(const QString& cameraName);
    void frameConsumed();

private slots:
    void appendLog(const QString& message);

private:
    QComboBox* cameraSelector = nullptr;
    VideoWidget* videoWidget = nullptr;
    QPlainTextEdit* logView = nullptr;
    QThread* receiverThread = nullptr;
    WorkManager* workManager = nullptr;
    MissionController* missionController = nullptr;
    VLMController* vlmController = nullptr;

};
#endif // MAINWINDOW_H
