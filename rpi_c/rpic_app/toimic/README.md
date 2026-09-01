# GuardX TOIMIC — 오디오 이벤트 감지 (비명 / 총성)

RPi C에 붙은 SPH0645(I2S MEMS 마이크)로 비명·총성을 감지해 VMS에 경보를
띄운다. **원시 오디오는 노드 밖으로 나가지 않는다** — 추론을 RPi C에서 하고
MQTT로는 판정 결과(JSON)만 보낸다. 화장실 같은 민감 구역에 마이크를 두는
전제라 이 부분이 설계의 제약 조건이다.

```
SPH0645(I2S) ─ arecord 48kHz/S32_LE
   → 100Hz 하이패스 → 48k→16k 다운샘플
   → [1단] DSP 게이트 (에너지 / crest factor)      ← 대부분 여기서 끝, CPU ~0
   → [2단] YAMNet 추론 (0.975초 창)                 ← 게이트 열렸을 때만
   → 클래스 융합 → 히스테리시스 + N-of-M 투표
   → MQTT guardx/alert/rpic → VMS AudioAlertPopup
```

## 1. 구성 요소

| 파일 | 내용 |
|---|---|
| `detector.py` | 감지기 본체 (전처리 · 게이트 · 추론 · 판정 · 발행) |
| `yamnet.tflite` | YAMNet 학습 가중치 3.9MB — AudioSet 521클래스 분류기 |
| `yamnet_class_map.csv` | 521개 클래스 인덱스 ↔ 이름 대응표 |
| `guardx-toimic.service` | systemd 유닛 (부팅 시 자동 기동) |
| `.venv/` | Python 가상환경 (git 추적 안 함 — 노드마다 직접 생성) |
| `requirements.txt` | Python 의존성 |

`rpic_subscriber`(액추에이터 앱)와 **링크되지 않는 독립 프로세스**다. 방송
RTP 수신기(`../broadcast_rtp/`)와 같은 구조라 C앱 빌드에 영향이 없다.

## 2. 설치 (RPi C에서 한 번)

```bash
cd rpi_c/rpic_app/toimic
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt

# AEC(방송·사이렌 중에도 감지) 용 — pip 가 아니라 시스템 패키지다
sudo apt install -y libspeexdsp1
```

동작 확인 (venv를 활성화한 상태에서):

```bash
python detector.py
```

`[대기] rms=... floor=...` 가 2~5초마다 찍히면 캡처·모델 로드가 정상이다.
소리를 지르면 `[게이트:지속음] ... 융합 scream=0.xx` 가 뜬다.

## 3. 현장 임계 맞추기 — 이거 안 하면 오탐이 난다

기본 임계는 임의값이다. 마이크를 실제 설치할 자리에 두고 배경소음을 재서
값을 뽑는다.

```bash
python detector.py --calibrate 30
```

30초간 조용한 상태로 두면 배경 RMS 분포와 함께 권장값을 출력한다. 출력된
`GUARDX_TOIMIC_ABS_FLOOR` / `GUARDX_TOIMIC_CREST_TH` 를 systemd 유닛의
`Environment=` 에 넣는다. **측정값을 기록해 두면 "왜 이 임계냐"에 답이 된다.**

측정은 감지기와 **같은 창 길이(0.975초)** 로 한다 — 짧은 청크로 재면 피크가
작게 잡혀 crest factor가 실제보다 낮게 나오고 권장값이 어긋나기 때문이다.

핸드드라이어처럼 간헐적으로 큰 소음이 있는 곳이라면, 그게 도는 동안에도
한 번 재서 두 값 중 큰 쪽을 쓴다.

## 4. 상시 실행 등록

```bash
sudo cp guardx-toimic.service /etc/systemd/system/
# 유닛 안의 WorkingDirectory / ExecStart 경로를 실제 위치로 수정
sudo systemctl daemon-reload
sudo systemctl enable --now guardx-toimic
journalctl -u guardx-toimic -f
```

## 5. 튜닝 근거

