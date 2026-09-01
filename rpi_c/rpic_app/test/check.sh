#!/usr/bin/env bash
# GuardX RPi C 환경 점검 — 코드 외의 준비 상태를 기능별로 확인한다.
#
# 항목마다 [OK] / [경고] / [실패] 를 찍고, 기대값이 아니면 그 자리에서
# "무엇을 실행하면 되는지"를 출력한다. 마지막에 기능별 사용 가능 여부를 낸다.
#
#   cd rpic_app/test && ./check.sh     전체 점검
#   ./check.sh --quiet                 실패/경고만 출력
#
# 이 스크립트는 아무것도 고치지 않는다. 읽기 전용이다.

set -uo pipefail   # -e 는 쓰지 않는다: 한 항목이 실패해도 끝까지 점검해야 한다

QUIET=0
[ "${1:-}" = "--quiet" ] && QUIET=1

# 리포 경로는 이 스크립트 위치에서 유도한다(어디서 실행하든 동일하게 동작).
# 이 파일은 rpi_c/rpic_app/test/ 에 있으므로 세 단계 위가 리포 루트다.
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${TEST_DIR}/../../.." && pwd)"

if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_BAD=$'\033[31m'
    C_HEAD=$'\033[1;36m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
else
    C_OK=""; C_WARN=""; C_BAD=""; C_HEAD=""; C_DIM=""; C_OFF=""
fi

N_OK=0; N_WARN=0; N_BAD=0
CUR=""          # 현재 점검 항목 이름
FEAT=""         # 현재 항목이 어느 기능에 속하는지 (BCAST/MIC/SOUND/ACT/BASE)

# 기능별 차단 사유. 비어 있으면 그 기능은 사용 가능.
BLOCK_BCAST=""; BLOCK_MIC=""; BLOCK_SOUND=""; BLOCK_ACT=""

group() {
    [ "$QUIET" = 1 ] && return
    printf '\n%s─── %s %s\n' "$C_HEAD" "$1" "$C_OFF"
}

# section <항목명> [기능...]  — 기능은 공백 구분, 실패 시 그 기능들이 차단된다.
section() { CUR="$1"; shift; FEAT="$*"; }

_block() {
    local why="$1" f
    for f in $FEAT; do
        case "$f" in
            BCAST) BLOCK_BCAST="${BLOCK_BCAST}${BLOCK_BCAST:+, }${why}" ;;
            MIC)   BLOCK_MIC="${BLOCK_MIC}${BLOCK_MIC:+, }${why}" ;;
            SOUND) BLOCK_SOUND="${BLOCK_SOUND}${BLOCK_SOUND:+, }${why}" ;;
            ACT)   BLOCK_ACT="${BLOCK_ACT}${BLOCK_ACT:+, }${why}" ;;
            BASE)  BLOCK_BCAST="${BLOCK_BCAST}${BLOCK_BCAST:+, }${why}"
                   BLOCK_MIC="${BLOCK_MIC}${BLOCK_MIC:+, }${why}"
                   BLOCK_SOUND="${BLOCK_SOUND}${BLOCK_SOUND:+, }${why}" ;;
        esac
    done
}

ok()   { N_OK=$((N_OK+1)); [ "$QUIET" = 1 ] || printf '%s[OK]%s   %s\n' "$C_OK" "$C_OFF" "$CUR${1:+ — $1}"; }
warn() { N_WARN=$((N_WARN+1)); printf '%s[경고]%s %s\n' "$C_WARN" "$C_OFF" "$CUR${1:+ — $1}"; }
bad()  { N_BAD=$((N_BAD+1)); printf '%s[실패]%s %s\n' "$C_BAD" "$C_OFF" "$CUR${1:+ — $1}"; _block "$CUR"; }

fix()  { printf '%s         %s%s\n' "$C_DIM" "$*" "$C_OFF"; }

