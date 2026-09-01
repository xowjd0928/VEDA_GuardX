#!/bin/bash
# 03_watch_rpic.sh - RPi C 역할을 흉내내 액추에이터 명령 구독 모니터
# (실제 RPi C가 붙어 있으면 이 스크립트 없이 C의 rpic_subscriber로 확인)
set -e

HOST="${MQTT_HOST:-localhost}"

echo "guardx/actuator/rpic 구독 중 (Ctrl+C로 종료)"
mosquitto_sub -h "$HOST" -q 1 -v -t "guardx/actuator/rpic"
