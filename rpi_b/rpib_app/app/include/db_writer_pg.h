#ifndef DB_WRITER_PG_H
#define DB_WRITER_PG_H

#include "guardx_err.h"
#include "db_queue.h"

/*
 * db_writer_pg - PostgreSQL 기록 백엔드 (PHASE 5)
 *
 * db_writer.c의 워커 스레드에서만 호출된다. 콜백 스레드는 이 파일을
 * 모른다 - 판단 경로와 DB 사이에 큐가 있다는 PHASE 3의 구조가 여기서
 * 값을 한다. DB가 수십 초 멈춰도 판단은 계속 돈다.
 *
 * 접속 정보는 threshold_loader.c와 동일하게 libpq 표준 환경변수
 * (PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE)로 받는다. 다만 이쪽은
 * 연결을 유지한다 - 초당 1회 INSERT라 매번 맺고 끊을 수 없다.
 *   !!! PGCONNECT_TIMEOUT을 함께 설정할 것(권장 3초). 기본값은 OS의
 *   TCP 타임아웃까지 기다려서, 재연결 시도 한 번에 워커가 수십 초
 *   갇히고 그동안 큐가 차서 드롭이 시작된다 !!!
 */

guardx_err_t pg_writer_open(void);
void         pg_writer_close(void);

/* 레코드 1건 기록. 반환값이 GUARDX_OK가 아니면 호출측(db_writer.c)이
 * JSONL로 폴백해야 한다 - 연결 장애든 데이터 오류든 마찬가지다.
 * "일단 어딘가에는 남긴다"가 원칙이고, 나중에 backfill_jsonl.sql로
 * 복구한다. */
guardx_err_t pg_write_record(const db_record_t *rec);

#endif /* DB_WRITER_PG_H */