# systemctl is-active/is-enabled 는 "정상 응답"에도 0 이 아닌 코드를 낸다
# (inactive=3, disabled=1). `|| echo unknown` 을 붙이면 출력이 두 줄이 되어
# 판정이 깨지므로, 첫 줄만 취하고 빈 값일 때만 unknown 으로 본다.
unit_state() {
    local v
    v=$(systemctl is-active "$1" 2>/dev/null | head -1)
    printf '%s' "${v:-unknown}"
}
unit_enabled() {
    local v
    v=$(systemctl is-enabled "$1" 2>/dev/null | head -1)
    printf '%s' "${v:-unknown}"
}

printf '\n=== GuardX RPi C 환경 점검 ===\n'
printf '리포: %s\n' "$REPO_ROOT"

# =====================================================================
group "공통 — 부팅 설정 / 사운드카드"
# =====================================================================

section "config.txt" BASE
CONFIG_TXT=/boot/firmware/config.txt
[ -f "$CONFIG_TXT" ] || CONFIG_TXT=/boot/config.txt

if [ ! -f "$CONFIG_TXT" ]; then
    bad "찾을 수 없음"
    fix "/boot/firmware/config.txt 또는 /boot/config.txt 위치를 확인하세요."
else
    ok "$CONFIG_TXT"

    section "오디오 오버레이" BASE
    if grep -qE '^[[:space:]]*dtoverlay=guardx-i2s-duplex' "$CONFIG_TXT" \
       && grep -qE '^[[:space:]]*dtparam=audio=off' "$CONFIG_TXT"; then
        ok "guardx-i2s-duplex + audio=off"
    else
        bad "설정이 빠졌음"
        fix "$CONFIG_TXT 에 아래 두 줄이 있어야 합니다. 추가 후 재부팅:"
        fix "  dtparam=audio=off"
        fix "  dtoverlay=guardx-i2s-duplex,sdmode-pin=4"
        fix "max98357a 오버레이가 살아 있으면 앞에 # 를 붙여 주석 처리합니다."
    fi

    section "오버레이 바이너리(.dtbo)" BASE
    DTBO=/boot/firmware/overlays/guardx-i2s-duplex.dtbo
    [ -f "$DTBO" ] || DTBO=/boot/overlays/guardx-i2s-duplex.dtbo
    if [ -f "$DTBO" ]; then
        ok "$DTBO"
    else
        bad "guardx-i2s-duplex.dtbo 없음"
        fix "이 파일은 리포에 없고 장비에만 있습니다. 다른 RPi C 에서 복사하거나 dts 를 재컴파일:"
        fix "  dtc -@ -I dts -O dtb -o guardx-i2s-duplex.dtbo guardx-i2s-duplex.dts"
        fix "  sudo cp guardx-i2s-duplex.dtbo /boot/firmware/overlays/"
    fi

    section "I2C (PCA9685용)" ACT
    if grep -qE '^[[:space:]]*dtparam=i2c_arm=on' "$CONFIG_TXT"; then
        ok
    else
        bad "dtparam=i2c_arm=on 없음"
        fix "$CONFIG_TXT 에 추가하고 재부팅:  dtparam=i2c_arm=on"
    fi

    # 리드 스위치 접점이 GND 라 풀업이 없으면 리밋이 안 잡힌다(온보드 풀업이면 불필요).
    section "리드센서 풀업 (gpio=17/27)" ""
    if grep -qE '^gpio=17=ip,pu' "$CONFIG_TXT" && grep -qE '^gpio=27=ip,pu' "$CONFIG_TXT"; then
        ok
    else
        warn "설정 없음 — 셔터가 리밋에서 안 멈출 수 있음"
        fix "온보드 풀업 리드센서 모듈이면 무시해도 됩니다. 아니면 추가 후 재부팅:"
        fix "  gpio=17=ip,pu"
        fix "  gpio=27=ip,pu"
    fi
fi

