#ifndef UI_H
#define UI_H

#include <QWidget>
#include <QMainWindow>

#include "workmanager.h"

class UI : public QMainWindow
{
    Q_OBJECT
public:
    UI(QWidget* parent = nullptr);
    ~UI();
};

#endif // UI_H
