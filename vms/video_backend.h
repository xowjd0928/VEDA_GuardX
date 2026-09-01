#pragma once

#include <QObject>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

class QImage;
class QUrl;
class QWidget;

/**
 * @brief 영상 표시부 추상 인터페이스
 *
 * ChannelView가 GStreamer(GPU 경로)와 QMediaPlayer(안전망) 중 어느 쪽으로
 * 그리는지 몰라도 되게 분리한다.
 *
 * 이전에는 QGraphicsScene에 아이템을 넣는 방식이었으나, GPU 상주 렌더링을
 * 하려면 QRhiWidget(=위젯)이 필요해 위젯 반환 방식으로 바꿨다.
 */
class VideoBackend : public QObject
{
    Q_OBJECT

public:
    explicit VideoBackend(QObject *parent = nullptr) : QObject(parent) {}

    /** @brief 영상 위젯 생성 (소유권은 parent 위젯) */
    virtual QWidget *create_widget(QWidget *parent) = 0;

    /** @brief 스트림 재생 시작 */
    virtual void play(const QUrl &url) = 0;

    /** @brief 재생 중지 */
    virtual void stop() = 0;

    /**
     * @brief 마지막으로 표시된 프레임의 타임스탬프
     * @return ms 단위 PTS, 알 수 없으면 -1
     */
    virtual qint64 current_frame_pts_ms() const = 0;

    /** @brief 실측 촬영→표시 지연 (ms). 알 수 없으면 -1 */
    virtual qint64 glass_latency_ms() const { return -1; }

    /**
     * @brief 표시 중인 프레임의 카메라 UTC (epoch ms). 알 수 없으면 -1
     *
     * RTCP SR 기준 — ONVIF 메타데이터의 tt:Frame UtcTime과 같은 시계라
     * (2026-08-05 양트랙 캡처로 실증) 박스↔프레임 타임스탬프 매칭의
     * 기준 시각으로 쓴다. 웹 UI가 영상 시각으로 overlayList를 고르는
     * 것과 같은 축.
     */
    virtual qint64 current_frame_utc_ms() const { return -1; }

    /**
     * @brief 화면에 띄울 상태 문구 (정상이면 빈 문자열)
     *
     * 검은 화면만 보여주면 운영자는 "카메라가 죽었나 프로그램이 죽었나"를
     * 알 수 없다. 재접속 중인지 아닌지를 반드시 화면에 알린다.
     */
    virtual QString status_text() const { return QString(); }

    /**
     * @brief 얼굴 모자이크를 실제 영상 픽셀로 그릴 수 있는가
     *
     * CPU에 프레임(QImage)이 있는 경로(ffmpeg/qmediaplayer)만 가능하다.
     * GPU 상주 경로(GStreamer/QRhi)는 픽셀 접근이 없어 false — 그 경우
     * BoxOverlay가 불투명 마스크로 대신 가린다.
     */
    virtual bool supports_pixel_mosaic() const { return false; }

    /** @brief 모자이크 영역 지정 (위젯 좌표) — 지원 백엔드만 구현 */
    virtual void set_mosaic_rects(const QVector<QRectF> &rects) { Q_UNUSED(rects); }

    // ---- direct 경로 (2026-08-04) — 오버레이를 sink가 GPU에서 영상과 합성 ----
    /**
     * @brief 오버레이가 GPU 합성 경로인가
     *
     * true면 ChannelView는 투명 위젯(BoxOverlay) 대신 크롬 전체를 QImage로
     * 그려 set_overlay_image로 넘긴다 — 네이티브 영상 창 위에 Qt 위젯을
     * 얹을 수 없기(airspace) 때문.
     */
    virtual bool gpu_composition() const { return false; }

    /** @brief 합성용 오버레이 이미지 교체 (영상 프레임 비율 캔버스, BGRA) */
    virtual void set_overlay_image(const QImage &image) { Q_UNUSED(image); }

    /** @brief 협상된 영상 해상도 — 오버레이 캔버스 크기의 기준 (모르면 0×0) */
    virtual QSize video_size() const { return QSize(); }