section "사운드카드 MAX98357A" BASE
if aplay -l 2>/dev/null | grep -q 'MAX98357A'; then
    SPK_DEV=$(aplay -l 2>/dev/null | sed -n 's/.*MAX98357A.*device \([0-9]*\):.*/\1/p' | head -1)
    if [ "${SPK_DEV:-}" = "1" ]; then
        ok "재생 device 1"
    else
        warn "재생이 device ${SPK_DEV:-?} (예상 1)"
        fix "receive.sh / rpic_audio.h 의 DEV 번호를 실제 값에 맞추거나,"
        fix "오버레이가 guardx-i2s-duplex 인지 확인하세요(기본 max98357a 는 device 0)."
    fi
else
    bad "카드가 안 보임"
    fix "오버레이를 먼저 해결하고 재부팅하세요. 그래도 없으면 배선 확인:"
    fix "  BCLK=물리12  LRCLK=물리35  DIN=물리40  SD_MODE=물리7  VIN=5V"
fi

section "/etc/asound.conf" ""
if [ -e /etc/asound.conf ]; then
    warn "존재함 — 디버깅 중 만든 잔재일 수 있음"
    fix "내용이 깨져 있으면 ALSA 전체가 죽습니다. 직접 만든 게 아니면 삭제:"
    fix "  sudo rm -f /etc/asound.conf"
else
    ok "없음(정상)"
fi

# =====================================================================
group "방송 — VMS 마이크 → RPi C 스피커 (Opus/RTP)"
# =====================================================================

RECV="$REPO_ROOT/rpi_c/rpic_app/broadcast_rtp/receive.sh"

section "receive.sh 존재" BCAST
if [ -f "$RECV" ]; then ok; else bad "없음: $RECV"; fi

section "receive.sh 실행권한" BCAST
if [ -f "$RECV" ] && [ -x "$RECV" ]; then
    ok
elif [ -f "$RECV" ]; then
    bad "실행권한 없음 — systemd 가 status=203/EXEC 로 죽습니다"
    fix "chmod +x $RECV"
fi

# 이 두 가지가 빠지면 마이크와 동시에 쓸 때 소리가 안 난다(에러 없이 무음).
section "receive.sh S32LE 반영" BCAST
if [ -f "$RECV" ] && grep -q 'format=S32LE' "$RECV" && grep -q 'hw:CARD=MAX98357A' "$RECV"; then
    ok "hw 직결 + S32LE"
elif [ -f "$RECV" ]; then
    bad "옛 버전 — TOIMIC 과 동시에 쓰면 무음"
    fix "git pull 로 최신을 받으세요. 직접 고치려면 두 곳입니다:"
    fix "  1) ALSA_DEV 기본값을  hw:CARD=MAX98357A,DEV=1  로 (plughw 아님)"
    fix "  2) '! audioresample quality=10 \\' 다음 줄에 추가:"
    fix "     ! audio/x-raw,format=S32LE,rate=48000,channels=2 \\"
fi

section "GStreamer 플러그인" BCAST
MISSING_GST=""
for p in udpsrc rtpjitterbuffer rtpopusdepay opusdec audioconvert audioresample alsasink; do
    gst-inspect-1.0 "$p" >/dev/null 2>&1 || MISSING_GST="$MISSING_GST $p"
done
if [ -z "$MISSING_GST" ]; then
    ok "수신 파이프라인 전체"
else
    bad "누락:$MISSING_GST"
    fix "sudo apt install -y gstreamer1.0-tools gstreamer1.0-plugins-base \\"
    fix "     gstreamer1.0-plugins-good gstreamer1.0-plugins-bad"
fi

# =====================================================================
group "마이크 — TOIMIC 비명/총성 감지"
# =====================================================================

DET="$REPO_ROOT/rpi_c/rpic_app/toimic/detector.py"

section "detector.py 존재" MIC
if [ -f "$DET" ]; then ok; else bad "없음: $DET"; fi

# 캡처는 DEV=0, 재생은 DEV=1. 포맷은 S32_LE 여야 재생과 I2S 프레임이 맞는다.
section "detector.py 캡처 설정" MIC BCAST SOUND
if [ ! -f "$DET" ]; then
    bad "확인 불가"
