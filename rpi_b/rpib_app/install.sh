#!/bin/bash
# install.sh - RPi B 배포용 설치 스크립트 (root로 실행)
#
# 1) app/rpib_engine 을 /opt/guardx/rpib/ 로 복사
# 2) 브로커 설정을 /etc/mosquitto/conf.d/ 로 복사
# 3) systemd 유닛 설치 + enable
set -e

if [ "$EUID" -ne 0 ]; then
    echo "root로 실행하세요: sudo ./install.sh"
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="/opt/guardx/rpib"

echo "[1/5] 빌드"
make -C "$ROOT_DIR/app"

echo "[2/5] 산출물 배치: $INSTALL_DIR"
mkdir -p "$INSTALL_DIR/app"
cp "$ROOT_DIR/app/rpib_engine" "$INSTALL_DIR/app/"

echo "[3/5] 브로커 설정 설치 (mosquitto, 평문 1883 + mTLS 8883 병행)"
if ! command -v mosquitto > /dev/null; then
    echo "mosquitto 미설치. 설치: sudo apt install mosquitto"
    exit 1
fi
if [ ! -f /etc/mosquitto/certs/ca.crt ] || [ ! -f /etc/mosquitto/certs/rpib.crt ] || [ ! -f /etc/mosquitto/certs/rpib.key ]; then
    echo "!!! /etc/mosquitto/certs/{ca.crt,rpib.crt,rpib.key} 가 없습니다."
    echo "    common/certs/gen_certs.sh로 발급한 인증서를 먼저 배치하세요:"
    echo "    sudo mkdir -p /etc/mosquitto/certs"
    echo "    sudo cp ca.crt rpib.crt rpib.key /etc/mosquitto/certs/"
    echo "    (권한은 아래에서 이 스크립트가 맞춰줍니다)"
    exit 1
fi

# 브로커는 root로 떠서 설정을 읽은 뒤 mosquitto 유저로 권한을 내려놓는다.
# 키를 root:root 600으로 두면 그 이후(설정 재적재 등)에 읽지 못해 기동이나
# SIGHUP에서 실패한다. 그룹 읽기를 열어주는 게 표준 배치다.
chown root:mosquitto /etc/mosquitto/certs/ca.crt \
                     /etc/mosquitto/certs/rpib.crt \
                     /etc/mosquitto/certs/rpib.key
chmod 644 /etc/mosquitto/certs/ca.crt /etc/mosquitto/certs/rpib.crt
chmod 640 /etc/mosquitto/certs/rpib.key
cp "$ROOT_DIR/broker/guardx_broker.conf" /etc/mosquitto/conf.d/
cp "$ROOT_DIR/../common/certs/guardx_mtls.conf" /etc/mosquitto/conf.d/
systemctl restart mosquitto

echo "[4/5] systemd 유닛 설치"
cp "$ROOT_DIR"/systemd/*.service /etc/systemd/system/
systemctl daemon-reload

echo "[5/5] 부팅 시 자동 시작 활성화"
systemctl enable mosquitto rpib_engine

echo ""
echo "완료. 지금 바로 시작하려면:"
echo "  sudo systemctl start rpib_engine"
echo "상태 확인: systemctl status rpib_engine"
echo "로그 확인: journalctl -u rpib_engine -f"
echo "이벤트 기록: tail -f $INSTALL_DIR/rpib_events.jsonl"
