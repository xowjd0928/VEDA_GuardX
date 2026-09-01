#!/usr/bin/env bash
# GuardX RPi C - 방송/사이렌 우선순위 통합 시험
#
# 스피커 한 대를 방송과 화재 사이렌이 배타적으로 나눠 쓰는지 9개 시나리오로
# 확인한다. VMS 없이 MQTT 를 직접 발행해 RPi C 만 시험한다 - 실패 지점을
# VMS 쪽과 섞지 않으려는 것이다(VMS 연동은 이 시험이 끝난 뒤).
#
#   sudo ./audio_priority_test.sh            전체
#   sudo ./audio_priority_test.sh 3 5        3번과 5번만
#
# 전제:
#   - rpic_subscriber 가 실행 중 (조율기가 그 안에 있다)
#   - guardx-broadcast-rtp 유닛이 설치돼 있고 enable 은 아님
#   - 브로커에 접속 가능 (아래 BROKER/인증서 변수)
#
# 각 단계는 사람이 귀로 확인해야 하는 것을 물어본다 - 스피커 소리는 자동으로
# 판정할 수 없기 때문이다. 장치 점유처럼 기계가 볼 수 있는 것은 자동 판정한다.

set -uo pipefail

BROKER="${GUARDX_BROKER:-172.20.33.251}"
PORT="${GUARDX_BROKER_PORT:-8883}"
CA="${GUARDX_CA:-/etc/guardx/certs/ca.crt}"
CERT="${GUARDX_CERT:-/etc/guardx/certs/rpic.crt}"
KEY="${GUARDX_KEY:-/etc/guardx/certs/rpic.key}"

TOPIC_FIRE="guardx/display/rpic/fire"
TOPIC_BCAST="guardx/broadcast/rpic/command"
TOPIC_READY="guardx/broadcast/rpic/ready"

if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_BAD=$'\033[31m'; C_HEAD=$'\033[1;36m'
    C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
else
    C_OK=""; C_BAD=""; C_HEAD=""; C_DIM=""; C_OFF=""
fi

PASS=0; FAIL=0
FAILED_LIST=""

pub() {
    local topic="$1" payload="$2"
    if [ "$PORT" = "8883" ]; then
        mosquitto_pub -h "$BROKER" -p "$PORT" \
            --cafile "$CA" --cert "$CERT" --key "$KEY" \
            -q 1 -t "$topic" -m "$payload"
    else
        mosquitto_pub -h "$BROKER" -p "$PORT" -q 1 -t "$topic" -m "$payload"
    fi
}

fire()      { pub "$TOPIC_FIRE"  "{\"node_id\":\"test\",\"zone_bitmap\":$1}"; }
bcast()     { pub "$TOPIC_BCAST" "{\"node_id\":\"test\",\"action\":\"$1\",\"session_id\":$2}"; }

# 실제 VMS 는 방송 중 2초마다 KEEPALIVE 를 재발행한다. 그게 없으면 RPi C 가
# 5초 뒤 방송을 만료 처리하는데, 사람이 y/n 을 입력하는 사이 그 시간이 그냥
# 지나가 버려 시나리오가 통째로 어긋난다(4번에서 사이렌이 되살아나고, 5번의
# STOP 은 이미 끝난 세션이라 무시된다). 그래서 여기서도 VMS 처럼 보낸다.
KA_PID=""
ka_start() {
    ka_stop
    ( while :; do sleep 2; bcast KEEPALIVE "$1" >/dev/null 2>&1 || exit 0; done ) &
    KA_PID=$!
}
ka_stop() {
    [ -n "$KA_PID" ] && kill "$KA_PID" 2>/dev/null
    wait "$KA_PID" 2>/dev/null
    KA_PID=""
}
trap 'ka_stop; fire 0 >/dev/null 2>&1' EXIT INT TERM

# 방송 시작/종료 + KEEPALIVE 수명 관리를 한 쌍으로 묶는다.
bcast_on()  { bcast START "$1"; ka_start "$1"; }
bcast_off() { ka_stop; bcast STOP "$1"; }

# 재생 서브스트림이 열려 있는가(= 누군가 스피커를 쥐고 있는가).
speaker_held() {
    local f
    for f in /proc/asound/card*/pcm1p/sub0/hw_params; do
        [ -e "$f" ] || continue
        grep -q closed "$f" || return 0
    done
    return 1
}