else
    DET_DEV_OK=0; DET_FMT_OK=0
    grep -qE 'GUARDX_TOIMIC_ALSA".*DEV=0' "$DET" && DET_DEV_OK=1
    grep -qE '"-f",[[:space:]]*"S32_LE"' "$DET" && DET_FMT_OK=1
    if [ "$DET_DEV_OK" = 1 ] && [ "$DET_FMT_OK" = 1 ]; then
        ok "DEV=0 + S32_LE"
    else
        bad "캡처 장치/포맷이 기대와 다름 (DEV=0:$DET_DEV_OK, S32_LE:$DET_FMT_OK)"
        fix "캡처가 재생(DEV=1)과 겹치거나 포맷이 S32_LE 가 아니면 스피커가 무음이 됩니다."
        grep -nE 'GUARDX_TOIMIC_ALSA|"-f",' "$DET" | head -3 | sed 's/^/           /'
    fi
fi

section "yamnet 가상환경" MIC
VENV_PY="$REPO_ROOT/rpi_c/rpic_app/toimic/yamnet/bin/python"
if [ -x "$VENV_PY" ]; then
    ok "$VENV_PY"
else
    bad "없음"
    fix "경로명이 .venv 가 아니라 yamnet 입니다:"
    fix "  cd $REPO_ROOT/rpi_c/rpic_app/toimic && python3 -m venv yamnet"
    fix "  . yamnet/bin/activate && pip install -r requirements.txt"
fi

section "실행 중인 캡처 포맷" ""
AREC=$(pgrep -a arecord 2>/dev/null | head -1 || true)
if [ -z "$AREC" ]; then
    warn "arecord 미실행 (TOIMIC 이 꺼져 있음)"
    fix "sudo systemctl start guardx-toimic"
elif printf '%s' "$AREC" | grep -q 'S32_LE'; then
    ok "S32_LE"
else
    warn "실행 중인 캡처가 S32_LE 가 아님 — 스피커가 무음이 됩니다"
    fix "현재: $AREC"
fi

# =====================================================================
group "상황음 — 화재/거수자 알림음 (rpic_subscriber)"
# =====================================================================

section "rpic_subscriber 빌드" SOUND ACT
BIN="$REPO_ROOT/rpi_c/rpic_app/app/rpic_subscriber"
if [ ! -x "$BIN" ]; then
    bad "실행 파일 없음"
    fix "cd $REPO_ROOT/rpi_c/rpic_app/app && make"
    fix "빌드에 libasound2-dev 가 필요합니다:  sudo apt install -y libasound2-dev"
else
    # 소스보다 오래된 바이너리 = git pull 후 make 를 빼먹은 상태
    NEWER=$(find "$REPO_ROOT/rpi_c/rpic_app/app" "$REPO_ROOT/rpi_c/rpic_app/hal" \
                 \( -name '*.c' -o -name '*.h' \) 2>/dev/null \
            | while read -r f; do [ "$f" -nt "$BIN" ] && echo "$f"; done | head -3)
    if [ -n "$NEWER" ]; then
        bad "소스가 바이너리보다 최신 — 재빌드 필요"
        fix "cd $REPO_ROOT/rpi_c/rpic_app/app && make      (안 되면 make clean && make)"
        printf '%s           최신 소스: %s%s\n' "$C_DIM" "$(echo "$NEWER" | tr '\n' ' ')" "$C_OFF"
    else
        ok "소스보다 최신"
    fi
fi

section "rpic_audio S32 반영" SOUND
AUD_C="$REPO_ROOT/rpi_c/rpic_app/app/src/rpic_audio.c"
if [ -f "$AUD_C" ] && grep -q 'SND_PCM_FORMAT_S32_LE' "$AUD_C"; then
    ok
elif [ -f "$AUD_C" ]; then
    bad "옛 버전(S16_LE) — TOIMIC 과 동시에 쓰면 상황음이 무음"
    fix "git pull 후 재빌드하세요."
else
    bad "없음: $AUD_C"
fi

section "MQTT 브로커 설정 (mqtt_sub.h)" SOUND ACT
MQTT_H="$REPO_ROOT/rpi_c/rpic_app/app/include/mqtt_sub.h"
if [ ! -f "$MQTT_H" ]; then
    bad "없음: $MQTT_H"
