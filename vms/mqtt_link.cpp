#include "mqtt_link.h"
#include "credentials.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QSysInfo>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>

#ifdef HAVE_MOSQUITTO
// mosquitto 2.1부터 <mosquitto.h>가 libcommon까지 끌어오는데, 그 안의
// libcommon_cjson.h가 별도 설치가 필요한 <cjson/cJSON.h>를 요구한다.
// 우리는 클라이언트 API만 쓰므로 자기완결적인 libmosquitto.h만 포함해
// 의존을 끊는다. (2.0 이하는 평평한 <mosquitto.h> 하나뿐이라 그쪽으로 폴백)
#if __has_include(<mosquitto/libmosquitto.h>)
#include <mosquitto/libmosquitto.h>
#else
#include <mosquitto.h>
#endif

namespace {
/**
 * @brief MOSQ_ERR_* → 사람이 읽을 문구
 *
 * mosquitto_strerror()는 libcommon_string.h에 있는데 그 헤더가 libcommon.h가
 * 먼저 정의하는 매크로에 의존해 단독 포함이 안 되고, 심볼도 별도 DLL
 * (mosquitto_common)에 있다. 로그 문구 하나 때문에 의존을 늘릴 이유가 없어
 * 자주 보는 코드만 직접 옮겼다. 모르는 코드는 숫자로 남긴다.
 */
QString mosq_err(int rc)
{
    switch (rc) {
    case MOSQ_ERR_SUCCESS:      return "성공";
    case MOSQ_ERR_NOMEM:        return "메모리 부족";
    case MOSQ_ERR_PROTOCOL:     return "프로토콜 오류";
    case MOSQ_ERR_INVAL:        return "잘못된 인자";
    case MOSQ_ERR_NO_CONN:      return "브로커에 연결되지 않음";
    case MOSQ_ERR_CONN_REFUSED: return "연결 거부됨";
    case MOSQ_ERR_NOT_FOUND:    return "대상 없음";
    case MOSQ_ERR_CONN_LOST:    return "연결 끊김";
    case MOSQ_ERR_TLS:          return "TLS 오류";
    case MOSQ_ERR_PAYLOAD_SIZE: return "payload 크기 초과";
    case MOSQ_ERR_NOT_SUPPORTED:return "지원하지 않는 기능";
    case MOSQ_ERR_AUTH:         return "인증 실패";
    case MOSQ_ERR_ERRNO:        return "시스템 오류(errno)";
    case MOSQ_ERR_EAI:          return "호스트 이름을 찾을 수 없음";
    default:                    return QString("rc=%1").arg(rc);
    }
}
} // namespace
#endif

MqttLink *MqttLink::instance()
{
    static MqttLink link;
    return &link;
}

bool MqttLink::available()
{
#ifdef HAVE_MOSQUITTO
    return true;
#else
    return false;
#endif
}

MqttLink::MqttLink(QObject *parent) : QObject(parent)
{
    // 규약 0절은 "client id = node id"지만 VMS만 예외다. VMS는 동시에 여러 대가
    // 뜰 수 있고, MQTT는 client id가 겹치면 기존 세션을 강제로 끊는다.
    //
    // 인증서 이름(= CN = 계정)과 client id 는 다른 것이다(08-12 분리):
    //  - CN 은 이 PC 의 신원이다. 언제나 "vms-{hostname}" — 브로커가
    //    use_identity_as_username 이라 계정·ACL 이 여기서 나온다.
    //  - client id 는 **세션** 식별자다. 같은 PC 에서 앱 두 개를 병행하면
    //    (개발) 같은 id 가 서로를 끊으므로, [mqtt] client_id 로 한쪽만
    //    바꿔 준다. 계정은 안 바뀐다 — 브로커는 CN 만 본다(08-12 실측).
    m_cert_name = QString("vms-%1").arg(QSysInfo::machineHostName());
    const QString override_id = Credentials::mqtt_client_id();
    m_client_id = override_id.isEmpty() ? m_cert_name : override_id;
    if (!override_id.isEmpty())
        qInfo().noquote()
            << QString("[MqttLink] client_id 오버라이드: %1 (계정/CN 은 %2 그대로)")
                   .arg(m_client_id, m_cert_name);
}

