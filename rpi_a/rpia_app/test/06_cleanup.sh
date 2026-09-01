#!/bin/bash
# 06_cleanup.sh - 모듈 언로드 및 정리
for m in rpia_button rpia_irtemp rpia_temphum; do
    if lsmod | grep -q "^$m "; then
        rmmod "$m" && echo "$m 언로드"
    fi
done
if lsmod | grep -q "^rpia_adc "; then
    modprobe -r rpia_adc && echo "rpia_adc 언로드"
fi
pkill -f rpia_publisher 2>/dev/null && echo "rpia_publisher 종료" || true
pkill -f "mosquitto -c.*mosquitto_test" 2>/dev/null && echo "테스트 브로커 종료" || true
echo "완료"