else
    MQTT_TLS=$(grep -E '^#define[[:space:]]+MQTT_USE_TLS' "$MQTT_H" | awk '{print $3}')
    MQTT_HOST=$(grep -E '^#define[[:space:]]+MQTT_BROKER_HOST' "$MQTT_H" \
                | sed -n 's/.*"\([^"]*\)".*/\1/p')
    if [ "${MQTT_TLS:-0}" = "1" ] && [ -n "${MQTT_HOST:-}" ] \
       && [ "$MQTT_HOST" != "localhost" ] && [ "$MQTT_HOST" != "127.0.0.1" ]; then
        ok "TLS=1, host=${MQTT_HOST}"
    else
        warn "로컬 테스트 기본값으로 보임 (TLS=${MQTT_TLS:-?}, host=${MQTT_HOST:-?})"
        fix "운영 배포라면 $MQTT_H 를 고치고 반드시 재빌드하세요:"
        fix "  #define MQTT_USE_TLS        1"
        fix "  #define MQTT_BROKER_HOST    \"<RPi B IP>\""
    fi
fi

section "rpic_subscriber 구동" SOUND ACT
if [ -f /etc/systemd/system/rpic_subscriber.service ]; then
    ac=$(unit_state rpic_subscriber)
    if [ "$ac" = "active" ]; then
        ok "systemd active"
    else
        bad "유닛은 있으나 실행 중이 아님 (active=${ac})"
        fix "sudo systemctl start rpic_subscriber"
    fi
elif pgrep -x rpic_subscriber >/dev/null 2>&1; then
    ok "수동 실행 중"
else
    bad "유닛 미설치 + 미실행"
    fix "상시 운영:  cd $REPO_ROOT/rpi_c/rpic_app && sudo ./install.sh"
    fix "임시 실행:  cd $REPO_ROOT/rpi_c/rpic_app/app && sudo ./rpic_subscriber"
fi

# =====================================================================
group "액추에이터 — 셔터 / 펌프 / 서보 / 팬"
# =====================================================================

section "커널 드라이버 (/dev/rpic_*)" ACT
MISSING_DEV=""
for d in rpic_pca9685 rpic_stepper rpic_pump; do
    [ -e "/dev/$d" ] || MISSING_DEV="$MISSING_DEV $d"
done
if [ -z "$MISSING_DEV" ]; then
    ok "pca9685 / stepper / pump"
else
    bad "노드 없음:$MISSING_DEV"
    fix "rpic_subscriber 는 이게 없으면 시작 즉시 종료됩니다:"
    fix "  cd $REPO_ROOT/rpi_c/rpic_app/drivers && make"
    fix "  sudo insmod rpic_pca9685.ko && sudo insmod rpic_stepper.ko && sudo insmod rpic_pump.ko"
    fix "실물 없이 테스트하려면 각 insmod 뒤에 simulate=1 을 붙입니다."
fi

section "팬 하드웨어 PWM (pwmchip0)" ""
if [ -d /sys/class/pwm/pwmchip0 ]; then
    ok
else
    warn "없음 — 팬이 no-op(soft) 모드로 동작"
    fix "config.txt 에 추가 후 재부팅:  dtoverlay=pwm,pin=12,func=4"
fi

# =====================================================================
group "오디오 장치 독점 충돌"
# =====================================================================

# ALSA hw 장치는 한 프로세스만 연다. RTP 수신기(GStreamer)는 방송이 없어도
# 대기 상태로 스피커를 계속 잡고 있어서, 그 사이 화재 사이렌이 open 에 실패한다
# (rpic_audio 는 재생할 때만 여는 구조라 미리 선점해 둘 수도 없다).
# 파일·서비스 존재만 보고 "사용 가능"이라 하면 이 상황을 놓친다.
# 온디맨드 구조에서는 "지금 방송 세션이 도는가"에 따라 판정이 뒤집힌다.
# 방송 중 점유는 정상, 유휴 중 점유는 사이렌을 막는 결함이다.
RTP_STATE=$(unit_state guardx-broadcast-rtp)