임계값을 임의로 정하지 않았다. 각 항목은 음향 이벤트 검출에서 통용되는
방식이고, 질문받으면 아래 한 줄로 답이 된다.

| 항목 | 값 (demo) | 근거 |
|---|---|---|
| 하이패스 | 100Hz 2차 | SPH0645의 DC 오프셋 제거가 최소 요건. 화장실은 환풍기·공조 저주파가 상시로 깔려 컷오프를 DC가 아니라 100Hz에 둔다. 비명(200Hz~)·총성(광대역)의 유효 대역은 안 건드린다 |
| 스트림 연속성 | 필터 zi + 리샘플 overlap-save | 오디오를 0.25초 청크로 읽으므로 청크마다 상태를 이어주지 않으면 경계에 불연속(클릭)이 생긴다. 하이패스는 `zi`로, 리샘플러는 직전 입력 꼬리를 덧대는 overlap-save로 잇는다. **안 하면 0.25초마다 생기는 클릭이 crest factor를 부풀려 총성 오탐이 된다** |
| 클래스 융합 | 가중 합산 | AudioSet은 부모-자식 온톨로지라 상위 개념으로 점수를 합치는 게 표준 사용법. 실제 비명은 `Screaming` 하나에 안 몰리고 `Shout`/`Yell`로 분산된다. 핵심 1.0 / 인접 0.4~0.8 가중치라 인접 클래스만으로는 임계를 못 넘는다 |
| 진입 임계 | 융합 0.35 | YAMNet 출력은 보정된 확률이 아니라 클래스별 임계 조정이 정석. 0.5는 단일 클래스 기준값이라 융합 후에는 낮추는 게 맞다 |
| 유지 임계 | 0.25 | 히스테리시스 — 임계 경계에서 판정이 깜빡이는 것을 막는 표준 기법 |
| 지속 판정 (비명) | 3창 중 2창 | 고정 연속 카운트보다 N-of-M 투표가 관용적. 중간에 한 창이 흔들려도 안 놓친다 |
| 지속 판정 (총성) | 1창 + 임계 +0.10 | **비명과 같은 규칙을 쓰면 안 된다.** 총성은 10~50ms 단발이라 정렬된 창이 하나만 생겨 2창 요구를 영영 못 채운다. 1창으로 하되 임계를 올리고, crest 게이트(임펄스가 아니면 여기까지 오지도 않음)를 사전 확률로 삼아 오탐을 억제한다 |
| 정규화 | 게이트 통과 후 창 단위 피크 정규화 | SPH0645 감도가 −26 dBFS/94dB SPL로 낮아 그대로 넣으면 확신도가 떨어진다. **게이트 전에 걸면 무음까지 증폭돼 오탐이 늘기 때문에 순서가 핵심** |
| 게이트 하한 | 배경 p95 + 12dB | 현장 실측 기반(§3). 임의값 아님 |
| 임펄스 창 정렬 | crest > 8 | 총성은 10~50ms 임펄스인데 YAMNet 창은 975ms다. 창 끝에 걸리면 희석돼 확신도가 낮게 나오므로, 피크가 창의 앞 25% 지점에 오도록 뒤 소리를 더 모은 뒤 추론한다 |
| 재경보 억제 | 5초 | 한 사건으로 팝업이 연속으로 뜨는 것 방지 |

### 프로파일

`GUARDX_TOIMIC_PROFILE` 로 두 세트를 전환한다.

- **`demo`** (기본): 놓치지 않는 쪽. 임계 낮음, 확증 2/3창, 재경보 5초.
- **`prod`**: 오탐 억제. 임계 높음, 확증 3/4창, 재경보 15초.

개별 항목은 `GUARDX_TOIMIC_<이름>` 으로 따로 덮어쓸 수 있다
(`ENTER_TH`, `SUSTAIN_TH`, `VOTE_N`, `VOTE_M`, `SUSTAIN_X`, `ABS_FLOOR`,
`CREST_TH`, `IMPULSE_MIN`, `COOLDOWN`).

