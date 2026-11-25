#include "missionlogic.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QStringList>

namespace {
QStringList scenarios(const QJsonObject& mission)
{
    QStringList result;
    for (const auto& value : mission.value("conditions").toObject().value("scenarios").toArray()) {
        const QString code = value.toString().trimmed();
        if (!code.isEmpty() && code != "any" && !result.contains(code))
            result.append(code);
    }
    return result;
}

bool accepts(const QJsonValue& conditions, const QString& actual)
{
    const auto values = conditions.toArray();
    if (values.isEmpty())
        return true;
    for (const auto& value : values) {
        const auto expected = value.toString().toLower();
        if (expected == "any" || expected == actual || (expected == "cloudy" && actual == "overcast"))
            return true;
    }
    return false;
}
}

QString MissionLogic::createPrompt(const QJsonObject& mission, const QJsonObject& scenarioPrompts,
                                  const QString& templatePath, QString& error)
{
    error.clear();
    QFile file(templatePath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("프롬프트 파일 열기 실패: %1").arg(templatePath);
        return {};
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const auto root = document.object();
    const auto common = root.value("common_prompt").toString();
    const auto output = root.value("output_prompt").toString();
    const auto composition = root.value("composition").toObject();
    QString header = composition.value("selected_event_header_template").toString();
    if (file.error() != QFile::NoError || parseError.error != QJsonParseError::NoError
        || common.trimmed().isEmpty() || output.trimmed().isEmpty()
        || !header.contains("{{SELECTED_EVENT_NAMES}}")) {
        error = QStringLiteral("프롬프트 파일의 common_prompt, output_prompt, composition을 확인하세요.");
        return {};
    }
    const auto selected = scenarios(mission);
    QStringList events;
    for (const auto& code : selected) {
        const auto text = scenarioPrompts.value(code).toString();
        if (text.trimmed().isEmpty()) {
            error = QStringLiteral("서버 시나리오 프롬프트가 없습니다: %1").arg(code);
            return {};
        }
        events.append(text);
    }
    header.replace("{{SELECTED_EVENT_NAMES}}", selected.isEmpty() ? QStringLiteral("none")
                   : selected.join(composition.value("selected_event_names_separator").toString(", ")));
    return QStringList{common, header,
                       events.join(composition.value("event_separator").toString("\n\n")), output}.join("\n\n");
}

bool MissionLogic::matches(const QJsonObject& mission, const QString& answer, QString& reason)
{
    reason.clear();
    QString time, weather, currentEvent;
    QMap<QString, QString> results;
    bool malformed = false;
    for (const auto& line : answer.split('\n')) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const auto key = line.left(colon).trimmed();
        const auto value = line.mid(colon + 1).trimmed();
        if (key == "Time") {
            malformed |= !time.isEmpty();
            time = value.toLower();
        } else if (key == "Weather") {
            malformed |= !weather.isEmpty();
            weather = value.toLower();
        } else if (key == "Event") {
            currentEvent = value;
        } else if (key == "Result") {
            malformed |= currentEvent.isEmpty() || results.contains(currentEvent);
            results.insert(currentEvent, value.toLower());
            currentEvent.clear();
        }
    }
    const QStringList times{"daytime", "nighttime"};
    const QStringList weathers{"clear", "overcast", "rain", "snow", "fog"};
    if (malformed || !times.contains(time) || !weathers.contains(weather)) {
        reason = QStringLiteral("추론 응답의 Time/Weather/Result 형식이 잘못되었습니다.");
        return false;
    }
    const auto conditions = mission.value("conditions").toObject();
    if (!accepts(conditions.value("timeConditions"), time))
        reason = QStringLiteral("시간 조건 불일치");
    if (!accepts(conditions.value("weatherConditions"), weather))
        reason += (reason.isEmpty() ? "" : ", ") + QStringLiteral("날씨 조건 불일치");
    const auto selected = scenarios(mission);
    // Server event prompts may use descriptive Output Event names instead of codes.
    // Every selected event must have a distinct affirmative result.
    bool eventsMatch = results.size() == selected.size();
    for (auto result = results.cbegin(); result != results.cend(); ++result)
        eventsMatch &= result.value() == "yes";
    if (!eventsMatch)
        reason += (reason.isEmpty() ? "" : ", ") + QStringLiteral("시나리오 미검출 또는 결과 누락");
    // V1 skips GPS road matching for fixed infrastructure cameras.
    if (mission.value("deviceType").toString() != "infra"
        && !accepts(conditions.value("roadEnvironments"), QString()))
        reason += (reason.isEmpty() ? "" : ", ") + QStringLiteral("GPS 도로 판정 정보 없음");
    return reason.isEmpty();
}

QString MissionLogic::scenePath(const QString& saveRoot, const QJsonObject& storage, int index, QString& error)
{
    error.clear();
    const auto scenes = storage.value("scenes").toArray();
    if (saveRoot.trimmed().isEmpty() || index < 0 || index >= scenes.size()) {
        error = QStringLiteral("미션 저장 경로 또는 장면 목록이 잘못되었습니다.");
        return {};
    }
    QString parent = storage.value("writePath").toString();
    while (parent.startsWith('/'))
        parent.remove(0, 1);
    const QString relative = scenes.at(index).toObject().value("relativePath").toString();
    const QString combined = QDir::cleanPath(parent.isEmpty() ? relative : parent + "/" + relative);
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || combined == "." || combined == ".."
        || combined.startsWith("../") || relative.split('/').contains("..") || parent.split('/').contains("..")) {
        error = QStringLiteral("미션 저장 경로에 허용되지 않는 상대 경로가 있습니다.");
        return {};
    }
    return QDir(saveRoot).absoluteFilePath(combined);
}