section "스피커 점유 프로세스" SOUND
SPK_HOLDERS=$(fuser -v /dev/snd/pcmC*D1p 2>&1 | tail -n +2 \
              | awk 'NF {print $NF}' | sort -u | tr '\n' ' ')
HOLDER_N=$(printf '%s' "$SPK_HOLDERS" | wc -w)

if [ "$HOLDER_N" -eq 0 ]; then
    ok "점유 없음 — 사이렌이 장치를 열 수 있음"
elif printf '%s' "$SPK_HOLDERS" | grep -q 'gst-launch'; then
    if [ "$RTP_STATE" = "active" ]; then
        ok "방송 세션 진행 중 — 수신기 점유는 정상 (${SPK_HOLDERS})"
    else
        bad "유휴 상태인데 RTP 수신기가 스피커를 점유 중 (${SPK_HOLDERS})"
        fix "방송 세션이 없는데 장치를 쥐고 있으면 화재 사이렌이 EBUSY 로 실패합니다."
        fix "  sudo systemctl stop guardx-broadcast-rtp"
        fix "조율기가 기동/화재 때 자동으로 정지시키므로, 이 상태가 남아 있다면"
        fix "rpic_subscriber 가 안 떠 있거나 systemctl 권한이 없는 것입니다."
    fi
elif printf '%s' "$SPK_HOLDERS" | grep -q 'rpic_subscriber'; then
    ok "상황음/사이렌 재생 중 (${SPK_HOLDERS})"
else
    warn "예상 밖 프로세스가 점유 중 (${SPK_HOLDERS})"
    fix "사이렌이 필요할 때 충돌할 수 있습니다."
fi

# 온디맨드로 바뀌면서 부팅 자동 시작은 오히려 결함이 된다 - 방송이 없는데도
# 장치를 물고 떠 있게 되기 때문이다. 조율기가 필요할 때만 start 한다.
# 조율기가 수신기를 켜고 끄려면 systemctl 권한이 있어야 한다. root 로 돌면
# 자동으로 되고, 아니면 sudoers 규칙이 있어야 sudo -n 이 통한다.
section "조율기의 방송 수신기 제어 권한" SOUND BCAST
if [ "$(id -u)" = "0" ]; then
    ok "root 로 점검 중 — rpic_subscriber 도 root 면 문제 없음"
elif sudo -n -l /usr/bin/systemctl stop guardx-broadcast-rtp >/dev/null 2>&1; then
    # -l 은 "허용되는가"만 묻고 실제로 실행하지 않는다. is-active 같은 다른
    # 하위명령으로 시험하면 sudoers 허용 목록(start/stop)에 없어 늘 실패한다.
    ok "sudo -n 으로 제어 가능 (sudoers 규칙 있음)"
else
    warn "일반 계정에서 systemctl 제어 불가"
    fix "rpic_subscriber 를 root(systemd 유닛 기본)로 돌리면 필요 없습니다."
    fix "손으로 띄워 쓸 거라면 sudoers 규칙을 설치하세요:"
    fix "  sudo install -m 0440 -o root -g root \\"
    fix "       $REPO_ROOT/rpi_c/rpic_app/sudoers.d/guardx-rpic /etc/sudoers.d/guardx-rpic"
    fix "  sudo visudo -c"
fi

section "RTP 수신기 자동 시작 (꺼져 있어야 정상)" SOUND
RTP_ENABLED=$(unit_enabled guardx-broadcast-rtp)
case "$RTP_ENABLED" in
    disabled|static|unknown) ok "enabled 아님 (${RTP_ENABLED})" ;;
    *)
        bad "부팅 자동 시작이 켜져 있음 (${RTP_ENABLED})"
        fix "온디맨드 구조에서는 조율기가 START 때만 켭니다. 자동 시작은 끄세요:"
        fix "  sudo systemctl disable guardx-broadcast-rtp"
        ;;
esac

# =====================================================================
group "LED 매트릭스 — RS-485 → STM32"
# =====================================================================