MqttLink::~MqttLink()
{
#ifdef HAVE_MOSQUITTO
    // 접속 워커가 아직 돌고 있으면 기다린다 — 안 기다리면 워커가 이미 죽은
    // 핸들로 mosquitto_connect 를 계속 붙들고 있게 된다.
    if (m_worker && m_worker->isRunning())
        m_worker->wait();
    if (m_mosq) {
        mosquitto_disconnect(m_mosq);
        mosquitto_destroy(m_mosq);
        m_mosq = nullptr;
    }
    mosquitto_lib_cleanup();
#endif
}

#ifndef HAVE_MOSQUITTO

// ---------------------------------------------------------------- 스텁 구현
// libmosquitto 없이 빌드된 경우. 등록만 받고 아무것도 하지 않는다.

void MqttLink::start()
{
    if (m_started)
        return;
    m_started = true;
    qWarning() << "[MqttLink] libmosquitto 없이 빌드됨 — MQTT 비활성. "
                  "구독한 값은 오지 않으며 호출부는 기본값 경로를 탄다.";
}

void MqttLink::subscribe(const QString &topic, Handler handler, int qos)
{
    m_subs.append({ topic, std::move(handler), qos });
    qInfo() << "[MqttLink] (스텁) 구독 등록:" << topic;
}

bool MqttLink::publish(const QString &, const QByteArray &, int, bool)
{
    return false;
}

void MqttLink::poll() {}
void MqttLink::begin_connect(bool) {}
void MqttLink::finish_connect(int) {}
void MqttLink::schedule_retry() {}
void MqttLink::resubscribe_all() {}
void MqttLink::on_connect(mosquitto *, void *, int) {}
void MqttLink::on_disconnect(mosquitto *, void *, int) {}
void MqttLink::on_message(mosquitto *, void *, const struct mosquitto_message *) {}

#else

// ------------------------------------------------------------ 실제 구현

