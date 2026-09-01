#!/bin/bash
# 01_load_modules.sh - 드라이버 빌드 후 실 하드웨어 모드로 전체 로드
# 실행 위치: rpia_app/test/  (RPi 위에서 root 또는 sudo로 실행)
#
# 전제: rpia_adc는 오버레이(dtoverlay=rpia-adc)가 적용돼 있어야 probe 바인딩됨.
#       (없으면 RUN.md A-1 참조해서 오버레이 설치 후 재부팅)
set -e

DRIVERS_DIR="$(cd "$(dirname "$0")/../drivers" && pwd)"

echo "[1/3] 드라이버 빌드"
make -C "$DRIVERS_DIR"

echo "[2/3] 기존 모듈 언로드 (있다면)"
for m in rpia_button rpia_irtemp rpia_temphum rpia_adc; do
    if lsmod | grep -q "^$m "; then
        if [ "$m" = "rpia_adc" ]; then modprobe -r rpia_adc; else rmmod "$m"; fi
        echo "  - $m 언로드"
    fi
done

echo "[3/3] 실 하드웨어 모드로 로드"
modprobe rpia_adc                                # SPI, 오버레이 probe
insmod "$DRIVERS_DIR/rpia_temphum.ko"            # I2C 0x44
insmod "$DRIVERS_DIR/rpia_irtemp.ko"             # I2C 0x5a
insmod "$DRIVERS_DIR/rpia_button.ko"             # GPIO23(=base 512+23)

echo ""
echo "== 로드 확인 =="
lsmod | grep rpia_
echo ""
echo "== /dev 노드 확인 =="
ls -l /dev/rpia_*
echo ""
echo "== /dev/spidev0.0 (없어야 정상) =="
ls -l /dev/spidev0.0 2>/dev/null || echo "  (없음 - CE0가 rpia_adc로 넘어감, 정상)"
echo ""
echo "== dmesg 마지막 로그 =="
dmesg | grep rpia_ | tail -8
