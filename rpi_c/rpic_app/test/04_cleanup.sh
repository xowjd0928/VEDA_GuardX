#!/bin/bash
# 04_cleanup.sh - 테스트 환경 정리 (root 또는 sudo)
# 서브스크라이버/브로커는 각자 터미널에서 Ctrl+C로 끄고, 모듈만 여기서 내린다.

for m in rpic_pca9685 rpic_stepper rpic_pump; do
    if lsmod | grep -q "^$m "; then
        rmmod "$m"
        echo "$m 언로드"
    fi
done

rm -f "$(dirname "$0")/mosquitto_test.conf"
echo "정리 완료"
