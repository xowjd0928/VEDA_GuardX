/*
 * db_writer.c - rpib_ingest 동기 기록 (libpq 직접 호출)
 *
 * db_writer.h 주석 참조 - 비동기 큐 없이 콜백 스레드에서 바로 쓴다.
 * SQL 파라미터 바인딩 방식은 원본 rpib_app/app/src/db_writer_pg.c와
 * 동일(PQexecParams, 텍스트 파라미터) - 그쪽에서 검증된 패턴 그대로.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libpq-fe.h>

#include "db_writer.h"

static PGconn *conn;
static time_t  last_attempt;

#define PG_RETRY_SEC 5

static int ensure_conn(void)
{
    time_t now;

    if (conn && PQstatus(conn) == CONNECTION_OK)
        return 1;

    now = time(NULL);
    if (now - last_attempt < PG_RETRY_SEC)
        return 0;
    last_attempt = now;

    if (conn) {
        PQreset(conn);
        if (PQstatus(conn) == CONNECTION_OK) {
            fprintf(stderr, "db: pg reconnected\n");
            return 1;
        }
        return 0;
    }

    conn = PQconnectdb("");
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "db: pg connect failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        conn = NULL;
        return 0;
    }
    printf("db: pg connected\n");
    return 1;
}

static void log_fail(const char *what)
{
    fprintf(stderr, "db: pg %s failed: %s", what,
            conn ? PQerrorMessage(conn) : "(no connection)\n");
}

static void rollback(void)
{
    PGresult *res = PQexec(conn, "ROLLBACK");
    PQclear(res);
}

/* 항상 새 행을 만든다(원본 rpib_engine과 동일한 평범한 INSERT) -
 * composite_score는 아예 언급 안 하므로 NULL로 시작한다.
 * rpib_decision은 sensor_seq로 이 행을 찾지 않는다(RPi A 재시작마다
 * 리셋되는 카운터라 전역 유일하지 않음 - fire_schema.sql 주석 참조) -
 * 대신 "이 zone의 최신 reading_id"를 갱신한다(db_writer.c 원본 UPDATE
 * 참조 없음, rpib_decision/app/src/db_writer.c가 그 로직). */
#define SQL_READING \
    "INSERT INTO sensor_reading (zone_id, sensor_seq, received_at) " \
    "VALUES ($1::smallint, $2::bigint, to_timestamp($3::bigint / 1000.0)) " \
    "RETURNING reading_id"

#define SQL_VALUES \
    "INSERT INTO sensor_value (reading_id, channel_id, value, is_valid) VALUES " \
    "($1::bigint, 1, $2::real,  $3::boolean), " \
    "($1::bigint, 2, $4::real,  $5::boolean), " \
    "($1::bigint, 3, $6::real,  $7::boolean), " \
    "($1::bigint, 4, $8::real,  $9::boolean), " \
    "($1::bigint, 5, $10::real, $11::boolean), " \
    "($1::bigint, 6, $12::real, $13::boolean)"

#define SQL_BUTTON \
    "INSERT INTO button_event (zone_id, sensor_seq, press_count, occurred_at) " \
    "VALUES ($1::smallint, $2::bigint, $3::int, to_timestamp($4::bigint / 1000.0))"

guardx_err_t db_writer_open(void)
{
    last_attempt = 0;
    if (!ensure_conn())
        fprintf(stderr, "db: pg unavailable at startup, will retry per record\n");
    return GUARDX_OK;
}

void db_writer_close(void)
{
    if (conn) {
        PQfinish(conn);
        conn = NULL;
    }
}

guardx_err_t db_write_sensor_raw(const sensor_msg_t *m, uint64_t timestamp_ms,
                                 int zone_id)
{
    char b_zone[8], b_seq[24], b_ts[24], b_rid[24];
    char b_val[6][32];
    const char *p1[3], *p2[13];
    const char *valid[6];
    PGresult *res;
    int i;

    if (!ensure_conn())
        return GUARDX_ERR_OPEN;

    snprintf(b_zone, sizeof(b_zone), "%d", zone_id);
    snprintf(b_seq, sizeof(b_seq), "%u", m->seq);
    snprintf(b_ts, sizeof(b_ts), "%llu", (unsigned long long)timestamp_ms);
    p1[0] = b_zone;
    p1[1] = b_seq;
    p1[2] = b_ts;

    res = PQexec(conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        log_fail("BEGIN");
        return GUARDX_ERR_WRITE;
    }
    PQclear(res);

    res = PQexecParams(conn, SQL_READING, 3, NULL, p1, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1) {
        PQclear(res);
        log_fail("UPSERT sensor_reading");
        rollback();
        return GUARDX_ERR_WRITE;
    }
    snprintf(b_rid, sizeof(b_rid), "%s", PQgetvalue(res, 0, 0));
    PQclear(res);

    snprintf(b_val[0], sizeof(b_val[0]), "%d",   m->gas_raw);
    snprintf(b_val[1], sizeof(b_val[1]), "%d",   m->spark_raw);
    snprintf(b_val[2], sizeof(b_val[2]), "%.3f", m->temperature);
    snprintf(b_val[3], sizeof(b_val[3]), "%.3f", m->humidity);
    snprintf(b_val[4], sizeof(b_val[4]), "%.3f", m->irtemp_ambient);
    snprintf(b_val[5], sizeof(b_val[5]), "%.3f", m->irtemp_object);

    valid[0] = m->gas_valid     ? "t" : "f";
    valid[1] = m->spark_valid   ? "t" : "f";
    valid[2] = m->temphum_valid ? "t" : "f";
    valid[3] = m->temphum_valid ? "t" : "f";
    valid[4] = m->irtemp_valid  ? "t" : "f";
    valid[5] = m->irtemp_valid  ? "t" : "f";

    p2[0] = b_rid;
    for (i = 0; i < 6; i++) {
        p2[1 + i * 2] = b_val[i];
        p2[2 + i * 2] = valid[i];
    }

    res = PQexecParams(conn, SQL_VALUES, 13, NULL, p2, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        log_fail("UPSERT sensor_value");
        rollback();
        return GUARDX_ERR_WRITE;
    }
    PQclear(res);

    res = PQexec(conn, "COMMIT");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        log_fail("COMMIT");
        return GUARDX_ERR_WRITE;
    }
    PQclear(res);
    return GUARDX_OK;
}

guardx_err_t db_write_button(const button_msg_t *m, uint64_t timestamp_ms,
                             int zone_id)
{
    char b_zone[8], b_seq[24], b_cnt[24], b_ts[24];
    const char *p[4] = { b_zone, b_seq, b_cnt, b_ts };
    PGresult *res;

    if (!ensure_conn())
        return GUARDX_ERR_OPEN;

    snprintf(b_zone, sizeof(b_zone), "%d", zone_id);
    snprintf(b_seq, sizeof(b_seq), "%u", m->seq);
    snprintf(b_cnt, sizeof(b_cnt), "%u", m->press_count);
    snprintf(b_ts, sizeof(b_ts), "%llu", (unsigned long long)timestamp_ms);

    res = PQexecParams(conn, SQL_BUTTON, 4, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        log_fail("INSERT button_event");
        return GUARDX_ERR_WRITE;
    }
    PQclear(res);
    return GUARDX_OK;
}
