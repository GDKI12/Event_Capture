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
const QString SENSOR_LIST_FILE = "/home/tesla/EdgeInfravision/EdgeInfra_Capture_v4/config/sensor_list.json";


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
            std::string cDstIp;
            std::string cVssHealthyURL;
            std::string cBaseUrl;
            std::string cAuthId;
            std::string cSecretKey;

            cRootPath = toml::find<std::string>(data, "setting", "root_path");
            cSavePath = toml::find<std::string>(data, "setting", "save_path");
            cDstIp = toml::find<std::string>(data, "setting","dst_ip");

            cVssHealthyURL = toml::find<std::string>(data, "setting", "vss_healthy_url");
            cBaseUrl = toml::find<std::string>(data, "setting", "base_url");
            cAuthId = toml::find<std::string>(data, "setting", "auth_id");
            cSecretKey = toml::find<std::string>(data, "setting", "secret_key");

            vssHealthyURL = QString::fromStdString(cVssHealthyURL);
            baseURL = QString::fromStdString(cBaseUrl);
            authId = QString::fromStdString(cAuthId);
            secretKey = QString::fromStdString(cSecretKey);

            rootPath = QString::fromStdString(cRootPath);
            savePath = QString::fromStdString(cSavePath);
            ip = QString::fromStdString(cDstIp);

            port = toml::find<int>(data, "setting","dst_port");
            timeInterval = toml::find<int>(data, "setting","time_interval");
            timeInterval *= 10;

            videoLength = toml::find<int>(data, "setting","video_length");
            videoLength = videoLength * 10;

            mode = toml::find<bool>(data, "setting","live_mode");
            width = toml::find<int>(data, "setting", "width");
            height = toml::find<int>(data, "setting", "height");

            QFile snesorListFile(SENSOR_LIST_FILE);



        } catch (const std::exception& e)
        {
            qCritical() << "failed to load config settings";
        }
    }
public:
    QString vssHealthyURL;
    QString baseURL;
    QString authId;
    QString secretKey;

    QString rootPath;
    QString savePath;
    QString ip;
    int port;
    int videoLength;
    int timeInterval;
    int width;
    int height;
    bool mode;
    int rawSize;
};

#endif // CONFIG_H


