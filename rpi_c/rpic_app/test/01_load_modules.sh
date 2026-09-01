#!/bin/bash
# 01_load_modules.sh - 드라이버 빌드 후 시뮬레이션 모드로 전체 로드
# 실행 위치: rpic_app/test/  (RPi 위에서 root 또는 sudo로 실행)
set -e

DRIVERS_DIR="$(cd "$(dirname "$0")/../drivers" && pwd)"

echo "[1/3] 드라이버 빌드"
make -C "$DRIVERS_DIR"

echo "[2/3] 기존 모듈 언로드 (있다면)"
for m in rpic_pca9685 rpic_stepper rpic_pump; do
    if lsmod | grep -q "^$m "; then
        rmmod "$m"
        echo "  - $m 언로드"
    fi
done

# 팬은 커널 모듈이 아니라 App이 sysfs(pwmchip0)로 직접 제어하므로
# insmod 대상이 아니다. (실물에선 dtoverlay=pwm,pin=12,func=4 필요)
echo "[3/3] 시뮬레이션 모드로 로드"
insmod "$DRIVERS_DIR/rpic_pca9685.ko" simulate=1
insmod "$DRIVERS_DIR/rpic_stepper.ko" simulate=1
insmod "$DRIVERS_DIR/rpic_pump.ko"    simulate=1
# rpic_amp 제거됨 - MAX98357A SD_MODE는 ASoC, 소리는 rpic_audio(ALSA)가 담당

echo ""
echo "== 로드 확인 =="
lsmod | grep rpic_
echo ""
echo "== /dev 노드 확인 =="
ls -l /dev/rpic_*
echo ""
echo "== dmesg 마지막 로그 =="
dmesg | grep rpic_ | tail -8
