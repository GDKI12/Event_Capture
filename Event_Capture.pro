QT       += core gui network concurrent

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS TEST

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

### toml11
INCLUDEPATH += /home/tesla/3rdparty/toml11

### opencv-4.5.1
INCLUDEPATH += /usr/local/include/opencv4

### iceoryx - v2.90.0
INCLUDEPATH += /usr/local/include/iceoryx/v2.90.0

LIBS += -L/usr/local/lib -lopencv_world

### iceoryx
LIBS += -L/usr/local/lib -liceoryx_hoofs -liceoryx_platform -liceoryx_posh -liceoryx_posh_config -liceoryx_posh_gateway -liceoryx_posh_roudi

SOURCES += \
    apicontroller.cpp \
    camworker.cpp \
    main.cpp \
    vsslogger.cpp \
    workmanager.cpp

HEADERS += \
    vssProtocol.h \
    apicontroller.h \
    camworker.h \
    config.h \
    define.h \
    vsslogger.h \
    workmanager.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
