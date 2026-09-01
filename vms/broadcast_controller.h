#pragma once

#include <QByteArray>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QTimer>

#include <memory>

class BroadcastRtpSender;

/**
 * @brief VMS 마이크 → Opus/RTP/UDP 실시간 방송
 *
 * **2026-08-10: MQTT/PCM 경로를 제거하고 RTP 전용으로 확정했다** (RPi C 담당자
 * 합의). 그전에는 `broadcast/transport` 로 두 경로를 고를 수 있었고 기본이 RTP,
 * MQTT 가 기존 정본이었다. 없앤 이유:
 *  - RPi C 수신부가 RTP(독립 GStreamer)로 옮겨가면서 MQTT 오디오 구독이 불필요해짐
 *  - MQTT 경로에만 있던 결함(발행 실패 시 QIODevice 자기 신호 안에서 소유자
 *    QAudioSource 를 파괴하는 use-after-free)이 원인째 사라진다
 *  - 노캔·레벨미터가 GStreamer 단(RTP)에만 있어, MQTT 모드에선 눌러도 아무
 *    일이 없는 버튼이 화면에 남아 있었다
 *
 * 노캔은 **항상 켠다** — 현장 마이크는 팬·공조 소음이 항상 깔린다. UI 토글을
 * 없애고 기본값으로 굳혔다(비상 탈출구로 레지스트리 `broadcast/denoise` 는 남김).
 */
class BroadcastController : public QObject
{
    Q_OBJECT

public:
    explicit BroadcastController(QObject *parent = nullptr);
    ~BroadcastController() override;

    bool active() const { return m_active; }

    /**
     * @brief 노캔(노이즈 제거) 적용 여부 — **기본 켬**
     *
     * UI 토글은 08-10에 제거했다. 현장에서 노캔이 오히려 문제가 되는 마이크가
     * 나오면 레지스트리 `broadcast/denoise=false` 로 끌 수 있게만 남겨 둔다.
     */
    bool denoise() const;

    /**
     * @brief 방송 출력 음량 0~100 % (기본 70)
     *
     * 이 VMS 에 저장된다(레지스트리 broadcast/volume). 방송권을 가진 VMS 의
     * 값만 실제로 들리는데, 그건 규칙이 아니라 구조다 — 음량은 송출 측
     * 파이프라인에서 곱해지므로 방송하지 않는 VMS 의 슬라이더는 아무
     * 소리에도 닿지 않는다.
     *
     * **사이렌과는 무관하다.** 사이렌은 RPi C 가 로컬 음원으로 재생하므로
     * 이 값을 0 으로 내려도 화재 경보 소리는 그대로다.
     */
    int volume_percent() const;

    /**
     * @brief 다른 VMS 가 지금 방송 중인가 (RPi C 가 retained 로 알려준 값)
     *
     * 소유자를 아는 것은 스피커를 실제로 쥔 RPi C 뿐이다. VMS 끼리 서로
     * 알리게 하면 죽은 VMS 의 마지막 주장이 영원히 남는다.
     */
    bool other_broadcasting() const;

    /** @brief 현재 방송 중인 VMS 의 client id. 없으면 빈 문자열 */
    QString current_owner() const { return m_remote_owner; }

    /**
     * @brief RTP 방송 목적지 호스트를 확정해서 돌려준다(필요하면 저장까지).
     *
     * 비어 있으면 기본값(RPi C)을 채우고, 예전 버그로 브로커 주소가 저장된
     * PC는 한 번만 교정한다.
     *
     * **목적지를 정하는 코드는 여기 한 곳뿐이어야 한다.** 예전엔 같은 로직이
     * 컨트롤러와 방송 카드에 복사돼 있어서 잘못된 값이 두 경로로 퍼졌다.
     */
    static QString resolve_rtp_host(QSettings &s);

public slots:
    /**
     * @brief 방송 시작. 다른 VMS 가 쥐고 있으면 RPi C 가 거절한다.
     *
     * 거절되면 takeover_required() 가 나온다 — 화면이 확인창을 띄우고,
     * 운영자가 고르면 start_takeover() 로 다시 온다. 여기서 조용히
     * 인수하지 않는 것이 요점이다: 남의 방송을 예고 없이 끊으면 안 된다.
     */
    void start();

