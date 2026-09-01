#include "broadcast_controller.h"

#include "auth.h"
#include "broadcast_pipeline.h"
#include "broadcast_protocol.h"
#include "broadcast_rtp_sender.h"
#include "credentials.h"
#include "mqtt_link.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QRandomGenerator>   // 순서가 어긋난 건 의도 - main 병합 충돌을 피한다

// 2026-08-10: MQTT/PCM 방송 경로를 제거하고 RTP 전용으로 확정했다(§헤더).
// 마이크 캡처와 패킷화는 전부 GStreamer 파이프라인(BroadcastRtpSender)이 한다 —
// QAudioSource/QIODevice, PCM 패킷 헤더 조립(put_u16/32le·cstring)은 그때 사라졌다.
//
// 2026-08-11: command 발행만 되살렸다(QJson*·QDateTime·QRandomGenerator·MqttLink).
// 미디어가 아니라 제어 신호다. RPi C 의 RTP 수신기가 상시 실행이라 방송이 없는
// 동안에도 스피커를 물고 있었고, 그 사이 화재 사이렌이 장치를 못 열어 통째로
// 누락됐다. 이제 RPi C 가 START 에 수신기를 띄우고 STOP 에 내려 장치를 비운다.
static constexpr int BROADCAST_CMD_QOS = 1;

namespace {

/** @brief RPi B 가 retained 로 알려주는 노드 주소 (핸드오프 §3) */
const QString ENDPOINTS_TOPIC = QStringLiteral("guardx/db/rpib/endpoints");

/**
 * @brief 이번 실행에서 받은 RTP 목적지. 못 받았으면 빈 문자열
 *
 * MqttLink 콜백은 GUI 스레드라 락이 필요 없다(코드맵 F0-3 — 앱 코드가 GUI
 * 스레드 밖에서 도는 곳은 영상 콜백 3개뿐이다).
 */
QString g_endpoint_host;

/** @brief 구독은 한 번만 — 컨트롤러가 여러 번 만들어져도 중복되지 않게 */
bool g_endpoints_subscribed = false;

} // namespace

BroadcastController::BroadcastController(QObject *parent) : QObject(parent)
{
    // 점유 상태는 **버튼을 누르기 전에** 알아야 한다 — ON 을 눌러본 뒤에야
    // "다른 VMS 가 방송 중"을 알게 되면 확인창이 한 박자 늦게 뜬다.
    // retained 라 구독하는 순간 현재 소유자가 온다.
    if (!m_state_subscribed) {
        m_state_subscribed = true;
        MqttLink::instance()->subscribe(
            QString::fromLatin1(GUARDX_BROADCAST_STATE_TOPIC),
            [this](const QByteArray &p) { on_remote_state(p); },
            BROADCAST_CMD_QOS);
    }

    if (g_endpoints_subscribed)
        return;
    g_endpoints_subscribed = true;

    // retained 라 구독하는 순간 현재 값이 온다. 이후 주소가 바뀌면 다시 온다.
    MqttLink::instance()->subscribe(ENDPOINTS_TOPIC, [](const QByteArray &payload) {
        const QJsonObject o = QJsonDocument::fromJson(payload).object();
        const QString host = o.value("rpic_rtp_host").toString();
        if (host.isEmpty()) {
            // 빈 값으로 캐시를 지우지 않는다. 서버는 "표가 비면 발행하지
            // 않는다"고 약속했지만, 그 약속이 깨져도 방송이 죽으면 안 된다.
            qWarning() << "[Broadcast] endpoints 에 rpic_rtp_host 가 없다 — 무시";
            return;
        }

        const bool changed = (host != g_endpoint_host);
        g_endpoint_host = host;

        QSettings s("GuardX", "VMS");
        if (s.value("broadcast/rtp_host").toString() != host) {
            // 캐시 갱신은 조용히 하지 않는다 — 방송이 어디로 나가는지가
            // 바뀌는 일이고, 손으로 넣어둔 값을 덮는 경우도 있다.
            qInfo().noquote()
                << QString("[Broadcast] RTP 목적지 갱신 %1 → %2 (updated_at %3)")
                       .arg(s.value("broadcast/rtp_host").toString(), host,
                            o.value("updated_at").toString("-"));
            s.setValue("broadcast/rtp_host", host);
        } else if (changed) {
            qInfo().noquote() << QString("[Broadcast] RTP 목적지 확인: %1").arg(host);
        }
    }, 1);
}

