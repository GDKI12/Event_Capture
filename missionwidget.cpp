#include "missionwidget.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace {
QString conditionText(const QJsonValue& value)
{
    static const QHash<QString, QString> labels = {
        {"any", "상관없음"},
        {"clear", "맑음"}, {"overcast", "흐림"}, {"cloudy", "흐림"},
        {"rain", "비"}, {"snow", "눈"}, {"fog", "안개"},
        {"daytime", "주간"}, {"nighttime", "야간"},
        {"highway", "고속도로"}, {"urban_arterial", "도심 간선도로"},
        {"urban_local", "도심 이면도로"}, {"intersection_sig", "신호 교차로"},
        {"intersection_unsig", "비신호 교차로"}, {"intersection_round", "회전 교차로"},
        {"school_zone", "스쿨존"}, {"parking_area", "주차장"},
        {"unpaved", "비포장도로"}, {"tunnel", "터널"}, {"overpass", "고가도로 밑"},
        {"lane_keep", "차로 유지"}, {"lane_change_merge", "차로 변경/합류"},
        {"overtake", "주·정차 차량 추월"}, {"inter_sig", "신호 교차로 통과"},
        {"inter_unsig", "비신호 교차로 통과"}, {"inter_round", "회전 교차로 통과"},
        {"ped_cross", "보행자 횡단"}, {"cyclist", "이륜차·자전거"},
        {"construction", "공사 구간"}, {"parked_obs", "불법주차·장애물"},
        {"passenger", "승객 승하차"}, {"emergency", "긴급차량 대응"},
        {"cut_in", "갑작스런 끼어들기"}, {"sudden_stop", "전방 차량 급정지"},
        {"wrong_way", "역주행"}, {"police_signal", "경찰관 수신호"},
        {"road_accident", "도로 사고 현장"}
    };
    QStringList result;
    for (const auto& item : value.toArray()) {
        const QString code = item.toString();
        if (!code.isEmpty())
            result.append(labels.value(code, code));
    }
    return result.isEmpty() ? QStringLiteral("-") : result.join(", ");
}

QString numberText(const QJsonValue& value)
{
    return value.isDouble() ? QString::number(value.toDouble(), 'g', 12) : QStringLiteral("-");
}
}

MissionWidget::MissionWidget(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 12, 10, 12);
    layout->setSpacing(10);
    title = new QLabel(tr("미션 확인 중…"), this);
    title->setObjectName("missionTitle");
    title->setAlignment(Qt::AlignCenter);
    title->setWordWrap(true);
    title->setStyleSheet("font-size: 18px; font-weight: bold; color: #315578;");
    layout->addWidget(title);
    receivedAt = new QLabel(this);
    receivedAt->setObjectName("missionReceivedAt");
    receivedAt->setWordWrap(true);
    receivedAt->hide();
    layout->addWidget(receivedAt);
    details = new QPlainTextEdit(this);
    details->setObjectName("missionDetails");
    details->setReadOnly(true);
    details->setFrameShape(QFrame::NoFrame);
    details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(details, 1);
    heartbeatStatus = new QLabel(tr("Heartbeat: 연결 확인 중"), this);
    heartbeatStatus->setObjectName("heartbeatStatus");
    heartbeatStatus->setWordWrap(true);
    layout->addWidget(heartbeatStatus);
}

void MissionWidget::showMission(const QJsonObject& mission)
{
    hasMission = !mission.isEmpty();
    receivedAt->setVisible(hasMission);
    if (!hasMission) {
        title->setText(QStringLiteral("No Mission"));
        details->clear();
        return;
    }

    title->setText(QStringLiteral("Mission"));
    receivedAt->setText(tr("수신: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
    const auto conditions = mission.value("conditions").toObject();
    const auto volume = mission.value("volume").toObject();
    const auto storage = mission.value("storageTarget").toObject();
    const QString writePath = storage.value("writePath").toString();
    QString project = mission.value("projectName").toString();
    if (project.isEmpty()) {
        for (const QString& part : writePath.split('/')) {
            if (part.startsWith("PRJ"))
                project = part;
        }
    }
    QStringList lines;
    lines << tr("미션 ID: %1").arg(mission.value("dispatchId").toString())
          << tr("프로젝트명: %1").arg(project.isEmpty() ? "-" : project)
          << tr("수집장치: %1").arg(mission.value("deviceType").toString("-"))
          << tr("클립 길이: %1초").arg(numberText(volume.value("clipLengthSec")))
          << tr("목표 장면 수: %1").arg(numberText(volume.value("targetSceneCount")))
          << QString()
          << tr("날씨: %1").arg(conditionText(conditions.value("weatherConditions")))
          << tr("시간: %1").arg(conditionText(conditions.value("timeConditions")))
          << tr("도로환경: %1").arg(conditionText(conditions.value("roadEnvironments")))
          << tr("시나리오: %1").arg(conditionText(conditions.value("scenarios")))
          << QString()
          << tr("저장 경로: %1").arg(writePath.isEmpty() ? "-" : writePath);
    details->setPlainText(lines.join('\n'));
}

void MissionWidget::showCheckFailed(const QString& message)
{
    if (!hasMission) {
        title->setText(tr("미션 확인 실패"));
        details->setPlainText(message);
    }
}

void MissionWidget::showHeartbeatStatus(bool success)
{
    heartbeatStatus->setText(success ? tr("Heartbeat: 정상") : tr("Heartbeat: 실패 · 재시도 대기"));
}
