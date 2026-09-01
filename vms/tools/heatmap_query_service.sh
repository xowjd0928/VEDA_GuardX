#!/usr/bin/env bash
#
# 하루치 히트맵 집계 서비스 — RPi B Poller 대역 (참조 구현)
#
# guardx/db/rpib/query/heatday 요청을 구독해 하루치를 10분 슬롯으로 집계하고,
# 요청에 실려 온 reply_to 토픽으로 돌려준다.
#
# ── 왜 "하루 단위"인가 ──────────────────────────────────────────────
# 슬라이더를 움직일 때마다 조회하면 요청이 폭주하고 응답 순서가 뒤바뀐다.
# 하루치를 통째로 보내면 요청이 *날짜당 1회*로 줄고, 슬라이더·누적·날짜
# 다중선택은 전부 클라이언트가 캐시 위에서 계산한다 (네트워크 0).
# 보존 기간(14일) 전체를 10분 단위로 보내면 수십 MB지만 하루치는 1~2MB다.
#
# ── 실행 ────────────────────────────────────────────────────────────
#   export PGPASSWORD='...'
#   ./heatmap_query_service.sh
#
# ── 최종 형태 ───────────────────────────────────────────────────────
# RPi B Poller(C++/libpqxx + libmosquitto)가 같은 토픽을 구독하고 같은 SQL을
# 돌려야 한다. 아래 SQL을 그대로 옮기면 된다. 커넥션을 유지하므로 요청마다
# psql 프로세스를 띄우는 이 스크립트보다 훨씬 빠르다.
set -uo pipefail

DB_HOST="${DB_HOST:-172.20.33.251}"
DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-guardx}"
DB_USER="${DB_USER:-guardx_reader}"

MQTT_HOST="${MQTT_HOST:-127.0.0.1}"
MQTT_PORT="${MQTT_PORT:-1883}"
REQ_TOPIC="guardx/db/rpib/query/heatday"

# 표시용 타임존. 날짜 경계를 이걸로 자른다 (운영자가 보는 '하루').
TZ_NAME="${TZ_NAME:-Asia/Seoul}"

PSQL="/c/Program Files/PostgreSQL/18/bin/psql.exe"
MOSQ_SUB="/c/Program Files/mosquitto/mosquitto_sub.exe"
MOSQ_PUB="/c/Program Files/mosquitto/mosquitto_pub.exe"

for exe in "$PSQL" "$MOSQ_SUB" "$MOSQ_PUB"; do
    [ -x "$exe" ] || { echo "[쿼리서비스] 실행 파일 없음: $exe" >&2; exit 1; }
done

[ -n "${PGPASSWORD:-}" ] || { echo "[쿼리서비스] PGPASSWORD 미설정" >&2; exit 1; }

jget() { printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p"; }
jnum() { printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\([0-9.]*\).*/\1/p"; }

echo "[쿼리서비스] 시작 — ${REQ_TOPIC} 구독 (Ctrl+C 중지)"
echo "             DB: ${DB_USER}@${DB_HOST}:${DB_PORT}/${DB_NAME}  TZ=${TZ_NAME}"
echo

"$MOSQ_SUB" -h "$MQTT_HOST" -p "$MQTT_PORT" -t "$REQ_TOPIC" -q 1 | while read -r line; do
    [ -z "$line" ] && continue

    req_id=$(jget "$line" req_id)
    reply=$(jget "$line" reply_to)
    date=$(jget "$line" date)
    cell=$(jnum "$line" cell);          cell=${cell:-60}
    slot=$(jnum "$line" slot_min);      slot=${slot:-10}
    cat_id=$(jnum "$line" category);    cat_id=${cat_id:-1}
    lk=$(jnum "$line" min_likelihood);  lk=${lk:-0.30}

    if [ -z "$req_id" ] || [ -z "$reply" ] || [ -z "$date" ]; then
        echo "[쿼리서비스] req_id/reply_to/date 없는 요청 무시"
        continue
    fi

    echo "[쿼리서비스] $(date +%H:%M:%S) 요청 ${date}  (req ${req_id:0:8}…)"

    # 하루를 slot_min 단위로 나눈 인덱스(0~143)와 카메라 격자로 집계.
    # ts 절대범위 조건은 파티션 프루닝을 살리기 위해 필수 (schema.sql:171).
    SQL="WITH d AS (
           SELECT '${date}'::date AS day
         )
         SELECT coalesce(json_agg(json_build_array(s, channel, gx, gy, w)), '[]'::json)::text
         FROM (
           SELECT floor(
                    extract(epoch FROM (ts AT TIME ZONE '${TZ_NAME}')
                                     - (SELECT day FROM d)::timestamp) / (${slot}*60)
                  )::int AS s,
                  channel,
                  floor(ST_X(geom)/${cell})::int AS gx,
                  floor(ST_Y(geom)/${cell})::int AS gy,
                  count(*) AS w
           FROM detections
           WHERE ts >= ((SELECT day FROM d)::timestamp AT TIME ZONE '${TZ_NAME}')
             AND ts <  (((SELECT day FROM d) + 1)::timestamp AT TIME ZONE '${TZ_NAME}')
             AND category = ${cat_id}
             AND likelihood >= ${lk}
           GROUP BY 1, 2, 3, 4
         ) t;"

    cells=$("$PSQL" -h "$DB_HOST" -p "$DB_PORT" -d "$DB_NAME" -U "$DB_USER" \
                    -tAc "$SQL" 2>/tmp/hq_err.txt)
    rc=$?

    if [ $rc -ne 0 ] || [ -z "$cells" ]; then
        err=$(tr -d '\r\n"' < /tmp/hq_err.txt | tail -c 200)
        printf '{"node_id":"db-bridge","req_id":"%s","date":"%s","ok":false,"error":"%s"}' \
               "$req_id" "$date" "$err" \
            | "$MOSQ_PUB" -h "$MQTT_HOST" -p "$MQTT_PORT" -t "$reply" -q 1 -s
        echo "[쿼리서비스]   → 실패: $err"
        continue
    fi

    printf '{"node_id":"db-bridge","req_id":"%s","date":"%s","ok":true,"cells":%s}' \
           "$req_id" "$date" "$cells" \
        | "$MOSQ_PUB" -h "$MQTT_HOST" -p "$MQTT_PORT" -t "$reply" -q 1 -s
    echo "[쿼리서비스]   → 응답 ${#cells} bytes"
done
