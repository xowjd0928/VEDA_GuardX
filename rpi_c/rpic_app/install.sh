#!/bin/bash
# install.sh - RPi C 배포용 설치 스크립트 (root로 실행)
#
# 1) drivers/*.ko, app/rpic_subscriber 를 /opt/guardx/rpic/ 로 복사
# 2) systemd 유닛을 /etc/systemd/system/ 로 복사
# 3) systemctl enable (부팅 시 자동 적재/실행)
set -e

if [ "$EUID" -ne 0 ]; then
    echo "root로 실행하세요: sudo ./install.sh"
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="/opt/guardx/rpic"

echo "[1/6] 빌드"
make -C "$ROOT_DIR/drivers"
make -C "$ROOT_DIR/app"

echo "[2/6] 산출물 배치: $INSTALL_DIR"
mkdir -p "$INSTALL_DIR/drivers" "$INSTALL_DIR/app" "$INSTALL_DIR/assets/audio"
cp "$ROOT_DIR"/drivers/*.ko "$INSTALL_DIR/drivers/"
cp "$ROOT_DIR/app/rpic_subscriber" "$INSTALL_DIR/app/"
cp "$ROOT_DIR"/assets/audio/*.wav "$INSTALL_DIR/assets/audio/"

echo "[3/6] systemd 유닛 설치"
cp "$ROOT_DIR"/systemd/*.service /etc/systemd/system/
# 방송 수신기 유닛도 깔되 enable 하지 않는다 - 온디맨드다. 방송이 없는 동안
# 떠 있으면 스피커를 물고 있어 화재 사이렌이 EBUSY 로 막힌다(audio_arbiter.h).
cp "$ROOT_DIR"/broadcast_rtp/guardx-broadcast-rtp.service /etc/systemd/system/
systemctl daemon-reload
systemctl disable guardx-broadcast-rtp 2>/dev/null || true

# 조율기가 그 유닛을 start/stop 하려면 권한이 필요하다. rpic_subscriber 를
# root 로 돌리면(기본 유닛에 User= 없음) 없어도 되지만, 손으로 띄우는 개발
# 환경까지 같이 동작하게 sudoers 규칙을 깐다.
echo "[4/6] sudoers 규칙 설치 (조율기의 방송 수신기 제어)"
install -m 0440 -o root -g root \
        "$ROOT_DIR/sudoers.d/guardx-rpic" /etc/sudoers.d/guardx-rpic
if ! visudo -c >/dev/null; then
    echo "  경고: sudoers 검사에 실패했습니다. 방금 넣은 파일을 되돌립니다."
    rm -f /etc/sudoers.d/guardx-rpic
fi

# RS-485 변환기의 ttyUSB 번호는 재연결마다 바뀐다. 별칭이 있어야 데몬과 CLI가
# 같은 경로를 쓸 수 있다(udev/99-guardx-rs485.rules 주석 참조).
echo "[5/6] udev 규칙 설치 (RS-485 별칭)"
cp "$ROOT_DIR"/udev/99-guardx-rs485.rules /etc/udev/rules.d/
udevadm control --reload-rules
udevadm trigger --subsystem-match=tty --action=add
if [ -e /dev/guardx-rs485 ]; then
    echo "  /dev/guardx-rs485 -> $(readlink -f /dev/guardx-rs485)"
else
    echo "  경고: /dev/guardx-rs485 가 아직 없습니다."
    echo "        변환기가 꽂혀 있는지 확인하거나, 뽑았다 다시 꽂으세요."
fi

echo "[6/6] 부팅 시 자동 시작 활성화"
for svc in rpic_pca9685 rpic_stepper rpic_pump rpic_subscriber; do
    systemctl enable "${svc}.service"
done

echo ""
echo "완료. 지금 바로 시작하려면:"
echo "  sudo systemctl start rpic_pca9685 rpic_stepper rpic_pump"
echo "  sudo systemctl start rpic_subscriber"
echo "상태 확인: systemctl status rpic_subscriber"
echo "로그 확인: journalctl -u rpic_subscriber -f"
