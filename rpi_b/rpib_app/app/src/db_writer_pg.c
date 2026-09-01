/*
 * db_writer_pg.c - PostgreSQL 기록 백엔드 구현 (PHASE 5)
 *
 * JSONL 한 줄이 여기서 관계형으로 풀린다:
 *   sensor 레코드  -> sensor_reading 1건 + sensor_value 6건 (한 트랜잭션)
 *   button         -> button_event 1건
 *   transition     -> fire_event 1건
 *   command        -> fire_event_command 1건 (직전 fire_event에 연결)
 *
 * 큐에 원본 구조체가 들어 있어 재파싱이 없다 - PHASE 3에서 문자열이
 * 아니라 struct로 정한 결정이 여기서 회수된다.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libpq-fe.h>

#include "db_writer_pg.h"

static PGconn *conn;
static time_t  last_attempt;

/* 직전에 INSERT한 fire_event의 event_id. 뒤따라오는 REC_COMMAND들이
 * 여기에 붙는다. 0이면 "붙일 사건이 없음"으로, 명령 기록을 건너뛴다 -
 * 전이 기록이 실패해 폴백된 상황에서 명령만 엉뚱한(이전) 사건에
 * 매달리는 것을 막는다. */
static long long last_event_id;

/* 재연결 시도 간격. PQreset()이 최악의 경우 PGCONNECT_TIMEOUT만큼
 * 블로킹되므로, 매 레코드마다 시도하면 워커가 사실상 멈춘다. */
#define PG_RETRY_SEC 5

/* ---------------------------------------------------------------------
 * !!! decision_cause_t 와 sensor_channel.channel_id 는 번호가 다르다 !!!
 *
 *   CAUSE_GAS      = 1  ->  1 gas_raw          (우연히 같음)
 *   CAUSE_TEMP     = 2  ->  3 temperature
 *   CAUSE_SPARK    = 3  ->  2 spark_raw
 *   CAUSE_HUMIDITY = 4  ->  4 humidity         (우연히 같음)
 *   CAUSE_IRTEMP   = 5  ->  6 irtemp_object    (5는 irtemp_ambient - 판단 입력이 아님)
 *
 * (int)cause를 그대로 넣으면 FK 제약도 통과하고 에러도 안 나면서
 * 모든 화재 기록의 원인이 조용히 틀린다. 불꽃 화재가 "온도"로 남는다.
 * 채널을 추가하면 여기와 fire_schema.sql의 시드를 같이 고칠 것.
 * --------------------------------------------------------------------- */
static int cause_to_channel_id(decision_cause_t c)
{
    switch (c) {
    case DECISION_CAUSE_GAS:      return 1;   /* gas_raw */
    case DECISION_CAUSE_SPARK:    return 2;   /* spark_raw */
    case DECISION_CAUSE_TEMP:     return 3;   /* temperature */
    case DECISION_CAUSE_HUMIDITY: return 4;   /* humidity */
    case DECISION_CAUSE_IRTEMP:   return 6;   /* irtemp_object */
    default:                      return 0;   /* 0 = NULL로 기록 */
    }
}

/* --- 연결 관리 -------------------------------------------------------- */

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
            last_event_id = 0;   /* 재연결 후 옛 event_id는 신뢰하지 않는다 */
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

/* --- INSERT 문 -------------------------------------------------------- */

#define SQL_READING \
    "INSERT INTO sensor_reading (zone_id, sensor_seq, composite_score, received_at) " \
    "VALUES ($1::smallint, $2::bigint, $3::real, to_timestamp($4::bigint / 1000.0)) " \
    "RETURNING reading_id"

/* channel_id 1~6은 fire_schema.sql의 sensor_channel 시드와 같은 순서다:
 * 1 gas_raw / 2 spark_raw / 3 temperature / 4 humidity /
 * 5 irtemp_ambient / 6 irtemp_object.
 * 온도·습도는 SHT30 하나라 temphum_valid를, 주변·표면온도는 MLX90614
 * 하나라 irtemp_valid를 공유한다. */
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

#define SQL_EVENT \
    "INSERT INTO fire_event (zone_id, event_type, cause_channel_id, trigger_seq, occurred_at) " \
    "VALUES ($1::smallint, $2, $3::smallint, $4::bigint, to_timestamp($5::bigint / 1000.0)) " \
    "RETURNING event_id"

/* command_id를 C에서 매핑하지 않고 서브쿼리로 찾는다 - 매핑표를 코드와
 * DB 양쪽에 두면 언젠가 어긋나기 때문. command_key는 guardx_protocol.h의
 * GUARDX_CMD_* 문자열 그대로다. */
#define SQL_COMMAND \
    "INSERT INTO fire_event_command (event_id, command_id, action, value, published_seq) " \
    "VALUES ($1::bigint, " \
    "        (SELECT command_id FROM actuator_command WHERE command_key = $2), " \
    "        $3, $4::int, $5::bigint) " \
    "ON CONFLICT (event_id, command_id) DO NOTHING"

/* --- 레코드별 기록 ---------------------------------------------------- */

