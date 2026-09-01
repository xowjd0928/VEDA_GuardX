#!/bin/bash
# 05_driver_smoke.sh - App 없이 드라이버 단독 스모크 테스트
# /dev 노드를 직접 read해서 바이너리 값이 기대와 일치하는지 확인한다.
# (od로 리틀엔디언 정수 해석)
set -e

echo "== adc (uint16 x2: gas_raw CH1, spark_raw CH0, 0~1023) =="
od -An -t u2 -N 4 /dev/rpia_adc

echo "== temphum (int16 x2: temp_x10, hum_x10) =="
od -An -t d2 -N 4 /dev/rpia_temphum

echo "== irtemp (int16 x2: ambient_x10, object_x10) =="
od -An -t d2 -N 4 /dev/rpia_irtemp

echo "== button (uint32, 블로킹 read) =="
echo "  버튼은 눌림이 없으면 read가 블로킹된다."
echo "  실물 버튼을 누르거나, simulate=1 로드 시 ./04_inject.sh button 실행 후"
echo "  아래가 깨어나는지 확인:"
timeout 10 od -An -t u4 -N 4 /dev/rpia_button \
    && echo "  -> 버튼 이벤트 수신 성공" \
    || echo "  -> 10초 내 이벤트 없음 (타임아웃)"
