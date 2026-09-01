#!/bin/bash
# 04_inject.sh - 시뮬레이션 값 주입 헬퍼 (simulate=1로 로드된 드라이버용)
#
# NOTE: rpia_adc(가스/불꽃, SPI)는 시뮬레이션 모드가 없어 실 하드웨어가
#       필요하다. 여기서 주입 가능한 건 simulate=1을 지원하는 3종뿐이다:
#       temphum / irtemp / button.
# 사용법:
#   ./04_inject.sh temphum 235 602   # 온도 23.5도, 습도 60.2% (x10 정수)
#   ./04_inject.sh irtemp 250 315    # 주변 25.0도, 대상 31.5도 (x10 정수)
#   ./04_inject.sh button            # 비상 버튼 1회 눌림
#   ./04_inject.sh status            # 현재 주입값 확인
set -e

DBGFS=/sys/kernel/debug

case "$1" in
temphum)
    # 음수 온도는 s16의 2의 보수 unsigned 표기 필요 (예: -5.0도 = 65486)
    echo "$2" > "$DBGFS/rpia_temphum/temperature_x10"
    echo "$3" > "$DBGFS/rpia_temphum/humidity_x10"
    echo "temphum = temp_x10:$2 hum_x10:$3"
    ;;
irtemp)
    echo "$2" > "$DBGFS/rpia_irtemp/ambient_x10"
    echo "$3" > "$DBGFS/rpia_irtemp/object_x10"
    echo "irtemp = ambient_x10:$2 object_x10:$3"
    ;;
button)
    echo 1 > "$DBGFS/rpia_button/press"
    echo "button pressed (1 event fired)"
    ;;
status)
    echo "temp_x10:    $(cat $DBGFS/rpia_temphum/temperature_x10)"
    echo "hum_x10:     $(cat $DBGFS/rpia_temphum/humidity_x10)"
    echo "ir_amb_x10:  $(cat $DBGFS/rpia_irtemp/ambient_x10)"
    echo "ir_obj_x10:  $(cat $DBGFS/rpia_irtemp/object_x10)"
    ;;
*)
    grep '^#   ' "$0" | sed 's/^#   //'
    exit 1
    ;;
esac