static guardx_err_t ins_sensor(const db_record_t *rec)
{
    const sensor_msg_t *m = &rec->u.sensor;
    char b_zone[8], b_seq[24], b_score[24], b_ts[24], b_rid[24];
    char b_val[6][32];
    const char *p1[4], *p2[13];
    const char *valid[6];
    PGresult *res;
    int i;

    snprintf(b_zone, sizeof(b_zone), "%d", rec->zone_id);
    snprintf(b_seq, sizeof(b_seq), "%u", m->seq);
    snprintf(b_ts, sizeof(b_ts), "%llu", (unsigned long long)rec->ts_ms);
    snprintf(b_score, sizeof(b_score), "%.2f", rec->score);

    p1[0] = b_zone;
    p1[1] = b_seq;
    /* 음수 = 판단이 점수를 내지 않은 사이클(FIRE 동결) -> NULL.
     * 0으로 넣으면 "안전하다고 판단함"이 되어 의미가 정반대다. */
    p1[2] = (rec->score < 0.0f) ? NULL : b_score;
    p1[3] = b_ts;

    res = PQexec(conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        log_fail("BEGIN");
        return GUARDX_ERR_WRITE;
    }
    PQclear(res);

    res = PQexecParams(conn, SQL_READING, 4, NULL, p1, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1) {
        PQclear(res);
        log_fail("INSERT sensor_reading");
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
        log_fail("INSERT sensor_value");
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

static guardx_err_t ins_button(const db_record_t *rec)
{
    char b_zone[8], b_seq[24], b_cnt[24], b_ts[24];
    const char *p[4] = { b_zone, b_seq, b_cnt, b_ts };
    PGresult *res;

    snprintf(b_zone, sizeof(b_zone), "%d", rec->zone_id);
    snprintf(b_seq, sizeof(b_seq), "%u", rec->u.button.seq);
    snprintf(b_cnt, sizeof(b_cnt), "%u", rec->u.button.press_count);
    snprintf(b_ts, sizeof(b_ts), "%llu", (unsigned long long)rec->ts_ms);

    res = PQexecParams(conn, SQL_BUTTON, 4, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        log_fail("INSERT button_event");
        return GUARDX_ERR_WRITE;
    }
    PQclear(res);
    return GUARDX_OK;
}

static guardx_err_t ins_event(const db_record_t *rec)
{
    int ch = cause_to_channel_id(rec->u.trans.cause);
    int is_fire = (rec->u.trans.ev == DECISION_EVENT_FIRE);
    char b_zone[8], b_ch[8], b_seq[24], b_ts[24];
    const char *p[5];
    PGresult *res;

    snprintf(b_zone, sizeof(b_zone), "%d", rec->zone_id);
    snprintf(b_ch, sizeof(b_ch), "%d", ch);
    snprintf(b_seq, sizeof(b_seq), "%u", rec->u.trans.trigger_seq);
    snprintf(b_ts, sizeof(b_ts), "%llu", (unsigned long long)rec->ts_ms);

    p[0] = b_zone;
    p[1] = is_fire ? "fire_confirmed" : "recovered";
    /* 해제 사건은 원인이 없다. DECISION_LAST_CAUSE()는 직전 화재의
     * 원인을 그대로 들고 있으므로, 그걸 그대로 적으면 "이 원인 때문에
     * 해제됐다"는 잘못된 기록이 된다. 스키마가 NULL을 허용하는 이유. */
    p[2] = (is_fire && ch > 0) ? b_ch : NULL;
    p[3] = b_seq;
    p[4] = b_ts;

    res = PQexecParams(conn, SQL_EVENT, 5, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1) {
        PQclear(res);
        log_fail("INSERT fire_event");
        last_event_id = 0;   /* 뒤따르는 명령들이 엉뚱한 사건에 붙지 않게 */
        return GUARDX_ERR_WRITE;
    }
    last_event_id = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return GUARDX_OK;
}

static guardx_err_t ins_command(const db_record_t *rec)
{
    char b_eid[24], b_val[16], b_seq[24];
    const char *p[5];
    PGresult *res;

    if (last_event_id == 0) {
        fprintf(stderr, "db: pg command '%s' has no owning fire_event, "
                "falling back\n", rec->u.cmd.command_key);
        return GUARDX_ERR_WRITE;
    }

    snprintf(b_eid, sizeof(b_eid), "%lld", last_event_id);
    snprintf(b_val, sizeof(b_val), "%d", rec->u.cmd.value);
    snprintf(b_seq, sizeof(b_seq), "%u", rec->u.cmd.published_seq);

    p[0] = b_eid;
    p[1] = rec->u.cmd.command_key;
    p[2] = rec->u.cmd.action;
    p[3] = rec->u.cmd.has_value ? b_val : NULL;   /* ON/OFF는 value 없음 */
    p[4] = b_seq;

    res = PQexecParams(conn, SQL_COMMAND, 5, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        log_fail("INSERT fire_event_command");
        return GUARDX_ERR_WRITE;
    }
    PQclear(res);
    return GUARDX_OK;
}

/* --- 공개 인터페이스 -------------------------------------------------- */

guardx_err_t pg_writer_open(void)
{
    last_event_id = 0;
    last_attempt = 0;

    /* 최초 연결 실패는 치명적으로 다루지 않는다 - 임계값 로더와 같은
     * 폴백 철학이다. DB가 늦게 올라오는 부팅 순서에서도 엔진은 떠야
     * 하고, 그동안 기록은 JSONL로 흘러간 뒤 백필하면 된다. */
    if (!ensure_conn())
        fprintf(stderr, "db: pg unavailable at startup, "
                "falling back to jsonl until it returns\n");
    return GUARDX_OK;
}

void pg_writer_close(void)
{
    if (conn) {
        PQfinish(conn);
        conn = NULL;
    }
}

guardx_err_t pg_write_record(const db_record_t *rec)
{
    if (!rec)
        return GUARDX_ERR_INVALID;
    if (!ensure_conn())
        return GUARDX_ERR_OPEN;

    switch (rec->type) {
    case REC_SENSOR:     return ins_sensor(rec);
    case REC_BUTTON:     return ins_button(rec);
    case REC_TRANSITION: return ins_event(rec);
    case REC_COMMAND:    return ins_command(rec);
    }
    return GUARDX_ERR_INVALID;
}
