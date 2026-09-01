#!/usr/bin/env bash
# setup_security.sh — RPi B 보안 프로비저닝 (멱등 — 재실행 안전)
#
# 사용 (Pi에서):  sudo bash setup_security.sh
#        옵션:    sudo bash setup_security.sh --refresh-cert   (카메라 인증서 재추출)
#
# 수행 항목:
#   1) 카메라 TLS 인증서 추출·고정 (camera.pem + CAM_INSECURE=0 + CAM_CAINFO)
#   2) 자격증명 파일 권한 잠금 (config.env·camera.pem → 600)
#   3) systemd 샌드박스 드롭인 (읽기전용 FS + 권한 상승 차단)
#   4) 폴러 재시작 + 검증 (journal에서 [http] SSL 오류 유무 확인)
#
# SSH 키 전용 전환은 의도적으로 제외 — 원격 락아웃 위험이 있어 사람이
# 세션을 열어둔 채 수동으로 할 것 (TODO §보안 참조).
set -euo pipefail

RPI_B_DIR="${RPI_B_DIR:-/home/juan/7th_VEDA_GROUP2/rpi_b}"
ENV_FILE="$RPI_B_DIR/config.env"
PEM_FILE="$RPI_B_DIR/camera.pem"
DROPIN_DIR="/etc/systemd/system/guardx-poller.service.d"

[ -f "$ENV_FILE" ] || { echo "config.env 없음: $ENV_FILE"; exit 1; }

# config.env 에서 키 읽기/쓰기 (있으면 교체, 없으면 추가)
get_kv() { grep -E "^(export )?$1=" "$ENV_FILE" | tail -1 | sed -E "s/^(export )?$1=//; s/^['\"]//; s/['\"]\$//"; }
set_kv() {
  if grep -qE "^(export )?$1=" "$ENV_FILE"; then
    sed -i -E "s|^(export )?$1=.*|$1=$2|" "$ENV_FILE"
  else
    echo "$1=$2" >> "$ENV_FILE"
  fi
}

CAM_HOST="$(get_kv CAM_HOST)"
[ -n "$CAM_HOST" ] || { echo "config.env 에 CAM_HOST 없음"; exit 1; }

# state 디렉터리: config.env STATE_PATH 기준 (기본 ./state/poller_state.json).
# ReadWritePaths 는 존재하지 않는 경로면 NAMESPACE 실패로 서비스가 아예 못 뜬다
# (실사고 2026-07-28) — 반드시 실경로를 알아내 미리 만들어 둔다.
STATE_PATH="$(get_kv STATE_PATH)"
[ -n "$STATE_PATH" ] || STATE_PATH="./state/poller_state.json"
case "$STATE_PATH" in
  /*) STATE_DIR="$(dirname "$STATE_PATH")" ;;
  *)  STATE_DIR="$RPI_B_DIR/$(dirname "${STATE_PATH#./}")" ;;
esac
mkdir -p "$STATE_DIR"
chown juan:juan "$STATE_DIR"
echo "state 디렉터리: $STATE_DIR"

# ── 1) 인증서 추출 + 고정 ──
if [ ! -s "$PEM_FILE" ] || [ "${1:-}" = "--refresh-cert" ]; then
  echo "[1/4] 카메라($CAM_HOST) 인증서 추출 -> camera.pem"
  openssl s_client -connect "$CAM_HOST:443" -showcerts </dev/null 2>/dev/null \
    | openssl x509 > "$PEM_FILE"
  [ -s "$PEM_FILE" ] || { echo "인증서 추출 실패 — 카메라 도달 확인"; exit 1; }
else
  echo "[1/4] camera.pem 이미 존재 — 유지 (재추출: --refresh-cert)"
fi
openssl x509 -in "$PEM_FILE" -noout -subject -enddate   # 어떤 신원을 고정했는지 표시

# 공개키 핀 계산 (sha256//BASE64) — 카메라 인증서 CN은 hanwha-security.com
# 도메인이라 IP 접속 시 호스트명 검증이 항상 실패 → CAINFO(strict) 대신
# CURLOPT_PINNEDPUBLICKEY 로 신원 고정 (http_client.cpp 핀 모드).
PIN="sha256//$(openssl x509 -in "$PEM_FILE" -pubkey -noout \
  | openssl pkey -pubin -outform der 2>/dev/null \
  | openssl dgst -sha256 -binary | openssl enc -base64)"
echo "공개키 핀: $PIN"
set_kv CAM_INSECURE 0
set_kv CAM_PINNED_KEY "$PIN"

# ── 2) 파일 권한 ──
echo "[2/4] config.env·camera.pem -> 600"
chown juan:juan "$ENV_FILE" "$PEM_FILE"
chmod 600 "$ENV_FILE" "$PEM_FILE"

# ── 3) systemd 샌드박스 ──
echo "[3/4] systemd 드롭인 설치: $DROPIN_DIR/hardening.conf"
mkdir -p "$DROPIN_DIR"
cat > "$DROPIN_DIR/hardening.conf" <<EOF
[Service]
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=$STATE_DIR /run/postgresql
ProtectKernelTunables=yes
ProtectControlGroups=yes
RestrictSUIDSGID=yes
EOF
systemctl daemon-reload

# ── 4) 재시작 + 검증 ──
echo "[4/4] 폴러 재시작 + 20초 관찰"
systemctl restart guardx-poller
sleep 20
if journalctl -u guardx-poller --since "-20s" | grep -q "\[http\]"; then
  echo "⚠ [http] 오류 감지 — 아래 로그 확인 (인증서 hostname 불일치 가능성):"
  journalctl -u guardx-poller --since "-20s" | grep "\[http\]" | head -5
  echo "임시 복구: config.env 의 CAM_INSECURE=1 후 restart. 로그를 공유할 것."
  exit 1
fi
if ! systemctl is-active --quiet guardx-poller; then
  echo "⚠ 폴러 비활성 — journalctl -u guardx-poller -n 30 확인"
  exit 1
fi
echo "✅ 완료: TLS 고정 + 권한 600 + 샌드박스 가동, 폴러 정상"
echo "   (남은 수동 항목: SSH PasswordAuthentication no — TODO §보안)"
