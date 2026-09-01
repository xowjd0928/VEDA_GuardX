#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class QThread;
class QTimer;
struct mosquitto;   // libmosquitto 헤더를 여기 노출하지 않는다 (전방 선언)

/**
 * @brief MQTT 브로커 연결 + 구독 관리 (싱글턴)
 *
 * VMS가 DB에 직접 붙는 대신 RPi B가 발행하는 값을 받아쓰기 위한 통로다.
 * 자세한 배경은 docs/DB_LINK_AND_MQTT_MIGRATION.md 참조.
 *
 * 스레드: 별도 스레드를 만들지 않는다. QTimer로 mosquitto_loop()를 GUI
 * 스레드에서 논블로킹 호출하므로 콜백도 GUI 스레드에서 실행된다 —
 * 위젯을 건드리는 핸들러를 마샬링 없이 그대로 쓸 수 있다.
 *
 * libmosquitto 없이 빌드하면(HAVE_MOSQUITTO 미정의) 전 기능이 무동작
 * 스텁이 된다. subscribe()는 등록만 받고 콜백은 영영 오지 않으므로,
 * 호출부는 "값이 아직 안 온 상태"의 폴백 경로를 그대로 타게 된다.
 */
class MqttLink : public QObject
{
    Q_OBJECT

public:
    using Handler = std::function<void(const QByteArray &payload)>;

    static MqttLink *instance();

    /** @brief 브로커 접속 시작 (비동기 — 여기서 기다리지 않는다) */
    void start();

    /**
     * @brief 토픽 구독 + 메시지 도착 시 호출될 핸들러 등록
     *
     * start() 전에 불러도 된다 — 접속이 되면(재접속 포함) 등록된 토픽을
     * 한꺼번에 다시 구독한다. 브로커가 죽었다 살아나도 구독이 복구된다.
     *
     * @param qos 기본 1. 실제 전달 QoS는 min(발행 QoS, 구독 QoS)이므로
     *            상태성 토픽을 0으로 주면 발행측이 1이어도 0으로 떨어진다.
     */
    void subscribe(const QString &topic, Handler handler, int qos = 1);

    /** @brief 발행. 미연결이면 false (큐잉하지 않는다) */
    bool publish(const QString &topic, const QByteArray &payload,
                 int qos = 1, bool retain = false);

    // ---- 요청-응답 (RPC) ---------------------------------------------------
    using ReplyHandler = std::function<void(const QJsonObject &reply)>;
    using ErrorHandler = std::function<void(const QString &reason)>;

    /**
     * @brief `ok:false` 응답을 **통째로** 받고 싶을 때 (선택)
     *
     * 기본 경로(`on_error`)는 실패를 사람이 읽을 문구 하나로 줄인다. 화면이
     * 사유를 그대로 보여주기만 하면 그걸로 충분했다.
     *
     * 로그인은 다르다. 계약(`docs/` 로그인 핸드오프 §5)의 실패 응답은
     * `{ok:false, reason:"locked", retry_after_s:60}` 처럼 **기계가 읽어야 하는
     * 필드**를 싣고, 화면은 `reason`으로 문구를 고르고 남은 초로 카운트다운을
     * 돌린다. 문자열로 줄이면 그 둘이 사라진다.
     *
     * 그래서 사유를 해석해야 하는 호출부만 이걸 준다. 주면 `ok:false`가
     * 여기로 오고, 안 주면 지금까지처럼 `on_error`로 간다 —
     * 기존 호출부는 한 줄도 바뀌지 않는다.
     */
    using FailHandler = std::function<void(const QJsonObject &reply)>;

    /** @brief 기본 응답 대기 시간 — RPi B 질의는 보통 1초 안에 온다 */
    static const int DEFAULT_TIMEOUT_MS = 8000;

