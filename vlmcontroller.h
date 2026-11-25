#ifndef VLMCONTROLLER_H
#define VLMCONTROLLER_H

#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>

class VLMController : public QObject
{
    Q_OBJECT
public:
    explicit VLMController(QObject *parent = nullptr);
    ~VLMController() override;
    void configure(const QString& configPath);
    void isVLMAlive(const QString& videoPath, const QString& prompt);
    void cancel();

signals:
    void logMessage(const QString& message);
    void finished(const QString& videoPath, const QString& answer, double seconds);
    void failed(const QString& message);

private:
    QNetworkAccessManager manager;
    QSet<QNetworkReply*> replies;
    QString baseUrl = "http://127.0.0.1:8000";
    QString model = "cosmos-reason2";
    int healthTimeoutMs = 30000;
    int inferTimeoutMs = 600000;
    bool busy = false;
    quint64 generation = 0;
    void infer(const QString& videoPath, const QString& prompt);
    void setTimeout(QNetworkReply* reply, int timeoutMs);
    QString error(QNetworkReply* reply) const;

};

#endif // VLMCONTROLLER_H
