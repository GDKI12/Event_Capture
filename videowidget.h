#ifndef VIDEOWIDGET_H
#define VIDEOWIDGET_H

#include <QImage>
#include <QWidget>

class VideoWidget : public QWidget
{
public:
    explicit VideoWidget(QWidget* parent = nullptr);
    void setFrame(const QImage& image);
    void clear(const QString& message);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage frame;
    QString placeholder;
};

#endif // VIDEOWIDGET_H
