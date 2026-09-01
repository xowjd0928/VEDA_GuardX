#pragma once

#include "video_backend.h"

#include <QImage>
#include <QWidget>

class QMediaPlayer;
class QVideoSink;

/**
 * @brief QVideoSink 프레임을 QPainter로 그리는 단순 위젯 (안전망 경로 전용)
 *
 * QVideoWidget은 네이티브 서피스를 써서 위에 올린 자식 위젯(감지 박스
 * 오버레이)이 가려진다. 그래서 프레임을 직접 받아 그린다.
 */
class SinkVideoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SinkVideoWidget(QWidget *parent = nullptr);
    void set_image(const QImage &image);

    /** @brief 모자이크 영역(위젯 좌표) — 프레임 픽셀을 뭉개서 그린다 */
    void set_mosaic_rects(const QVector<QRectF> &rects);

protected:
    void paintEvent(QPaintEvent *ev) override;

private:
    QImage m_image;
    QVector<QRectF> m_mosaic;
};

/**
 * @brief 기존 QMediaPlayer(FFmpeg) 경로 — 비교 기준선 + 데모 안전망
 *
 * 내부 버퍼링을 끌 수 없어 지연이 1초 이상이다. GStreamer가 없는 환경에서만
 * 쓰인다.
 */
class QMediaPlayerBackend : public VideoBackend
{
    Q_OBJECT

public:
    explicit QMediaPlayerBackend(QObject *parent = nullptr);

    QWidget *create_widget(QWidget *parent) override;
    void play(const QUrl &url) override;
    void stop() override;
    qint64 current_frame_pts_ms() const override;
    QString status_text() const override { return m_status; }
    bool supports_pixel_mosaic() const override { return true; }
    void set_mosaic_rects(const QVector<QRectF> &rects) override;

private:
    SinkVideoWidget *m_widget = nullptr;  ///< 소유권은 부모 위젯
    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_sink = nullptr;
    QString m_status;
};
