# GuardX 방송 — Opus/RTP/UDP 업그레이드 경로

기존 PCM/MQTT 방송(`app/broadcast_audio.c`)은 **그대로 두고**, 미디어만
Opus/RTP/UDP로 옮기는 선택 경로다. 제어(방송 시작/종료 UX)는 VMS의 방송 버튼이
담당하고, 이 폴더는 **RPi C 수신부**(독립 GStreamer 프로세스)다.

```
VMS(마이크) → webrtcdsp(노캔) → opusenc 64k/FEC → rtpopuspay → udpsink
   ──UDP/5004──▶ udpsrc → rtpjitterbuffer(do-lost) → rtpopusdepay
                → opusdec(FEC+PLC) → alsasink(MAX98357A)
```

- 액추에이터 앱(`rpic_subscriber`)과 **링크되지 않는다** → C앱/빌드 무영향.
- 기존 MQTT 방송과 병존. VMS 레지스트리 `broadcast/transport` 로 어느 쪽을 쓸지 고른다.

## 1. RPi C 준비 (한 번)

```bash
sudo apt install -y gstreamer1.0-tools gstreamer1.0-plugins-base \
     gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
```
(`opusenc/opusdec`=plugins-bad, `rtpopus*`·`udpsrc`=plugins-good, `alsasink`=plugins-base)

## 2. 먼저 미디어 경로만 검증 (앱 없이 gst-launch로)

앱·VMS 건드리기 전에 **파이프만 먼저 확인**하는 것이 안전하다.

**RPi C (수신)**:
```bash
chmod +x receive.sh
./receive.sh
```

**아무 리눅스 PC(송신 테스트)** — VMS 대신 임시로:
```bash
gst-launch-1.0 -v audiotestsrc ! audioconvert ! audioresample \
  ! audio/x-raw,rate=48000,channels=1 ! opusenc bitrate=24000 \
  ! rtpopuspay pt=96 ! udpsink host=<RPi C IP> port=5004
```
→ RPi C 스피커에서 삐- 소리가 나면 미디어 경로 OK. (Windows에서 마이크로 보내려면
`audiotestsrc` 대신 `autoaudiosrc` 또는 `wasapisrc`)

## 3. 상시 수신 서비스로 등록 — 기본을 RTP로 두려면 이걸 반드시

**VMS 쪽 기본 전송방식이 RTP다** (아무것도 안 건드려도 RTP로 붙는다). 그래서
RPi C도 **부팅하면 RTP 수신기가 자동으로 떠 있어야** 아무것도 안 만져도 바로
방송이 된다. 아래로 등록하고 `enable`까지 해두면 그렇게 된다.

```bash
sudo cp guardx-broadcast-rtp.service /etc/systemd/system/
# 유닛의 ExecStart 경로를 실제 위치로 수정
sudo systemctl daemon-reload
sudo systemctl enable --now guardx-broadcast-rtp   # enable = 재부팅해도 자동 시작
journalctl -u guardx-broadcast-rtp -f
```

`rpic_subscriber`(액추에이터 앱)도 평소처럼 항상 켜둔다 — RTP 모드에서는 VMS가
MQTT 방송 명령 자체를 안 보내므로 `rpic_subscriber`의 스피커 사용과 충돌하지
않는다(§4b).

## 4. VMS 쪽 확인 (레지스트리로 직접 만질 필요 없음)

VMS 방송 카드(DEVICE 화면)에 `[MQTT]`/`[RTP]` 전환 버튼이 있다 — 기본이 이미
`RTP`라 **평소엔 그냥 ON만 누르면 된다.** 대상 IP도 비어있으면 VMS가
credentials.ini의 MQTT 브로커 주소로 자동 채운다.

수동으로 확인/변경하고 싶을 때만 레지스트리를 본다(값 이름의 `/`가 하위 키
구분자이므로 아래처럼 `broadcast` 하위 키에 값이 들어간다):
```
reg query "HKCU\Software\GuardX\VMS\broadcast"
reg add   "HKCU\Software\GuardX\VMS\broadcast" /v transport /t REG_SZ /d mqtt /f   # 되돌릴 때만
```

## 4b. 모드 전환 — MQTT ↔ RTP (스피커 장치 충돌 방지)

`rpic_subscriber`(액추에이터 앱)는 두 모드에서 항상 켜둔 채로 둔다. 스피커를 두고
다투는 건 RTP 수신기(GStreamer, `receive.sh`) 하나뿐이다 — 이게 켜져 있으면
MQTT 방송이 스피커 장치를 열지 못해 `Device or resource busy`로 실패한다.
그래서 MQTT로 바꿔 써야 할 때만 `../test/mqtt_switch.sh`를 실행한다:

```bash
cd ../test
./mqtt_switch.sh mqtt    # RTP 수신기 정지 — MQTT 방송 즉시 사용 가능
./mqtt_switch.sh rtp     # RTP 수신기 시작 — RTP 방송 사용 가능 (보통은 systemd가 이미 켜둠)
./mqtt_switch.sh status  # 지금 상태 확인
```

기본이 RTP + systemd `enable`이면 평소엔 이 스크립트를 쓸 일이 없다 — **어쩌다
한 번 MQTT로 바꿔 써야 할 때만** `./mqtt_switch.sh mqtt` 한 줄이면 된다. `mqtt`로
바꿔도 systemd 서비스 자체는 **정지만 되고 삭제되지 않는다** — `enable` 상태가
유지되므로 재부팅하면 다시 RTP로 돌아온다(의도된 기본 동작). 계속 mqtt로 두고
싶으면 별도로 `sudo systemctl disable guardx-broadcast-rtp`.

VMS 쪽 전송방식 버튼(DEVICE 화면, `[MQTT]`/`[RTP]`)과 **세트로** 맞춰 누르면 된다 —
VMS 버튼은 VMS가 어떻게 보낼지만 정하고, 이 스크립트는 RPi C가 받을 준비를 한다.

## 4c. 음질 · 노캔 (2026-08-06)

**무엇이 바뀌었나** — 초기 버전은 24 kbps 모노에 DSP가 전혀 없었다. 대역폭이
아니라 명료도에서 손해가 컸고(자음이 뭉갠다), 패킷 하나만 잃어도 그대로 "뚝"
끊겼다. 송·수신을 세트로 손봤다:

| 위치 | 변경 | 효과 |
|---|---|---|
| VMS 송신 | `opusenc` 24k → **64k**, `audio-type=voice`, `complexity=10` | 명료도 |
| VMS 송신 | `inband-fec=true packet-loss-percentage=10` | 손실 복원 |
| VMS 송신 | **`webrtcdsp`** (잡음억제 high + AGC + 하이패스) | 노캔 |
| VMS 송신 | `queue leaky=downstream` | 막힐 때 지연 대신 드롭 |
| RPi C 수신 | `rtpjitterbuffer do-lost=true` | FEC/PLC가 동작할 조건 |
| RPi C 수신 | `opusdec use-inband-fec=true plc=true` | 끊김 → 부드러운 보간 |

FEC는 **양쪽이 다 켜져야** 의미가 있다 — VMS만 올리고 `receive.sh`를 예전 것으로
두면 복원본을 그냥 버린다. RPi C에서 `git pull` 후 서비스 재시작을 잊지 말 것:

```bash
sudo systemctl restart guardx-broadcast-rtp
```

**노캔 끄고 켜기** — VMS 방송 카드의 `노캔 ON/OFF` 버튼. 방송 중에 눌러도 즉시
반영된다(파이프라인 재시작 없음). 기본값은 켬.

**webrtcdsp가 없는 설치본**에서는 자동으로 `audiocheblimit`(하이패스) +
`audiodynamic`(노이즈 게이트) 폴백을 쓴다 — plugins-good만 있으면 동작한다.
어느 쪽이 쓰였는지는 방송 시작 시 상태 표시줄에 나온다(`노캔 webrtcdsp` 등).
Windows GStreamer는 "Complete" 설치면 webrtcdsp가 들어있다. 확인:

```bash
gst-inspect-1.0 webrtcdsp
```

**비트레이트 조정** — 현장이 저대역폭이면 레지스트리로 낮춘다(기본 64000):

```
reg add "HKCU\Software\GuardX\VMS\broadcast" /v opus_bitrate /t REG_DWORD /d 32000 /f
```

## 5. 알려진 한계 (이 단계 범위 밖)

- **상황음 일시중단 미연동**: 기존 MQTT 경로는 방송 중 `audio_event` 상황음을
  멈추지만, RTP 경로는 독립 프로세스라 아직 그 연동이 없다. 필요하면 후속으로
  방송 상태를 MQTT로 알려 `audio_event_set_paused()`를 호출하게 한다.
- **자기간섭**: 방송 스피커 소리를 같은 노드의 SPH0645가 주워담을 수 있다
  (감지기 오작동 방지 위해 방송 중 감지 게이트 필요 — 별도 항목).
- **암호화 없음**: LAN 내부 전제(평문 RTP). 외부로 나가면 SRTP 필요.