void MqttLink::start()
{
    if (m_started)
        return;
    m_started = true;

    // 생성자가 Credentials::load() 보다 먼저 돌았을 수 있다(구독 등록은
    // start() 전에 허용되는 규약이라). 여기서 한 번 더 반영한다 —
    // mosquitto_new 는 이 아래라 세션 id 는 아직 안 굳었다.
    {
        const QString override_id = Credentials::mqtt_client_id();
        if (!override_id.isEmpty() && override_id != m_client_id) {
            m_client_id = override_id;
            qInfo().noquote()
                << QString("[MqttLink] client_id 오버라이드: %1 (계정/CN 은 %2 그대로)")
                       .arg(m_client_id, m_cert_name);
        }
    }

    mosquitto_lib_init();

    // clean_session = true — 오프라인 동안 쌓인 과거 메시지가 필요 없다.
    // 큐잉되면 재접속 시 몇 초 전 상태가 우수수 재생된다.
    // 상태성 토픽은 retained라 재구독만 하면 현재 값을 즉시 다시 받는다.
    m_mosq = mosquitto_new(m_client_id.toUtf8().constData(), true, this);
    if (!m_mosq) {
        qCritical() << "[MqttLink] mosquitto_new 실패 — MQTT 비활성";
        set_fault(QStringLiteral("MQTT init failed — restart the app"));
        return;
    }

    mosquitto_connect_callback_set(m_mosq, &MqttLink::on_connect);
    mosquitto_disconnect_callback_set(m_mosq, &MqttLink::on_disconnect);
    mosquitto_message_callback_set(m_mosq, &MqttLink::on_message);

    // ---- mTLS (8883) ------------------------------------------------------
    //
    // 인증서 세 개가 다 설정돼 있으면 TLS 로 붙는다. 별도 on/off 스위치를 두지
    // 않는 이유: 상태가 둘로 갈리면 "켰는데 경로가 비어 평문으로 나가는" 조합이
    // 생긴다. **설정 = 의도**로 두고, 되돌릴 때는 경로를 지운다.
    //
    // ⚠ 실패하면 평문으로 **폴백하지 않는다.** 자동 강등은 보안 설정이 조용히
    //    사라지는 가장 흔한 방식이다 — 붙지 않는 편이 정직하다.
    // 인증서 파일명은 CN 규약이다(파일명 = CN = 계정). client_id 가 아니다 —
    // 세션 id 를 오버라이드해도 신원은 이 PC 의 것 하나다.
    const QString ca_path   = Credentials::mqtt_tls_ca(m_cert_name);
    const QString cert_path = Credentials::mqtt_tls_cert(m_cert_name);
    const QString key_path  = Credentials::mqtt_tls_key(m_cert_name);

    // ⚠ **인증서가 없다고 평문으로 내려가지 않는다.** TLS 를 안 쓰는 경우는
    //    `[mqtt] tls=0` 으로 **명시했을 때뿐**이다. 파일이 없는 것은 사고이고,
    //    그때 평문으로 시도하면 브로커(8883)가 끊어 rc=7 재시도만 돌면서
    //    "왜 화면이 비지"만 남는다 — 08-11 리팩터에서 실제로 그렇게 됐었다.
    const bool tls = !Credentials::mqtt_tls_opt_out();
    if (tls) {
        const QByteArray ca   = QFile::encodeName(ca_path);
        const QByteArray cert = QFile::encodeName(cert_path);
        const QByteArray key  = QFile::encodeName(key_path);

        // 파일이 없으면 tls_set 은 성공하고 **접속할 때** 알 수 없는 오류로
        // 죽는다. 여기서 먼저 확인해 원인을 이름으로 말한다 — 로그에는
        // 전체 경로를, 화면(fault)에는 다음 행동을.
        for (const QString &f : { ca_path, cert_path, key_path }) {
            if (!QFile::exists(f)) {
                qCritical().noquote()
                    << QString("[MqttLink] TLS 파일이 없다: %1 — **접속을 포기한다** "
                               "(평문으로 내려가지 않는다). %2 에 "
                               "ca.crt · %3.crt · %3.key 를 두거나, 평문으로 "
                               "되돌리려면 credentials.ini 에 [mqtt] tls=0")
                           .arg(f, Credentials::certs_dir(), m_cert_name);
                set_fault(QString("certificate missing: %1 — put ca.crt / %2.crt / "
                                  "%2.key in %3")
                              .arg(QFileInfo(f).fileName(), m_cert_name,
                                   QDir::toNativeSeparators(Credentials::certs_dir())));
                return;
            }
        }

        const int trc = mosquitto_tls_set(m_mosq, ca.constData(), nullptr,
                                          cert.constData(), key.constData(),
                                          nullptr);
        if (trc != MOSQ_ERR_SUCCESS) {
            qCritical() << "[MqttLink] mosquitto_tls_set 실패:" << mosq_err(trc)
                        << "— 평문으로 내려가지 않고 멈춘다";
            set_fault(QString("TLS setup failed (%1) — check certificate files")
                          .arg(mosq_err(trc)));
            return;
        }

        // ⚠ mosquitto_tls_insecure_set() 을 부르지 않는다. 그것을 켜면 hostname
        //    검증이 꺼져 mTLS 의 절반(상대가 그 서버가 맞는가)이 사라진다.
        //    그래서 host 는 **인증서 SAN 에 있는 이름/IP** 여야 한다 —
        //    rpib(DNS)와 172.20.33.251(IP) 둘 다 SAN 에 있다(08-12 확인).
        //    FQDN(rpib.tail…ts.net)은 없어서 검증에서 끊긴다.
        qInfo().noquote()
            << QString("[MqttLink] mTLS 사용 — %1 (ca=%2 cert=%3)")
                   .arg(QFileInfo(cert_path).absolutePath(),
                        QFileInfo(ca_path).fileName(),
                        QFileInfo(cert_path).fileName());

        // 브로커가 use_identity_as_username 이라 **CN 이 곧 계정**이다.
        // ini 로 tls_cert 경로를 바꿔 파일명이 CN 규약(m_cert_name)과 다르면
        // ACL 이 엉뚱한 계정으로 걸릴 수 있다 — 값이 다르면 알려준다.
        // (client_id 와 다른 것은 정상이다 — 세션 id 는 계정이 아니다.)
        const QString cn_hint = QFileInfo(cert_path).completeBaseName();
        if (!cn_hint.isEmpty() && cn_hint != m_cert_name) {
            qWarning().noquote()
                << QString("[MqttLink] ⚠ 인증서 파일명(%1)이 CN 규약(%2)과 다르다 "
                           "— 파일명=CN=계정 규약이면 ACL 이 어긋난다")
                       .arg(cn_hint, m_cert_name);
        }
    }

    if (!tls) {
        // 여기 오는 경로는 하나뿐이다 — `[mqtt] tls=0`. 사고가 아니라 선택이다.
        // 다만 포트까지 되돌리지 않으면 브로커(8883=TLS 리스너)가 끊는다.
        // 절반만 되돌린 상태는 rc=7 재시도만 돌아 원인이 안 보인다.
        if (Credentials::mqtt_port() == 8883) {
            qWarning() << "[MqttLink] 평문 접속([mqtt] tls=0) 인데 포트가 8883 이다 "
                          "— 브로커가 끊는다. 롤백하려면 port=1883 도 함께";
            set_fault(QStringLiteral("tls=0 but port is 8883 — set port=1883 "
                                     "as well, or remove tls=0"));
        } else {
            qWarning() << "[MqttLink] 평문 접속 — [mqtt] tls=0 으로 명시됨";
        }
    }

    const QString user = Credentials::mqtt_user();
    if (!user.isEmpty() && !tls) {
        // mTLS 에서는 계정이 인증서 CN 으로 정해진다(use_identity_as_username).
        // 사용자명을 따로 보내면 어느 쪽이 계정인지 두 갈래가 된다.
        mosquitto_username_pw_set(m_mosq, user.toUtf8().constData(),
                                  Credentials::mqtt_password().toUtf8().constData());
    }

    m_poll = new QTimer(this);
    m_poll->setInterval(POLL_INTERVAL_MS);
    connect(m_poll, &QTimer::timeout, this, &MqttLink::poll);

    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, [this] { begin_connect(true); });

    m_host = Credentials::mqtt_host();
    m_port = Credentials::mqtt_port();

    // 접속은 워커 스레드에서 블로킹으로 한다 — 이유는 begin_connect() 주석.
    // 폴링은 붙은 다음에 시작한다(붙기 전에는 돌릴 것이 없고, 워커와 같은
    // 핸들을 동시에 만지면 안 된다).
    begin_connect(false);

    qInfo().noquote() << QString("[MqttLink] 시작 — %1:%2 (%3) client_id %4")
                             .arg(m_host).arg(m_port)
                             .arg(tls ? "mTLS" : "평문", m_client_id);
}

