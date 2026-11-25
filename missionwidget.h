#ifndef MISSIONWIDGET_H
#define MISSIONWIDGET_H

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProgressBar;

class MissionWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MissionWidget(QWidget* parent = nullptr);

public slots:
    void showMission(const QJsonObject& mission);
    void showCheckFailed(const QString& message);
    void showHeartbeatStatus(bool success);
    void showProcessingStatus(const QString& status);
    void showProgress(int saved, int target);

private:
    QLabel* title;
    QLabel* receivedAt;
    QLabel* heartbeatStatus;
    QPlainTextEdit* details;
    QLabel* processingStatus;
    QProgressBar* progress;
    bool hasMission = false;
};

#endif // MISSIONWIDGET_H