holder() { fuser -v /dev/snd/pcmC*D1p 2>&1 | tail -n +2 | awk 'NF {print $NF}' | sort -u | tr '\n' ' '; }
# systemctl is-active 는 inactive 일 때 exit 3 을 낸다. `|| echo unknown` 을
# 붙이면 출력이 "inactive\nunknown" 두 줄이 되어 비교가 전부 깨진다.
rtp_state() {
    local v
    v=$(systemctl is-active guardx-broadcast-rtp 2>/dev/null | head -1)
    printf '%s' "${v:-unknown}"
}

step()  { printf '\n%s── [%s] %s%s\n' "$C_HEAD" "$1" "$2" "$C_OFF"; }
info()  { printf '%s     %s%s\n' "$C_DIM" "$*" "$C_OFF"; }

# 자동 판정
expect() {
    local what="$1" want="$2" got="$3"
    if [ "$want" = "$got" ]; then
        PASS=$((PASS+1)); printf '%s     [통과]%s %s = %s\n' "$C_OK" "$C_OFF" "$what" "$got"
    else
        FAIL=$((FAIL+1)); FAILED_LIST="${FAILED_LIST}\n  - ${what} (기대 ${want}, 실제 ${got})"
        printf '%s     [실패]%s %s: 기대 %s, 실제 %s\n' "$C_BAD" "$C_OFF" "$what" "$want" "$got"
    fi
}

# 사람 판정 - 소리는 기계가 못 듣는다
ask() {
    local q="$1" ans=""
    printf '%s     [확인]%s %s (y/n) ' "$C_HEAD" "$C_OFF" "$q"
    read -r ans
    case "$ans" in
        y|Y) PASS=$((PASS+1)) ;;
        *)   FAIL=$((FAIL+1)); FAILED_LIST="${FAILED_LIST}\n  - ${q}" ;;
    esac
}

held_str() { if speaker_held; then echo held; else echo free; fi; }

# ------------------------------------------------------------------ 시나리오

t1_idle() {
    step 1 "유휴 상태 - 아무도 스피커를 잡지 않아야 한다"
    fire 0; sleep 1
    bcast STOP 1 >/dev/null 2>&1; sleep 2
    info "점유: $(holder)  수신기: $(rtp_state)"
    expect "유휴 시 스피커" free "$(held_str)"
    expect "유휴 시 수신기" inactive "$(rtp_state)"
}

t2_broadcast() {
    step 2 "일반 방송 시작/종료"
    bcast_on 1001; sleep 3
    info "점유: $(holder)  수신기: $(rtp_state)"
    expect "방송 중 수신기" active "$(rtp_state)"
    expect "방송 중 스피커" held "$(held_str)"
    info "READY ACK 확인은 아래 명령을 다른 터미널에서 미리 띄워두고 보세요:"
    info "  mosquitto_sub -h $BROKER -p $PORT --cafile $CA --cert $CERT --key $KEY -t $TOPIC_READY -v"
    bcast_off 1001; sleep 3
    expect "방송 종료 후 수신기" inactive "$(rtp_state)"
    expect "방송 종료 후 스피커" free "$(held_str)"
}

t3_fire_during_broadcast() {
    step 3 "방송 중 화재 - 방송을 끊고 사이렌"
    bcast_on 1002; sleep 3
    expect "방송 시작됨" active "$(rtp_state)"
    fire 1; sleep 3
    info "점유: $(holder)  수신기: $(rtp_state)"
    expect "화재 후 수신기" inactive "$(rtp_state)"
    ask "사이렌이 반복해서 울리고 있습니까?"
}

t4_broadcast_during_fire() {
    step 4 "화재 중 운영자 방송 - 사이렌을 멈추고 방송"
    info "(3번에 이어 화재가 계속인 상태여야 합니다)"
    bcast_on 1003; sleep 4
    expect "화재 중 방송 수신기" active "$(rtp_state)"
    ask "사이렌이 멎었습니까?"
}

t5_siren_resume() {
    step 5 "방송 종료 후 화재 지속 - 사이렌 재개"
    bcast_off 1003; sleep 3
    expect "방송 종료 후 수신기" inactive "$(rtp_state)"
    ask "사이렌이 다시 울립니까?"
}

t6_fire_clear() {
    step 6 "화재 해제 - 사이렌 즉시 정지"
    fire 0; sleep 2
    ask "사이렌이 멎었습니까?"
    expect "해제 후 스피커" free "$(held_str)"
}

