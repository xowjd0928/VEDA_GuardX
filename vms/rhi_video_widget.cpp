#include "rhi_video_widget.h"
#include "channel_sync.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>

// 채널 싱크 표시 시계 주기. 8ms(=125Hz)면 60/144Hz 모니터 어느 쪽이든
// 표시 시점 양자화 오차가 프레임 간격보다 훨씬 작다.
static const int SYNC_TICK_MS = 8;

static QShader load_shader(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[RhiVideoWidget] 셰이더 로드 실패:" << path;
        return QShader();
    }
    return QShader::fromSerialized(f.readAll());
}

RhiVideoWidget::RhiVideoWidget(std::shared_ptr<FrameQueue> queue, QWidget *parent)
    : QRhiWidget(parent), m_queue(std::move(queue))
{
    // 영상은 불투명하게 전체를 덮으므로 알파 불필요
    setColorBufferFormat(QRhiWidget::TextureFormat::RGBA8);

    if (ChannelSync::instance()->enabled()) {
        // 싱크 모드에서는 "새 프레임 도착"이 아니라 표시 시계가 그리기를 몰아야
        // 한다. 도착이 없어도 표시할 때가 된 프레임이 있을 수 있기 때문.
        // 단, 표시할 프레임이 실제로 생겼을 때만 그린다 — 그러지 않으면
        // 15fps 스트림을 초당 125번 다시 그려 CPU/GPU를 낭비한다.
        m_tick = new QTimer(this);
        connect(m_tick, &QTimer::timeout, this, [this] {
            if (m_queue->has_due(presentation_deadline()))
                update();
        });
        m_tick->start(SYNC_TICK_MS);
    }
}

RhiVideoWidget::~RhiVideoWidget()
{
    release_frame();
}

void RhiVideoWidget::request_update()
{
    if (!m_tick)  // 싱크 모드는 타이머가 몰기 때문에 중복 호출 불필요
        update();
}

void RhiVideoWidget::initialize(QRhiCommandBuffer *)
{
    if (m_rhi != rhi()) {
        // 백엔드/디바이스가 바뀌면 전부 새로 만든다
        m_pipeline.reset();
        m_srb.reset();
        m_sampler.reset();
        m_tex_y.reset();
        m_tex_uv.reset();
        m_ubuf.reset();
        m_vbuf.reset();
        m_static_uploaded = false;
        m_tex_size = QSize();
        m_rhi = rhi();
        qDebug() << "[RhiVideoWidget] RHI 백엔드:" << m_rhi->backendName();
    }

    if (!m_vbuf) {
        // 화면을 덮는 사각형 (TriangleStrip). uv는 텍스처 원점이 좌상단인
        // 전제 — 상하 반전은 실행 후 화면으로 확인했다.
        static const float verts[] = {
            // x,     y,     u,    v
            -1.0f, -1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 0.0f,
        };
        m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable,
                                      QRhiBuffer::VertexBuffer, sizeof(verts)));
        m_vbuf->create();
        m_static_uploaded = false;
    }

    if (!m_ubuf) {
        // mat4 mvp + mat4 colorMatrix
        m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                      QRhiBuffer::UniformBuffer, 128));
        m_ubuf->create();
    }

    if (!m_sampler) {
        m_sampler.reset(m_rhi->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_sampler->create();
    }

    // 텍스처는 첫 프레임의 해상도를 알아야 만들 수 있다 —
    // 파이프라인 생성도 그때(ensure_textures)까지 미룬다.
}