BroadcastController::~BroadcastController()
{
    stop();
}

bool BroadcastController::denoise() const
{
    // 기본 켬 — 현장 마이크는 팬·공조 소음이 항상 깔린다. UI 토글은 08-10에
    // 없앴고, 이 키는 현장에서 노캔이 오히려 방해가 될 때의 탈출구로만 남긴다.
    return QSettings("GuardX", "VMS").value("broadcast/denoise", true).toBool();
}

int BroadcastController::volume_percent() const
{
    const int v = QSettings("GuardX", "VMS")
                      .value("broadcast/volume",
                             guardx::broadcast::kVolumeDefaultPercent)
                      .toInt();
    // 손으로 편집된 레지스트리 값도 그대로 파이프라인에 들어가므로 여기서
    // 한 번 조인다. 범위를 벗어난 값은 gst 가 조용히 거절해 음량이 안
    // 먹는 형태로만 드러난다.
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

void BroadcastController::set_volume_percent(int percent)
{
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    if (percent == volume_percent())
        return;
    QSettings("GuardX", "VMS").setValue("broadcast/volume", percent);
    // 방송 중이면 즉시 반영한다 — 슬라이더를 움직였는데 다음 방송부터
    // 적용되면 조절이 아니라 설정이다.
    if (m_rtp)
        m_rtp->set_volume_percent(percent);
    emit volume_changed(percent);
}

QString BroadcastController::resolve_rtp_host(QSettings &s)
{
    // ── 3단 폴백 (핸드오프 §3 · 로드맵 0d) ───────────────────────────────
    //  ① 이번 실행에서 받은 endpoints 값   — DB 가 정본이다
    //  ② 레지스트리 캐시                    — 브로커가 죽어 있어도 방송은 나가야 한다
    //  ③ 컴파일 상수                        — 최후. 예전 동작과 같다
    //
    // ⚠ ①이 ②를 덮는다. 손으로 `broadcast/rtp_host` 를 넣어 두었더라도 DB 값이
    //   오면 그쪽이 이긴다 — 주소를 DB 로 옮긴 이유가 "IP 가 바뀌면 양쪽 재빌드"를
    //   없애는 것이었기 때문이다. 되돌릴 탈출구는 `broadcast/rtp_host_pin=1`.
    if (!g_endpoint_host.isEmpty()
        && !s.value("broadcast/rtp_host_pin", false).toBool()) {
        return g_endpoint_host;
    }

    const QString stored = s.value("broadcast/rtp_host").toString();
    const QString correct = QString::fromLatin1(GUARDX_BROADCAST_RTP_HOST);

    // (1) 한 번도 안 정한 PC — "아무것도 안 만져도 붙는다"를 만족시키려면
    //     버튼을 눌러본 적이 없어도 여기서 채워져야 한다.
    if (stored.isEmpty()) {
        s.setValue("broadcast/rtp_host", correct);
        return correct;
    }

    // (2) 예전 버그로 잘못 저장된 값 교정.
    //     예전 코드는 목적지를 브로커 주소(RPi B)로 채웠다. 그 PC는 코드를
    //     고쳐도 저장된 값이 그대로라 계속 RPi B로 쏜다. 저장된 값이 정확히
    //     브로커 주소일 때만, 한 번만 바로잡는다. 플래그를 남기므로 사용자가
    //     나중에 일부러 브로커 주소를 지정하면 그건 건드리지 않는다.
    if (!s.value("broadcast/rtp_host_migrated", false).toBool()) {
        s.setValue("broadcast/rtp_host_migrated", true);
        if (stored == Credentials::mqtt_host() && stored != correct) {
            qWarning("[broadcast] 방송 목적지가 MQTT 브로커(%s)로 잘못 저장돼 "
                     "있어 %s(RPi C)로 교정합니다.",
                     qPrintable(stored), qPrintable(correct));
            s.setValue("broadcast/rtp_host", correct);
            return correct;
        }
    }
    return stored;
}

bool BroadcastController::other_broadcasting() const
{
    if (!m_remote_active)
        return false;
    // 소유자가 우리면 "다른 VMS"가 아니다. client id 는 브로커가 유일성을
    // 보장하므로 이름만으로 충분하다(같은 id 로 둘이 붙으면 브로커가
    // 서로를 끊어낸다).
    return m_remote_owner != MqttLink::instance()->client_id();
}

void BroadcastController::on_remote_state(const QByteArray &payload)
{
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    const bool active = o.value("active").toBool();
    const QString owner = o.value("owner").toString();
    const quint32 session = quint32(o.value("session_id").toDouble());
    const QString reason = o.value("reason").toString();

    m_remote_active = active;
    m_remote_owner = active ? owner : QString();
    m_remote_session = active ? session : 0;
    emit remote_state_changed();

    // 우리 세션이 끝났다는 통보인가. 세션이 다르면 남의 이야기다 —
    // 진행 중인 우리 방송을 남의 종료 소식으로 끊으면 안 된다.
    if (active || session != m_session || m_session == 0)
        return;
    if (!m_active && !m_awaiting_ready)
        return;   // 우리가 스스로 끈 것 — 이미 정리됐다

    if (reason == QLatin1String("taken_over"))
        abort_local("The broadcast ended: another administrator VMS "
                    "took over the broadcast right");
    else if (reason == QLatin1String("timeout"))
        abort_local("The broadcast ended because the connection dropped");
    else if (reason == QLatin1String("fire"))
        abort_local("The broadcast ended: a fire alarm took the speaker");
    else if (reason == QLatin1String("error"))
        abort_local("The broadcast ended: the RPi C receiver failed");
}

void BroadcastController::abort_local(const QString &reason)
{
    // STOP 을 보내지 않는다. 이미 RPi C 가 끝낸 방송이고, 늦은 STOP 은
    // 그 사이 시작된 **다른 VMS 의** 방송을 끊을 수 있다(세션 대조로
    // 걸리긴 하지만 보내지 않는 편이 확실하다).
    if (m_keepalive)
        m_keepalive->stop();
    if (m_link_watch)
        m_link_watch->stop();
    cancel_pending();
    if (m_rtp)
        m_rtp->stop();   // 마이크를 즉시 접는다

    m_session = 0;
    if (m_active) {
        m_active = false;
        emit level_changed(-100.0);
        emit active_changed(false);
    }
    // 자동으로 다시 가져오지 않는다 — 다시 방송하려면 운영자가 ON 을
    // 눌러야 한다. 자동 복귀는 두 VMS 가 서로 뺏는 싸움이 된다.
    emit status_changed(reason, true);
}

void BroadcastController::check_link_alive()
{
    if (!m_active && !m_awaiting_ready)
        return;
    if (MqttLink::instance()->online()) {
        m_link_ok_ms = QDateTime::currentMSecsSinceEpoch();
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() - m_link_ok_ms
        < GUARDX_BROADCAST_TIMEOUT_MS)
        return;

    // RPi C 는 이미 방송을 접었다(같은 만료 시간을 쓴다). 그 사실을
    // 알리는 메시지도 끊긴 링크로는 못 오므로 여기서 스스로 접는다.
    abort_local("The broadcast ended because the connection dropped");
}

void BroadcastController::start_takeover()
{
    m_takeover = true;
    start();
}

void BroadcastController::start()
{
    if (m_active)
        return;

    // 권한 백스톱 (§5) — 버튼 잠금은 표면이다. 방송은 현장 스피커로 소리가
    // 나가는 조작이라 특히 실수로 나가면 안 된다.
    if (!Auth::can(Auth::Action::Broadcast)) {
        const QString why = Auth::deny_reason(Auth::Action::Broadcast);
        qWarning().noquote() << "[Broadcast] 권한 없는 방송 시작 차단 —" << why;
        emit status_changed(why, true);
        return;
    }

    QSettings s("GuardX", "VMS");
    const QString host = resolve_rtp_host(s);
    const int port = s.value("broadcast/rtp_port",
                             int(GUARDX_BROADCAST_RTP_PORT)).toInt();
    if (host.isEmpty()) {
        emit status_changed(
            "The RTP destination IP (broadcast/rtp_host) is not configured.",
            true);
        return;
    }
    if (!m_rtp) {
        m_rtp = std::make_unique<BroadcastRtpSender>(this);
        // fail() -> stop() -> m_rtp->stop() 은 파이프라인만 뜯고 m_rtp 객체는
        // 살려두므로, 신호를 쏜 객체가 파괴되지 않는다(옛 MQTT 경로와 다른 점).
        // BroadcastRtpSender::poll_bus 도 emit 전에 msg/bus 를 정리하고 즉시
        // 반환하므로 여기서 동기 처리해도 안전하다.
        connect(m_rtp.get(), &BroadcastRtpSender::error, this,
                [this](const QString &m) { fail(m); });
        connect(m_rtp.get(), &BroadcastRtpSender::level_changed,
                this, &BroadcastController::level_changed);
    }
    // 비트레이트는 레지스트리로 조정 가능(저대역폭 현장은 낮추면 된다).
    const int bitrate = s.value("broadcast/opus_bitrate",
                                int(GUARDX_BROADCAST_OPUS_BITRATE)).toInt();
    const bool nc = denoise();
    // AGC 기본 끔 — 마이크·스피커가 가까우면 하울링을 앞당긴다.
    const bool agc = s.value("broadcast/agc", false).toBool();
    // RPi C 수신기는 방송 중에만 뜬다(평상시 스피커를 비워 화재 사이렌이
    // 언제든 장치를 열 수 있게 하려고). 그래서 바로 쏘면 앞부분이 잘린다 —
    // START 를 보내고 READY ACK 를 받은 뒤에 송출한다.
    m_host = host;
    m_port = port;
    m_bitrate = bitrate;
    m_nc = nc;
    m_agc = agc;

    if (!m_ready_subscribed) {
        m_ready_subscribed = true;
        MqttLink::instance()->subscribe(
            QString::fromLatin1(GUARDX_BROADCAST_READY_TOPIC),
            [this](const QByteArray &p) { on_ready(p); }, BROADCAST_CMD_QOS);
    }
    if (!m_link_watch) {
        m_link_watch = new QTimer(this);
        connect(m_link_watch, &QTimer::timeout, this,
                &BroadcastController::check_link_alive);
    }
    m_link_ok_ms = QDateTime::currentMSecsSinceEpoch();
    m_link_watch->start(int(GUARDX_BROADCAST_KEEPALIVE_MS));
    if (!m_ready_wait) {
        m_ready_wait = new QTimer(this);
        m_ready_wait->setSingleShot(true);
        connect(m_ready_wait, &QTimer::timeout,
                this, &BroadcastController::on_ready_timeout);
    }

    // 인수든 아니든 **새** session_id 를 쓴다. 이전 세션의 늦은 STOP 이나
    // KEEPALIVE 가 방금 시작한 방송에 닿지 않게 하는 유일한 장치다.
    m_session = QRandomGenerator::global()->generate();
    m_awaiting_ready = true;
    publish_state(true, /*keepalive=*/false, m_takeover);
    m_takeover = false;   // 한 번 쓰고 버린다 — 다음 ON 은 다시 확인창부터
    m_ready_wait->start(int(GUARDX_BROADCAST_READY_WAIT_MS));
    emit status_changed("waiting for the RPi C receiver...", false);
}

void BroadcastController::on_ready(const QByteArray &payload)
{
    if (!m_awaiting_ready)
        return;   // 이미 취소됐거나 방송 중 — 늦게 온 응답은 버린다

    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    // 세션이 다르면 이전 시도의 응답이다. 짝을 정확히 맞춰야 엉뚱한 준비
    // 완료로 송출을 시작하지 않는다.
    if (quint32(o.value("session_id").toDouble()) != m_session)
        return;

    const QString result = o.value("result").toString();

    if (result == QLatin1String(GUARDX_BROADCAST_RESULT_BUSY)) {
        // 다른 VMS 가 쥐고 있다. 조용히 인수하지 않고 화면에 물어본다 —
        // 거의 동시에 눌린 경우도 여기로 온다(먼저 처리된 쪽이 이긴다).
        cancel_pending();
        m_session = 0;
        if (m_link_watch)
            m_link_watch->stop();
        const QString owner = o.value("owner").toString();
        emit status_changed("idle", false);
        emit takeover_required(owner);
        return;
    }

    if (result != QLatin1String(GUARDX_BROADCAST_RESULT_READY)) {
        const QString why = o.value("reason").toString();
        cancel_pending();
        publish_state(false);
        m_session = 0;
        emit status_changed(
            QString("RPi C receiver failed to start - %1")
                .arg(why.isEmpty() ? QString("no reason given") : why),
            true);
        return;
    }

    cancel_pending();

    if (!m_rtp->start(m_host, m_port, m_bitrate, m_nc, m_agc,
                      volume_percent())) {
        // 시작을 알려놓고 실패했다. 되돌리지 않으면 RPi C 가 방송 중이라 믿고
        // 사이렌을 계속 눌러둔다 — 안전 기능이 걸리므로 반드시 취소한다.
        publish_state(false);
        m_session = 0;
        return;
    }

    // 재발행 타이머는 송출이 실제로 시작된 뒤에만 돈다.
    if (!m_keepalive) {
        m_keepalive = new QTimer(this);
        connect(m_keepalive, &QTimer::timeout, this,
                [this] { publish_state(true, /*keepalive=*/true); });
    }
    m_keepalive->start(int(GUARDX_BROADCAST_KEEPALIVE_MS));

    m_active = true;
    emit active_changed(true);
    // 값은 main 의 멤버(m_bitrate·m_nc)를 쓴다 — RPi C 수신 확인 핸드셰이크가
    // 들어오면서 start() 지역변수에서 멤버로 옮겨졌다. 문구는 영문(12번).
    emit status_changed(QString("on air (Opus %1k · noise cancel %2)")
                            .arg(m_bitrate / 1000)
                            .arg(m_nc ? m_rtp->denoise_backend()
                                      : QString("off")),
                        false);
}

void BroadcastController::on_ready_timeout()
{
    if (!m_awaiting_ready)
        return;

    cancel_pending();
    // 송출하지 않는다 — 받을 사람이 없는데 쏘면 소리는 안 나고 화면만 "방송
    // 중"이 된다. 그 오해가 화재 상황에서 특히 위험하다.
    publish_state(false);
    m_session = 0;
    emit status_changed(
        QString("The RPi C receiver did not answer within %1 s - "
                "not starting the broadcast")
            .arg(GUARDX_BROADCAST_READY_WAIT_MS / 1000),
        true);
}

void BroadcastController::cancel_pending()
{
    m_awaiting_ready = false;
    if (m_ready_wait)
        m_ready_wait->stop();
}

void BroadcastController::stop()
{
    if (m_awaiting_ready) {
        // 준비 대기 중에 사용자가 껐다 — 아직 송출 전이므로 예약만 접는다.
        cancel_pending();
        publish_state(false);
        m_session = 0;
        emit status_changed("idle", false);
        return;
    }
    if (!m_active)
        return;   // 소멸자에서도 불리므로 빈 정지에 상태 방출을 하지 않는다

    if (m_keepalive)
        m_keepalive->stop();
    if (m_link_watch)
        m_link_watch->stop();
    if (m_rtp)
        m_rtp->stop();
    publish_state(false);
    m_session = 0;
    m_active = false;
    emit level_changed(-100.0);   // 레벨 미터를 무음으로 되돌린다
    emit active_changed(false);
    emit status_changed("idle", false);
}

void BroadcastController::publish_state(bool start, bool keepalive,
                                        bool takeover)
{
    QJsonObject cmd;
    cmd["node_id"] = "vms";
    cmd["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    cmd["action"] =
        !start ? QString::fromLatin1(GUARDX_BROADCAST_ACTION_STOP)
        : keepalive ? QString::fromLatin1(GUARDX_BROADCAST_ACTION_KEEPALIVE)
                    : QString::fromLatin1(GUARDX_BROADCAST_ACTION_START);
    // session_id 는 RPi C 파서가 요구하는 필드다(중복 START·낡은 STOP 판별용).
    cmd["session_id"] = static_cast<qint64>(m_session);
    // owner 는 "누가 방송 중인지"를 다른 VMS 에 보여주기 위한 것이고,
    // 세션 판별은 여전히 session_id 로만 한다.
    cmd[QString::fromLatin1(GUARDX_BROADCAST_FIELD_OWNER)] =
        MqttLink::instance()->client_id();
    if (start && !keepalive)
        cmd[QString::fromLatin1(GUARDX_BROADCAST_FIELD_TAKEOVER)] = takeover;

    // 실패해도 방송 자체는 진행한다 — 브로커가 죽었다고 관제사의 육성 안내를
    // 막을 이유는 없다. 다만 그 경우 RPi C 는 수신기를 못 띄우므로 소리가 안
    // 난다는 것을 상태줄에 남긴다.
    if (!MqttLink::instance()->publish(
            QString::fromLatin1(GUARDX_BROADCAST_COMMAND_TOPIC),
            QJsonDocument(cmd).toJson(QJsonDocument::Compact),
            BROADCAST_CMD_QOS)) {
        qWarning() << "[Broadcast] 방송 상태 발행 실패 —"
                   << (start ? "START" : "STOP");
        if (start)
            emit status_changed(
                "Broker not connected - RPi C cannot start its receiver",
                true);
    }
}

void BroadcastController::fail(const QString &message)
{
    stop();
    emit status_changed(message, true);
}