## 6. 방송·사이렌 중 감지 (AEC)

한 노드에서 스피커와 마이크가 같이 돈다. 예전에는 스피커가 울리는 동안
감지를 통째로 껐다(아래 §6-2 뮤트). 안전한 선택이었지만 **방송 중에 난
비명과 사이렌 중에 난 총성을 영영 못 잡는다** — 하필 그때가 제일 중요한
순간이다.

지금은 스피커로 나간 소리의 **디지털 사본**을 받아 마이크 신호에서 지운다.

```
  사이렌·상황음  rpic_audio.c ─┐
                                ├─ udp://127.0.0.1:5005 (16k mono S16LE) ─→ aec.py
  방송           receive.sh  ──┘                                              │
                                                                    speex 적응 필터
  마이크 (SPH0645) ──→ 하이패스 ──→ 16k 다운샘플 ──────────────────────→ 반향 제거
                                                                              │
                                                                  게이트 → YAMNet → MQTT
```

규격은 `shared/audio_ref_protocol.h`. 마이크로 되잡지 않고 사본을 보내는
이유는 그 파일 주석에 적어 두었다 — 요약하면, 재생 경로(안전 기능)가
감지 기능 때문에 더 잘 죽는 구조를 피하려는 것이다.

### 기대 동작

| 상황 | 결과 |
|---|---|
| 방송만 출력 중 | 경보 없음 |
| 사이렌만 출력 중 | 경보 없음 |
| 방송 + 실제 비명 | 비명 경보 |
| 사이렌 + 실제 총성 | 총성 경보 |

감지 결과는 **VMS 알림으로만** 나간다. 방송이나 사이렌 상태를 바꾸지
않는다 — TOIMIC 은 출력 장치를 다투는 상대가 아니라 입력 센서다.

### 조정

| 환경변수 | 기본 | 뜻 |
|---|---|---|
| `GUARDX_TOIMIC_AEC` | `1` | `0` 이면 AEC 를 끄고 예전 뮤트로 되돌린다 |
| `GUARDX_TOIMIC_AEC_TAIL_MS` | `300` | 적응 필터가 덮는 반향 꼬리 길이 |
| `GUARDX_TOIMIC_AEC_MAX_LEAD_MS` | `400` | 기준 신호가 앞설 수 있는 한도 |
| `GUARDX_AUDIO_REF_PORT` | `5005` | 기준 신호 UDP 포트 (양쪽 같이 바꿀 것) |

꼬리 길이는 **재생 지연보다 길어야** 한다. 방송 `alsasink` 가 100 ms
버퍼를 쓰므로 300 ms 면 여유가 있다. 사이렌이 안 지워지면 여기부터 늘린다.

### 확인 절차 (하드웨어 필요)

```bash
# 1) 기준 신호가 실제로 오는지 — 재생 중에만 바이트가 흘러야 한다
sudo tcpdump -ni lo udp port 5005 -c 20

# 2) 감지기 기동 로그에 AEC 활성이 찍히는지
python detector.py | head -20
#   [AEC] speexdsp 활성 — frame=160 tail=300ms port=5005
#   재생 중 동작: AEC 로 반향 제거 후 상시 감지

# 3) 방송만 틀고 5분 — 경보가 하나도 안 나야 한다
# 4) 방송 중 비명 — 경보가 나야 한다
# 5) 사이렌만 5분 → 경보 없음 / 사이렌 중 총성 → 경보
```

3·5 에서 오탐이 나면 꼬리 길이를 먼저 늘려 보고, 그래도 남으면
`GUARDX_TOIMIC_PROFILE=prod` 로 임계를 올린다. 그래도 안 되면
`GUARDX_TOIMIC_AEC=0` 으로 예전 동작(재생 중 뮤트)으로 되돌릴 수 있다 —
오탐이 쏟아지는 것보다는 그때만 못 잡는 편이 낫다.

### 남은 것

