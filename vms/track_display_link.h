#pragma once

#include <QObject>
#include <QString>

class QTimer;

/**
 * @brief 추적 대상 지목을 RPi B로 보내는 통로 (싱글턴)
 *
 * TRACKING 패널의 [매트릭스 송출] 토글이 켜지면, 그 대상을 현장 LED
 * 매트릭스(STM32 HUB75 평면도)에 찍으라고 RPi B에 알린다.
 *
 * **좌표는 보내지 않는다.** 화면이 들고 있는 것은 그리기용 박스라 예측
 * 위치가 없다 — 카메라가 계산한 predicted_x/predicted_y(현재 위치 + 속도
 * x 2초)는 폴러를 거쳐 DB(detections.predicted_geom)에만 있다. 그래서
 * VMS는 "누구를 볼 것인가"만 정하고, 좌표 조회·평면도 변환·발행은 DB 옆에
 * 있는 RPi B(guardx_mqttd, task_track_display)가 한다.
 *
 * 이 클래스는 화면을 모른다. 반대로 TrackingPanel은 네트워크를 모른다
 * (그 위젯의 설계 주석 참조) — 둘을 잇는 것은 LiveViewer다. 화면 채널 ->
 * DB 채널 변환도 거기서 이뤄져야 매핑이 한 곳에만 남는다.
 *
 * ── 발행 (guardx/db/rpib/cmd/track_display) ──
 *   {"node_id":"vms","timestamp":1234567890,"action":"START",
 *    "global_id":0,"channel":0,"object_id":6461,"label":"P-6461"}
 *   {"node_id":"vms","timestamp":1234567890,"action":"STOP"}
 *
 * START는 KEEPALIVE_MS마다 되풀이해 보낸다. 한 번만 보내면 RPi B나
 * 브로커가 재시작했을 때 지목이 사라진 줄 아무도 모른 채 LED만 멎는다.
 * 반대로 VMS가 죽으면 재전송이 끊겨 RPi B가 스스로 무장을 푼다 — 죽은
 * 관제 PC가 가리키던 자리를 현장 LED가 계속 붙들고 있지 않게 하는 것이
 * 이 주기 재전송의 진짜 목적이다.
 */
class TrackDisplayLink : public QObject
{
    Q_OBJECT

public:
    static TrackDisplayLink *instance();

    /**
     * @brief 대상 지목 시작(또는 대상 교체)
     *
     * @param global_id  채널을 넘어 사람을 묶는 재식별 id. 0이면 미배정 —
     *                   이때만 channel/object_id로 대신 찾는다 (TrackId와
     *                   같은 분기).
     * @param db_channel DB detections.raw_channel 과 같은 축의 채널 번호.
     *                   호출자가 화면 채널을 변환해서 넘긴다.
     * @param label      로그·화면 표기용 ("P-6461"). 식별에는 쓰지 않는다.
     */
    void start(int global_id, int db_channel, int object_id,
               const QString &label);

    /** @brief 지목 해제. RPi B가 즉시 점을 지운다 */
    void stop();

    bool active() const { return m_active; }

signals:
    /** @brief 송출 상태 변화 (버튼 표기 동기화용) */
    void active_changed(bool active);

private:
    explicit TrackDisplayLink(QObject *parent = nullptr);

    /** @brief 현재 대상으로 START 한 건 발행 (주기 재전송도 여기로) */
    void publish_start();

    /// 브로커가 끊긴 동안의 START는 큐잉되지 않는다(MqttLink::publish 규약).
    /// 재전송 주기가 그 공백을 메운다 — 다시 붙으면 다음 주기에 복구된다.
    static const int KEEPALIVE_MS = 5000;

    QTimer *m_keepalive = nullptr;
    bool m_active = false;

    int m_global_id = 0;
    int m_channel = -1;
    int m_object_id = -1;
    QString m_label;

    long m_seq = 0;
};
