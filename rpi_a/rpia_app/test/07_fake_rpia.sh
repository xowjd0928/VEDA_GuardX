#!/bin/bash
# 07_fake_rpia.sh - RPi A에서 B(mTLS 8883)로 센서 데이터를 발행하는 헬퍼.
# 02_fake_rpia.sh(평문 전용, RPi B용)를 건드리지 않고, mTLS 경로로 실제
# A->B 홉을 태우기 위한 별도 스크립트. 페이로드/케이스는 02와 동일하다.
#
# 사용법:
#   ./07_fake_rpia.sh normal [N]
#   ./07_fake_rpia.sh gas [N]
#   ./07_fake_rpia.sh temp [N]
#   ./07_fake_rpia.sh spark [N]
#   ./07_fake_rpia.sh combo [N]
#   ./07_fake_rpia.sh mid [N]
#   ./07_fake_rpia.sh crossfire [N]
#   ./07_fake_rpia.sh degraded [N]
#   ./07_fake_rpia.sh invalid [N]
#   ./07_fake_rpia.sh crippled [N]
#   ./07_fake_rpia.sh button
#   ./07_fake_rpia.sh fire_demo
#
# 접속 설정(환경변수로 덮어쓰기 가능):
#   MQTT_HOST=172.20.33.251 MQTT_PORT=8883 MQTT_TLS=1
#   MQTT_CA=/etc/guardx/certs/ca.crt
#   MQTT_CERT=/etc/guardx/certs/rpia.crt
#   MQTT_KEY=/etc/guardx/certs/rpia.key
#   MQTT_INSECURE=0
# 평문으로 쓰려면: MQTT_TLS=0 MQTT_PORT=1883 ./07_fake_rpia.sh fire_demo
set -e

HOST="${MQTT_HOST:-172.20.33.251}"
TLS="${MQTT_TLS:-1}"
if [ "$TLS" = "1" ]; then
    PORT="${MQTT_PORT:-8883}"
    CA="${MQTT_CA:-/etc/guardx/certs/ca.crt}"
    CERT="${MQTT_CERT:-/etc/guardx/certs/rpia.crt}"
    KEY="${MQTT_KEY:-/etc/guardx/certs/rpia.key}"
    TLS_ARGS=(--cafile "$CA" --cert "$CERT" --key "$KEY")
    [ "${MQTT_INSECURE:-0}" = "1" ] && TLS_ARGS+=(--insecure)
else
    PORT="${MQTT_PORT:-1883}"
    TLS_ARGS=()
fi

TOPIC_SENSOR="guardx/sensor/rpia"
TOPIC_BUTTON="guardx/sensor/rpia/button"
SEQ=$RANDOM

now_ms() { date +%s%3N; }

pub_sensor() {
    local sv="${7:-true}" iv="${8:-true}" tv="${9:-true}"
    local payload="{\"node_id\":\"rpia\",\"timestamp\":$(now_ms),\"seq\":$SEQ,\"values\":{\"gas_raw\":$1,\"spark_raw\":$2,\"temperature\":$3,\"humidity\":$4,\"irtemp_ambient\":23.0,\"irtemp_object\":$5},\"valid\":{\"gas\":$6,\"temphum\":$tv,\"spark\":$sv,\"irtemp\":$iv}}"
    SEQ=$((SEQ + 1))
    echo "PUB $payload"
    mosquitto_pub -h "$HOST" -p "$PORT" "${TLS_ARGS[@]}" -q 0 -t "$TOPIC_SENSOR" -m "$payload"
}

cycles() {
    local n=$1; shift
    for _ in $(seq "$n"); do
        pub_sensor "$@"
        sleep 1
    done
}

case "$1" in
normal)    cycles "${2:-1}" 200  100  23.5  60.2   25.0     true  ;;
gas)       cycles "${2:-3}" 800  100  23.5  60.2   25.0     true  ;;
temp)      cycles "${2:-3}" 200  100  60.0  60.2   25.0     true  ;;
spark)     cycles "${2:-3}" 200  800  23.5  60.2   25.0     true  ;;
combo)     cycles "${2:-3}" 650  620  52.5  25.5   68.0     true  ;;
crossfire) cycles "${2:-3}" 200  620  23.5  60.2   68.0     true  ;;
mid)       cycles "${2:-3}" 650  500  45.0  30.0   64.0     true  ;;
degraded)  cycles "${2:-3}"   0  560  57.5  18.5   76.0     false ;;
invalid)   cycles "${2:-1}"   0  100  23.5  60.2   25.0     false ;;
crippled)  cycles "${2:-8}" 200  100  23.5  60.2   25.0     true false false ;;
button)
    payload="{\"node_id\":\"rpia\",\"timestamp\":$(now_ms),\"seq\":$SEQ,\"event\":\"emergency_button\",\"press_count\":1}"
    echo "PUB $payload"
    mosquitto_pub -h "$HOST" -p "$PORT" "${TLS_ARGS[@]}" -q 2 -t "$TOPIC_BUTTON" -m "$payload"
    ;;
fire_demo)
    echo "== 1) 정상 2사이클 =="
    cycles 2 200 100 23.5 60.2 25.0 true
    echo "== 2) 5채널 상승 3사이클 (3번째에 FIRE) =="
    cycles 3 650 620 52.5 25.5 68.0 true
    echo "== 3) 정상 10사이클 (10번째에 RECOVER) =="
    cycles 10 200 100 23.5 60.2 25.0 true
    echo "fire_demo 완료"
    ;;
*)
    echo "usage: $0 {normal|gas|temp|spark|combo|crossfire|mid|degraded|invalid|crippled|button|fire_demo} [N]"
    exit 1
    ;;
esac