bool RhiVideoWidget::ensure_textures(const GstVideoInfo &info)
{
    const QSize size(GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info));
    if (size.isEmpty())
        return false;

    if (m_tex_size == size && m_tex_y && m_tex_uv && m_pipeline)
        return true;

    m_tex_size = size;

    m_tex_y.reset(m_rhi->newTexture(QRhiTexture::R8, size));
    if (!m_tex_y->create())
        return false;

    m_tex_uv.reset(m_rhi->newTexture(QRhiTexture::RG8,
                                     QSize(size.width() / 2, size.height() / 2)));
    if (!m_tex_uv->create())
        return false;

    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage
                   | QRhiShaderResourceBinding::FragmentStage, m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            m_tex_y.get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage,
            m_tex_uv.get(), m_sampler.get()),
    });
    if (!m_srb->create())
        return false;

    if (!m_pipeline) {
        const QShader vs = load_shader(QStringLiteral(":/shaders/nv12.vert.qsb"));
        const QShader fs = load_shader(QStringLiteral(":/shaders/nv12.frag.qsb"));
        if (!vs.isValid() || !fs.isValid())
            return false;

        m_pipeline.reset(m_rhi->newGraphicsPipeline());
        m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_pipeline->setShaderStages({
            { QRhiShaderStage::Vertex, vs },
            { QRhiShaderStage::Fragment, fs },
        });

        QRhiVertexInputLayout layout;
        layout.setBindings({ { 4 * sizeof(float) } });
        layout.setAttributes({
            { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
            { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
        });
        m_pipeline->setVertexInputLayout(layout);
        m_pipeline->setShaderResourceBindings(m_srb.get());
        m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        if (!m_pipeline->create())
            return false;
    }

    return true;
}

QMatrix4x4 RhiVideoWidget::color_matrix(const GstVideoInfo &info)
{
    const GstVideoColorimetry &c = GST_VIDEO_INFO_COLORIMETRY(&info);
    const bool full_range = (c.range == GST_VIDEO_COLOR_RANGE_0_255);
    const bool bt709 = (c.matrix == GST_VIDEO_COLOR_MATRIX_BT709);

    // vec4(y, u, v, 1) -> rgb. 마지막 열은 오프셋(레벨 보정 + 크로마 -0.5).
    QMatrix4x4 m;
    if (full_range) {
        if (bt709) {
            m.setRow(0, QVector4D(1.0f, 0.000000f,  1.581000f, -0.790500f));
            m.setRow(1, QVector4D(1.0f, -0.188062f, -0.469967f,  0.329014f));
            m.setRow(2, QVector4D(1.0f, 1.862900f,  0.000000f, -0.931450f));
        } else {
            m.setRow(0, QVector4D(1.0f, 0.000000f,  1.402000f, -0.701000f));
            m.setRow(1, QVector4D(1.0f, -0.344136f, -0.714136f,  0.529136f));
            m.setRow(2, QVector4D(1.0f, 1.772000f,  0.000000f, -0.886000f));
        }
    } else {
        if (bt709) {
            m.setRow(0, QVector4D(1.164383f, 0.000000f,  1.792741f, -0.969436f));
            m.setRow(1, QVector4D(1.164383f, -0.213249f, -0.532909f,  0.300013f));
            m.setRow(2, QVector4D(1.164383f, 2.112402f,  0.000000f, -1.129267f));
        } else {
            m.setRow(0, QVector4D(1.164383f, 0.000000f,  1.596027f, -0.871079f));
            m.setRow(1, QVector4D(1.164383f, -0.391762f, -0.812968f,  0.529299f));
            m.setRow(2, QVector4D(1.164383f, 2.017232f,  0.000000f, -1.081682f));
        }
    }
    m.setRow(3, QVector4D(0.0f, 0.0f, 0.0f, 1.0f));
    return m;
}

qint64 RhiVideoWidget::presentation_deadline() const
{
    // 채널 싱크: "촬영시각 + 목표지연"이 지난 프레임만 표시 대상
    ChannelSync *sync = ChannelSync::instance();
    if (!sync->enabled())
        return -1;
    // 무리 이탈 채널은 정렬을 포기하고 최신 프레임 즉시 — 붙잡아봤자 무리를
    // 따라올 수 없고, 목표를 거기 맞추면 건강한 채널 전부가 느려진다.
    if (sync->outlier(m_sync_channel))
        return -1;
    const qint64 target = sync->target_latency_ms();
    if (target < 0)
        return -1;
    return QDateTime::currentMSecsSinceEpoch() - target;
}

bool RhiVideoWidget::acquire_frame()
{
    GstSample *sample = m_queue->take_for_display(presentation_deadline());
    if (!sample)
        return false;

    GstCaps *caps = gst_sample_get_caps(sample);
    GstVideoInfo info;
    if (!caps || !gst_video_info_from_caps(&info, caps)) {
        gst_sample_unref(sample);
        return false;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstVideoFrame frame;
    if (!buffer || !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return false;
    }

    release_frame();  // 이전 프레임은 이 시점에 해제 (업로드 커밋 완료 후)

    m_held_sample = sample;
    m_held_frame = frame;
    m_frame_mapped = true;
    m_color_matrix = color_matrix(info);
    return true;
}

void RhiVideoWidget::release_frame()
{
    if (m_frame_mapped) {
        gst_video_frame_unmap(&m_held_frame);
        m_frame_mapped = false;
    }
    if (m_held_sample) {
        gst_sample_unref(m_held_sample);
        m_held_sample = nullptr;
    }
}

void RhiVideoWidget::render(QRhiCommandBuffer *cb)
{
    // 렌더 구간 측정 시작. 여기서 재는 것은 **CPU 측 제출 시간**(텍스처 업로드
    // 기술 + 드로 커맨드 인코딩)이지 GPU가 실제로 그린 시간이 아니다 —
    // GPU 완료 시각은 RHI 레벨에서 알 수 없다. 우리가 통제할 수 있는 몫이
    // 여기까지라 이 경계로 잡았다.
    QElapsedTimer render_timer;
    render_timer.start();

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();

    if (!m_static_uploaded) {
        static const float verts[] = {
            -1.0f, -1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 0.0f,
        };
        u->uploadStaticBuffer(m_vbuf.get(), verts);
        m_static_uploaded = true;
    }

    const bool have_frame = acquire_frame();
    bool ready = false;

    if (have_frame) {
        const GstVideoInfo *info = &m_held_frame.info;
        if (ensure_textures(*info)) {
            // Y (R8) 와 UV (RG8) 를 각자의 stride 그대로 업로드한다.
            // 데이터는 복사하지 않고, 다음 render()까지 map을 유지해 수명을 보장.
            QRhiTextureSubresourceUploadDescription desc_y(
                GST_VIDEO_FRAME_PLANE_DATA(&m_held_frame, 0),
                GST_VIDEO_FRAME_PLANE_STRIDE(&m_held_frame, 0)
                    * GST_VIDEO_FRAME_HEIGHT(&m_held_frame));
            desc_y.setDataStride(GST_VIDEO_FRAME_PLANE_STRIDE(&m_held_frame, 0));
            u->uploadTexture(m_tex_y.get(),
                             QRhiTextureUploadDescription({ { 0, 0, desc_y } }));

            QRhiTextureSubresourceUploadDescription desc_uv(
                GST_VIDEO_FRAME_PLANE_DATA(&m_held_frame, 1),
                GST_VIDEO_FRAME_PLANE_STRIDE(&m_held_frame, 1)
                    * (GST_VIDEO_FRAME_HEIGHT(&m_held_frame) / 2));
            desc_uv.setDataStride(GST_VIDEO_FRAME_PLANE_STRIDE(&m_held_frame, 1));
            u->uploadTexture(m_tex_uv.get(),
                             QRhiTextureUploadDescription({ { 0, 0, desc_uv } }));

            GstBuffer *buf = gst_sample_get_buffer(m_held_sample);
            static GstCaps *ntp_caps = gst_caps_new_empty_simple("timestamp/x-ntp");
            // 매 프레임 새로 정한다. 예전엔 메타가 없는 프레임에서 직전 값을
            // 그대로 들고 있어, 그 프레임의 TOTAL 이 남의 촬영시각 기준으로
            // 계산됐다 (RTCP SR 이 끊긴 구간에서 TOTAL 이 슬금슬금 커지는 원인).
            m_displayed_capture_ms = -1;
            if (GstReferenceTimestampMeta *meta =
                    gst_buffer_get_reference_timestamp_meta(buf, ntp_caps)) {
                m_displayed_capture_ms = qint64(meta->timestamp / GST_MSECOND)
                                         - Q_INT64_C(2208988800000);
            }
            m_displayed_pts = GST_BUFFER_PTS_IS_VALID(buf)
                                  ? qint64(GST_BUFFER_PTS(buf)) : -1;
            ready = true;
        }
    } else if (m_pipeline && m_tex_y) {
        ready = true;  // 새 프레임이 없으면 직전 텍스처를 그대로 다시 그린다
    }

    if (ready) {
        char data[128];
        QMatrix4x4 mvp = m_rhi->clipSpaceCorrMatrix();
        memcpy(data, mvp.constData(), 64);
        memcpy(data + 64, m_color_matrix.constData(), 64);
        u->updateDynamicBuffer(m_ubuf.get(), 0, 128, data);
    }

    const QColor clear = QColor(0, 0, 0);
    cb->beginPass(renderTarget(), clear, { 1.0f, 0 }, u);

    if (ready && m_pipeline) {
        const QSize out = renderTarget()->pixelSize();
        cb->setGraphicsPipeline(m_pipeline.get());
        cb->setViewport({ 0, 0, float(out.width()), float(out.height()) });
        cb->setShaderResources(m_srb.get());
        const QRhiCommandBuffer::VertexInput vb(m_vbuf.get(), 0);
        cb->setVertexInput(0, 1, &vb);
        cb->draw(4);
    }

    cb->endPass();

    // 새 프레임을 올린 경우에만 기록한다. 재도색(직전 텍스처 다시 그리기)은
    // 업로드가 없어 훨씬 짧고, 섞으면 렌더 구간이 실제보다 좋아 보인다.
    if (have_frame) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_queue->stats().add(PipelineStats::Render, render_timer.elapsed());

        // TOTAL 도 net 과 **같은** 시계 오프셋으로 보정한다. 기준이 다르면
        // 구간 합과 총합이 안 맞아 둘 다 못 믿게 된다. 오프셋을 아직 못 잡았으면
        // (RTCP SR 미수신) 넣지 않는다 — 원시값은 시계차가 섞인 쓰레기다.
        if (m_displayed_capture_ms > 0 && m_queue->stats().offset_known()) {
            m_queue->stats().add(
                PipelineStats::Total,
                m_queue->stats().apply_offset(now - m_displayed_capture_ms));
        }

        // 추적 중인 프레임이면 여기가 마지막 단계다
        m_queue->trace().mark_rendered(m_displayed_pts, now);
    }

    // 완성된 추적 리포트가 있으면 찍는다. 큐에서 폐기된 프레임의 리포트도
    // 여기로 나온다 — 폐기는 큐 뮤텍스 안에서 일어나므로 그 자리에서 못 찍는다.
    if (const QString trace = m_queue->trace().take_report(); !trace.isEmpty())
        qInfo().noquote() << trace;
}

void RhiVideoWidget::releaseResources()
{
    release_frame();
    m_pipeline.reset();
    m_srb.reset();
    m_tex_uv.reset();
    m_tex_y.reset();
    m_sampler.reset();
    m_ubuf.reset();
    m_vbuf.reset();
    m_static_uploaded = false;
    m_tex_size = QSize();
    m_rhi = nullptr;
}
