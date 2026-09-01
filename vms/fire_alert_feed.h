#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

/**
 * @brief 화재/비상버튼 경보 수신기 (싱글턴) — rpib_engine 판정 결과의 소비자
 *
 * AlertFeed(혼잡)와 같은 edge+retained 이중화 패턴을 쓰되, 화재는 구역별이
 * 아니라 "사이트 전체 단일 상태"라 채널 배열 대신 State 하나만 유지한다
 * (decision.c가 전 센서를 하나로 종합 판정하므로 "몇 번 구역이 화재"가 아니라
 * "화재냐 아니냐" 하나뿐).
 *
 * ```
 * ① guardx/alert/fire            (edge, retain=false) — 확정/해제 전이 순간
 * ② guardx/db/rpib/fire_incident (retained)           — 지금 화재가 열려있는지 스냅샷
 * ③ guardx/alert/button          (edge, retain=false) — 비상 버튼 눌림
 * ```
 *
 * ①만으로는 안 되는 이유는 AlertFeed와 동일하다 — 화재 중에 VMS를 켜거나
 * 재접속하면 이미 진행 중인 화재를 영영 모른다. ②가 그 구멍을 막는다.
 * ②는 RPi B 상태 발행 틱(기본 30초)에만 재발행돼 ①보다 낡을 수 있어
 * timestamp로 방어한다(AlertFeed의 m_last_live_ms와 같은 방식).
 *
 * 버튼은 "상태"가 아니라 "사건"이라 스냅샷이 없다 — 눌린 순간만 의미 있고,
 * VMS가 꺼져 있던 동안의 버튼 이력을 재접속 시 복원하는 건 초기 범위 밖이다.
 */
class FireAlertFeed : public QObject
{
    Q_OBJECT

public:
    /** @brief 화재의 현재 단일 상태 (사이트 전체) */
    struct State {
        bool active = false;
        QString cause;       ///< sensor_channel.channel_key. 해제 시 빈 문자열
        QDateTime since;     ///< 이 상태가 된 시각(로컬)
        int zone_id = 0;     ///< fire_zone.zone_id. 0 = 아직 못 받음(표시용, 판정에는 안 씀)
    };

    static FireAlertFeed *instance();

    State state() const { return m_state; }

signals:
    /** @brief 화재 활성/해제가 바뀜 */
    void state_changed();
    /**
     * @brief 비상 버튼이 눌림 (매 눌림마다 발생, 상태 없음)
     * @param cumulative_count 이 VMS 세션이 켜진 뒤로 몇 번째 눌림인지 (1부터)
     * @param zone_id 눌린 버튼이 속한 zone (main.c의 button payload에 이미 실려 있음)
     *
     * RPi A가 payload에 싣는 press_count는 "직전 읽기 이후 새로 눌린 횟수"라
     * (드라이버가 read 시 리셋) 정상적인 단일 누름이면 거의 항상 1이다 —
     * "몇 번째 눌림인가"를 나타내지 않는다. 그래서 여기서는 그 필드를 쓰지
     * 않고 수신 횟수를 직접 센다.
     *
     * ⚠ VMS 세션 기준 카운트다 — 재시작하면 1부터 다시 세고, VMS가 여러 대면
     * 각자 따로 센다. 재시작에도 안 흔들리는 진짜 누적 값이 필요해지면
     * button_event_id(BIGSERIAL)를 DB에서 조회하는 방식으로 바꿔야 한다
     * (지금은 구현 단순성을 택함).
     */
    void button_pressed(int cumulative_count, int zone_id, const QDateTime &ts);

private:
    explicit FireAlertFeed(QObject *parent = nullptr);

    void on_live_alert(const QByteArray &payload);
    void on_incident_snapshot(const QByteArray &payload);
    void on_button(const QByteArray &payload);

    State m_state;
    qint64 m_last_live_ms = 0;   ///< 낡은 스냅샷 판별 기준
    int m_button_total = 0;      ///< 이 세션에서 수신한 버튼 이벤트 총수 (재시작 시 리셋)
};
