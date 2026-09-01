#!/bin/bash
# local_e2e.sh - A -> B -> C 전 구간을 한 대에서 검증
#
# 세 노드를 전부 이 기기 위에 띄우고, A의 가스 센서 드라이버에 값을
# 주입해서 C의 액추에이터 드라이버가 실제로 움직이는지까지 관통 확인한다.
#
#   [A] rpia 드라이버(simulate) -> rpia_publisher --+
#                                                   | guardx/sensor/rpia
#   [B] 브로커 + rpib_engine  <---------------------+
#                             --+
#                               | guardx/actuator/rpic
#   [C] rpic_subscriber  <------+  -> rpic 드라이버(simulate) -> dmesg
#
# 각 노드의 test/ 스크립트가 조각별 검증이라면, 이건 전체를 한 번에 본다.
# 실물 센서/액추에이터는 없으므로 전부 simulate=1 모드.
#
# 사용법: sudo ./local_e2e.sh
set -e

REPO="$(cd "$(dirname "$0")" && pwd)"
RPIA="$REPO/rpi_a/rpia_app"
RPIB="$REPO/rpi_b/rpib_app"
RPIC="$REPO/rpi_c/rpic_app"
LOGDIR="$REPO/.e2e_logs"

RPIA_CFG="$RPIA/app/include/mqtt_pub.h"

if [ "$EUID" -ne 0 ]; then
    echo "root로 실행하세요: sudo ./local_e2e.sh"
    exit 1
fi

# --- 정리 (스크립트가 어떻게 끝나든 실행) -----------------------------
BROKER_PID=""; A_PID=""; B_PID=""; C_PID=""
STARTED_BROKER=0

cleanup() {
    echo ""
    echo "=== 정리 중 ==="
    for pid in "$A_PID" "$B_PID" "$C_PID"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    sleep 1
    [ "$STARTED_BROKER" = "1" ] && [ -n "$BROKER_PID" ] && kill "$BROKER_PID" 2>/dev/null || true

    for m in rpic_amp rpic_pump rpic_pca9685 rpia_button rpia_spark rpia_temphum rpia_gas; do
        lsmod | grep -q "^$m " && rmmod "$m" 2>/dev/null || true
    done

    # rpi_a 헤더 원상복구
    if [ -f "$RPIA_CFG.e2e_bak" ]; then
        mv "$RPIA_CFG.e2e_bak" "$RPIA_CFG"
        echo "rpi_a mqtt_pub.h 원상복구됨 (재배포 전 app 재빌드 필요)"
    fi
    echo "정리 완료. 로그는 $LOGDIR 에 남아있음"
}
trap cleanup EXIT

mkdir -p "$LOGDIR"

# --- 1) rpi_a를 로컬 테스트용으로 임시 전환 ---------------------------
# rpi_a는 운영 기준(TLS=1 + RPi B 실제 IP 자리표시자)이라 이대로는
# localhost 브로커에 못 붙는다. 백업 뜨고 바꿨다가 종료 시 되돌린다.
echo "=== 1) rpi_a 설정 임시 전환 (TLS=0, localhost) ==="
cp "$RPIA_CFG" "$RPIA_CFG.e2e_bak"
sed -i \
    -e 's|^#define MQTT_USE_TLS .*|#define MQTT_USE_TLS        0   /* [E2E 임시] 원본은 1 */|' \
    -e 's|^#define MQTT_BROKER_HOST .*|#define MQTT_BROKER_HOST    "localhost"   /* [E2E 임시] 원본은 192.168.0.XXX */|' \
    "$RPIA_CFG"
grep -E '^#define MQTT_(USE_TLS|BROKER_HOST)' "$RPIA_CFG"

# --- 2) 빌드 ----------------------------------------------------------
echo ""
echo "=== 2) 빌드 (드라이버 7종 + 앱 3종) ==="
make -C "$RPIA/drivers" > "$LOGDIR/build_a_drv.log" 2>&1 && echo "  rpi_a 드라이버 OK"
make -C "$RPIC/drivers" > "$LOGDIR/build_c_drv.log" 2>&1 && echo "  rpi_c 드라이버 OK"
make -C "$RPIA/app"     > "$LOGDIR/build_a_app.log" 2>&1 && echo "  rpia_publisher OK"
make -C "$RPIB/app"     > "$LOGDIR/build_b_app.log" 2>&1 && echo "  rpib_engine OK"
make -C "$RPIC/app"     > "$LOGDIR/build_c_app.log" 2>&1 && echo "  rpic_subscriber OK"

# --- 3) 드라이버 로드 (전부 simulate) ---------------------------------
echo ""
echo "=== 3) 드라이버 로드 (simulate=1) ==="
for m in rpic_amp rpic_pump rpic_pca9685 rpia_button rpia_spark rpia_temphum rpia_gas; do
    lsmod | grep -q "^$m " && rmmod "$m" || true
