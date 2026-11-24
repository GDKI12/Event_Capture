#ifndef MISSIONWIDGET_H
#define MISSIONWIDGET_H

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QPlainTextEdit;

class MissionWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MissionWidget(QWidget* parent = nullptr);

public slots:
    void showMission(const QJsonObject& mission);
    void showCheckFailed(const QString& message);
    void showHeartbeatStatus(bool success);

private:
    QLabel* title;
    QLabel* receivedAt;
    QLabel* heartbeatStatus;
    QPlainTextEdit* details;
    bool hasMission = false;
};

#endif // MISSIONWIDGET_H