void MqttLink::begin_connect(bool reconnect)
{
    if (!m_mosq || m_connecting)
        return;
    m_connecting = true;

    // ⚠ 워커가 소켓을 만드는 동안 poll()이 같은 핸들로 mosquitto_loop 를 부르면
    // 안 된다. 한 핸들을 두 스레드에서 동시에 쓰는 것은 허용되지 않는다.
    if (m_poll)
        m_poll->stop();

    const QByteArray host = m_host.toUtf8();
    const int port = m_port;
    mosquitto *const mosq = m_mosq;

    m_worker = QThread::create([this, mosq, host, port, reconnect] {
        const int rc = reconnect
                           ? mosquitto_reconnect(mosq)
                           : mosquitto_connect(mosq, host.constData(), port,
                                               KEEPALIVE_SEC);
        // 결과 처리는 GUI 스레드로 넘긴다 — 타이머·시그널이 전부 거기 산다.
        QMetaObject::invokeMethod(
            this, [this, rc] { finish_connect(rc); }, Qt::QueuedConnection);
    });
    connect(m_worker, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &QThread::destroyed, this, [this] { m_worker = nullptr; });
    m_worker->start();
}

void MqttLink::finish_connect(int rc)
{
    m_connecting = false;

    if (rc == MOSQ_ERR_SUCCESS) {
        // CONNACK 은 아직이다 — 첫 poll() 안에서 on_connect 가 불린다.
        if (m_poll)
            m_poll->start();
        return;
    }

    qWarning() << "[MqttLink] 접속 실패:" << mosq_err(rc) << "— 재시도 예약";
    schedule_retry();
}

