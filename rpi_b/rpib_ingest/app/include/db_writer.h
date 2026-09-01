#ifndef DB_WRITER_H
#define DB_WRITER_H

#include <stdint.h>

#include "guardx_err.h"
#include "sensor_parser.h"

/*
 * db_writer.h - rpib_ingest 전용 기록 (동기)
 *
 * 원래 rpib_engine의 db_queue.c(비동기 큐+워커 스레드)를 여기선 안 쓴다 -
 * ingest는 "받은 걸 그대로 저장"만 하고 그 결과를 기다리는 다음 판단이
 * 없으므로, 큐를 또 만드는 비용이 안 맞는다고 판단했다(2026-08-10 설계
 * 논의). 대신 INSERT ... ON CONFLICT로 rpib_decision과 안전하게 병합한다
 * (migration_sensor_reading_unique.sql). Postgres가 몇 초 느려지면 이
 * 콜백 스레드가 그만큼 블로킹되는 트레이드오프는 감수한다 - 원본의
 * JSONL 폴백(DB 다운 시에도 유실 없음)은 이 단순화에서 빠졌다.
 * 필요해지면 db_queue.c를 다시 가져와 붙이면 된다.
 */

guardx_err_t db_writer_open(void);
void         db_writer_close(void);

/* sensor_reading(zone_id, sensor_seq) UPSERT (composite_score는 NULL로
 * 시작 - rpib_decision이 나중에 채운다) + sensor_value 6행 UPSERT.
 * 하나의 트랜잭션. */
guardx_err_t db_write_sensor_raw(const sensor_msg_t *msg, uint64_t timestamp_ms,
                                 int zone_id);

guardx_err_t db_write_button(const button_msg_t *msg, uint64_t timestamp_ms,
                             int zone_id);

#endif /* DB_WRITER_H */
