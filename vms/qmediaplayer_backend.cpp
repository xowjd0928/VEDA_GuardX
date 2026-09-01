#include "qmediaplayer_backend.h"

#include <QMediaPlayer>
#include <QPainter>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

SinkVideoWidget::SinkVideoWidget(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
}

void SinkVideoWidget::set_image(const QImage &image)
{
    m_image = image;
    update();
}

void SinkVideoWidget::set_mosaic_rects(const QVector<QRectF> &rects)
{
    if (m_mosaic == rects)
        return;
    m_mosaic = rects;
    update();
}

void SinkVideoWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    if (m_image.isNull()) {
        painter.fillRect(rect(), Qt::black);
        return;
    }
    // GStreamer 경로와 동일하게 뷰 영역을 꽉 채운다 (박스 좌표가 단순 비례)
    painter.drawImage(rect(), m_image);

    // 얼굴 모자이크: 해당 영역의 실제 프레임 픽셀을 1/8로 줄였다가
    // 보간 없이 되키운다 — 진짜 픽셀레이션 (덧칠 마스크가 아님)
    if (!m_mosaic.isEmpty()) {
        const double sx = double(m_image.width()) / qMax(1, width());
        const double sy = double(m_image.height()) / qMax(1, height());
        for (const QRectF &raw : m_mosaic) {
            const QRectF r = raw.intersected(QRectF(rect()));
            const QRect ir = QRect(qRound(r.x() * sx), qRound(r.y() * sy),
                                   qRound(r.width() * sx), qRound(r.height() * sy))
                                 .intersected(m_image.rect());
            if (ir.width() < 4 || ir.height() < 4)
                continue;
            const QImage blocks =
                m_image.copy(ir).scaled(qMax(2, ir.width() / 8),
                                        qMax(2, ir.height() / 8),
                                        Qt::IgnoreAspectRatio,
                                        Qt::FastTransformation);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.drawImage(r, blocks);
        }
    }
}

QMediaPlayerBackend::QMediaPlayerBackend(QObject *parent) : VideoBackend(parent)
{
    m_player = new QMediaPlayer(this);
    m_sink = new QVideoSink(this);
    m_player->setVideoSink(m_sink);

    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) {
                if (m_widget && frame.isValid())
                    m_widget->set_image(frame.toImage());
            });

    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &text) {
                m_status = QString("재생 오류 — %1").arg(text);
                emit status_changed();
            });
}

QWidget *QMediaPlayerBackend::create_widget(QWidget *parent)
{
    m_widget = new SinkVideoWidget(parent);
    return m_widget;
}

void QMediaPlayerBackend::set_mosaic_rects(const QVector<QRectF> &rects)
{
    if (m_widget)
        m_widget->set_mosaic_rects(rects);
}

void QMediaPlayerBackend::play(const QUrl &url)
{
    m_status.clear();
    emit status_changed();
    m_player->setSource(url);
    m_player->play();
}

void QMediaPlayerBackend::stop()
{
    m_player->stop();
}

qint64 QMediaPlayerBackend::current_frame_pts_ms() const
{
    // QMediaPlayer는 프레임 단위 PTS를 노출하지 않는다 — position()은 근사치.
    return m_player->position();
}