지금은 speex 의 **에코 캔슬러만** 쓴다. 잔향 억제기
(`speex_preprocess` + `SPEEX_PREPROCESS_SET_ECHO_STATE`)를 붙이면 선형
필터가 못 지운 잔여 반향이 더 줄어든다. 현장에서 3·5 가 안 나오면 그게
다음 지렛대다.

## 6-2. 방송 중 자동 뮤트 (AEC 폴백)

`libspeexdsp` 가 없거나 기준 신호 포트를 못 열면 AEC 가 꺼지고 아래
동작으로 돌아간다. AEC 없이 감지를 강행하면 스피커 소리를 비명으로
오인해 오탐이 쏟아진다 — 그건 못 잡는 것보다 나쁘다.

한 노드에서 스피커와 마이크가 같이 돈다. 방송이 나가면 **스피커 소리를
마이크가 되받아** 감지기가 그걸 분류한다 — 방송 내용에 따라 오탐이 난다.

**단순히 "재생 서브스트림이 RUNNING인지"만 보면 안 된다.** RTP 수신기
(`guardx-broadcast-rtp.service`)는 부팅 시 자동 실행되어 상시 켜져 있고,
방송이 없어도 스피커 장치를 계속 열어둔 채 대기한다 — 이 상태에서도 ALSA는
`state: RUNNING`을 보고한다. 그래서 대신 그 프로세스(`owner_pid`)의 **CPU
사용량이 실제로 튀는지**로 구분한다: 장치만 열어두고 대기 중이면 CPU를 거의
안 쓰지만, 실제 Opus 디코드+리샘플+재생이 돌면 튄다. 울리는 동안 감지를
정지하고, 재생이 끝나도 잔향 때문에 0.6초 더 닫아 둔다. 뮤트 중에는 배경소음
기준선 적응도 얼린다 — 방송 음량으로 기준선이 올라가면 방송이 끝난 뒤 한동안
진짜 비명을 놓치기 때문이다.

**왜 CPU 사용량인가 (그리고 왜 이 정도로 충분한가).** 시스템 전체 CPU가
아니라 이 특정 PID(RTP 수신 전용 프로세스) 하나의 커널 CPU 시간 카운터만
본다 — 프로세스별 카운터라 다른 프로세스가 뭘 하든 안 섞인다. 이 프로세스는
고정된 파이프라인(`udpsrc → jitterbuffer → depay → decode → resample →
alsasink`) 하나만 돌리므로 "무관한 이유로 CPU가 튄다"는 시나리오가 거의
없다. 남는 위험은 포트로 들어오는 잡음 UDP 패킷이나 스케줄러 틱 하나가
드물게 우연히 찍히는 것 정도인데, 그래도 오판정 나면 결과는 "그 폴링 한 번
감지를 쉰다"이지 잘못된 경보가 아니라 위험도가 낮다. **그래도 이 드문 단발
오판정까지 걸러내려고 연속 2회(`GUARDX_TOIMIC_CPU_DEBOUNCE`, 기본 0.5초)
튀어야 뮤트로 인정한다** — 실제 방송은 실측상 폴링마다 연속으로 튀므로 지장
없고, 우연한 단발 스파이크만 걸러진다.

더 "직접적인" 방법(방송 파이프라인이 실제 프레임 출력 시점에 직접 신호를
보내는 것)도 검토했지만, 지금 파이프라인은 `gst-launch` CLI 한 줄이라 그
신호를 정확히 뽑으려면 Python GObject 바인딩 같은 새 의존성이 필요하다.
CPU 방식이 실기기 검증(§ 아래)으로 이미 대기 0 / 실제 방송 연속 1~3틱으로
깔끔히 갈리는 걸 확인했고 실패해도 피해가 없어서, 마감을 고려해 새 의존성을
넣지 않는 쪽을 택했다.

