#!/bin/bash
# 04_cleanup.sh - 테스트 환경 정리
# 엔진/브로커/모니터는 각자 터미널에서 Ctrl+C로 종료.

rm -f "$(dirname "$0")/mosquitto_test.conf"
rm -f "$(dirname "$0")/../app/rpib_events.jsonl"
echo "정리 완료 (이벤트 기록 파일 삭제됨)"
