#ifndef UI_H
#define UI_H

#include <QWidget>
#include <QMainWindow>
#include <QProgressBar>
#include <QPlainTextEdit>

#include "workmanager.h"

class UI : public QMainWindow
{
    Q_OBJECT
public:
    UI(QWidget* parent = nullptr);
    ~UI();
public slots:
    void appendLog(const QString& line);
    void clearLog();
    void initProgressBar(int);
    void setValueProgressBar(int);
private:
    WorkManager manager;
    QProgressBar *progressBar;
    QPlainTextEdit *logDisplay;

};

#endif // UI_H