    /** @brief 진행 중인 다른 방송을 끊고 방송권을 가져온다 */
    void start_takeover();
    /** @brief 음량 저장 + 방송 중이면 즉시 반영 */
    void set_volume_percent(int percent);
    void stop();

signals:
    void active_changed(bool active);
    void volume_changed(int percent);

    /**
     * @brief 다른 VMS 가 방송 중이라 시작하지 못했다 — 확인창을 띄울 것
     * @param owner 현재 방송 중인 VMS (사람이 읽는 표시용)
     */
    void takeover_required(const QString &owner);

    /** @brief 다른 VMS 의 방송 점유 상태가 바뀌었다 (버튼 문구 갱신용) */
    void remote_state_changed();
    void status_changed(const QString &message, bool error);
    /** @brief 송출 오디오 레벨(RMS dBFS) — 마이크가 잡히는지 눈으로 확인용 */
    void level_changed(double rms_db);

private:
    void fail(const QString &message);

    /** @brief RPi C 가 알려주는 점유 상태(retained) 수신 */
    void on_remote_state(const QByteArray &payload);

    /**
     * @brief 브로커가 끊긴 채로 만료 시간이 지났는지 본다
     *
     * RPi C 는 KEEPALIVE 가 끊기면 5초 뒤 방송을 접지만, 그 사실을 알리는
     * 메시지도 같은 끊긴 링크로는 못 온다. 그래서 VMS 도 스스로 센다 —
     * 안 그러면 소리는 안 나가는데 화면만 "방송 중"으로 남는다.
     */
    void check_link_alive();

    /** @brief 방송을 우리 쪽에서 접는다(사유 문구 포함, STOP 은 보내지 않음) */
    void abort_local(const QString &reason);

    /** @brief READY ACK 를 받았을 때 실제 RTP 송출을 시작한다 */
    void on_ready(const QByteArray &payload);
    /** @brief READY 를 제한시간 내 못 받았을 때 — 송출하지 않고 취소한다 */
    void on_ready_timeout();
    /** @brief 대기 중이던 준비 절차를 접는다(성공·실패·중단 공통 뒷정리) */
    void cancel_pending();

    /**
     * @brief 방송 시작/종료를 RPi C 에 알린다(guardx/broadcast/rpic/command).
     *
     * RTP 전용으로 바꾸면서 이 발행이 사라졌었는데, RPi C 가 스피커를 방송과
     * 화재 사이렌 중 하나에만 내주려면 "지금 방송 중인가"를 알아야 한다.
     * 미디어는 여전히 RTP/UDP 로만 흐르고, 이건 제어 신호 하나뿐이다.
     */
    void publish_state(bool start, bool keepalive = false,
                       bool takeover = false);

    std::unique_ptr<BroadcastRtpSender> m_rtp;
    bool m_active = false;
    /** 세션 식별자 — RPi C 가 늦게 도착한 STOP 을 걸러내는 데 쓴다 */
    quint32 m_session = 0;

    /**
     * 방송 중 START/KEEPALIVE 재발행 타이머.
     *
     * VMS 가 비정상 종료해 STOP 이 유실돼도 RPi C 가 만료를 보고 스스로 방송을
     * 접고 스피커를 반납하게 만든다 — 그래야 그 뒤에 나는 화재 사이렌이 막히지
     * 않는다.
     */
    QTimer *m_keepalive = nullptr;
    /** READY ACK 대기 시한 */
    QTimer *m_ready_wait = nullptr;
    /** READY 를 기다리는 중인가 — 이때는 아직 RTP 를 쏘지 않는다 */
    bool m_awaiting_ready = false;
    bool m_ready_subscribed = false;
    bool m_state_subscribed = false;

    /// RPi C 가 알려준 현재 점유 상태. 우리 것도 포함된다 —
    /// other_broadcasting() 이 owner 비교로 걸러낸다.
    bool m_remote_active = false;
    QString m_remote_owner;
    quint32 m_remote_session = 0;

    /// 이번 시작 시도가 인수인가. START 페이로드에만 실린다.
    bool m_takeover = false;

    /// 브로커가 마지막으로 살아 있던 시각(방송 중에만 의미 있음).
    qint64 m_link_ok_ms = 0;
    QTimer *m_link_watch = nullptr;

    /** start() 가 정해둔 송출 파라미터. READY 가 와야 실제로 쓴다. */
    QString m_host;
    int m_port = 0;
    int m_bitrate = 0;
    bool m_nc = false;
    bool m_agc = false;
};