# 변환기를 뽑았다 꽂으면 ttyUSB 번호가 올라간다. 별칭이 있으면 그 문제가 사라진다.
section "RS-485 장치" ""
if [ -e /dev/guardx-rs485 ]; then
    ok "/dev/guardx-rs485 -> $(readlink -f /dev/guardx-rs485 2>/dev/null)"
elif ls /dev/ttyUSB* >/dev/null 2>&1; then
    warn "별칭 없음 — ttyUSB 번호에 의존 중 ($(ls /dev/ttyUSB* | tr '\n' ' '))"
    fix "번호는 재연결마다 바뀝니다. udev 규칙을 설치하세요:"
    fix "  sudo cp $REPO_ROOT/rpi_c/rpic_app/udev/99-guardx-rs485.rules /etc/udev/rules.d/"
    fix "  sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=tty --action=add"
else
    warn "USB 시리얼 장치가 없음 — 변환기가 안 꽂혀 있습니다"
    fix "CVBE-008 을 꽂고 확인:  dmesg | tail    (ch341 attach 줄)"
fi

section "매트릭스 송출 설정" ""
if [ "${GUARDX_MATRIX_DEV:-}" = "off" ]; then
    warn "GUARDX_MATRIX_DEV=off — LED 송출이 꺼져 있습니다"
    fix "실제로 내보내려면 이 환경변수를 지우거나 장치 경로를 주세요."
else
    ok "활성 (GUARDX_MATRIX_DEV=${GUARDX_MATRIX_DEV:-미지정 → 자동 선택})"
fi

# =====================================================================
group "systemd 유닛"
# =====================================================================

check_unit() {
    local unit="$1" want_exec="$2" feat="$3"
    local path="/etc/systemd/system/${unit}.service"

    section "${unit} 설치" $feat
    if [ ! -f "$path" ]; then
        bad "유닛 없음"
        fix "sudo cp <리포의 ${unit}.service> /etc/systemd/system/"
        fix "sudo systemctl daemon-reload && sudo systemctl enable --now ${unit}"
        return
    fi
    ok

    local execline exebin
    execline=$(grep -m1 '^ExecStart=' "$path" | cut -d= -f2-)
    exebin="${execline%% *}"

    section "${unit} ExecStart" $feat
    if [ ! -x "$exebin" ]; then
        bad "경로가 실행 불가: $exebin"
        fix "sudo nano $path   → ExecStart 를 실제 배치 경로로 수정"
        fix "sudo systemctl daemon-reload && sudo systemctl restart ${unit}"
    elif [ -n "$want_exec" ] && ! printf '%s' "$execline" | grep -q "$want_exec"; then
        warn "'${want_exec}' 가 없음 — 경로 확인 필요"
        fix "현재: $execline"
    else
        ok
    fi

    # ALSA 는 audio 그룹 권한이 필요하다. root 가 아닌데 이게 빠지면 장치 open 이 실패한다.
    section "${unit} audio 그룹" $feat
    if grep -qE '^SupplementaryGroups=.*audio' "$path" || grep -qE '^User=root' "$path"; then
        ok
    else
        bad "SupplementaryGroups=audio 없음 — ALSA 장치를 못 엽니다"
        fix "sudo nano $path   → [Service] 아래에 추가:  SupplementaryGroups=audio"
        fix "sudo systemctl daemon-reload && sudo systemctl restart ${unit}"
    fi

    section "${unit} 자동시작/구동" $feat
    local en ac
    en=$(unit_enabled "$unit")
    ac=$(unit_state "$unit")
    if [ "$en" = "enabled" ] && [ "$ac" = "active" ]; then
        ok "enabled + active"
    else
        bad "enabled=${en}, active=${ac}"
        fix "sudo systemctl enable --now ${unit}"
        fix "그래도 안 뜨면:  journalctl -u ${unit} -n 30 --no-pager"
    fi
}

check_unit guardx-toimic        "yamnet/bin/python" MIC

