#ifndef DB_WRITER_H
#define DB_WRITER_H

#include <stdbool.h>
#include <stdint.h>

#include "guardx_err.h"

/*
 * db_writer.h - rpib_dispatch 전용 기록 (동기)
 *
 * fire_event_command 1행만 쓴다 - event_id는 rpib_decision이 내부
 * 메시지에 실어 보낸 값을 그대로 쓴다(자체 조회 없음).
 */

guardx_err_t db_writer_open(void);
void         db_writer_close(void);

guardx_err_t db_write_command(long long event_id, const char *command_key,
                              const char *action, int value, bool has_value,
                              uint32_t published_seq);

#endif /* DB_WRITER_H */