void MqttLink::poll()
{
    if (!m_mosq)
        return;

    // 타임아웃 0 = 논블로킹. 콜백이 이 호출 안에서 동기 실행되므로
    // 결과적으로 GUI 스레드에서 핸들러가 돌아간다.
    const int rc = mosquitto_loop(m_mosq, 0, 1);
    if (rc == MOSQ_ERR_SUCCESS)
        return;

    // NO_CONN은 접속 전/끊김 상태에서 매 틱 나온다 — 로그로 도배하지 않는다
    if (rc != MOSQ_ERR_NO_CONN)
        qDebug() << "[MqttLink] loop 오류:" << mosq_err(rc);

    if (m_online)
        set_online(false);

    if (!m_retry->isActive())
        schedule_retry();
}

void MqttLink::schedule_retry()
{
    if (!m_retry || m_retry->isActive())
        return;

    m_retry->start(m_backoff_ms);
    qDebug() << "[MqttLink] 재접속 대기" << m_backoff_ms << "ms";
    m_backoff_ms = qMin(m_backoff_ms * 2, BACKOFF_MAX_MS);
}

void MqttLink::resubscribe_all()
{
    for (const Sub &s : m_subs) {
        const int rc = mosquitto_subscribe(m_mosq, nullptr,
                                           s.topic.toUtf8().constData(), s.qos);
        if (rc == MOSQ_ERR_SUCCESS)
            qInfo() << "[MqttLink] 구독:" << s.topic << "(qos" << s.qos << ")";
        else
            qWarning() << "[MqttLink] 구독 실패:" << s.topic
                       << mosq_err(rc);
    }
}

void MqttLink::subscribe(const QString &topic, Handler handler, int qos)
{
    m_subs.append({ topic, std::move(handler), qos });

    // 이미 붙어 있으면 즉시 구독, 아니면 접속 콜백이 일괄 처리한다
    if (m_online && m_mosq)
        mosquitto_subscribe(m_mosq, nullptr, topic.toUtf8().constData(), qos);
}

bool MqttLink::publish(const QString &topic, const QByteArray &payload,
                       int qos, bool retain)
{
    if (!m_online || !m_mosq)
        return false;

    const int rc = mosquitto_publish(m_mosq, nullptr,
                                     topic.toUtf8().constData(),
                                     payload.size(), payload.constData(),
                                     qos, retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        qWarning() << "[MqttLink] 발행 실패:" << topic << mosq_err(rc);
        return false;
    }
    return true;
}

void MqttLink::on_connect(mosquitto *, void *self, int rc)
{
    auto *link = static_cast<MqttLink *>(self);
    if (rc != 0) {
        // TCP·TLS 는 지나왔는데 브로커가 CONNACK 에서 거절한 것 — 재시도로
        // 나아지지 않는 종류다(계정·ACL). 화면에도 말한다.
        qWarning() << "[MqttLink] 접속 거부 (rc=" << rc << ")"
                   << mosq_err(rc);
        link->set_fault(QString("broker refused the connection (rc=%1)").arg(rc));
        link->schedule_retry();
        return;
    }

    qInfo() << "[MqttLink] 브로커 접속됨";
    link->m_backoff_ms = 1000;      // 성공했으니 백오프 리셋
    link->set_online(true);
    link->resubscribe_all();
}