    /**
     * @brief 지금 화면에 영상을 실제로 그리고 있는가 (첫 프레임 이후 ~ 끊김 전)
     *
     * direct 경로에서만 의미가 있다. 네이티브 창은 **버퍼가 올 때만** 그려지고
     * Qt 는 그 영역을 칠하지 않으므로(WA_NoSystemBackground), false 인 동안엔
     * ChannelView 가 창을 숨기고 Qt 로 직접 그려야 한다. 안 그러면 그 자리에
     * 직전에 있던 화면(다른 탭 등)이 그대로 남는다.
     */
    virtual bool has_frame() const { return true; }

    /**
     * @brief 영상 표시를 이만큼 지연시킨다 (ms) — 박스↔영상 정합용
     *
     * 메타데이터는 5Hz 라 임의의 순간에 최신 박스는 평균 100ms 낡아 있다
     * (2026-08-12 실측: utc-e p50 −100, 디코더를 바꿔도 한 자리도 안 움직임).
     * 영상을 그만큼 늦추면 그 나이와 맞는다 — 08-04 이전 구조가 우연히 하던
     * 일이다 (영상 500~700ms 지연 ↔ 박스 300~400ms 나이).
     *
     * ⚠ **외삽(dead reckoning)이 아니다.** 추정으로 박스를 앞당기는 대신
     * 영상을 늦춰 실측끼리 맞춘다 — 추측 오차가 없는 대신 지연을 지불한다.
     * 값은 운영자가 +/- 키로 직접 맞춘다.
     */
    virtual void set_video_delay_ms(int ms) { Q_UNUSED(ms); }

    /**
     * @brief 마지막 프레임을 다시 제시하라고 sink 에 요청한다
     *
     * 탭 전환처럼 창이 숨었다 다시 뜨면 네이티브 창의 픽셀이 낡은 채로 남는다.
     * 다음 버퍼가 오면 덮이지만, 스트림이 느리거나 끊겨 있으면 그 "다음"이
     * 영영 안 온다. GstVideoOverlay 가 바로 이걸 위한 API 를 준다.
     */
    virtual void expose() {}

signals:
    /** @brief 상태 문구가 바뀜 — 오버레이 갱신용 */
    void status_changed();

    /**
     * @brief 스트림 세션을 (재)요청했다 — 이제 첫 프레임을 기다린다
     *
     * 이 순간 SUNAPI `setsynchronizationpoint` 로 키프레임을 강제하면,
     * 다음 I프레임까지 기다리던 검은 화면(GOV 60 @30fps ≈ 최악 2초)이
     * ~0.2초로 줄어든다. 최초 연결·재접속 복구·프로파일 전환을 한 훅으로 덮는다.
     *
     * status_changed 로는 이 시점을 못 고른다 — "연결 중…"과 "재접속 중"이
     * 둘 다 비어있지 않은 문구인데, 후자는 파이프라인을 이미 teardown 한
     * **백오프 대기** 상태라 키프레임을 받을 세션이 없다.
     */
    void session_started();
};

/** @brief QSettings("video_backend") 값에 따라 백엔드를 생성한다 */
VideoBackend *create_video_backend(int channel, QObject *parent = nullptr);

/**
 * @brief RTSP 전송 방식 ("udp" | "tcp") — **기본값이 사는 단 한 곳**
 *
 * 세 백엔드(direct·gstreamer·ffmpeg)가 각자 QSettings 에서 같은 키를 읽으면서
 * 기본값 문자열을 **따로** 들고 있었다. 기본을 바꾸려면 세 파일을 고쳐야 하고,
 * 둘만 고치면 어느 백엔드로 도느냐에 따라 전송이 달라지는 상태가 된다
 * (2026-08-20: video_backend 기본값이 실제로 그렇게 어긋나 있었다).
 *
 * 기본은 **udp** — ARCHITECTURE.md §Transport:
 * "TCP turns loss into an unbounded backlog on any jitter; UDP+drop stays live."
 * 레지스트리 `rtsp_transport` 로 덮어쓸 수 있고(`tcp`), 아는 값이 아니면
 * 경고를 남기고 기본으로 되돌린다.
 *
 * ⚠ udp 는 RTCP 소켓 바인딩이 되어야 지연 HUD·채널 동기가 산다. Tailscale 이
 *   떠 있으면 udpsrc 가 RTCP 포트를 못 잡는다 (LAN_TEST_CHECKLIST §1.1).
 */
QString rtsp_transport();
