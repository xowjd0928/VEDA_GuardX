#!/bin/bash
# actuator_test.sh - GuardX RPi C 액추에이터 간편 테스트 도구
#
# 이 스크립트는 "RPi B 역할"로 MQTT 명령을 쏴서, RPi C에 붙은 액추에이터
# (서보/팬/스텝모터/펌프)를 손쉽게 테스트하기 위한 것입니다.
# (앰프는 I2S/ALSA - 액추에이터 켤 때 소리가 자동으로 나며 별도 명령 없음)
# RPi C에서 `rpic_subscriber`가 켜져 있어야 명령이 먹습니다.
#
# ── 다른 사람에게 줄 때 ──────────────────────────────────────────────
#  1) 이 파일을 아무 데나 복사       (예: scp actuator_test.sh pi@RPiB:~/)
#  2) 실행 권한 부여                 chmod +x actuator_test.sh
#  3) mosquitto 클라이언트 설치      sudo apt install -y mosquitto-clients
#  4) 아래 BROKER 값이 맞는지 확인   (RPi B/브로커 IP)
#  5) 그냥 실행하면 번호 메뉴가 뜸   ./actuator_test.sh
# ─────────────────────────────────────────────────────────────────────
#
# 명령어로 바로 쓰기(메뉴 없이):
#   ./actuator_test.sh fan 60        # 팬 60%
#   ./actuator_test.sh fan off       # 팬 끄기
#   ./actuator_test.sh servo1 90     # 가스밸브 서보 90도
#   ./actuator_test.sh pump on|off   # 워터펌프
#   ./actuator_test.sh shutter close # 화재셔터 닫기
#   ./actuator_test.sh shutter open  # 화재셔터 열기
#   ./actuator_test.sh shutter stop  # 화재셔터 정지
#   ./actuator_test.sh sound 1       # 스피커: 0=기본 1=화재 2=강도 3=비상
#   ./actuator_test.sh alloff        # 전부 끄기(안전 상태)
#   ./actuator_test.sh demo          # 전체 자동 시퀀스

# ===== 여기만 환경에 맞게 바꾸세요 =====================================
BROKER="${MQTT_HOST:-172.20.33.251}"   # RPi B(브로커) IP. 필요시 이 줄만 수정
TOPIC="guardx/actuator/rpic"
# =====================================================================

SEQ=$RANDOM

# mosquitto_pub 설치 확인
if ! command -v mosquitto_pub >/dev/null 2>&1; then
    echo "[오류] mosquitto_pub 이 없습니다. 먼저 설치하세요:"
    echo "       sudo apt install -y mosquitto-clients"
    exit 1
fi

now_ms() { date +%s%3N; }

# pub <command> <action> [value]  -> JSON 조립 후 발행
pub() {
    local payload
    if [ -n "$3" ]; then
        payload="{\"node_id\":\"rpic\",\"timestamp\":$(now_ms),\"seq\":$SEQ,\"command\":\"$1\",\"action\":\"$2\",\"value\":$3}"
    else
        payload="{\"node_id\":\"rpic\",\"timestamp\":$(now_ms),\"seq\":$SEQ,\"command\":\"$1\",\"action\":\"$2\"}"
    fi
    SEQ=$((SEQ + 1))
    echo "  → 보냄: $1 $2 $3"
    if ! mosquitto_pub -h "$BROKER" -q 1 -t "$TOPIC" -m "$payload"; then
        echo "  [실패] 브로커($BROKER)에 못 보냈습니다. IP/네트워크를 확인하세요."
    fi
}

# 전부 끄기(안전 상태)
all_off() {
    pub fan OFF
    pub water_pump OFF
    pub shutter STOP
    pub servo_1 SET 0
    echo "전부 끔(안전 상태)."
}

# 전체 자동 시퀀스
run_demo() {
    pub servo_1 SET 90;  sleep 1   # 가스밸브
    pub fan SET 70;      sleep 1
    pub shutter CLOSE;   sleep 2
    pub water_pump ON;   sleep 1
    pub sound SET 1;     sleep 2   # 화재음
    all_off
    echo "데모 완료."
}

# ── 명령어 모드 (인자가 있으면 그거 하나 실행하고 종료) ──────────────
if [ $# -gt 0 ]; then
    case "$1" in
    servo1)  pub servo_1 SET "$2" ;;
    fan)
        case "$2" in
        on)  pub fan ON ;;
        off) pub fan OFF ;;
        *)   pub fan SET "$2" ;;
        esac ;;
    shutter)
        case "$2" in
        close) pub shutter CLOSE ;;
        open)  pub shutter OPEN ;;
        stop)  pub shutter STOP ;;
        *) echo "셔터 동작은 close / open / stop 중 하나여야 합니다."; exit 1 ;;
        esac ;;
    pump)    [ "$2" = "on" ] && pub water_pump ON || pub water_pump OFF ;;
    sound)   pub sound SET "$2" ;;   # 0 기본/1 화재/2 강도/3 비상
    alloff)  all_off ;;
    demo)    run_demo ;;
    *)
        echo "모르는 명령: $1"
        echo "쓸 수 있는 것: fan / servo1 / pump / shutter / sound / alloff / demo"
        exit 1 ;;
    esac
    exit 0
fi

# ── 메뉴 모드 (인자 없이 실행하면 번호 메뉴) ─────────────────────────
echo "======================================"
echo " GuardX 액추에이터 테스트  (브로커: $BROKER)"
echo " * RPi C에서 rpic_subscriber가 켜져 있어야 합니다."
echo "======================================"

while true; do
    echo ""
    echo " 1) 팬 켜기 (속도 입력)      2) 팬 끄기"
    echo " 3) 가스밸브 서보 각도"
    echo " 4) 워터펌프 ON             5) 워터펌프 OFF"
    echo " 6) 화재셔터 (close / open / stop, 리밋센서 자동 정지)"
    echo " 7) 스피커 (0=기본 1=화재 2=강도 3=비상)"
    echo " a) 전부 끄기(안전)         d) 데모 시퀀스        q) 종료"
    read -rp "번호 선택: " choice

    case "$choice" in
    1) read -rp "  팬 속도(0~100): " v; pub fan SET "$v" ;;
    2) pub fan OFF ;;
    3) read -rp "  가스밸브 각도(0~180): " v; pub servo_1 SET "$v" ;;
    4) pub water_pump ON ;;
    5) pub water_pump OFF ;;
    6)
        read -rp "  셔터 동작(close/open/stop): " v
        case "$v" in
        close) pub shutter CLOSE ;;
        open)  pub shutter OPEN ;;
        stop)  pub shutter STOP ;;
        *) echo "  close / open / stop 중에서 골라주세요." ;;
        esac ;;
    7) read -rp "  스피커 상황(0=기본 1=화재 2=강도 3=비상): " v; pub sound SET "$v" ;;
    a|A) all_off ;;
    d|D) run_demo ;;
    q|Q) echo "종료합니다."; exit 0 ;;
    *) echo "  1~7, a, d, q 중에서 골라주세요." ;;
    esac
done
