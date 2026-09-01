#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

/**
 * @brief 혼잡 경보 수신기 (싱글턴) — RPi B `task_alert` 판정 결과의 소비자
 *
 * 두 경로를 합쳐 "지금 각 채널이 어느 단계인가"를 유지한다.
 *
 * ```
 * ① guardx/alert/rpib          (edge, retain=false) — 전이 순간
 * ② guardx/db/rpib/incidents   (retained)           — 열린 incident 스냅샷
 * ③ guardx/db/rpib/query/incidents (요청-응답)      — 이력 (ALERT·REPORT 표)
 * ```
 *
 * **①만으로는 안 된다.** 폴러는 상태가 *바뀔 때만* retain=false로 쏘므로,
 * 경보 발생 뒤에 VMS를 켜면 이미 열린 critical을 영영 모른다. ②가 그 구멍을
 * 막는다 (접속 즉시 현재 상태 수신).
 *
 * ⚠ ②는 폴러의 설정 틱(기본 30초)에만 재발행되는데 ①은 즉시 온다. 그래서
 * ②가 ①보다 **낡을 수 있다** — 그대로 적용하면 방금 해제된 채널이 30초 동안
 * 다시 critical로 되돌아간다. payload의 timestamp를 채널별 마지막 라이브
 * 시각과 비교해 낡은 스냅샷은 그 채널만 무시한다.
 *
 * 이력(③)은 DB가 진실원천이다. 라이브 경보가 오면 이력을 다시 당겨오므로
 * 표는 자동으로 따라온다 — 라이브분을 표에 직접 끼워 넣지 않는다(중복 방지).
 */
class AlertFeed : public QObject
{
    Q_OBJECT

public:
    enum Severity { None = 0, Warn = 1, Critical = 2 };

    /** @brief 이력 표 한 줄 (DB alerts 1행) */
    struct Event {
        QDateTime ts;               ///< 발생 시각 (로컬 변환됨)
        int channel = -1;
        Severity severity = None;   ///< incident의 **현재** 단계 (승급 반영)
        bool predicted = false;     ///< source_type == "prediction"
        bool resolved = false;      ///< incident.status == "resolved"
        int incident_id = 0;
        QString message;            ///< "[혼잡 critical] zone_A(ch1) 예측 8/10명 (80%)"
    };

    /** @brief 채널의 현재 단계 스냅샷 (LIVE 타일·ALERT 카드용) */
    struct State {
        Severity severity = None;
        bool predicted = false;
        int count = -1;             ///< 라이브 경보가 실어온 값 (-1 = 모름)
        int capacity = -1;
        QDateTime since;            ///< 이 단계가 된 시각
    };

    static AlertFeed *instance();

    // ---- 카메라/앱 경보 특수 키 (§4c-2) — 채널 0..3과 겹치지 않는 음수 ----
    static const int DEV_LINK = -1;  ///< 카메라 오프라인
    static const int DEV_APP = -2;   ///< 앱 죽음·AutoStart 구멍
    static const int DEV_RES = -3;   ///< 장비 자원 임계 지속

    /**
     * @brief 카메라/앱 경보 발행 (§4c-2) — CameraStatus가 부른다
     *
     * 혼잡 경보(채널)와 별개 소스지만 같은 인프라(팝업·내비 배지)를 탄다.
     * 같은 (단계, 메시지)의 재발행은 무시된다 — 상태 전이 때만 반영(스팸 금지).
     * 해제는 sev=None으로.
     */
    void raise_device_alert(int key, Severity sev, const QString &message);
    /** @brief 해당 단계인 장비 경보 수 (내비 배지 합산용) */
    int device_count(Severity sev) const;
    /** @brief min_sev 이상인 장비 경보 메시지들 (팝업 본문용) */
    QStringList device_messages(Severity min_sev) const;

    State state(int ch) const;
    Severity severity(int ch) const;
    /** @brief 전 채널·장비 중 가장 나쁜 단계 (내비 배지·전역 표시용) */
    Severity worst() const;
    /** @brief critical 수 — 채널 + 장비 경보 (0이면 배지 숨김) */
    int critical_count() const;

    const QVector<Event> &history() const { return m_history; }
    bool history_pending() const { return !m_req_id.isEmpty(); }

    /** @brief 이력 재조회 (기본 24시간). 응답은 history_changed 로 온다 */
    void request_history(int hours = 24);

signals:
    /** @brief 채널 단계가 바뀜 — 타일 색·배지 갱신용 */
    void state_changed();

    /** @brief 새 경보가 도착함 (해제 포함) — 배너·토스트 등 즉시 반응용 */
    void alert_raised(int channel, int severity);

    /** @brief 이력이 갱신됨 (도착 또는 실패) */
    void history_changed();

    /**
     * @brief 오디오 이벤트 경보 (비명/총성) — RPi C 감지기(`guardx/alert/rpic`)
     *
     * 혼잡 경보와 **완전히 독립**이다. 혼잡 상태머신(m_state)을 건드리지 않고
     * 이 신호만 발생시킨다 — AudioAlertPopup 이 받아서 표시한다.
     *
     * @param ts 사건 시각(payload). 수신 시각이 아니다 — 팝업이 이걸 그대로
     *           표시한다. 없는 payload 면 수신 시각으로 대체된다.
     */
    void audio_alert(int channel, const QString &event, double confidence,
                     const QDateTime &ts);

    /** @brief 장비/앱 경보 전이 (§4c-2) — key는 DEV_* 상수 */
    void device_alert(int key, int severity, const QString &message);

private:
    explicit AlertFeed(QObject *parent = nullptr);

    void on_live_alert(const QByteArray &payload);
    void on_incidents_snapshot(const QByteArray &payload);
    void on_audio_alert(const QByteArray &payload);
    void apply_history(const QJsonObject &reply);

    State m_state[4];
    qint64 m_last_live_ms[4] = { 0, 0, 0, 0 };  ///< 낡은 스냅샷 판별 기준
    QHash<int, Severity> m_device;              ///< §4c-2 장비/앱 경보 단계
    QHash<int, QString> m_device_msg;
    QVector<Event> m_history;
    QString m_req_id;                            ///< 진행 중인 이력 요청
    int m_hours = 24;
};
