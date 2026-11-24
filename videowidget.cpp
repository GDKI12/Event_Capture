#include "videowidget.h"

#include <QPainter>

VideoWidget::VideoWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void VideoWidget::setFrame(const QImage& image)
{
    frame = image;
    update();
}

void VideoWidget::clear(const QString& message)
{
    frame = QImage();
    placeholder = message;
    update();
}

void VideoWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#15191d"));
    const QRect viewport = rect().adjusted(1, 1, -1, -1);
    if (!frame.isNull()) {
        const QSize scaled = frame.size().scaled(viewport.size(), Qt::KeepAspectRatio);
        const QRect target(QPoint(viewport.center().x() - scaled.width() / 2,
                                  viewport.center().y() - scaled.height() / 2), scaled);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, frame);
    } else {
        painter.setPen(QColor("#c8cdd2"));
        painter.drawText(viewport.adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, placeholder);
    }
    painter.setPen(QColor("#315578"));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}