    /**
     * @brief 요청 1건을 보내고 짝이 맞는 응답을 콜백으로 돌려준다
     *
     * MQTT에는 요청-응답이 없다. 규약은 "요청 토픽 하나 + 응답 토픽 하나 +
     * payload의 `req_id`로 짝 맞추기"인데, 그 부기 코드를 화면마다 베끼다
     * 보니 네 벌이 됐다(heatday·set_zone·occseries·incidents). 여기로 모은다.
     *
     * 자동으로 채워 넣는 것: `node_id` · `timestamp` · `req_id` · `reply_to`.
     * 호출부는 질의 고유 인자만 담으면 된다.
     *
     * 응답 토픽은 이 VMS 인스턴스 전용(`guardx/db/{client_id}/result`)이라
     * 여러 대가 떠도 서로의 응답을 보지 않는다. 첫 request() 때 한 번만
     * 구독한다.
     *
     * @param on_error 타임아웃·미연결·`ok:false` 를 모두 여기로 보낸다.
     *                 "응답 없음"과 "에러"를 화면이 구분해 표시할 수 있도록
     *                 사유 문구를 넘긴다. 생략하면 경고 로그만 남는다.
     * @param on_fail  주면 `ok:false` 가 (문자열 대신) 응답 객체째 여기로 온다.
     *                 타임아웃·미연결은 그대로 `on_error` 다 — 서버가 거절한
     *                 것과 서버에 닿지도 못한 것은 다른 사건이다.
     * @return req_id. 미연결이면 빈 문자열(이 경우 on_error가 **즉시** 불린다).
     *
     * @note 콜백은 GUI 스레드에서 불린다 (MqttLink의 스레드 규약 참조).
     *       요청이 살아 있는 동안 캡처한 객체의 수명은 호출부 책임이다 —
     *       위젯을 캡처한다면 QPointer 를 쓰거나 소멸 시 cancel() 할 것.
     */
    QString request(const QString &topic, const QJsonObject &params,
                    ReplyHandler on_reply, ErrorHandler on_error = {},
                    int timeout_ms = DEFAULT_TIMEOUT_MS,
                    FailHandler on_fail = {});

    /** @brief 진행 중인 요청 취소 (응답이 와도 콜백을 부르지 않는다) */
    void cancel(const QString &req_id);

    /** @brief 이 인스턴스 전용 응답 토픽 */
    QString reply_topic() const;

    bool online() const { return m_online; }

    /**
     * @brief MQTT client id — 기본 "vms-{hostname}", `[mqtt] client_id` 로 오버라이드
     *
     * ⚠ 계정이 아니다. 계정은 인증서 CN(cert_name())이 정한다
     * (브로커 use_identity_as_username). 이 값은 세션 식별자일 뿐이라
     * 개발 병행 시 인스턴스마다 달리 줄 수 있다.
     */
    QString client_id() const { return m_client_id; }

    /** @brief 인증서 이름 = CN = 계정 — 언제나 "vms-{hostname}" */
    QString cert_name() const { return m_cert_name; }

    /**
     * @brief 접속이 안 되는 **설정 수준의 이유** (영문 한 줄). 없으면 빈 문자열
     *
     * "브로커가 잠깐 안 보인다"(재시도가 맞는 상태)와 "설정이 틀렸다"(기다려도
     * 안 붙는 상태)를 화면이 구분해 말하기 위한 것. 후자만 여기 담는다 —
     * 인증서 없음, TLS 설정 실패, 평문/포트 조합 오류, 브로커의 접속 거부.
     * 값이 있으면 로그인 화면이 "서버 확인 중" 대신 이 문구와 다음 행동을
     * 보여준다. 08-12 사고(팀원 전원 1883 무한 재시도, 화면은 침묵)의 재발 방지.
     */
    QString fault() const { return m_fault; }

    /** @brief 이 빌드에 libmosquitto가 들어있는지 */
    static bool available();

signals:
    /** @brief 접속 상태 변화 (UI에 "브로커 끊김" 표시용) */
    void online_changed(bool online);

    /** @brief fault() 변화 — 로그인 화면 상태줄 갱신용 */
    void fault_changed();

private:
    explicit MqttLink(QObject *parent = nullptr);
    ~MqttLink() override;

    void poll();                       ///< QTimer 주기 — mosquitto_loop 호출

