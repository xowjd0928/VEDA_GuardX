# GuardX 오디오 벤치마크

TOIMIC(RPi C 비명·총성 감지)과 VMSMIC(VMS 방송 마이크) 두 경로의 성능을 잰다.

**지금 재는 것**: 지연시간, CPU, 메모리.
**아직 안 재는 것**: 정확도. 라벨이 붙은 음원이 있어야 하고, 붙일 자리는
아래 "데이터셋 붙이기"에 이미 만들어 두었다.

측정은 **실제 서비스가 도는 기계**에서 한다.

| 대상 | 실행 기계 | 도구 |
|---|---|---|
| TOIMIC | RPi C | `benchmark/toimic/bench_toimic.py` |
| VMSMIC | VMS PC | `bench_vmsmic` (VMS CMake 타깃) |

실기가 없을 때는 도구만 확인하고 끝낸다. 두 도구 모두 **하드웨어가 없으면
0 을 찍지 않고 없다고 말한다** — 0 은 "빠르다"로 읽히기 때문이다.

---

## VMSMIC (VMS PC)

VMS 프로젝트의 CMake 타깃이다. GStreamer 가 발견될 때만 생성된다.

```bash
cmake -S vms -B build
cmake --build build --target bench_vmsmic
```

```bash
./build/bench_vmsmic --describe          # 파이프라인만 출력하고 종료
./build/bench_vmsmic --seconds 20        # 기본: fakesink (망에 안 쏨)
./build/bench_vmsmic --udp --host 172.20.33.114 --port 5004
./build/bench_vmsmic --seconds 30 --json out.json
```

측정 항목:

| 항목 | 뜻 |
|---|---|
| startup to first packet | PLAYING 전환 → 첫 Opus 패킷 |
| encode latency | 그 패킷이 필요로 한 마지막 입력 샘플 도착 → 패킷 생성 |
| packet interval | 나가는 패킷 간격 (20ms 프레이밍이므로 20 근처여야 정상) |
| payload bitrate | 실제로 만들어진 비트레이트 (VBR 이라 목표보다 낮을 수 있다) |
| reported latency | 파이프라인이 스스로 보고하는 `GST_QUERY_LATENCY` |
| cpu / rss | 프로세스 비용 |

파이프라인 서술은 `vms/broadcast_pipeline.h` 한 곳에서 온다. 방송 코드와
벤치마크가 **같은** 파이프라인을 쓰게 하려는 것이다 — 베껴 쓰면 숫자는
그럴듯한데 다른 것을 잰 상태가 되고, 그건 측정을 안 한 것보다 나쁘다.

기본 sink 가 `fakesink` 인 이유: 벤치마크가 현장 스피커를 울리면 안 된다.
망까지 포함해 재려면 `--udp` 를 준다.

---

## TOIMIC (RPi C)

감지기와 **같은 venv** 에서 돌린다. `detector.py` 를 그대로 임포트하므로,
감지기를 고치면 벤치마크도 자동으로 그 코드를 잰다.

```bash
cd rpi_c/rpic_app/toimic && . .venv/bin/activate
python ../../../benchmark/toimic/bench_toimic.py
python ../../../benchmark/toimic/bench_toimic.py --live 30    # 실제 마이크
python ../../../benchmark/toimic/bench_toimic.py --json out.json
```

측정 항목:

| 항목 | 뜻 |
|---|---|
| preprocess | 48k S32 스테레오 청크 → 16k 모노 (하이패스 + 리샘플) |
| inference | YAMNet 한 창(0.975s) 추론 |
| decide | 클래스 융합 + 히스테리시스 + N-of-M 투표 |
| hop total | 한 홉(0.25s 주기) 전체 |
| realtime margin | hop total p95 가 홉 예산(250ms)의 몇 % 인가 |

**합격 기준은 realtime margin 하나다.** p95 가 250ms 를 넘으면 감지가
실시간을 못 따라가고, 그 순간부터 지연이 쌓인다.

기본 모드는 합성 입력(잡음 + 주기적 임펄스)을 쓴다. 마이크가 없어도 돌고,
게이트를 통과하는 창과 통과 못 하는 창이 섞여 판정기까지 포함한 비용이
나온다. `--live` 는 실제 ALSA 캡처를 쓰므로 RPi C 에서만 의미가 있다.

---

## 데이터셋 붙이기 (정확도 측정)

라벨 음원이 생기면 그때 `--dataset` 으로 정확도를 잰다. 지금 형식을 먼저
못 박아 두는 이유는, 음원을 모을 때 **무엇을 모아야 하는지**가 정해져 있어야
하기 때문이다.

```
<dataset>/
  manifest.csv        relative_path,label
  clips/xxx.wav       16kHz 이상 WAV (모노 권장)
```

`label` 은 `scream` · `gunshot` · `none` 셋 중 하나다. `none` 은 오탐을 재는
데 쓰이므로 **반드시 함께 모아야 한다** — 비명만 모으면 재현율만 보이고,
현장에서 실제로 문제가 되는 오탐률은 영원히 안 보인다. 화장실 환경음
(물 내리는 소리, 핸드드라이어, 문 닫힘, 환풍기)이 `none` 의 핵심이다.

```bash
python bench_toimic.py --dataset ./clips
```

이벤트별로 tp/fp/fn 과 precision·recall·f1 을 찍는다. 임계값은 현재
프로파일(`GUARDX_TOIMIC_PROFILE`)의 것을 그대로 쓰므로, demo 와 prod 를
바꿔가며 돌리면 프로파일 비교가 된다.

공개 데이터셋 후보: AudioSet(비명·총성 클래스), ESC-50. 다만 둘 다 화장실
환경음이 없으므로 `none` 은 현장 녹음으로 채우는 편이 실제 성능에 가깝다.

---

## 결과 보관

`--json` 으로 남긴 결과는 `benchmark/results/` 에 모은다(git 에는 올리지
않는다). 비교할 때는 반드시 **같은 기계, 같은 프로파일**끼리 본다 — RPi C 와
개발 PC 의 숫자를 나란히 놓으면 아무 뜻도 없다.