void MqttLink::on_disconnect(mosquitto *, void *self, int rc)
{
    auto *link = static_cast<MqttLink *>(self);
    qWarning() << "[MqttLink] 브로커 연결 끊김 (rc=" << rc << ")";
    link->set_online(false);
    link->schedule_retry();
}

void MqttLink::on_message(mosquitto *, void *self,
                          const struct mosquitto_message *msg)
{
    auto *link = static_cast<MqttLink *>(self);
    link->handle_message(QString::fromUtf8(msg->topic),
                         QByteArray(static_cast<const char *>(msg->payload),
                                    msg->payloadlen));
}

#endif  // HAVE_MOSQUITTO

// ------------------------------------------------- 빌드 무관 공통 부분

void MqttLink::handle_message(const QString &topic, const QByteArray &payload)
{
    // ⚠ **m_subs 를 직접 순회하면 안 된다** (2026-08-10 use-after-free 수정).
    //
    // 핸들러는 이 스택 위에서 동기로 실행되고(mosquitto_loop 이 GUI 스레드
    // 타이머에서 돌므로), 그 안에서 subscribe() 가 다시 불릴 수 있다:
    //
    //   handle_message  →  CrowdPage::on_dates       (crowd_page.cpp:319)
    //                   →  on_selection_changed      (:329)
    //                   →  ensure_days_loaded        (:355) request()
    //                   →  ensure_reply_subscription (mqtt_link.cpp:336)
    //                   →  subscribe()               (:229) m_subs.append()
    //
    // append 가 QList 를 재할당하면 순회 중인 참조와 begin/end 가 해제된
    // 메모리를 가리킨다 — 남은 항목에서 해제된 std::function 을 호출한다.
    // dates 토픽이 retained 라 **콜드 스타트에서 거의 매번** 밟혔다
    // (그때는 아직 아무도 request() 를 안 해 m_reply_subscribed 가 false).
    // AlertFeed::on_live_alert → request_history() 도 같은 경로다.
    //
    // 비용: QList 는 COW 라 이 복사는 refcount 증가 1회다. 핸들러가 실제로
    // append 할 때만 m_subs 가 분리되고, 그때 우리 스냅샷이 옛 버퍼를 붙잡아
    // 준다. 구독 해제 경로는 없으므로(append-only) "해제된 핸들러가 불린다"는
    // 반대 위험도 없다. 순회 중 추가된 구독이 이번 메시지를 못 받는 것은
    // 의도된 동작이다.
    const QList<Sub> subs = m_subs;
    for (const Sub &s : subs) {
        // 와일드카드는 아직 안 쓴다 — 정확히 일치하는 토픽만 넘긴다
        if (s.topic == topic && s.handler)
            s.handler(payload);
    }
}

void MqttLink::set_online(bool on)
{
    if (m_online == on)
        return;
    m_online = on;
    // 붙었다는 사실이 곧 반증이다 — 남아 있던 설정 오류 표시를 걷는다.
    if (on)
        set_fault(QString());
    emit online_changed(on);
}

void MqttLink::set_fault(const QString &f)
{
    if (m_fault == f)
        return;
    m_fault = f;
    emit fault_changed();
}

// ------------------------------------------ 요청-응답 (빌드 무관 공통 부분)
//
// publish()/subscribe() 위에만 서 있으므로 스텁 빌드에서도 그대로 컴파일된다
// (그 경우 publish가 false를 돌려주고 on_error가 즉시 불린다).

QString MqttLink::reply_topic() const
{
    // guardx/{도메인}/{노드ID}/{하위} — 규약 형식. 노드는 이 VMS 인스턴스다.
    return QString("guardx/db/%1/result").arg(m_client_id);
}

void MqttLink::ensure_reply_subscription()
{
    if (m_reply_subscribed)
        return;
    m_reply_subscribed = true;

    subscribe(reply_topic(), [this](const QByteArray &p) { on_reply(p); }, 1);

    // 대기 중인 요청이 있을 때만 도는 청소 타이머
    m_sweep = new QTimer(this);
    m_sweep->setInterval(SWEEP_INTERVAL_MS);
    connect(m_sweep, &QTimer::timeout, this, &MqttLink::sweep_timeouts);
}