# 방송 수신기는 check_unit 을 쓰지 않는다 - 그쪽은 "enabled + active"를 정상으로
# 보는데, 온디맨드 구조에서는 둘 다 평상시엔 아니어야 한다. 설치 여부와 경로만 본다.
section "유닛 guardx-broadcast-rtp 설치" BCAST
RTP_UNIT_PATH=/etc/systemd/system/guardx-broadcast-rtp.service
if [ ! -f "$RTP_UNIT_PATH" ]; then
    bad "유닛이 없음 — 조율기가 방송을 켤 수 없습니다"
    fix "sudo cp $REPO_ROOT/rpi_c/rpic_app/broadcast_rtp/guardx-broadcast-rtp.service /etc/systemd/system/"
    fix "sudo nano $RTP_UNIT_PATH      # ExecStart 를 실제 경로로"
    fix "sudo systemctl daemon-reload  # enable 은 하지 않습니다(온디맨드)"
else
    RTP_EXEC=$(grep -m1 '^ExecStart=' "$RTP_UNIT_PATH" | cut -d= -f2- | awk '{print $1}')
    if [ -x "$RTP_EXEC" ]; then
        ok "ExecStart 정상"
    else
        bad "ExecStart 경로가 실행 불가: $RTP_EXEC"
        fix "sudo nano $RTP_UNIT_PATH   → 실제 배치 경로로 수정 후 daemon-reload"
        fix "실행권한이면:  chmod +x $RTP_EXEC"
    fi
fi

# demo 프로파일은 "놓치지 않는 쪽" 우선이라 오탐이 늘어난다. 시연엔 맞다.
section "TOIMIC 감지 프로파일" ""
TOIMIC_UNIT=/etc/systemd/system/guardx-toimic.service
if [ ! -f "$TOIMIC_UNIT" ]; then
    warn "유닛이 없어 확인 불가"
elif grep -qE '^Environment=GUARDX_TOIMIC_PROFILE=demo' "$TOIMIC_UNIT"; then
    warn "demo 프로파일 — 시연용(오탐 늘어남)"
    fix "운영 배포 시 prod 로:"
    fix "  sudo sed -i 's/PROFILE=demo/PROFILE=prod/' $TOIMIC_UNIT"
    fix "  sudo systemctl daemon-reload && sudo systemctl restart guardx-toimic"
else
    ok
fi

# =====================================================================
# 기능별 판정
# =====================================================================

verdict() {
    local name="$1" why="$2"
    if [ -z "$why" ]; then
        printf '  %s● 사용 가능%s  %s\n' "$C_OK" "$C_OFF" "$name"
    else
        printf '  %s● 사용 불가%s  %s\n' "$C_BAD" "$C_OFF" "$name"
        printf '%s                막는 항목: %s%s\n' "$C_DIM" "$why" "$C_OFF"
    fi
}

printf '\n========================================\n'
printf '기능별 판정\n'
printf '========================================\n'
verdict "방송 (VMS 마이크 → RPi C 스피커)" "$BLOCK_BCAST"
verdict "마이크 (TOIMIC 비명/총성 감지)"    "$BLOCK_MIC"
verdict "상황음 (화재/거수자 알림음)"       "$BLOCK_SOUND"
verdict "액추에이터 (셔터/펌프/서보/팬)"    "$BLOCK_ACT"

printf '\n%sOK %d%s  %s경고 %d%s  %s실패 %d%s\n' \
       "$C_OK" "$N_OK" "$C_OFF" "$C_WARN" "$N_WARN" "$C_OFF" "$C_BAD" "$N_BAD" "$C_OFF"

if [ "$N_BAD" -gt 0 ]; then
    printf '\n실패 항목의 조치를 먼저 적용하세요.\n'
    exit 1
fi

printf '\n환경 준비 완료. 실제 동작은 아래로 확인하세요:\n'
printf '  1) TOIMIC 켜둔 채 VMS 방송 ON  → 스피커에서 목소리\n'
printf '  2) sound SET 1 발행            → 상황음\n'
printf '  3) 방송 끄고 비명 1초 이상     → VMS 오디오 팝업\n'
exit 0
