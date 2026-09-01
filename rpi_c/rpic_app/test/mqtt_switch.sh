#!/usr/bin/env bash
# GuardX RPi C 방송 모드 스위치 — MQTT ↔ RTP.
#
# rpic_subscriber(액추에이터 앱)는 두 모드 모두에서 항상 켜둔 채로 둔다.
# 스피커(ALSA)를 실제로 두고 다투는 건 RTP 수신기(receive.sh, GStreamer)
# 하나뿐이다 — MQTT 방송 세션은 방송 중일 때만 장치를 잠깐 열고 놓기 때문에,
# RTP 수신기만 안 켜져 있으면 MQTT 쪽은 항상 바로 쓸 수 있다.
#
# 기본 전송방식은 RTP다(systemd guardx-broadcast-rtp가 부팅 시 자동 시작).
# 이 스크립트로 하는 건 "지금 잠깐 끄고 켜는 것"뿐 — enable/disable을
# 건드리지 않으므로 재부팅하면 다시 RTP로 돌아온다(의도된 동작).
#
# 사용법:
#   ./mqtt_switch.sh mqtt   # RTP 수신기 정지 → VMS에서 MQTT 방송 즉시 사용 가능
#   ./mqtt_switch.sh rtp    # RTP 수신기 시작 → VMS에서 RTP 방송 사용 가능
#   ./mqtt_switch.sh status # 지금 뭐가 켜져 있는지 확인
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# receive.sh는 이 스크립트와 다른 폴더(../broadcast_rtp/)에 있다 —
# systemd 유닛이 없는 임시 환경에서만 이 경로로 직접 띄운다.
RECEIVE_SH="$HERE/../broadcast_rtp/receive.sh"
SERVICE="guardx-broadcast-rtp"

rtp_running() {
    systemctl is-active --quiet "$SERVICE" 2>/dev/null && return 0
    pgrep -f "gst-launch.*OPUS" >/dev/null 2>&1 && return 0
    return 1
}

stop_rtp() {
    if systemctl list-unit-files 2>/dev/null | grep -q "^${SERVICE}\.service"; then
        sudo systemctl stop "$SERVICE" 2>/dev/null || true
    fi
    pkill -f "gst-launch.*OPUS" 2>/dev/null || true
    # 장치 반납이 즉시 안 될 수 있어 짧게 대기
    sleep 0.3
}

start_rtp() {
    if systemctl list-unit-files 2>/dev/null | grep -q "^${SERVICE}\.service"; then
        sudo systemctl start "$SERVICE"
    else
        nohup "$RECEIVE_SH" >/tmp/guardx-broadcast-rtp.log 2>&1 &
        disown
    fi
}

case "${1:-status}" in
    mqtt)
        stop_rtp
        echo "[mqtt_switch] RTP 수신기 정지 — 스피커는 rpic_subscriber(MQTT)가 씁니다."
        echo "[mqtt_switch] 재부팅하면 RTP로 돌아갑니다(enable 상태 유지) — 계속 mqtt로 두려면"
        echo "              sudo systemctl disable guardx-broadcast-rtp 를 별도로 실행하세요."
        ;;
    rtp)
        start_rtp
        sleep 0.5
        if rtp_running; then
            echo "[mqtt_switch] RTP 수신기 시작됨 — VMS에서 RTP 모드로 방송하세요."
        else
            echo "[mqtt_switch] RTP 수신기 시작 실패 — /tmp/guardx-broadcast-rtp.log 확인하세요." >&2
            exit 1
        fi
        ;;
    status)
        if rtp_running; then
            echo "[mqtt_switch] 현재: RTP 수신기 켜짐 (MQTT 방송은 장치 충돌 남)"
        else
            echo "[mqtt_switch] 현재: RTP 수신기 꺼짐 (MQTT 방송 바로 사용 가능)"
        fi
        ;;
    *)
        echo "사용법: $0 {mqtt|rtp|status}" >&2
        exit 1
        ;;
esac
