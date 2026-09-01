#!/usr/bin/env bash
#
# DB -> MQTT 브릿지 (구역 설정) — RPi B Poller 대역
#
# zones + zone_thresholds 를 읽어 guardx/db/rpib/zones 에 retained 로 발행한다.
# VMS(v2)가 이 토픽을 구독하므로, 이 스크립트를 띄워두면
#   DBeaver에서 capacity_limit 수정 -> VMS OCC 분모가 재시작 없이 갱신
# 전 구간이 이어진다.
#
# ── 위치에 대하여 ────────────────────────────────────────────────────
# 이건 임시 대역이다. 최종적으로는 RPi B의 Poller(C++/libpqxx)가 같은 SQL을
# 돌리고 같은 payload를 발행해야 한다. 아래 SQL을 그대로 옮기면 된다.
# 그때까지 개발 PC에서 이 스크립트를 돌려 VMS 쪽을 검증한다.
#
# ── 실행 ────────────────────────────────────────────────────────────
#   export PGPASSWORD='...'                 # 비밀번호는 소스에 두지 않는다
#   ./db_zones_bridge.sh
#
#   환경변수로 덮어쓸 수 있다:
#     DB_HOST(172.20.33.251) DB_PORT(5432) DB_NAME(guardx) DB_USER(guardx_reader)
#     MQTT_HOST(127.0.0.1)   MQTT_PORT(1883)
#     INTERVAL(5)            — 폴링 주기(초)
#
# ── 왜 폴링인가 ─────────────────────────────────────────────────────
# zone_thresholds 는 거의 안 바뀌는 설정 테이블이라 5초 폴링으로 충분하다.
# 값이 바뀌었을 때만 발행한다 — 같은 값을 계속 쏘면 브로커 로그만 지저분해지고
# 얻는 게 없다. (제대로 하려면 Postgres LISTEN/NOTIFY 를 쓸 수 있지만,
# 트리거를 심어야 해서 이 단계에서는 과하다.)
set -uo pipefail

DB_HOST="${DB_HOST:-172.20.33.251}"
DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-guardx}"
DB_USER="${DB_USER:-guardx_reader}"

MQTT_HOST="${MQTT_HOST:-127.0.0.1}"
MQTT_PORT="${MQTT_PORT:-1883}"
TOPIC="guardx/db/rpib/zones"
DATES_TOPIC="guardx/db/rpib/dates"

# 날짜 경계를 자르는 기준. 쿼리 서비스와 반드시 같아야 한다.
TZ_NAME="${TZ_NAME:-Asia/Seoul}"

INTERVAL="${INTERVAL:-5}"

PSQL="/c/Program Files/PostgreSQL/18/bin/psql.exe"
MOSQ_PUB="/c/Program Files/mosquitto/mosquitto_pub.exe"

for exe in "$PSQL" "$MOSQ_PUB"; do
    [ -x "$exe" ] || { echo "[브릿지] 실행 파일 없음: $exe" >&2; exit 1; }
done

if [ -z "${PGPASSWORD:-}" ]; then
    echo "[브릿지] PGPASSWORD 가 설정되지 않았습니다." >&2
    echo "         export PGPASSWORD='...' 후 다시 실행하세요." >&2
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────
# payload 를 Postgres 가 직접 만든다. 문자열 조합을 셸에서 하지 않으므로
# 따옴표 이스케이프 문제가 원천적으로 없다.
#
# zone_id 와 channel 은 1:1 이 아니다 (실측: zone1->ch1, zone2->ch0).
# 반드시 zones 를 조인해 channel 을 실어 보내야 한다 — VMS 는 channel 로만
# 판단한다. 이 조인이 v2 에서 VMS 가 지운 그 쿼리다.
#
# capacity_limit 은 스키마상 NULL 가능이며, NULL 이면 JSON null 로 나간다.
# VMS 는 null 을 "값 없음"으로 보고 기존 값/기본값을 유지한다.
# ─────────────────────────────────────────────────────────────────────
read -r -d '' QUERY <<'SQL'
SELECT json_build_object(
         'node_id',   'db-bridge',
         'timestamp', (extract(epoch from now()) * 1000)::bigint,
         'zones',     coalesce(json_agg(
                        json_build_object(
                          'zone_id',        z.zone_id,
                          'channel',        z.channel,
                          'capacity_limit', t.capacity_limit,
                          'warn_ratio',     t.warn_ratio,
                          'critical_ratio', t.critical_ratio
                        ) ORDER BY z.channel
                      ), '[]'::json)
       )::text
