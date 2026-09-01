#ifndef ACTUATOR_REGISTRY_H
#define ACTUATOR_REGISTRY_H

/*
 * GuardX_App_Convention.md의 device registry를 액추에이터 방향으로
 * 적용한 것. RPi A가 OPEN_ALL/READ_ALL(주기 폴링)이었다면, RPi C는
 * OPEN_ALL/DISPATCH_CMD(명령 이벤트 단위)다.
 *
 *  - OPEN_ALL: 하나라도 실패하면 즉시 실패 (RPi A와 동일 정책.
 *    액추에이터는 "일부만 동작"이 더 위험하다 - 팬은 도는데 밸브를
 *    못 잠그는 상태로 기동을 허용하지 않는다)
 *  - DISPATCH_CMD: 명령 1건을 대응 액추에이터 핸들러로 라우팅.
 *    개별 실패는 에러 반환 + 로그로 격리하고 프로세스는 계속.
 */

#include <stdbool.h>
#include "guardx_hal.h"
#include "cmd_parser.h"

/* 전체 초기화 + 안전 상태 적용. 하나라도 open 실패하면 즉시 실패. */
guardx_err_t OPEN_ALL(void);

/* 파싱된 명령 1건 실행. 알 수 없는 command/action/value 범위 초과는
 * GUARDX_ERR_INVALID, 하드웨어 write 실패는 GUARDX_ERR_WRITE. */
guardx_err_t DISPATCH_CMD(const actuator_cmd_t *cmd);

/* Close the fire shutter when fire first becomes active. There is no matching
 * automatic OPEN on recovery; reopening is an explicit operator action. */
guardx_err_t actuator_shutter_close(void);

/* 안전 상태로 되돌린 뒤 전체 해제 */
void CLOSE_ALL(void);

#endif /* ACTUATOR_REGISTRY_H */
