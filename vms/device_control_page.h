#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>

class QLabel;
class QDoubleSpinBox;
class QPushButton;
class QStackedWidget;
class QTimer;
class RiskGauge;

/**
 * @brief DEVICE CONTROL 화면 — RPi A 환경센서 표시 + RPi C/STM32 액추에이터 수동 제어
 *
 * VMS는 RPi A/C와 직접 통신하지 않는다(fire_schema.sql "설계 확정: VMS→B→C").
 * 이 화면도 그 원칙을 따른다:
 *
 *   센서 표시  : ZoneSensorStore (guardx/db/rpib/sensors 구독을 대신 해준다).
 *                zone별 최신값·이력·더미 생성이 전부 거기 모여 있고, 이 화면은
 *                읽어서 그리기만 한다.
 *   THRESHOLD  : 위 Store가 함께 들고 있는 fire_threshold — 게이지·그래프 색
 *                (파랑/노랑/빨강)의 기준.
 *   액추에이터 : guardx/db/rpib/cmd/set_actuator 로 명령 요청 → RPi B가
 *                검증 후 manual_command에 기록하고 guardx/actuator/rpic로
 *                실제 발행. ACK 토픽이 미사용이라 VMS는 "RPi B가 접수했다"
 *                까지만 확인할 수 있다 — 버튼은 낙관적 토글이다.
 *   화재 해제  : guardx/cmd/rpib/clear_fire (PHASE 7에서 자동 해제를 없앤 뒤의
 *                유일한 해제 경로).
 *
 * ── 두 가지 보기 모드 (REPORT 화면과 같은 구조) ──
 *   ZONE 모드    : 구역 하나를 골라 센서 6타일 + 액추에이터 + 화재 해제
 *   COMPARE 모드 : 구역 여러 개를 골라 센서 6그래프에 구역별 선을 겹쳐 본다.
 *                  **액추에이터·화재 해제는 숨긴다** — 물리 명령은 대상 구역이
 *                  명확해야 하는데, 여러 구역을 섞어 보는 화면에서는 그
 *                  명확함이 깨지기 때문이다(더미 구역에서 명령을 막은 것과
 *                  같은 원칙).
 *
 * 센서 raw 값의 ppm 환산·위험 판정은 하지 않는다(RPi A 발행 규약의 raw
 * 정책, RPi B decision.c 책임). 여기서는 값과 유효성만 그대로 보여준다.
 */
class DeviceControlPage : public QWidget
{
    Q_OBJECT

public:
    explicit DeviceControlPage(QWidget *parent = nullptr);

    /** @brief 해당 구역을 ZONE 모드로 띄운다 (화재 팝업에서 넘어올 때).
     *  모르는 zone_id면 아무것도 하지 않는다 — 엉뚱한 구역을 여느니 그대로 둔다. */
    void show_zone(int zone_id);

    /** @brief RpiAlertPopup 등 외부에서 어느 노드가 바뀌었는지 구분하는 식별자 */
    enum class Node { A, B, C };

signals:
    /**
     * @brief RPi A/B/C의 온라인/오프라인이 **실제로 바뀔 때만** 발생
     *
     * check_staleness()/on_mqtt_online()/on_rpic_status() 세 곳 모두 매
     * 갱신마다 상태를 다시 계산해 재확인하지만, 그때마다 쏘면 팝업이 반복
     * 재오픈된다 — 이전 값과 달라졌을 때만 emit한다.
     */
    void node_state_changed(DeviceControlPage::Node node, bool online);

private:
    void build_ui();
    QWidget *build_header();
    QWidget *build_node_strip();
    QWidget *build_single_view();
    QWidget *build_compare_view();
    QWidget *build_sensor_panel();
    QWidget *build_actuator_panel();

    /** @brief Store 갱신 수신 — 지금 보고 있는 구역이면 다시 그린다 */
    void on_zone_updated(int zone_id);

    /** @brief ZONE 모드 6타일 갱신 (값·게이지·그래프) */
    void refresh_single();
    /** @brief COMPARE 모드 6그래프 갱신 (구역별 오버레이 + 범례) */
    void refresh_compare();

    /** @brief 1초 주기 — 마지막 센서 수신 이후 경과로 RPi A 온라인 여부 판정 */
    void check_staleness();

    /** @brief 지금 화면이 실제로 보고 있는 구역들.
     *  ZONE 모드면 선택 구역 하나, COMPARE면 칩으로 켠 구역 전부.
     *  신선도 판정이 이 목록을 따라야 "보고 있는데 끊긴 줄 모르는" 일이 없다. */
    QList<int> watched_zones() const;

    /** @brief MqttLink 온라인 상태 변화 → RPi B 점 갱신 */
    void on_mqtt_online(bool online);