    /**
     * @brief 접속(또는 재접속)을 **워커 스레드에서 블로킹으로** 시작한다
     *
     * ⚠ `mosquitto_connect_async()` 로 되돌리지 말 것. libmosquitto 2.1 은
     * 비블로킹 연결에서도 TCP 가 아직 안 붙은 소켓 위에 곧바로 TLS 를 올리려
     * 들고, 그 자리에서 `MOSQ_ERR_ERRNO` 로 실패한다 — **TLS 를 켠 경우에만**.
     * 평문에서는 async 가 정상이라, 기본이 8883/mTLS 로 바뀐 2026-08-12
     * 전까지는 이 함정이 안 보였다. 실측(08-13): 도달 불가 주소로도 4ms 만에
     * 같은 실패 → 네트워크가 아니라 로컬 경로다. 같은 인자로 블로킹
     * `mosquitto_connect()` 는 성공하고 CONNACK 까지 온다.
     *
     * 블로킹 호출을 GUI 스레드에서 하면 브로커가 죽었을 때 창이 통째로 멈춘다
     * (async 를 고른 원래 이유가 그것이다). 그래서 **접속만** 워커로 내보내고,
     * 붙은 뒤의 `mosquitto_loop()` 폴링은 종전대로 GUI 스레드에서 돈다.
     */
    void begin_connect(bool reconnect);

    /** @brief 워커의 결과를 GUI 스레드에서 받는다 (성공이면 폴링 재개) */
    void finish_connect(int rc);

    void schedule_retry();
    void resubscribe_all();
    void set_online(bool on);
    void set_fault(const QString &f);  ///< fault() 갱신 + 변화 시 fault_changed()

    // libmosquitto C 콜백 → 인스턴스 메서드로 넘기는 다리
    static void on_connect(mosquitto *m, void *self, int rc);
    static void on_disconnect(mosquitto *m, void *self, int rc);
    static void on_message(mosquitto *m, void *self,
                           const struct mosquitto_message *msg);

    void handle_message(const QString &topic, const QByteArray &payload);

    struct Sub {
        QString topic;
        Handler handler;
        int qos;
    };

    /** @brief 응답 대기 중인 요청 1건 */
    struct Pending {
        ReplyHandler on_reply;
        ErrorHandler on_error;
        qint64 deadline_ms;
        QString topic;      ///< 로그용 (어느 질의가 죽었는지)
        FailHandler on_fail; ///< 있으면 ok:false 가 이리로 (없으면 on_error)
    };

    void ensure_reply_subscription();   ///< 첫 request() 때 1회
    void on_reply(const QByteArray &payload);
    void sweep_timeouts();              ///< 데드라인 지난 요청 정리

    mosquitto *m_mosq = nullptr;
    QTimer *m_poll = nullptr;
    QTimer *m_retry = nullptr;
    QTimer *m_sweep = nullptr;         ///< 요청 타임아웃 청소 (대기 있을 때만)
    QList<Sub> m_subs;
    QHash<QString, Pending> m_pending; ///< req_id -> 대기 중인 요청
    bool m_reply_subscribed = false;
    QString m_client_id;
    QString m_cert_name;               ///< CN = 계정 = 인증서 파일명 (고정 규약)
    QString m_fault;                   ///< 설정 수준의 접속 불가 사유 (영문)
    bool m_online = false;
    bool m_started = false;
    int m_backoff_ms = 1000;           ///< 1s → 2s → 4s … 최대 30s

    QString m_host;                    ///< 워커가 쓰는 접속 대상 (start()에서 고정)
    int m_port = 0;
    bool m_connecting = false;         ///< 워커가 도는 동안 poll()·중복 시도 차단
    QThread *m_worker = nullptr;       ///< 접속 워커 (소멸자에서 기다린다)

    static const int POLL_INTERVAL_MS = 20;
    static const int BACKOFF_MAX_MS = 30000;
    static const int KEEPALIVE_SEC = 30;
    static const int SWEEP_INTERVAL_MS = 500;
};
