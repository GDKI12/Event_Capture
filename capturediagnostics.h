#ifndef CAPTUREDIAGNOSTICS_H
#define CAPTUREDIAGNOSTICS_H

// Qt Creator Application Output / stderr only; never routed to the UI log widget.
#ifndef QT_NO_DEBUG
#include <QDateTime>
#include <QDebug>
#define CAPTURE_TRACE(message) do { \
    qDebug().noquote() << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz") \
                      << "[CAPTURE]" << message; \
} while (false)
#else
#define CAPTURE_TRACE(message) do {} while (false)
#endif

#endif