이 방식은 **누가 재생하든 동일하게 잡힌다** — MQTT 방송(`broadcast_audio.c`),
RTP 방송(`../broadcast_rtp/receive.sh`), 상황음(`audio_event.c`) 전부. 방송
전송방식이 MQTT/RTP 중 무엇이든, 어느 프로세스가 스피커를 쓰든 연동이
따로 필요 없다. VMS·C앱을 한 줄도 안 고쳐도 된다.

**실기기 검증 완료 (2026-08-06).** `GUARDX_TOIMIC_DEBUG_MUTE=1`로 켜면 매
폴링마다 감시 중인 프로세스의 CPU delta·연속횟수를 출력한다:

```bash
GUARDX_TOIMIC_DEBUG_MUTE=1 python detector.py
```

실측 결과 — 방송 없이 대기: `delta=0` 연속 유지, 뮤트 안 걸림. VMS로 실제
발화 방송: `delta=1~3틱`이 폴링마다 연속으로 찍히며 뮤트 정상 작동. 둘 사이
여유가 커서 기본값(`GUARDX_TOIMIC_CPU_ACTIVE_TICKS=1`,
`GUARDX_TOIMIC_CPU_DEBOUNCE=2`)을 그대로 쓴다. 현장이 다르면 같은 방식으로
다시 켜서 확인하고 필요시 조정한다.

## 7. MQTT 인터페이스

토픽 `guardx/alert/rpic` · QoS 1 · retain false.
스키마는 `rpi_b/MQTT_ALERT_INTERFACE.md`의 혼잡 경보와 같은 모양을 따르되
`event`/`confidence`/`source` 로 구분한다.

```json
{"node_id":"rpic","timestamp":1785220328723,"seq":0,
 "event":"scream","zone_id":1,"channel":0,"incident_id":3,
 "severity":"critical","confidence":0.412,"source":"audio"}
```

| 필드 | 의미 |
|---|---|
| `event` | `"scream"` \| `"gunshot"` |
| `confidence` | 융합 점수 0~1 (원시 YAMNet 확률이 아님) |
| `incident_id` | 이벤트 종류별 에피소드 번호 (재시작 시 0 리셋) |
| `source` | `"audio"` 고정 — 혼잡(`detection`/`prediction`)과 구분 |

VMS 수신부는 이미 붙어 있다: `vms/alert_feed.cpp` 의 `on_audio_alert()` →
`AlertFeed::audio_alert` 시그널 → `vms/audio_alert_popup.cpp`.

**`clear` 이벤트는 보내지 않는다.** 혼잡 경보와 달리 오디오 팝업은 20초
자동 닫힘이라(`AUTOHIDE_MS`) 해제 신호가 필요 없고, 같은 토픽으로 보내면
팝업이 오히려 다시 뜬다.

빠른 확인:

```bash
mosquitto_sub -h 172.20.33.251 -t 'guardx/alert/rpic' -v
```

## 8. 알려진 한계

- **모델은 범용이다.** YAMNet은 AudioSet 521클래스 일반 분류기라 화장실
  특화 오탐(물 내리는 소리, 핸드드라이어)이 날 수 있다. §3 캘리브레이션과
  `prod` 프로파일이 1차 방어선이고, 근본 해결은 현장 데이터 파인튜닝이다.
- **8kHz 위는 못 본다.** YAMNet 입력이 16kHz 고정이라 그 위 대역은 버린다.
  모델을 바꾸지 않는 한 변경 불가.
- **마이크 1개 = 방향을 모른다.** 어느 칸에서 났는지는 알 수 없다. 존/채널은
  설정값(`GUARDX_TOIMIC_ZONE`/`CHANNEL`)으로 고정 표시된다.
- **잔향.** 타일 화장실은 잔향이 심해 트랜지언트가 뭉갠다. 알고리즘보다
  설치 위치로 완화한다 — 천장 중앙, 평행한 경면 사이 피하기.
- **평문 MQTT.** 다른 노드가 쓰는 mTLS를 아직 안 쓴다. 나가는 것이 판정
  결과 JSON뿐이라 원시 음성 유출 위험은 없지만, 경보 위조는 가능하다.
