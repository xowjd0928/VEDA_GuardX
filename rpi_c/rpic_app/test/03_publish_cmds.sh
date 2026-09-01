#!/bin/bash
# 03_publish_cmds.sh - RPi B 역할을 흉내내 액추에이터 명령 발행 헬퍼
# (프로토콜 규약 4-3 스키마, guardx/actuator/rpic, QoS 1)
#
# 사용법:
#   ./03_publish_cmds.sh servo1 90        # 가스밸브 서보 90도
#   ./03_publish_cmds.sh fan 70           # 팬 듀티 70%
#   ./03_publish_cmds.sh fan on|off       # 팬 ON(기본듀티)/OFF
#   ./03_publish_cmds.sh shutter close    # 화재셔터 닫기
#   ./03_publish_cmds.sh shutter open     # 화재셔터 열기
#   ./03_publish_cmds.sh shutter stop     # 화재셔터 정지
#   ./03_publish_cmds.sh pump on|off      # 워터펌프
#   ./03_publish_cmds.sh sound 1          # 스피커: 0=기본 1=화재 2=강도 3=비상
#   ./03_publish_cmds.sh demo             # 전체 시퀀스 자동 발행
#   ./03_publish_cmds.sh state            # 드라이버가 적용한 상태 확인(dmesg)
set -e

HOST="${MQTT_HOST:-localhost}"
TOPIC="guardx/actuator/rpic"
SEQ=$RANDOM

# now_ms: epoch 밀리초 (payload 공통 필드)
now_ms() { date +%s%3N; }

pub() {
    # $1 = command, $2 = action, $3 = value(옵션)
    local payload
    if [ -n "$3" ]; then
        payload="{\"node_id\":\"rpic\",\"timestamp\":$(now_ms),\"seq\":$SEQ,\"command\":\"$1\",\"action\":\"$2\",\"value\":$3}"
    else
        payload="{\"node_id\":\"rpic\",\"timestamp\":$(now_ms),\"seq\":$SEQ,\"command\":\"$1\",\"action\":\"$2\"}"
    fi
    SEQ=$((SEQ + 1))
    echo "PUB $payload"
    mosquitto_pub -h "$HOST" -q 1 -t "$TOPIC" -m "$payload"
}

onoff() {
    # $1 = command, $2 = on|off
    case "$2" in
    on)  pub "$1" ON ;;
    off) pub "$1" OFF ;;
    *)   echo "사용법: $0 $1 on|off"; exit 1 ;;
    esac
}

case "$1" in
servo1) pub servo_1 SET "$2" ;;
fan)
    case "$2" in
    on|off) onoff fan "$2" ;;
    *)      pub fan SET "$2" ;;
    esac
    ;;
shutter)
    case "$2" in
    close) pub shutter CLOSE ;;
    open)  pub shutter OPEN ;;
    stop)  pub shutter STOP ;;
    *) echo "셔터 동작은 close / open / stop 중 하나여야 합니다."; exit 1 ;;
    esac ;;
pump)   onoff water_pump "$2" ;;
sound)  pub sound SET "$2" ;;
demo)
    pub servo_1 SET 90;  sleep 1   # 가스밸브
    pub fan SET 70;      sleep 1
    pub shutter CLOSE;   sleep 2
    pub water_pump ON;   sleep 1
    pub sound SET 1;     sleep 2   # 화재음
    pub water_pump OFF
    pub fan OFF
    pub shutter STOP
    pub servo_1 SET 0
    echo "demo 완료 - 서브스크라이버 stdout/dmesg로 적용 확인"
    ;;
state)
    echo "== 드라이버 적용 로그 (dmesg) =="
    # 팬은 커널 모듈이 아니라 App sysfs 제어라 dmesg에 안 남음(App stdout 참조)
    dmesg | grep -E 'rpic_(pca9685|stepper|pump)' | tail -15
    ;;
*)
    grep '^#   ' "$0" | sed 's/^#   //'
    exit 1
    ;;
esac