done

insmod "$RPIA/drivers/rpia_gas.ko"     simulate=1 simulate_detected=0
insmod "$RPIA/drivers/rpia_temphum.ko" simulate=1
insmod "$RPIA/drivers/rpia_spark.ko"   simulate=1 simulate_detected=0
insmod "$RPIA/drivers/rpia_button.ko"  simulate=1
insmod "$RPIC/drivers/rpic_pca9685.ko" simulate=1
insmod "$RPIC/drivers/rpic_pump.ko"    simulate=1
insmod "$RPIC/drivers/rpic_amp.ko"     simulate=1
lsmod | grep -E '^rpi[ac]_' | awk '{print "  " $1}'

# --- 4) 브로커 --------------------------------------------------------
echo ""
echo "=== 4) 브로커 ==="
if ss -lnt | grep -q ':1883 '; then
    echo "  1883에 이미 브로커가 떠 있음 - 그대로 사용"
else
    cat > "$LOGDIR/mosquitto.conf" << 'EOF'
listener 1883 0.0.0.0
allow_anonymous true
EOF
    mosquitto -c "$LOGDIR/mosquitto.conf" > "$LOGDIR/broker.log" 2>&1 &
    BROKER_PID=$!
    STARTED_BROKER=1
    sleep 1
    echo "  테스트 브로커 기동 (pid $BROKER_PID)"
fi

# --- 5) 세 노드 앱 기동 ------------------------------------------------
echo ""
echo "=== 5) 노드 3개 기동 ==="
"$RPIC/app/rpic_subscriber" > "$LOGDIR/rpic.log" 2>&1 &
C_PID=$!
sleep 1

(cd "$RPIB/app" && ./rpib_engine) > "$LOGDIR/rpib.log" 2>&1 &
B_PID=$!
sleep 1

"$RPIA/app/rpia_publisher" > "$LOGDIR/rpia.log" 2>&1 &
A_PID=$!
sleep 2

for n in "A:$A_PID" "B:$B_PID" "C:$C_PID"; do
    name="${n%%:*}"; pid="${n##*:}"
    if kill -0 "$pid" 2>/dev/null; then
        echo "  $name 실행 중 (pid $pid)"
    else
        echo "  !! $name 죽었음 - $LOGDIR 로그 확인"
        exit 1
    fi
done

# --- 6) 화재 주입 ------------------------------------------------------
# A의 가스 드라이버 파라미터를 켠다 = 실제 센서가 가스를 감지한 것과
# 동일한 경로. 여기서부터는 사람 개입 없이 A -> B -> C가 알아서 돈다.
echo ""
echo "=== 6) 정상 구간 5초 (아무 일도 없어야 함) ==="
sleep 5

echo "=== 7) A 가스 센서 ON -> 화재 유발 ==="
echo Y > /sys/module/rpia_gas/parameters/simulate_detected
echo "  (A가 1Hz로 1500ppm 발행 -> B가 3사이클 연속 확인 후 확정)"
sleep 6

echo "=== 8) A 가스 센서 OFF -> 해제 대기 (10사이클) ==="
echo N > /sys/module/rpia_gas/parameters/simulate_detected
sleep 12

# 결과를 읽기 전에 앱을 세운다. 살아있는 채로 로그를 읽으면
# 아직 버퍼에 걸쳐 있는 줄을 놓친다.
echo ""
echo "=== 9) 노드 정지 (로그 flush) ==="
for pid in "$A_PID" "$B_PID" "$C_PID"; do
    [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
done
sleep 2
A_PID=""; B_PID=""; C_PID=""

# --- 9) 결과 ----------------------------------------------------------
echo ""
echo "############ 결과 ############"
echo ""
echo "--- [B] 판단 엔진 (화재 확정/해제) ---"
grep -E 'FIRE|recovered|->' "$LOGDIR/rpib.log" || echo "  (없음 - 실패)"
echo ""
echo "--- [C] 서브스크라이버 (명령 수신) ---"
grep -E 'ok$|failed' "$LOGDIR/rpic.log" || echo "  (없음 - 실패)"
echo ""
echo "--- [C] 드라이버 (실제 하드웨어 호출) ---"
dmesg | grep -E 'rpic_(pca9685|pump|amp):' | tail -12
echo ""
echo "--- [B] DB 기록 ---"
grep -E 'fire_confirmed|recovered' "$RPIB/app/rpib_events.jsonl" 2>/dev/null || echo "  (없음)"
echo ""
echo "전체 로그: $LOGDIR/{rpia,rpib,rpic,broker}.log"