    /**
     * @brief 노드 점의 **의미** — 점은 색이 아니라 이 값을 기억한다
     *
     * 마지막 QColor를 들고 있으면 화면 테마를 바꿨을 때 옛 팔레트의 색이 그대로
     * 굳는다. 상태를 저장해 두고 칠할 때마다 다시 판정해야 같은 뜻이 새 테마의
     * 색으로 나온다.
     *
     * 3가지를 두지만 화면엔 2색만 쓴다(paint_dot) — Online만 초록, Offline과
     * Unknown("아직 한 번도 못 받음")은 둘 다 빨강. 구분은 로그/디버깅용으로
     * 남겨둔다.
     */
    enum class NodeState { Unknown, Online, Offline };

    /**
     * @brief 노드 상태를 로그용 문자열로 (2026-08-10)
     *
     * 노드 점·경보 전이가 아무 기록도 안 남겨서, 오탐 팝업이 떴을 때 "센서가
     * 늦은 건지 브로커가 끊긴 건지"를 대조할 방법이 없었다. 전이마다 사유를 남긴다.
     */
    static QString node_state_text(NodeState s);

    /** @brief 점 하나를 의미대로 칠한다 (상태 기록은 호출부에서) */
    void paint_dot(QLabel *dot, NodeState state);

    /**
     * @brief 액추에이터 명령 발행 — guardx/db/rpib/cmd/set_actuator
     * @param value SET 계열만 사용 (onoff면 무시)
     */
    void send_actuator(const QString &command_key, const QString &action, int value = 0);

    /**
     * @brief RPi C 액추에이터 ACK 수신 — guardx/actuator/rpic/ack
     *
     * result=="ok"일 때만 state_label을 채운다. 실패 ACK로 라벨을 덮으면
     * "성공한 값"이 아니라 "시도한 값"이 남아 다음에 실제로 뭐가 켜져
     * 있는지 화면과 현실이 갈린다.
     */
    void on_actuator_ack(const QByteArray &payload);

    /** @brief RPi C 생존 신호 수신 — guardx/status/rpic (LWT, retain) */
    void on_rpic_status(const QByteArray &payload);

    /** @brief 수동 화재 해제 발행 — guardx/cmd/rpib/clear_fire */
    void send_clear_fire();

    /** @brief FireAlertFeed 상태 변화 → 선택 구역의 화재 배너·해제 버튼 갱신 */
    void on_fire_state_changed();

    /**
     * @brief RPi C 가 알려주는 팬 상태(retained) 수신
     *
     * AUTO 여부·단계·현재 듀티가 온다. 화면이 스스로 기억하지 않고 이
     * 값을 따르는 이유: 판단은 RPi C 가 하고(화재 > AUTO > 수동), VMS 가
     * 여러 대일 수 있어 한 대의 기억이 정답이 아니기 때문이다.
     */
    void on_fan_state(const QByteArray &payload);

    void set_status(const QString &text, bool error = false);
    /** @brief 액추에이터 명령 결과 전용 — set_status와 분리(센서가 1초마다 갱신돼 명령 결과가 바로 덮여씌워짐) */
    void set_actuator_status(const QString &text, bool error = false);

    /** @brief 센서 채널 하나의 표시 위젯 묶음 (ZONE 모드) */
    struct SensorTile {
        QLabel *value_label = nullptr;
        QLabel *state_label = nullptr;
        QWidget *gauge = nullptr;   // GaugeBar*, 전방선언 회피용으로 QWidget*
        QWidget *chart = nullptr;   // MiniLineChart*, 상동
    };

    /** @brief 센서 채널 하나의 비교 위젯 묶음 (COMPARE 모드) */
    struct CompareTile {
        QWidget *chart = nullptr;   // MiniLineChart*
        QLabel *legend = nullptr;   // "Z1 812 · Z2 530"
    };

    /** @brief 액추에이터 한 줄의 표시/입력 위젯 묶음 */
    struct ActuatorRow {
        QString command_key;
        QString kind;                    // "onoff" | "set" | "both" | "shutter"
        QLabel *state_label = nullptr;
        /// onoff/both 공통 — 토글 하나가 아니라 독립 버튼 둘이다. ON/OFF를
        /// 한 버튼으로 묶으면 VMS가 실제 상태를 모르는 채로 "다음 클릭이
        /// 뭘 보낼지"를 추측해야 한다(r.on 플래그, ACK 미수신이라 검증 불가).
        /// 각 버튼이 자기 명령만 보내면 그 추측이 필요 없다.
        /// 화재 중엔 btn_off와 함께 잠근다(on_fire_state_changed) — 자동
        /// 제어(rpib_engine)와 수동 명령이 충돌하는 걸 막는다.
        QPushButton *btn_on = nullptr;
        QPushButton *btn_off = nullptr;
        QDoubleSpinBox *value_box = nullptr; // set/both 만
        QPushButton *apply = nullptr;        // set/both 만 (value 전송)
        /// shutter 만 — OPEN/CLOSE/STOP. 예전엔 로컬 변수로 만들어 레이아웃에
        /// 바로 붙였는데, 그러면 화재 잠금이 **잡을 손잡이가 없다.**
        /// 실제로 08-10 리뷰에서 화재 중 셔터가 안 잠기는 구멍으로 잡혔다.
        QList<QPushButton *> btns_shutter;
        /// fan 만 — 자동 제어 토글. 화재·권한으로는 잠기지만 AUTO 자신은
        /// AUTO 상태로 잠기지 않는다(잠기면 끌 방법이 없어진다).
        QPushButton *btn_auto = nullptr;
    };

