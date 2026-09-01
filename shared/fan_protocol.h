#ifndef GUARDX_FAN_PROTOCOL_H
#define GUARDX_FAN_PROTOCOL_H

/*
 * GuardX 팬 자동 제어 규약.
 *
 * ── 왜 판단이 RPi C 에 있는가 ──
 * 혼잡 단계를 계산하는 것은 RPi B(zone_thresholds + task_alert)지만, 듀티를
 * 정해 팬에 넣는 것은 RPi C 다. 화재 시 무조건 0% 라는 규칙 때문이다 -
 * 그 판단이 브로커 건너편에 있으면 링크가 끊긴 화재에서 팬이 계속 돈다.
 * 안전 무효화는 액추에이터 옆에 둔다.
 *
 * 그래서 RPi B 는 "지금 몇 단계인가"만 보내고, RPi C 가
 *   화재 > AUTO > 수동
 * 순으로 최종 듀티를 정한다.
 *
 * ── 단계 -> 듀티 ──
 * 정상 40 / 주의 75 / 위험 90. 정상이 0 이 아닌 것은 화장실 환기라
 * 평상시에도 기본 환기가 돌아야 하기 때문이다 - AUTO 가 켜져 있으면
 * 화재를 빼고 팬이 완전히 멈추는 구간은 없다.
 *
 * 단계 경계(정원·warn_ratio·critical_ratio)는 zone_thresholds 에 있고 VMS
 * 에서 편집한다. 여기 새로 만들지 않는다 - 같은 임계를 두 곳에 두면 화면의
 * 혼잡 경보 단계와 팬 단계가 따로 논다.
 */

/*
 * RPi B -> RPi C. 지금 혼잡 단계. **retained**.
 *   {"node_id":"rpib","timestamp":N,"level":0|1|2}
 *
 * level 은 task_alert 의 sevOf() 결과와 같은 축이다(0 정상 / 1 주의 / 2 위험).
 * 존이 여럿이면 그중 **가장 나쁜 단계**를 보낸다 - 팬은 노드에 하나뿐이라
 * 존별로 다른 듀티를 낼 수 없고, 화재 zone_bitmap 을 "한 구역이라도 켜져
 * 있으면 화재"로 다루는 것과 같은 규칙이다.
 *
 * retained 인 이유: RPi C 가 재기동해도 현재 단계를 즉시 알아야 한다.
 * 안 그러면 다음 단계 변화까지 정상(40%)으로 돌아간다.
 */
#define GUARDX_FAN_LEVEL_TOPIC  "guardx/congestion/rpic/level"
#define GUARDX_FAN_LEVEL_QOS    1

/* RPi B -> VMS/RPi C. 혼잡 상태 전이 경보. retain=false, QoS 1.
 *   {"event":"congestion","channel":0..3,"severity":"warn|critical|clear",...}
 * VMS의 혼잡 경보와 RPi C의 혼잡 경고음은 같은 이벤트를 소비한다. */
#define GUARDX_CONGESTION_ALERT_TOPIC "guardx/alert/rpib"
#define GUARDX_CONGESTION_ALERT_QOS   1

/*
 * RPi C -> VMS. 팬이 지금 어떤 상태인가. **retained**.
 *   {"node_id":"rpic","timestamp":N,"auto":true,"level":1,
 *    "duty":75,"fire":false}
 *
 * VMS 는 이걸 보고 AUTO 토글 표시를 맞추고, AUTO 가 켜져 있으면 수동
 * ON/OFF/출력 위젯을 잠근다.
 *
 * ⚠ guardx/actuator/rpic/... 아래에 두지 않는다. RPi C 가 그 와일드카드를
 *   구독하므로 자기 발행을 도로 받아 명령으로 오인한다(ACK 무한 루프와
 *   같은 사고 - mqtt_sub.c 참조).
 */
#define GUARDX_FAN_STATE_TOPIC  "guardx/state/rpic/fan"
#define GUARDX_FAN_STATE_QOS    1

/* AUTO 켜고 끄기. 기존 액추에이터 명령 경로를 그대로 쓴다(ON/OFF).
 *   {"node_id":"rpic",...,"command":"fan_auto","action":"ON"} */
#define GUARDX_CMD_FAN_AUTO     "fan_auto"

/* 단계별 듀티(%) */
#define GUARDX_FAN_DUTY_NORMAL  40
#define GUARDX_FAN_DUTY_WARN    75
#define GUARDX_FAN_DUTY_CRIT    90

#endif /* GUARDX_FAN_PROTOCOL_H */
