#ifndef DB_WRITER_H
#define DB_WRITER_H

#include <stdint.h>

#include "decision.h"
#include "guardx_err.h"

/*
 * db_writer.h - rpib_decision 전용 기록 (동기)
 *
 * rpib_ingest/db_writer.h와 같은 이유로 비동기 큐를 안 쓴다. 추가로
 * 여기는 db_write_transition()이 event_id를 **동기로 돌려줘야** 한다 -
 * 원본(rpib_engine)은 같은 프로세스 안이라 db_writer_pg.c의 static
 * last_event_id로 fire_event_command가 "직전 이벤트"를 암묵적으로
 * 찾았지만, 이제 그 기록은 rpib_dispatch(다른 프로세스)가 하므로
 * event_id를 fire_internal.h 메시지에 명시적으로 실어 보내야 한다.
 */

guardx_err_t db_writer_open(void);
void         db_writer_close(void);

/* sensor_reading(zone_id, sensor_seq) UPSERT - composite_score만 채운다
 * (원시값 컬럼은 rpib_ingest 몫). score<0이면 NULL로 기록(판단이 그
 * 사이클엔 점수를 안 낸 경우 - decision.h 규약과 동일). */
guardx_err_t db_write_sensor_score(int zone_id, uint32_t sensor_seq, float score,
                                   uint64_t timestamp_ms);

/* fire_event 1건 기록, 성공 시 *out_event_id에 생성된 event_id를 채운다 -
 * rpib_dispatch로 보낼 fire_cmd_msg_t.event_id가 여기서 나온다. */
guardx_err_t db_write_transition(int zone_id, decision_event_t ev,
                                 decision_cause_t cause, uint32_t trigger_seq,
                                 uint64_t timestamp_ms, long long *out_event_id);

#endif /* DB_WRITER_H */