    /// RPi C 가 알려준 팬 상태. 화면은 이 값만 따른다.
    bool m_fan_auto = false;
    int m_fan_level = 0;
    int m_fan_duty = 0;

    QHash<QString, SensorTile> m_sensors;     // channel_key -> 위젯
    QHash<QString, CompareTile> m_compares;   // channel_key -> 위젯
    QHash<QString, ActuatorRow> m_actuators;  // command_key -> 위젯

    QStackedWidget *m_stack = nullptr;
    QWidget *m_actuator_panel = nullptr;   ///< COMPARE에서 숨긴다

    /// 지금 보고 있는 구역 (ZONE 모드). 액추에이터·화재 해제의 대상이기도 하다.
    int m_zone_id = 1;
    bool m_compare = false;
    QHash<int, bool> m_sel;                ///< COMPARE 선택 상태 (최소 1개 불변식)
    QHash<int, QPushButton *> m_zone_btns; ///< ZONE 셀렉터
    QHash<int, QPushButton *> m_chips;     ///< COMPARE 칩
    QPushButton *m_btn_compare = nullptr;

    QLabel *m_dot_a = nullptr;   // RPi A — 센서 수신 신선도로 판정
    QLabel *m_dot_b = nullptr;   // RPi B — MqttLink::online()
    QLabel *m_dot_c = nullptr;   // RPi C — guardx/status/rpic LWT
    /// 점 A·B·C의 마지막 의미. 테마 전환 때 이 값으로 다시 판정해 칠한다.
    NodeState m_node_a = NodeState::Unknown;
    NodeState m_node_b = NodeState::Unknown;
    NodeState m_node_c = NodeState::Unknown;

    /**
     * @brief 점(즉시 표시)과 팝업(장애 주장)을 분리하는 상태 (2026-08-10)
     *
     * 둘은 하는 일이 다르다:
     *  - **점**: "지금 신선한가" — STALE_MS(5s) 기준으로 즉시 칠한다.
     *    TopBar(top_bar.cpp:326)가 같은 기준을 쓰므로 두 지시기가 어긋나지 않는다.
     *  - **팝업**: "장애가 났다"는 주장이고 걸쇠(pending)라 운영자가 손으로
     *    닫아야 한다. 1Hz 발행에서 5초 공백 한 번에 이걸 띄우면 망이 조금
     *    출렁일 때마다 팝업이 뜬다 → **연속 3회(약 15초)** 를 요구한다.
     *
     * ⚠ 이 둘을 "일관성"이라는 이유로 합치지 말 것 — 의도된 분리다.
     */
    static constexpr int STALE_STREAK_FOR_ALERT = 3;
    int m_stale_streak = 0;          ///< 연속 stale tick 수 (1초 주기)
    bool m_alerted_a_offline = false; ///< 팝업에 마지막으로 알린 A 상태
    QLabel *m_status = nullptr;            // 센서 갱신 등 일반 상태 — 1초마다 바뀜
    QLabel *m_actuator_status = nullptr;   // 액추에이터 명령 결과 전용 — 클릭할 때만 바뀜
    /// 위 상태줄의 마지막 성격(실패인가) — 테마 전환 때 같은 색으로 되칠하려면
    /// 색이 아니라 이 판정을 남겨야 한다.
    bool m_act_error = false;
    /// 일반 상태줄이 마지막에 오류였나 — 테마 전환 때 같은 의미로 되칠하려고
    /// 남긴다 (색을 값으로 들고 있으면 옛 팔레트의 색이 굳는다)
    bool m_status_error = false;

    QPushButton *m_btn_clear_fire = nullptr;
    /// 화재 알림 배너. **스택 바깥(페이지 레벨)에 둔다** — 액추에이터 패널
    /// 안에 있으면 COMPARE 모드에서 패널째 숨겨져 정작 불이 났을 때 안 보인다.
    /// 배너는 "알림"이라 모드·선택 구역과 무관하게 떠야 하고, 해제 "조작"만
    /// 대상이 명확한 ZONE 모드에 남는다.
    QLabel *m_fire_state = nullptr;

    /// ZONE 모드 — 센서 패널 상단의 큰 위험도 게이지. 6개 센서를 종합한
    /// 결과라 그 타일들 바로 위가 의미상 제자리다.
    RiskGauge *m_risk = nullptr;
    QLabel *m_risk_note = nullptr;
    /// COMPARE 모드 — 구역마다 작은 게이지. "어느 구역이 제일 위험한가"가
    /// 비교 화면에서 제일 먼저 알고 싶은 것이라 칩 줄 옆에 나란히 둔다.
    QHash<int, RiskGauge *> m_risk_mini;

    QTimer *m_stale_timer = nullptr;
};
