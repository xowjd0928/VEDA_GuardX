/*
 * zone_loader.c - fire_zone 테이블 읽기 (PHASE 6, threshold_loader.c와
 * 같은 접속 패턴 - 부팅 시 1회 + 핫리로드용 짧은 연결).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>

#include "zone_loader.h"

#define SELECT_ZONES_SQL \
    "SELECT zone_id, zone_name, rpia_node_id, rpic_node_id " \
    "FROM fire_zone ORDER BY zone_id"

guardx_err_t zone_loader_load(fire_zone_t out[MAX_FIRE_ZONES], int *count)
{
    PGconn   *conn;
    PGresult *res;
    int n, i;

    /* 빈 문자열 -> libpq가 PG* 환경변수로 접속 정보를 채운다
     * (threshold_loader.c와 동일 - systemd EnvironmentFile로 주입 가능). */
    conn = PQconnectdb("");
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "zone_loader: connect failed: %s",
                PQerrorMessage(conn));
        PQfinish(conn);
        return GUARDX_ERR_OPEN;
    }

    res = PQexec(conn, SELECT_ZONES_SQL);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "zone_loader: query failed: %s",
                PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
        return GUARDX_ERR_READ;
    }

    n = PQntuples(res);
    if (n == 0) {
        /* fire_zone이 비어있으면 판정할 zone이 없다는 뜻 - 엔진이 아무
         * 센서 메시지도 처리할 수 없는 상태이므로 명백한 설정 오류다. */
        fprintf(stderr, "zone_loader: fire_zone 테이블이 비어있음 - "
                "최소 1행(현재 하드웨어 기준 zone_id=1) 필요\n");
        PQclear(res);
        PQfinish(conn);
        return GUARDX_ERR_INVALID;
    }
    if (n > MAX_FIRE_ZONES) {
        fprintf(stderr, "zone_loader: fire_zone 행이 %d개인데 "
                "MAX_FIRE_ZONES(%d) 초과 - 앞 %d개만 사용(zone_id 순)\n",
                n, MAX_FIRE_ZONES, MAX_FIRE_ZONES);
        n = MAX_FIRE_ZONES;
    }

    for (i = 0; i < n; i++) {
        out[i].zone_id = atoi(PQgetvalue(res, i, 0));
        snprintf(out[i].zone_name, sizeof(out[i].zone_name),
                 "%s", PQgetvalue(res, i, 1));
        snprintf(out[i].rpia_node_id, sizeof(out[i].rpia_node_id),
                 "%s", PQgetvalue(res, i, 2));
        snprintf(out[i].rpic_node_id, sizeof(out[i].rpic_node_id),
                 "%s", PQgetvalue(res, i, 3));
    }

    PQclear(res);
    PQfinish(conn);

    *count = n;
    printf("zone_loader: %d개 zone 로드됨\n", n);
    for (i = 0; i < n; i++)
        printf("  zone %d '%s' - A=%s C=%s\n",
               out[i].zone_id, out[i].zone_name,
               out[i].rpia_node_id, out[i].rpic_node_id);
    return GUARDX_OK;
}
