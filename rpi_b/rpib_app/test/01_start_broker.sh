#!/bin/bash
# 01_start_broker.sh - 테스트용 브로커 확보 (평문 1883)
#
# apt로 mosquitto를 깔면 시스템 서비스가 이미 1883을 물고 있는 경우가
# 대부분이라 (rpi_c 테스트에서 실제로 겪은 Address already in use),
# 무작정 새로 띄우지 않고 먼저 확인한다.
set -e

if ! command -v mosquitto > /dev/null; then
    echo "mosquitto 미설치. 설치: sudo apt install mosquitto mosquitto-clients"
    exit 1
fi

# 이미 1883이 열려 있으면 그걸 쓴다 (mosquitto 2.0 기본 설정은 익명
# 거부일 수 있으므로 실제 pub/sub으로 확인)
if ss -lnt 2>/dev/null | grep -q ':1883 '; then
    echo "1883 포트에 이미 브로커가 떠 있음. 익명 pub/sub 가능한지 확인..."
    if timeout 3 mosquitto_pub -h localhost -t guardx/selftest -m ping 2>/dev/null; then
        echo "-> 기존 브로커 그대로 사용하면 됨. 이 스크립트 종료."
        exit 0
    fi
    echo "-> 기존 브로커가 익명 접속을 거부함. 둘 중 하나:"
    echo "   a) sudo cp ../broker/guardx_broker.conf /etc/mosquitto/conf.d/ && sudo systemctl restart mosquitto"
    echo "   b) sudo systemctl stop mosquitto  (그 후 이 스크립트 재실행)"
    exit 1
fi

CONF="$(dirname "$0")/mosquitto_test.conf"

cat > "$CONF" << 'EOF'
# GuardX 테스트용 브로커 설정 (평문, 익명 허용 - 테스트 전용!)
listener 1883 0.0.0.0
allow_anonymous true
log_type all
EOF

echo "테스트 브로커 기동 (Ctrl+C로 종료)"
mosquitto -c "$CONF" -v
