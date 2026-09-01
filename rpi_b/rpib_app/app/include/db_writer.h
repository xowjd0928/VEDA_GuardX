#ifndef DB_WRITER_H
#define DB_WRITER_H

#include <stdint.h>
#include "guardx_err.h"
#include "sensor_parser.h"
#include "decision.h"

/*
 * DB 라이터 - 현재는 PostgreSQL 스텁.
 *
 * ERD/스키마가 미확정(README "알려진 미확정 사항")이라, 지금은
 * JSON Lines 파일로 append만 한다. 스키마가 확정되면 이 인터페이스
 * 뒤의 구현(db_writer.c)만 libpq로 교체하면 된다 - main.c는 그대로.
 *
 * 기록 대상 (규약의 QoS 결정 근거와 대응):
 *  - 센서 사이클:  SENSORS 테이블 예정분
 *  - 버튼 이벤트:  QoS2로 받은 "정확히 1회" 로그 (중복 기록 금지 요구)
 *  - 화재/해제:    판단 로직의 상태 전이 (감사 추적용)
 */

/* 기본 기록 경로. systemd 배포 시 WorkingDirectory 기준 상대경로.
 * !!! 프로토타입용 - 1Hz 센서 기록이 그대로 쌓이므로 운영 전환 시
 * PostgreSQL 교체 또는 최소 logrotate 필요 !!! */
#define DB_WRITER_PATH "rpib_events.jsonl"

guardx_err_t db_writer_open(void);
void         db_writer_close(void);

/* PHASE 3: 아래 함수들은 이제 "기록한다"가 아니라 "기록을 맡긴다"이다.
 * 큐에 넣고 즉시 반환하며, 실제 파일 쓰기는 워커 스레드가 한다
 * (db_queue.h 참조). 따라서 반환값 GUARDX_OK는 "적재 성공"이지
 * "기록 성공"이 아니다 - 기록 실패는 워커가 로그로만 알린다.
 * 호출측이 알아야 할 것은 여전히 없다: 기록 실패는 판단을 막지 않는다.
 *
 * score는 그 사이클의 종합 위험도(0~100). 판단이 점수를 내지 않은
 * 사이클(FIRE 상태에서 센서 무효로 동결)은 음수를 넘기면 되고, 기록에
 * null로 남는다. DECISION_LAST_SCORE()가 그 규약대로 반환한다. */
/* zone_id: fire_zone.zone_id - 어느 zone에서 온 사이클/사건인지
 * (PHASE 6, main.c가 도착한 토픽의 node_id로 찾아 넘긴다). */
guardx_err_t db_write_sensor(const sensor_msg_t *msg, uint64_t timestamp_ms,
                             float score, int zone_id);
guardx_err_t db_write_button(const button_msg_t *msg, uint64_t timestamp_ms,
                             int zone_id);
guardx_err_t db_write_transition(decision_event_t ev, decision_cause_t cause,
                                 uint32_t trigger_seq, uint64_t timestamp_ms,
                                 int zone_id);

/* PHASE 5: 실제로 발행에 성공한 제어 명령 1건 (fire_event_command).
 * 반드시 db_write_transition() 뒤에 호출할 것 - 소속 fire_event를
 * 따로 싣지 않고 "직전에 기록된 전이"에 붙이기 때문이다(db_queue.h 참조).
 * command_key는 guardx_protocol.h의 GUARDX_CMD_* 를 그대로 넘긴다.
 * ON/OFF처럼 값이 없는 명령은 has_value=false (DB에 NULL). */
guardx_err_t db_write_command(const char *command_key, const char *action,
                              int value, bool has_value,
                              uint32_t published_seq, uint64_t timestamp_ms);

#endif /* DB_WRITER_H */
