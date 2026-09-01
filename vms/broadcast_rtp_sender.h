#pragma once

#include <QObject>
#include <QString>

struct _GstElement;
class QTimer;

/**
 * @brief 방송 미디어 송신 — Opus/RTP/UDP (GStreamer)
 *
 * 기존 PCM/MQTT 송신(`BroadcastController`의 QAudioSource 경로)과 **독립**이다.
 * 마이크 → (노이즈 제거) → opusenc → rtpopuspay → udpsink 파이프라인 하나를
 * 별도로 돌린다. VMS의 영상 GStreamer 파이프라인과도 인스턴스가 분리돼 서로
 * 영향 없음.
 *
 * 제어(START/STOP)는 이 클래스가 아니라 상위(BroadcastController)가 맡는다 —
 * 여기서는 미디어 파이프라인의 생명주기만 담당한다.
 */
class BroadcastRtpSender : public QObject
{
    Q_OBJECT

public:
    explicit BroadcastRtpSender(QObject *parent = nullptr);
    ~BroadcastRtpSender() override;

    /**
     * @brief host:port 로 Opus/RTP 송신 시작. 실패 시 false + error() 발생
     * @param denoise 노이즈 제거(노캔) 사용 여부 — webrtcdsp가 있으면 그것을,
     *                없으면 하이패스+노이즈게이트 폴백을 쓴다.
     * @param agc     자동이득(AGC). 마이크와 스피커가 가까운 현장에서는 루프
     *                이득을 밀어올려 하울링을 앞당기므로 기본은 끔.
     */
    bool start(const QString &host, int port, int bitrate = 64000,
               bool denoise = true, bool agc = false,
               int volume_percent = 100);
    void stop();
    bool active() const { return m_pipeline != nullptr; }

    /** @brief 방송 중에도 노캔을 즉시 켜고 끈다(재시작 불필요). */
    void set_denoise(bool on);

    /**
     * @brief 출력 음량 0~100%. 방송 중에도 즉시 반영된다.
     *
     * 방송 중이 아니면 다음 start() 로 넘어간다. 사이렌에는 영향이 없다 —
     * 사이렌은 RPi C 가 로컬 음원으로 재생하므로 이 파이프라인을 지나지
     * 않는다.
     */
    void set_volume_percent(int percent);

    /** @brief 이번 세션에서 실제로 쓰인 노이즈 제거 방식(사람이 읽는 이름). */
    QString denoise_backend() const { return m_denoise_backend; }

signals:
    void error(const QString &message);
    /** @brief 송출 직전 오디오 레벨(RMS, dBFS). 무음이면 -60 이하 */
    void level_changed(double rms_db);

private:
    /** @brief 마이크 → udpsink 파이프라인 서술 문자열 생성 */
    QString build_description(const QString &host, int port, int bitrate,
                              bool denoise, bool agc, int volume_percent);
    /**
     * @brief 파이프라인 버스를 주기적으로 비운다.
     *
     * gst_bus_add_watch 는 GLib 메인루프가 돌아야 콜백이 뜬다. Qt는
     * Windows에서 GLib 루프를 돌리지 않으므로 여기서는 폴링으로 꺼낸다 —
     * 메인루프 종류와 무관하게 확실히 동작한다.
     *
     * (08-10: 영상 백엔드 3곳도 같은 이유로 폴링으로 전환됐다. 이 주석이
     *  "영상 백엔드가 쓰는 방식"이라며 워치를 가리키던 시절엔, 그 3곳의
     *  ERROR/EOS 처리가 실제로는 한 번도 안 불리고 있었다.)
     */
    void poll_bus();

    _GstElement *m_pipeline = nullptr;
    _GstElement *m_dsp = nullptr;          ///< webrtcdsp (있을 때만) — 런타임 토글용
    _GstElement *m_gate = nullptr;         ///< 폴백 노이즈게이트 — 런타임 토글용
    _GstElement *m_volume = nullptr;       ///< 출력 음량 — 런타임 조절용
    QTimer *m_bus_timer = nullptr;
    QString m_denoise_backend;
};
