#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class QComboBox;
class QPlainTextEdit;
class QThread;
class VideoWidget;
class WorkManager;

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
};
#endif // MAINWINDOW_H