t7_keepalive_timeout() {
    step 7 "KEEPALIVE 만료 (VMS 강제 종료 흉내)"
    bcast START 1004; sleep 3
    expect "방송 시작됨" active "$(rtp_state)"
    info "STOP 을 보내지 않고 6초 기다립니다 (만료 5초)"
    sleep 7
    expect "만료 후 수신기" inactive "$(rtp_state)"
    expect "만료 후 스피커" free "$(held_str)"
}

t8_app_restart() {
    step 8 "RPi C 앱 재시작 - 남은 수신기를 정리하는가"
    bcast START 1005; sleep 3
    expect "방송 시작됨" active "$(rtp_state)"
    info "조율기를 거치지 않고 rpic_subscriber 만 재시작합니다"
    if systemctl is-active rpic_subscriber >/dev/null 2>&1; then
        systemctl restart rpic_subscriber
    else
        info "유닛이 없습니다 - 수동 실행 중이라면 지금 Ctrl+C 후 다시 띄우고 엔터"
        read -r _
    fi
    sleep 4
    expect "재시작 후 수신기" inactive "$(rtp_state)"
    expect "재시작 후 스피커" free "$(held_str)"
}

t9_stale_stop() {
    step 9 "낡은 STOP / 중복 START"
    bcast_on 2001; sleep 3
    expect "세션 2001 방송" active "$(rtp_state)"
    info "다른 세션(2000)의 STOP - 무시돼야 합니다"
    bcast STOP 2000; sleep 2
    expect "낡은 STOP 뒤에도 방송" active "$(rtp_state)"
    info "같은 세션 중복 START - 무해해야 합니다"
    bcast START 2001; sleep 2
    expect "중복 START 뒤에도 방송" active "$(rtp_state)"
    bcast_off 2001; sleep 3
    expect "정상 STOP 후 종료" inactive "$(rtp_state)"
}

# ----------------------------------------------------------------------- 진행

command -v mosquitto_pub >/dev/null || {
    echo "mosquitto_pub 이 없습니다: sudo apt install -y mosquitto-clients"; exit 2; }

if ! pgrep -x rpic_subscriber >/dev/null 2>&1; then
    echo "경고: rpic_subscriber 가 안 보입니다. 조율기가 없으면 전부 실패합니다."
    echo "      계속하려면 엔터, 중단하려면 Ctrl+C"
    read -r _
fi

ALL="1 2 3 4 5 6 7 8 9"
SEL="${*:-$ALL}"

printf '브로커 %s:%s / 대상 유닛 guardx-broadcast-rtp\n' "$BROKER" "$PORT"
printf '시나리오: %s\n\n' "$SEL"
cat <<'NOTE'
[읽고 시작하세요]
  * 이 스크립트가 VMS 역할을 합니다. VMS 가 보내는 것과 같은 START/STOP 을
    직접 발행하므로, 시험 중에는 VMS 방송 버튼을 누르지 마세요 - 세션이
    둘이 되어 서로를 덮어쓰고 판정이 깨집니다.
  * "방송"이라고 해도 목소리는 안 납니다. 제어 메시지만 보내고 RTP 오디오는
    쏘지 않기 때문입니다. 이 시험이 보는 것은 "누가 스피커를 쥐고 있나"입니다.
  * 그래서 귀로 판단할 것은 사이렌뿐입니다. 방송 음성 확인은 이 시험을 전부
    통과한 뒤 VMS 로 따로 합니다.
NOTE
printf '\n엔터를 누르면 시작합니다 (중단은 Ctrl+C) '
read -r _

for n in $SEL; do
    case "$n" in
        1) t1_idle ;;
        2) t2_broadcast ;;
        3) t3_fire_during_broadcast ;;
        4) t4_broadcast_during_fire ;;
        5) t5_siren_resume ;;
        6) t6_fire_clear ;;
        7) t7_keepalive_timeout ;;
        8) t8_app_restart ;;
        9) t9_stale_stop ;;
        *) echo "알 수 없는 시나리오: $n" ;;
    esac
done

# 어떤 경로로 끝나든 화재 상태를 남기지 않는다
fire 0 >/dev/null 2>&1

printf '\n========================================\n'
printf '%s통과 %d%s  %s실패 %d%s\n' "$C_OK" "$PASS" "$C_OFF" "$C_BAD" "$FAIL" "$C_OFF"
if [ "$FAIL" -gt 0 ]; then
    printf '실패 항목:%b\n' "$FAILED_LIST"
    exit 1
fi
printf '전부 통과.\n'
exit 0
