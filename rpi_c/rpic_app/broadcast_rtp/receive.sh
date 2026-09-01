#!/usr/bin/env bash
# GuardX RPi C 방송 수신 (Opus/RTP/UDP) — 독립 GStreamer 파이프라인.
#
# 기존 MQTT/PCM 방송(app/broadcast_audio.c)과 무관한 별도 미디어 경로다.
# 이 스크립트는 액추에이터 앱과 링크되지 않은 독립 프로세스로 돌아,
# UDP 포트로 들어오는 Opus/RTP를 MAX98357A 스피커로 재생만 한다.
#
# 환경변수로 조정:
#   GUARDX_BROADCAST_RTP_PORT   수신 포트        (기본 5004, 프로토콜 상수와 일치)
#   GUARDX_BROADCAST_ALSA       ALSA 재생 장치   (기본 MAX98357A DEV=1, hw 직결)
#   GUARDX_BROADCAST_JITTER_MS  지터버퍼(ms)     (기본 80 = 기존 프리버퍼와 동일)
#   GUARDX_AUDIO_REF_PORT       TOIMIC 기준신호 포트 (기본 5005, 0 이면 끔)
set -euo pipefail

PORT="${GUARDX_BROADCAST_RTP_PORT:-5004}"
ALSA_DEV="${GUARDX_BROADCAST_ALSA:-hw:CARD=MAX98357A,DEV=1}"
JITTER_MS="${GUARDX_BROADCAST_JITTER_MS:-80}"
REF_PORT="${GUARDX_AUDIO_REF_PORT:-5005}"

echo "[broadcast-rtp] 수신 대기: udp/${PORT} → ${ALSA_DEV} (jitter ${JITTER_MS}ms)"
echo "[broadcast-rtp] TOIMIC 기준신호: udp/127.0.0.1:${REF_PORT} (16kHz mono S16LE)"

# ── TOIMIC 기준 신호 분기 ──
# 스피커로 나가는 것과 **같은** 디코딩 결과를 감지기에도 한 부 보낸다.
# 이게 없으면 감지기는 방송 중에 감지를 통째로 꺼야 하고(예전 동작),
# 하필 그때 난 비명을 영영 못 잡는다. 규격은 shared/audio_ref_protocol.h.
#
# leaky=downstream 을 양쪽 queue 에 두는 것이 요점이다. tee 는 느린
# 분기가 전체를 막으므로, 감지기 쪽이 밀리면 방송 소리까지 끊긴다 —
# 기준 신호는 버려도 되고 방송은 안 된다.
#
# 데이터그램 크기를 20ms 로 맞추지 않는다(그러려면 plugins-bad 의
# audiobuffersplit 이 필요하다). 목적지가 loopback 이라 MTU 가 64KB 고,
# 받는 쪽은 경계와 무관하게 이어 붙이므로 문제가 되지 않는다.

# Opus RTP는 clock-rate 48000 고정. payload 96 = VMS rtpopuspay pt=96 과 일치.
#
# 음질 관련 옵션 (VMS 송신부와 세트로 맞춰야 효과가 난다):
#   do-lost=true       손실 패킷 자리에 GAP 이벤트를 내보낸다. 이게 없으면
#                      아래 FEC/PLC가 복원할 기회 자체를 못 받는다.
#   use-inband-fec     송신부 opusenc inband-fec=true 가 실어보낸 저비트레이트
#                      복사본으로 잃어버린 프레임을 복원한다.
#   plc=true           FEC로도 못 살린 구간을 파형 보간으로 메운다("뚝" 대신 뭉갬).
#   quality=10         리샘플 품질(48k → 장치 레이트). 라즈베리파이도 여유.
#
# S32LE 고정이 중요하다 — TOIMIC(arecord)이 캡처를 S32_LE 로 열면 I2S 프레임이
# 32비트 슬롯으로 고정되는데, 재생만 16비트로 쓰면 샘플이 슬롯 하위쪽에 실려
# 진폭이 죽는다(에러 없이 무음처럼 들린다). 양방향 프레임 길이를 맞춰
# 마이크와 스피커를 동시에 쓸 수 있게 한다. 같은 이유로 plughw 가 아니라
# hw 직결이다 — plug 레이어가 16비트를 그대로 통과시켜 버린다.
#
# 지연은 늘리지 않았다 — queue는 디코더/ALSA 쓰기 스레드 분리용 50ms(막히면
# 쌓지 말고 버림)이고, alsasink buffer-time=100ms는 기본값 200ms보다 오히려
# 짧다. 체감 지연을 정하는 건 여전히 rtpjitterbuffer latency(기본 80ms)다.
exec gst-launch-1.0 -q \
  udpsrc port="${PORT}" buffer-size=524288 \
    caps="application/x-rtp,media=audio,encoding-name=OPUS,clock-rate=48000,payload=96" \
  ! rtpjitterbuffer latency="${JITTER_MS}" do-lost=true do-retransmission=false \
  ! rtpopusdepay \
  ! opusdec use-inband-fec=true plc=true \
  ! audioconvert \
  ! audioresample quality=10 \
  ! audio/x-raw,format=S32LE,rate=48000,channels=2 \
  ! tee name=spk \
  spk. ! queue max-size-time=50000000 max-size-buffers=0 max-size-bytes=0 leaky=downstream \
       ! alsasink device="${ALSA_DEV}" sync=false buffer-time=100000 latency-time=20000 \
  spk. ! queue max-size-time=50000000 max-size-buffers=0 max-size-bytes=0 leaky=downstream \
       ! audioconvert ! audioresample quality=4 \
       ! audio/x-raw,format=S16LE,rate=16000,channels=1 \
       ! udpsink host=127.0.0.1 port="${REF_PORT}" sync=false async=false
