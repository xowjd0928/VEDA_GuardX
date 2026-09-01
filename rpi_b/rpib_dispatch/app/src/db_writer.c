/*
 * db_writer.c - rpib_dispatch 동기 기록
 * SQL_COMMAND는 원본 db_writer_pg.c의 것과 동일 - command_key로 command_id를
 * 서브쿼리로 찾는 방식(매핑표를 코드에 안 둔다)도 그대로.
 */

#include <stdio.h>
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

#define SQL_COMMAND \
    "INSERT INTO fire_event_command (event_id, command_id, action, value, published_seq) " \
    "VALUES ($1::bigint, " \
    "        (SELECT command_id FROM actuator_command WHERE command_key = $2), " \
    "        $3, $4::int, $5::bigint) " \
    "ON CONFLICT (event_id, command_id) DO NOTHING"

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

guardx_err_t db_write_command(long long event_id, const char *command_key,
                              const char *action, int value, bool has_value,
                              uint32_t published_seq)
{
    char b_eid[24], b_val[16], b_seq[24];
    const char *p[5];
    PGresult *res;

    if (!ensure_conn())
        return GUARDX_ERR_OPEN;

    snprintf(b_eid, sizeof(b_eid), "%lld", event_id);
    snprintf(b_val, sizeof(b_val), "%d", value);
    snprintf(b_seq, sizeof(b_seq), "%u", published_seq);

    p[0] = b_eid;
    p[1] = command_key;
    p[2] = action;
    p[3] = has_value ? b_val : NULL;
    p[4] = b_seq;

    res = PQexecParams(conn, SQL_COMMAND, 5, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        fprintf(stderr, "db: pg INSERT fire_event_command failed: %s",
                PQerrorMessage(conn));
        return GUARDX_ERR_WRITE;
    }
    PQclear(res);
    return GUARDX_OK;
}
