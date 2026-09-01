#ifndef FAN_CONTROL_H
#define FAN_CONTROL_H

#include <stdbool.h>

#include "guardx_err.h"

/*
 * fan_control.h - 팬 듀티를 최종적으로 정하는 곳.
 *
 * 우선순위는 하나뿐이다:
 *
 *   화재  >  AUTO  >  수동
 *
 * 화재 중에는 AUTO 든 수동이든 무조건 0% 다. 이 판단이 RPi B 나 VMS 에
 * 있으면 링크가 끊긴 화재에서 팬이 계속 돈다 - 안전 무효화는 액추에이터
 * 옆에 있어야 한다. 규약 배경은 shared/fan_protocol.h 참조.
 *
 * ── AUTO ON ──
 * 혼잡 단계(RPi B 가 보내주는 0/1/2)를 40 / 75 / 90 % 로 옮긴다. 수동
 * 명령은 거절한다 - VMS 도 위젯을 잠그지만 그건 표면이고, 다른 경로로
 * 들어온 명령까지 막으려면 여기서도 막아야 한다.
 *
 * ── AUTO OFF ──
 * 자동 갱신을 멈추고 **현재 출력을 그대로 둔다.** 끄는 순간 0 으로
 * 떨어뜨리면 운영자가 수동으로 넘긴 것뿐인데 환기가 끊긴다.
 *
 * ── 화재 해제 ──
 * AUTO ON 이면 자동 제어를 재개한다. AUTO OFF 면 0% 인 채로 두고 수동
 * 조작만 풀어준다 - 아무도 안 눌렀는데 팬이 스스로 되살아나지 않는다.
 */

/* 팬을 안전 상태(0%)로 맞추고 상태를 발행한다. OPEN_ALL 뒤에 부른다. */
guardx_err_t fan_control_init(void);

/* 혼잡 단계 수신 (0 정상 / 1 주의 / 2 위험). 범위 밖은 무시한다. */
void fan_control_set_level(int level);

/* 화재 표시. 값이 같아도 무방하다 - 전이만 골라 처리한다. */
void fan_control_set_fire(bool active);

/* AUTO 켜기/끄기 (fan_auto 명령). */
void fan_control_set_auto(bool on);

/* AUTO 가 켜져 있는가. 수동 명령을 거절할지 판단하는 데 쓴다. */
bool fan_control_auto_enabled(void);

/*
 * 수동 듀티 적용 (fan 명령). AUTO 중이거나 화재 중이면 거절한다.
 *
 * 거절을 조용히 하지 않는 이유: 명령을 보낸 쪽은 적용됐다고 믿는데 팬은
 * 안 움직이는 상태가 제일 나쁘다. 로그를 남기고 오류를 돌려준다.
 */
guardx_err_t fan_control_manual(int duty);

/* 현재 상태를 retained 로 다시 발행한다. 브로커 재접속 직후에 부른다. */
void fan_control_republish(void);

#endif /* FAN_CONTROL_H */
