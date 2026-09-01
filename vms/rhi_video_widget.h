#pragma once

#include <QMatrix4x4>
#include <QRhiWidget>
#include <rhi/qrhi.h>   // QRhiBuffer/Texture/Pipeline — QRhiWidget이 끌어오지 않는다

#include <memory>

#include <gst/gst.h>
#include <gst/video/video.h>

#include "frame_queue.h"

class QTimer;

/**
 * @brief NV12 프레임을 GPU에서 직접 그리는 영상 위젯
 *
 * 이전 경로는 GPU->CPU->GPU 왕복이었다:
 *   D3D 디코더(GPU) -> videoconvert(CPU 다운로드 + BGRA 변환)
 *   -> QPainter::drawImage(CPU 스케일) -> QGraphicsView 합성(CPU) -> 다시 GPU
 *
 * 여기서는 디코더가 낸 NV12를 그대로 두 장의 텍스처(Y=R8, UV=RG8)로 올리고,
 * 색공간 변환과 스케일링을 프래그먼트 셰이더에서 처리한다. CPU가 픽셀을
 * 만지는 구간이 사라져 채널 수가 늘어도 CPU가 병목이 되지 않는다.
 */
class RhiVideoWidget : public QRhiWidget
{
    Q_OBJECT

public:
    explicit RhiVideoWidget(std::shared_ptr<FrameQueue> queue,
                            QWidget *parent = nullptr);
    ~RhiVideoWidget() override;

    /** @brief 정렬 이탈 판정용 채널 번호 (ChannelSync::outlier 조회 키) */
    void set_sync_channel(int channel) { m_sync_channel = channel; }

    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;

    /** @brief 새 프레임 도착 알림 (UI 스레드에서 호출) */
    void request_update();

    /** @brief 마지막으로 그린 프레임의 촬영 시각 (epoch ms, 모르면 -1) */
    qint64 displayed_capture_ms() const { return m_displayed_capture_ms; }

private:
    /** @brief 지금 표시해도 되는 촬영시각 상한 (싱크 미사용이면 -1) */
    qint64 presentation_deadline() const;

    /** @brief 표시 대상 프레임을 큐에서 꺼내 map (이전 프레임은 unmap) */
    bool acquire_frame();
    void release_frame();

    /** @brief caps 변경 시 텍스처 재생성 */
    bool ensure_textures(const GstVideoInfo &info);

    /** @brief caps의 colorimetry로 YUV->RGB 행렬 구성 */
    static QMatrix4x4 color_matrix(const GstVideoInfo &info);

    std::shared_ptr<FrameQueue> m_queue;
    int m_sync_channel = -1;

    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_tex_y;
    std::unique_ptr<QRhiTexture> m_tex_uv;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;

    bool m_static_uploaded = false;
    QSize m_tex_size;

    // 현재 map 상태인 프레임 — QRhi가 업로드를 커밋할 때까지 살아 있어야 하므로
    // 다음 render() 시작 시점에 해제한다 (복사 없이 안전하게)
    GstSample *m_held_sample = nullptr;
    GstVideoFrame m_held_frame;
    bool m_frame_mapped = false;

    QMatrix4x4 m_color_matrix;
    qint64 m_displayed_capture_ms = -1;
    qint64 m_displayed_pts = -1;   ///< 지금 올린 프레임의 PTS (추적 짝맞춤용)
    QTimer *m_tick = nullptr;  ///< 채널 싱크 사용 시 표시 시계
};