FROM zones z
JOIN zone_thresholds t USING (zone_id);
SQL

# ─────────────────────────────────────────────────────────────────────
# 데이터가 있는 날짜 목록 — CROWD 우측 목록의 근거.
#
# zones 와 같은 성격의 상태값이다(거의 안 바뀌고, 유실되면 목록이 영영 빈다)
# → QoS 1 + retained, 변경 시에만 발행.
#
# 날짜 경계는 표시 타임존 기준으로 자른다. 운영자가 보는 "하루"와 집계
# 단위가 어긋나면 안 되므로 쿼리 서비스와 같은 TZ 를 써야 한다.
# ─────────────────────────────────────────────────────────────────────
RANGE_QUERY="SELECT json_build_object(
         'node_id',   'db-bridge',
         'timestamp', (extract(epoch from now()) * 1000)::bigint,
         'dates',     coalesce(json_agg(d ORDER BY d), '[]'::json)
       )::text
FROM (
  SELECT DISTINCT (ts AT TIME ZONE '${TZ_NAME}')::date AS d
  FROM detections
) s;"

query_db() {   # $1 = SQL -> stdout 에 payload
    "$PSQL" -h "$DB_HOST" -p "$DB_PORT" -d "$DB_NAME" -U "$DB_USER" \
            -tAc "$1" 2>/tmp/bridge_err.txt
}

echo "[브릿지] 시작"
echo "         DB   : ${DB_USER}@${DB_HOST}:${DB_PORT}/${DB_NAME}"
echo "         MQTT : ${MQTT_HOST}:${MQTT_PORT}"
echo "           - ${TOPIC} (QoS 1, retained, 변경 시에만)"
echo "           - ${DATES_TOPIC} (QoS 1, retained, 변경 시에만)"
echo "         주기 : ${INTERVAL}초 확인, 값이 바뀔 때만 발행"
echo "         중지 : Ctrl+C"
echo

last_zones=""
last_dates=""
while true; do
    # ---- zones: 설정값이라 바뀔 때만 발행 (QoS 1) ----
    zones=$(query_db "$QUERY"); rc=$?
    if [ $rc -ne 0 ] || [ -z "$zones" ]; then
        echo "[브릿지] $(date +%H:%M:%S) zones 조회 실패 — $(tr -d '\r\n' < /tmp/bridge_err.txt | tail -c 160)"
    elif [ "$zones" != "$last_zones" ]; then
        if printf '%s' "$zones" | "$MOSQ_PUB" -h "$MQTT_HOST" -p "$MQTT_PORT" \
                                              -t "$TOPIC" -q 1 -r -s; then
            echo "[브릿지] $(date +%H:%M:%S) zones 발행 ← 값 변경 감지"
            echo "         $zones"
            last_zones="$zones"
        else
            echo "[브릿지] $(date +%H:%M:%S) zones 발행 실패 — 브로커 확인 필요"
        fi
    fi

    # ---- dates: 새 날짜가 생기거나 파티션이 drop 될 때만 바뀐다 (QoS 1) ----
    dates=$(query_db "$RANGE_QUERY"); rc=$?
    if [ $rc -ne 0 ] || [ -z "$dates" ]; then
        echo "[브릿지] $(date +%H:%M:%S) dates 조회 실패 — $(tr -d '\r\n' < /tmp/bridge_err.txt | tail -c 160)"
    elif [ "$dates" != "$last_dates" ]; then
        if printf '%s' "$dates" | "$MOSQ_PUB" -h "$MQTT_HOST" -p "$MQTT_PORT" \
                                              -t "$DATES_TOPIC" -q 1 -r -s; then
            echo "[브릿지] $(date +%H:%M:%S) dates 발행 ← 목록 변경 감지"
            echo "         $dates"
            last_dates="$dates"
        else
            echo "[브릿지] $(date +%H:%M:%S) dates 발행 실패"
        fi
    fi

    sleep "$INTERVAL"
done