QString MqttLink::request(const QString &topic, const QJsonObject &params,
                          ReplyHandler on_reply_cb, ErrorHandler on_error,
                          int timeout_ms, FailHandler on_fail)
{
    ensure_reply_subscription();

    const QString req_id = QUuid::createUuid().toString(QUuid::Id128);

    QJsonObject req = params;
    req["node_id"]   = m_client_id;
    req["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    req["req_id"]    = req_id;
    req["reply_to"]  = reply_topic();

    // QoS 1 — 요청이 유실되면 그 화면이 영영 안 채워진다. 중복 수신은 같은
    // 결과를 두 번 받을 뿐이라 무해하다 (질의는 읽기 전용·멱등).
    if (!publish(topic, QJsonDocument(req).toJson(QJsonDocument::Compact), 1)) {
        const QString why =
            available() ? QStringLiteral("broker not connected")
                        : QStringLiteral("MQTT not built into this binary");
        qWarning() << "[MqttLink] 요청 발행 실패:" << topic << "—" << why;
        if (on_error)
            on_error(why);
        return QString();
    }

    m_pending.insert(req_id,
                     { std::move(on_reply_cb), std::move(on_error),
                       QDateTime::currentMSecsSinceEpoch() + timeout_ms, topic,
                       std::move(on_fail) });
    if (m_sweep && !m_sweep->isActive())
        m_sweep->start();
    return req_id;
}

void MqttLink::cancel(const QString &req_id)
{
    if (req_id.isEmpty())
        return;
    m_pending.remove(req_id);
    if (m_pending.isEmpty() && m_sweep)
        m_sweep->stop();
}

void MqttLink::on_reply(const QByteArray &payload)
{
    QJsonParseError err{};
    const QJsonObject o = QJsonDocument::fromJson(payload, &err).object();
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "[MqttLink] 응답 파싱 실패:" << err.errorString();
        return;
    }

    auto it = m_pending.find(o.value("req_id").toString());
    if (it == m_pending.end())
        return;   // 이미 타임아웃/취소됐거나, 우리 것이 아닌 응답

    // 콜백이 곧바로 새 요청을 걸 수 있으므로 먼저 꺼내 놓고 부른다
    const Pending p = *it;
    m_pending.erase(it);
    if (m_pending.isEmpty() && m_sweep)
        m_sweep->stop();

    // ok:false 는 에러 경로로 — 화면이 "응답 없음"과 "에러"를 구분해야 한다
    if (o.contains("ok") && !o.value("ok").toBool()) {
        // 사유를 해석하는 호출부(로그인)에는 객체째 넘긴다 — 문자열로 줄이면
        // reason·retry_after_s 가 사라져 잠금 카운트다운을 만들 수 없다.
        if (p.on_fail) {
            qWarning() << "[MqttLink]" << p.topic
                       << "응답 거절:" << o.value("reason").toString("(reason 없음)");
            p.on_fail(o);
            return;
        }
        // 실패 필드 이름이 두 갈래다 — 기존 질의 규약은 `error`
        // (DB_LINK_AND_MQTT_MIGRATION.md), 로그인 계약은 `reason`.
        // 문구를 만드는 자리라 둘 다 받아 준다.
        const QString why = o.value("error").toString(
            o.value("reason").toString(QStringLiteral("unknown error")));
        qWarning() << "[MqttLink]" << p.topic << "응답 오류:" << why;
        if (p.on_error)
            p.on_error(why);
        return;
    }

    if (p.on_reply)
        p.on_reply(o);
}

void MqttLink::sweep_timeouts()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QStringList dead;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it)
        if (it->deadline_ms <= now)
            dead.append(it.key());

    for (const QString &id : dead) {
        const Pending p = m_pending.take(id);
        qWarning() << "[MqttLink] 응답 없음(타임아웃):" << p.topic;
        if (p.on_error)
            p.on_error(QStringLiteral("no reply (timeout)"));
    }

    if (m_pending.isEmpty() && m_sweep)
        m_sweep->stop();
}
