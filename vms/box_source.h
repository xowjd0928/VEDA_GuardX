#pragma once

#include <QObject>
#include <QVector>
#include <QDateTime>
class QTimer;

/**
 * @brief "촬영 시각이 미래" 허용치 — **카메라 시계끼리** 비교할 때
 *
 * HTTP 피드 전용이다: 감지 `ts` 를 같은 응답의 `served_utc` 와 견준다. 둘 다
 * 카메라가 찍은 값이라 시계차가 개입하지 않으므로 좁게 잡을 수 있다.
 * 5000ms 인 근거는 절삭이다 — `served_utc` 가 초 단위로 잘려서 오므로
 * (01:29:17.000) 같은 초 안의 최신 감지는 서빙 시각보다 커 보인다. 여유 없이
 * 자르면 가장 신선한 데이터를 버린다.
 */
inline constexpr int DETECTION_FUTURE_TOLERANCE_MS = 5000;

/**
 * @brief 같은 판정을 **PC 시계와 대조**할 때의 허용치 (ONVIF 경로·TrackHistory)
 *
 * 카메라측 기준 시각이 없는 경로는 PC 시계와 견줄 수밖에 없고, 그러면 두 시계의
 * 차이가 통째로 오차로 들어온다. 그래서 위 값과 **같은 숫자를 쓰면 안 된다.**
 *
 * ⚠ 2026-08-10 실측 — 이 현장의 카메라 시계는 PC보다 **최대 3.3초 앞선다**:
 * `[DirectSink] glass-to-sink` 원시값(도착−촬영)이 4채널 모두 −3.0 ~ −3.3초로
 * 수렴했고, RTCP SR 매핑이 자리잡기 전 구간에선 −3295 ~ +3584ms 로 흔들렸다.
 * 즉 정상 프레임의 UtcTime 이 PC 시계 기준 3.3초 미래로 보인다. 5초 허용치를
 * 그대로 쓰면 여유가 1.7초뿐이라, 시계가 조금만 더 벌어져도 **정상 박스가
 * 전멸**한다(ONVIF 록스텝 경로가 통째로 HTTP 폴백으로 떨어진다).
 *
 * 60초인 이유: 막으려는 것은 "몇 초 어긋남"이 아니라 **손상된 값**이다 —
 * 파싱 사고나 잘못된 epoch 는 분·시간 단위로 튄다. 그리고 이 값은 곧
 * **피해 상한**이기도 하다: TrackHistory 의 시계는 되돌아오지 않으므로,
 * 통과한 오염이 스스로 낫는 데 걸리는 시간이 정확히 이 값이다.
 */
inline constexpr int DETECTION_PC_CLOCK_SKEW_MS = 60000;

/**
 * @brief 감지 박스 하나 (DB detections 한 행)
 *
 * rect 좌표는 카메라 원본 해상도(2592x1520) 기준.
 * object_id는 박스 클릭 -> 추적 기능에서 사용한다.
 */
struct DetectionBox {
    int object_id;
    int sx;
    int sy;
    int ex;
    int ey;
    /**
     * 이 감지가 촬영된 시각 (카메라 시계).
     *
     * ⚠ **미래로 튄 값은 어떤 경로로도 들어오면 안 된다.** 소비자들이 "본 적
     * 있는 최대 ts = 지금"으로 시계를 세우기 때문에(`TrackHistory::m_clock`)
     * 한 점이 저장소를 영구 오염시킨다 — 그 시계는 되돌아오지 않는다.
     * 판정은 DETECTION_FUTURE_TOLERANCE_MS 로 한다.
     */
    QDateTime ts;
    int category = 1;  // v15: 1=Human, 2=Face, 3=Head (구버전 응답엔 없음 → 1)
    int parent_id = 0; // v15: Face/Head가 속한 사람의 object_id (Human은 0)

    // dead-reckoning (2026-08-05): test 앱이 산출한 속도(px/s, 원본 좌표계)와
    // 서빙 시점 기준 좌표 나이(served_ms − last_seen_ms). WiseAI가 5Hz라
    // 좌표는 200ms 계단인데, 이 속도로 계단 사이를 전진시킨다.
    // /detections(Face/Head)엔 없는 필드 → 0 유지, 사람(parent) 속도를 빌린다.
    double vx = 0;
    double vy = 0;
    int lost_ms = 0;

    /**
     * 채널을 넘어 같은 사람을 하나로 묶는 재식별 id. 동선(TrackHistory)의
     * 기준 키다 — object_id는 채널 안에서만 고유해서 CH1의 3번과 CH2의 3번이
     * 남남이다.
     *
     * 0 = 아직 배정 안 됨. 수신부가 실어 보내기 전까지는 항상 0이고,
     * 그 동안 동선은 (channel, object_id)로 대신 묶인다 (TrackId 참조).
     */
    int global_id = 0;
};


/**
 * @brief DB(detections)를 주기적으로 폴링해 최신 박스를 알려주는 공급자
 *
 * 위젯이 아니라 순수 데이터 공급자다. 화면에 어떻게 그릴지는 신경쓰지 않고,
 * 갱신될 때마다 boxes_updated 시그널로 결과만 넘긴다.
 */
class BoxSource : public QObject
{
    Q_OBJECT

public:
    /**
     * @param channel DB detections.channel 값
     */
    explicit BoxSource(int channel, QObject *parent = nullptr);

signals:
    /**
     * @brief 새 박스 도착 (frame_utc_ms: ONVIF 프레임 시각, HTTP 스냅샷은 -1)
     */
    void boxes_updated(const QVector<DetectionBox> &boxes, qint64 frame_utc_ms);

private:
    int m_channel;
};