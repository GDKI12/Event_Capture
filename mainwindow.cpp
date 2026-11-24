#include "mainwindow.h"
#include "videowidget.h"
#include "workmanager.h"
#include "missioncontroller.h"
#include "missionwidget.h"

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QThread>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Event Capture"));
    resize(1100, 780);
    setMinimumSize(640, 480);

    auto* central = new QFrame(this);
    central->setObjectName("capturePanel");
    central->setStyleSheet("QFrame#capturePanel { background: white; border: 1px solid #315578; }");
    setCentralWidget(central);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(16, 12, 16, 16);
    layout->setSpacing(14);

    auto* toolbar = new QHBoxLayout;
    cameraSelector = new QComboBox(central);
    cameraSelector->setObjectName("cameraSelector");
    cameraSelector->addItem(tr("캠 선택"), QString());
    cameraSelector->setMinimumWidth(140);
    cameraSelector->setEnabled(false);
    cameraSelector->setAccessibleName(tr("캠 선택"));
    cameraSelector->setToolTip(tr("표시할 카메라를 선택하세요."));
    toolbar->addWidget(cameraSelector);
    toolbar->addStretch();
    layout->addLayout(toolbar);

    auto* content = new QHBoxLayout;
    content->setSpacing(20);
    videoWidget = new VideoWidget(central);
    videoWidget->setObjectName("cameraView");
    videoWidget->clear(tr("카메라를 선택하세요."));
    content->addWidget(videoWidget, 5);

    auto* rightWidget = new QFrame(central);
    rightWidget->setObjectName("rightWidget");
    rightWidget->setMinimumWidth(260);
    rightWidget->setStyleSheet("QFrame#rightWidget { background: white; border: 1px solid #315578; }");
    auto* missionLayout = new QVBoxLayout(rightWidget);
    missionLayout->setContentsMargins(0, 0, 0, 0);
    auto* missionWidget = new MissionWidget(rightWidget);
    missionLayout->addWidget(missionWidget);
    content->addWidget(rightWidget, 2);
    layout->addLayout(content, 4);

    auto* logPanel = new QFrame(central);
    logPanel->setObjectName("logPanel");
    logPanel->setStyleSheet("QFrame#logPanel { border: 1px solid #315578; }");
    auto* logLayout = new QVBoxLayout(logPanel);
    logLayout->setContentsMargins(8, 6, 8, 6);
    logLayout->setSpacing(4);
    logLayout->addWidget(new QLabel(tr("로그 창"), logPanel));
    logView = new QPlainTextEdit(logPanel);
    logView->setObjectName("logView");
    logView->setReadOnly(true);
    logView->setMaximumBlockCount(2000);
    logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    logView->setFrameShape(QFrame::NoFrame);
    logView->setMinimumHeight(90);
    logLayout->addWidget(logView);
    layout->addWidget(logPanel, 1);

    receiverThread = new QThread(this);
    workManager = new WorkManager;
    missionController = new MissionController(this);
    connect(workManager, &WorkManager::configurationLoaded, missionController, &MissionController::start);
    connect(missionController, &MissionController::missionUpdated, missionWidget, &MissionWidget::showMission);
    connect(missionController, &MissionController::missionCheckFailed, missionWidget, &MissionWidget::showCheckFailed);
    connect(missionController, &MissionController::heartbeatStatusChanged, missionWidget, &MissionWidget::showHeartbeatStatus);
    connect(missionController, &MissionController::logMessage, this, &MainWindow::appendLog);
    workManager->moveToThread(receiverThread);
    connect(receiverThread, &QThread::started, workManager, &WorkManager::start);
    connect(receiverThread, &QThread::finished, workManager, &QObject::deleteLater);
    connect(workManager, &WorkManager::logMessage, this, &MainWindow::appendLog);
    connect(workManager, &WorkManager::cameraNamesChanged, this, [this](const QStringList& names) {
        cameraSelector->clear();
        cameraSelector->addItem(tr("캠 선택"), QString());
        for (const QString& name : names)
            cameraSelector->addItem(name, name);
        cameraSelector->setEnabled(!names.isEmpty());
        if (names.isEmpty())
            videoWidget->clear(tr("설정에 등록된 카메라가 없습니다."));
    });
    connect(cameraSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const QString name = cameraSelector->currentData().toString();
        videoWidget->clear(name.isEmpty() ? tr("카메라를 선택하세요.") : tr("%1: 영상 수신 대기 중").arg(name));
        emit cameraSelected(name);
    });
    connect(this, &MainWindow::cameraSelected, workManager, &WorkManager::selectCamera);
    connect(workManager, &WorkManager::frameReady, this, [this](const QString& name, const QImage& image) {
        if (name == cameraSelector->currentData().toString())
            videoWidget->setFrame(image);
        emit frameConsumed();
    });
    connect(this, &MainWindow::frameConsumed, workManager, &WorkManager::acknowledgeFrame);
    connect(workManager, &WorkManager::streamUnavailable, this, [this](const QString& name) {
        if (name == cameraSelector->currentData().toString())
            videoWidget->clear(tr("%1: 영상 수신 대기 중").arg(name));
    });
    appendLog(tr("[INFO] Event Capture 시작"));
    receiverThread->start();
}

MainWindow::~MainWindow()
{
    missionController->stop();
    if (receiverThread->isRunning()) {
        QMetaObject::invokeMethod(workManager, "stop", Qt::BlockingQueuedConnection);
        receiverThread->quit();
        receiverThread->wait();
    }
}

void MainWindow::appendLog(const QString& message)
{
    auto* scrollBar = logView->verticalScrollBar();
    const bool follow = scrollBar->value() == scrollBar->maximum();
    logView->appendPlainText(QDateTime::currentDateTime().toString("[yyyy-MM-dd HH:mm:ss.zzz] ") + message);
    if (follow)
        scrollBar->setValue(scrollBar->maximum());
}

