#!/bin/bash
# 02_start_broker.sh - 테스트용 mosquitto 브로커 기동 (평문 1883)
#
# 실제 배포에서는 RPi B의 mosquitto(mTLS, 8883)를 쓰지만,
# 테스트 단계에선 RPi C 로컬에 평문 브로커를 띄워 단독 검증한다.
# (mqtt_sub.h의 MQTT_USE_TLS=0, MQTT_BROKER_HOST="localhost" 기본값과 맞물림)
set -e

if ! command -v mosquitto > /dev/null; then
    echo "mosquitto 미설치. 설치: sudo apt install mosquitto mosquitto-clients"
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
