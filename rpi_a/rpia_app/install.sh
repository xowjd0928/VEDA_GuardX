#!/bin/bash
# install.sh - RPi A 배포용 설치 스크립트 (root로 실행)
#
# 1) drivers/*.ko, app/rpia_publisher 를 /opt/guardx/rpia/ 로 복사
# 2) rpia-adc SPI 오버레이 컴파일 + /boot/firmware/overlays/ 설치
# 3) systemd 유닛을 /etc/systemd/system/ 로 복사
# 4) systemctl enable (부팅 시 자동 적재/실행)
set -e

if [ "$EUID" -ne 0 ]; then
    echo "root로 실행하세요: sudo ./install.sh"
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="/opt/guardx/rpia"
OVERLAY_DIR="/boot/firmware/overlays"
CONFIG_TXT="/boot/firmware/config.txt"

echo "[1/5] 빌드 (.ko + App + 오버레이)"
make -C "$ROOT_DIR/drivers"
make -C "$ROOT_DIR/app"
dtc -@ -I dts -O dtb -o "$ROOT_DIR/drivers/rpia-adc.dtbo" \
    "$ROOT_DIR/drivers/rpia-adc-overlay.dts"

echo "[2/5] 산출물 배치: $INSTALL_DIR"
mkdir -p "$INSTALL_DIR/drivers" "$INSTALL_DIR/app"
cp "$ROOT_DIR"/drivers/*.ko "$INSTALL_DIR/drivers/"
cp "$ROOT_DIR/app/rpia_publisher" "$INSTALL_DIR/app/"

echo "[3/5] SPI 오버레이 설치"
cp "$ROOT_DIR/drivers/rpia-adc.dtbo" "$OVERLAY_DIR/"
if ! grep -q "^dtoverlay=rpia-adc" "$CONFIG_TXT"; then
    echo "  - $CONFIG_TXT 에 dtoverlay=rpia-adc 추가 (재부팅 후 적용)"
    printf '\n[all]\ndtoverlay=rpia-adc\n' >> "$CONFIG_TXT"
else
    echo "  - dtoverlay=rpia-adc 이미 있음"
fi
# SPI/I2C 인터페이스 활성화(멱등)
raspi-config nonint do_spi 0 || true
raspi-config nonint do_i2c 0 || true

echo "[4/5] systemd 유닛 설치"
cp "$ROOT_DIR"/systemd/*.service /etc/systemd/system/
systemctl daemon-reload

echo "[5/5] 부팅 시 자동 시작 활성화"
for svc in rpia_adc rpia_temphum rpia_irtemp rpia_button rpia_publisher; do
    systemctl enable "${svc}.service"
done

echo ""
echo "완료. rpia_adc는 오버레이(config.txt)가 필요하므로 최초 1회 재부팅 권장:"
echo "  sudo reboot"
echo ""
echo "재부팅 없이 지금 바로 시작하려면(오버레이가 이미 적용된 경우):"
echo "  sudo systemctl start rpia_adc rpia_temphum rpia_irtemp rpia_button"
echo "  sudo systemctl start rpia_publisher"
echo "상태 확인: systemctl status rpia_publisher"
echo "로그 확인: journalctl -u rpia_publisher -f"
