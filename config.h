#ifndef CONFIG_H
#define CONFIG_H

#include <iostream>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <QByteArray>
#include <QString>
#include <iostream>
#include <toml.hpp>
#include <QDebug>
#include <QFile>
#include <QJsonParseError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

const QString DEFAULT_PATH = "../config/config.toml";

class Config
{
public:
    Config() = default;
    ~Config() = default;
    void loadConfig()
    {
        try
        {
            QFile file(DEFAULT_PATH);
            if(file.exists())
                qDebug() <<"";
            auto data = toml::parse(DEFAULT_PATH.toStdString());

            std::string cRootPath;
            std::string cSavePath;
            std::string cLogPath;
            std::string cBaseUrl;
            std::string cAuthId;
            std::string cSecretKey;

            std::vector<std::string> camVector
                    = toml::find<std::vector<std::string>>(data, "setting", "cam_list");

            for(const std::string& cam : camVector)
                camList.append(QString::fromStdString(cam));

            cRootPath = toml::find<std::string>(data, "setting", "root_path");
            cSavePath = toml::find<std::string>(data, "setting", "save_path");
            cLogPath = toml::find<std::string>(data,"setting", "log_path");

            cBaseUrl = toml::find<std::string>(data, "setting", "base_url");
            cAuthId = toml::find<std::string>(data, "setting", "auth_id");
            cSecretKey = toml::find<std::string>(data, "setting", "secret_key");

            baseURL = QString::fromStdString(cBaseUrl);
            authId = QString::fromStdString(cAuthId);
            secretKey = QString::fromStdString(cSecretKey);

            rootPath = QString::fromStdString(cRootPath);
            savePath = QString::fromStdString(cSavePath);
            logPath = QString::fromStdString(cLogPath);

            timeInterval = toml::find<int>(data, "setting","time_interval");
            timeInterval *= 10;

            videoLength = toml::find<int>(data, "setting","video_length");
            videoLength = videoLength * 10;

            mode = toml::find<bool>(data, "setting","live_mode");
            width = toml::find<int>(data, "setting", "width");
            height = toml::find<int>(data, "setting", "height");


        } catch (const std::exception& e)
        {
            qCritical() << "failed to load config settings";
        }
    }

    QString getPrompt(const QString& scenario)
    {
        QFile file(PORMPT_FILE_PATH);
        if(!file.open(QIODevice::ReadOnly))
        {
            qCritical() << "Failt to open prompt file";
            return QString();
        }

        QByteArray root = file.readAll();
        file.close();

        QJsonDocument doc = QJsonDocument::fromJson(root);
        QJsonObject data = doc.object();
        QString prompt = data[scenario].toString();

        return prompt;
        qDebug() << "TEST";

    }
public:
    QString baseURL;
    QString authId;
    QString secretKey;

    QString rootPath;
    QString savePath;
    QString logPath;

    QList<QString> camList;

    int videoLength;
    int timeInterval;
    int width;
    int height;
    bool mode;
    int rawSize;
};

#endif // CONFIG_H


