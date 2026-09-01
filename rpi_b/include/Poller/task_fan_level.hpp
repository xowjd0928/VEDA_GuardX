#pragma once
#include <string>
// task_fan_level — 혼잡 판정 결과를 팬 단계로 내보낸다.
//
// 임계값을 새로 만들지 않는다. zone_thresholds(capacity_limit·warn_ratio·
// critical_ratio)로 task_alert이 이미 계산한 severity를 그대로 쓴다 — 같은
// 임계를 두 곳에 두면 화면의 혼잡 경보 단계와 팬 단계가 따로 논다.
//
// 듀티를 정하는 것은 여기가 아니라 RPi C다. 화재 시 무조건 0%라는 규칙
// 때문인데, 그 판단이 브로커 건너편에 있으면 링크가 끊긴 화재에서 팬이
// 계속 돈다. 규약은 shared/fan_protocol.h.

/**
 * @brief 경보 severity 문자열 -> 팬 단계
 *
 * VMS 가 받는 문자열과 **같은 값**에서 팬 단계를 뽑기 위한 것이다.
 * task_alert 은 mqttPublishAlert 에 넘기는 그 문자열을 그대로 여기에도
 * 넣는다 — 두 값이 갈라질 자리를 아예 없앤다.
 *
 * 숫자로 넘기지 않는 이유: sevOf() 의 순간값과 경보에 남은 단계는 서로
 * 다르다(히스테리시스). 순간값을 쓰면 화면은 critical 인데 팬은 75%로
 * 내려가 있는 상태가 생긴다.
 *
 *   "critical" -> 2   "warn" -> 1   "clear"/그 외 -> 0
 */
int fanLevelFromSeverityName(const std::string& severity);

/**
 * @brief 존 하나의 판정 결과를 기록한다 (task_alert이 부른다)
 *
 * @param level 0 정상 / 1 주의 / 2 위험. **-1 = 판단 불가**
 *              (임계 미설정·전 신호 stale). 불가는 0(정상)과 다르다 —
 *              0으로 뭉개면 값을 모르는 존이 최댓값 계산을 끌어내린다.
 */
void reportZoneLevel(int zone_id, int level);

/**
 * @brief 기록된 존들의 **최댓값**을 발행한다 (틱마다 1회)
 *
 * 팬은 노드에 하나뿐이라 존별로 다른 듀티를 낼 수 없다. 가장 나쁜 존을
 * 따르는 것은 화재 zone_bitmap을 "한 구역이라도 켜져 있으면 화재"로 다루는
 * 것과 같은 규칙이다.
 *
 * 값이 바뀔 때만 보내되 영원히 침묵하지는 않는다 — RPi C가 재기동하면
 * retained로 받지만, 브로커가 재시작해 retained가 날아간 경우를 대비해
 * 주기적으로 한 번은 다시 쏜다.
 */
void publishFanLevel();
