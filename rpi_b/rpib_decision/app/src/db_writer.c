/*
 * db_writer.c - rpib_decision 동기 기록 (libpq 직접 호출)
 * cause_to_channel_id 매핑은 원본 rpib_app/app/src/db_writer_pg.c와
 * 동일 - decision_cause_t 번호와 sensor_channel.channel_id 번호가
 * 우연히 다르므로(원본 주석 참조) 반드시 이 매핑을 거쳐야 한다.
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

/* !!! decision_cause_t 와 sensor_channel.channel_id 는 번호가 다르다 !!!
 * (원본 db_writer_pg.c 주석과 동일 근거로 그대로 유지) */
static int cause_to_channel_id(decision_cause_t c)
{
    switch (c) {
    case DECISION_CAUSE_GAS:      return 1;
    case DECISION_CAUSE_SPARK:    return 2;
    case DECISION_CAUSE_TEMP:     return 3;
    case DECISION_CAUSE_HUMIDITY: return 4;
    case DECISION_CAUSE_IRTEMP:   return 6;
    default:                      return 0;
    }
}

/* sensor_seq로는 못 찾는다(RPi A 재시작마다 리셋되는 카운터라 zone당
 * 전역 유일하지 않음 - fire_schema.sql sensor_reading 주석 참조).
 * 대신 "이 zone의 가장 최근 reading_id"를 갱신한다 - rpib_ingest가
 * 같은 사이클의 원시값을 이미 넣었을 것이라는 전제(보통 성립, 드물게
 * decision이 ingest보다 먼저 처리되면 그 사이클은 점수를 못 받고 다음
 * 사이클로 넘어간다 - 옛 데이터를 잘못 덮어쓰는 것보다 안전한 실패
 * 방향이라 이쪽을 택했다). */
#define SQL_SCORE \
    "UPDATE sensor_reading SET composite_score = $1::real " \
    "WHERE reading_id = (" \
    "  SELECT reading_id FROM sensor_reading" \
    "  WHERE zone_id = $2::smallint" \
    "  ORDER BY reading_id DESC LIMIT 1" \
    ")"

#define SQL_EVENT \
    "INSERT INTO fire_event (zone_id, event_type, cause_channel_id, trigger_seq, occurred_at) " \
    "VALUES ($1::smallint, $2, $3::smallint, $4::bigint, to_timestamp($5::bigint / 1000.0)) " \
    "RETURNING event_id"

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

guardx_err_t db_write_sensor_score(int zone_id, uint32_t sensor_seq, float score,
                                   uint64_t timestamp_ms)
{
    char b_zone[8], b_score[24];
    const char *p[2];
    PGresult *res;

    (void)timestamp_ms;   /* UPDATE라 received_at을 안 건드린다 - ingest 몫 그대로 유지 */

    if (!ensure_conn())
        return GUARDX_ERR_OPEN;

    snprintf(b_zone, sizeof(b_zone), "%d", zone_id);
    snprintf(b_score, sizeof(b_score), "%.2f", (double)score);

    p[0] = (score < 0.0f) ? NULL : b_score;   /* 동결 사이클 -> NULL */
    p[1] = b_zone;

    res = PQexecParams(conn, SQL_SCORE, 2, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        log_fail("UPDATE sensor_reading(score)");
        return GUARDX_ERR_WRITE;
    }
    if (strcmp(PQcmdTuples(res), "0") == 0)
        fprintf(stderr, "db: zone %d sensor_seq=%u score update matched no row "
                "(ingest hasn't written this cycle's row yet?)\n", zone_id, sensor_seq);
    PQclear(res);
    return GUARDX_OK;
}

guardx_err_t db_write_transition(int zone_id, decision_event_t ev,
                                 decision_cause_t cause, uint32_t trigger_seq,
                                 uint64_t timestamp_ms, long long *out_event_id)
{
    int ch = cause_to_channel_id(cause);
    int is_fire = (ev == DECISION_EVENT_FIRE);
    char b_zone[8], b_ch[8], b_seq[24], b_ts[24];
    const char *p[5];
    PGresult *res;

    if (!ensure_conn())
        return GUARDX_ERR_OPEN;

    snprintf(b_zone, sizeof(b_zone), "%d", zone_id);
    snprintf(b_ch, sizeof(b_ch), "%d", ch);
    snprintf(b_seq, sizeof(b_seq), "%u", trigger_seq);
    snprintf(b_ts, sizeof(b_ts), "%llu", (unsigned long long)timestamp_ms);

    p[0] = b_zone;
    p[1] = is_fire ? "fire_confirmed" : "recovered";
    p[2] = (is_fire && ch > 0) ? b_ch : NULL;   /* 해제엔 원인 없음 */
    p[3] = b_seq;
    p[4] = b_ts;

    res = PQexecParams(conn, SQL_EVENT, 5, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1) {
        PQclear(res);
        log_fail("INSERT fire_event");
        return GUARDX_ERR_WRITE;
    }
    if (out_event_id)
        *out_event_id = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return GUARDX_OK;
}
