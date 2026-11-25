#ifndef MISSIONLOGIC_H
#define MISSIONLOGIC_H

#include <QJsonObject>
#include <QString>

namespace MissionLogic {
QString createPrompt(const QJsonObject& mission, const QJsonObject& scenarioPrompts,
                     const QString& templatePath, QString& error);
bool matches(const QJsonObject& mission, const QString& answer, QString& reason);
QString scenePath(const QString& saveRoot, const QJsonObject& storage, int index, QString& error);
}

#endif
