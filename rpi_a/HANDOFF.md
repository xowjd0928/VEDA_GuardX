# GuardX RPi A 작업 인수인계 (HANDOFF)

> 다른 세션(Claude Code)에서 이어서 작업하기 위한 현재 상태 + TODO 정리.
> 최종 갱신: 2026-07-27

---

## 협업 규칙 (빌드/배포 환경) ★

- **코드 원본**: Windows(이 세션에서 Claude가 편집하는 git 워킹트리)
  `C:\Users\3-05\Downloads\7th_VEDA_GROUP2-main\7th_VEDA_GROUP2-main\`.
- **빌드 환경**: 별도 Ubuntu VM(VirtualBox). 커널 모듈은 여기서 **크로스 컴파일**,
  App은 Pi에서 **네이티브 빌드**. (절차: `CROSS_COMPILE.md`)
- **Windows ↔ Ubuntu 자동 동기화 없음** (git remote 없음, VM 공유 폴더는 별도
  구버전 복사본). Ubuntu는 로컬 `~/guardx-repo`를 작업본으로 쓴다.
- **변경 반영 방식**: Claude가 Windows 원본을 수정하면, **바뀐 코드를
  `파일 경로 + Before/After + 이유`로 상세히 서술**한다. 사용자가 그 서술을 보고
  우분투 로컬 리포에 **직접 반영**한다. (Claude가 우분투/공유폴더로 직접 밀어넣지 않음)
- 문서(HANDOFF/CROSS_COMPILE/RUN/README)도 Windows 원본이 최신 기준.

---

## 0. 한 줄 요약

RPi A(센서 노드)의 커널 드라이버를 **실제 하드웨어 검증까지 끝냈고**, App(드라이버→HAL→JSON→MQTT)도 실측 발행까지 확인됨. gas/spark를 raw ADC로 통일하고 IR온도를 주변+대상 2채널로 확장하는 **스키마 변경 + 대청소**를 완료한 상태. 남은 건 **① 시스템 환경(드라이버 자동 적재/systemd 배포)**, **② RPi B 착수**, (선택) 드라이버 리팩터링.

---

## 1. 시스템 개요

3-노드 화재감지 IoT (Raspberry Pi):

- **RPi A** — 센서 노드. 커널 드라이버 + MQTT 퍼블리셔. **(현재 작업 대상, 거의 완료)**
- **RPi B** — 브로커 + 판단 로직. **미착수.**
- **RPi C** — 액추에이터(릴레이 등). **미착수.**

버튼은 이중 경로:
- **제어**: RPi A → RPi C 하드웨어 직결 릴레이 인터록 (MQTT 우회)
- **로깅**: RPi A → RPi B MQTT QoS2

### 개발/테스트 환경
- 편집: Windows (`C:\Users\3-05\Downloads\7th_VEDA_GROUP2-main\7th_VEDA_GROUP2-main\`)
- 테스트 타깃: 물리 Raspberry Pi 4, 커널 `6.18.34+rpt-rpi-v8` (파일 수동 복사)

---

## 2. 이 Pi4 커널 API 지형 (⚠️ 매우 중요 — 다른 Pi/커널과 다름)

| 버스 | 상태 | 핵심 |
|---|---|---|
| **SPI** | legacy API 제거됨 | `spi_busnum_to_master` 없음, `bus_find_device_by_name(&spi_bus_type,...)` 실패. → **DT 오버레이 + `spi_driver`(probe) 필수** |
| **GPIO** | legacy 동작함 | **gpiochip base = 512.** 전역 GPIO 번호 = BCM + 512 (BCM23 → **535**). `gpio_request(535)` 방식 OK |
| **I2C** | legacy 그대로 동작 | `i2c_get_adapter(1)` OK, 오버레이 불필요 |

- 에러 **517 = -EPROBE_DEFER**. (512~517 = ERESTARTSYS..EPROBE_DEFER 커널 내부 코드)
- SPI 불통 삽질했던 근본 원인은 **칩 결함 아님 → config.txt 오버레이 잔재**였음. `raspi-config nonint do_spi 1; do_spi 0; reboot`로 리셋해 해결.

---

## 3. 센서/드라이버 구성 (RPi A 규칙 1-1: 물리 칩 1개 = 커널 모듈 1개)

| 드라이버(.ko) | 버스 | 장치 | /dev 노드 | 값 |
|---|---|---|---|---|
| `rpia_adc` | SPI (오버레이+probe) | MCP3008 | `/dev/rpia_adc` | uint16 x2: **CH0=불꽃(TS0226), CH1=가스(MQ-2)**, 0~1023 raw |
| `rpia_temphum` | I2C 0x44 | SHT30 | `/dev/rpia_temphum` | int16 x2: temp_x10, hum_x10 |
| `rpia_irtemp` | I2C 0x5A | MLX90614 | `/dev/rpia_irtemp` | int16 x2: **ambient_x10, object_x10** |
| `rpia_button` | GPIO 23(=535) | 버튼 | `/dev/rpia_button` | uint32: press_count (poll+블로킹 read) |

- **MCP3008 채널 주의**: CH0 = 불꽃/spark, CH1 = 가스/gas (세션 중 정정된 값).
- **MLX90614**: reg 0x06=Tamb(주변), 0x07=Tobj1(대상). `°C = raw*0.02 - 273.15`, x10 스케일 = `(raw*20 - 273150)/100`.
- **SHT30**: cmd 0x2400 → 15ms → 6바이트 read, CRC8(poly 0x31, init 0xFF).
- **버튼**: GPIO535, falling-edge, 내부 pull-up(`gpiod_set_config` + `PIN_CONFIG_BIAS_PULL_UP`).

### JSON 스키마 (최종)
```json
{
  "values": {
    "gas_raw": 484, "spark_raw": 1022,
    "temperature": 26.6, "humidity": 60.2,
    "irtemp_ambient": 26.6, "irtemp_object": 29.0
  },
  "valid": { "gas": true, "temphum": true, "spark": true, "irtemp": true }
}
```
버튼 이벤트(별도 토픽 `guardx/sensor/rpia/button`, QoS2):
```json
{ "event": "emergency_button", "press_count": 3 }
```

### MQTT
- 프로덕션: `172.20.33.251:8883` mTLS (`MQTT_USE_TLS 1` 기본)
- 로컬 테스트 오버라이드:
  ```bash
  make EXTRA_CFLAGS='-DMQTT_USE_TLS=0 -DMQTT_BROKER_HOST=\"localhost\"'
  ```
  → localhost:1883 평문.

---

## 4. ✅ 완료된 작업

### 커널 드라이버 (실 하드웨어에서 로드/read 검증 완료)
- `rpia_adc.c` — `spi_driver` + probe + DT 오버레이로 재작성. simulate/debugfs/controller-lookup 제거. CH0/CH1 raw read.
- `rpia-adc-overlay.dts` — spidev0(CS0) 비활성 + `compatible="guardx,rpia-adc"` 노드 선언.
- `rpia_irtemp.c` — 주변(TAMB)+대상(TOBJ1) 2채널 read로 확장. debugfs `ambient_x10`/`object_x10`.
- `rpia_button.c` — legacy `gpio_request(535)` 방식(platform_driver 롤백). simulate 유지.
- 헤더/구조체(`irtemp_data_t {ambient, object}`, 채널 매크로 등) 동기화.
- **로드 결과 검증**: adc(spi0.0 probe), temphum(0x44), irtemp(0x5a), button(gpio=535 irq=60) 전부 정상, `drv_read.c` 값 정상.

### App (빌드/실측 발행 검증 완료)
- `guardx_hal.c` — gas/spark를 ppm/bool 환산 제거하고 **raw 그대로** 전달. (⚠️ `rpia_gas_*`/`rpia_spark_*`는 `/dev/rpia_adc` 읽는 **HAL 래퍼 함수** — 이름만 gas/spark, 삭제하면 안 됨)
- `json_builder.c` — `gas_raw`/`spark_raw`/`irtemp_ambient`/`irtemp_object` 필드.
- `mqtt_pub.h` — `MQTT_USE_TLS`/`MQTT_BROKER_HOST` `#ifndef` 가드(오버라이드용).
- `app/Makefile` — `$(EXTRA_CFLAGS)` 추가.
- **검증**: `gas_raw:484 spark_raw:1022 irtemp_ambient:26.6 irtemp_object:29.0` 정상 발행 확인.

### 대청소 (스키마 변경 후 잔재 정리 완료)
- 삭제: `rpia_gas.c/.h`, `rpia_spark.c/.h`, `systemd/rpia_gas.service`, `rpia_spark.service`, (구)`rpia-button-overlay.dts`.
- 신규: `systemd/rpia_irtemp.service`.
- systemd 의존성 수정: `rpia_publisher.service`가 adc/temphum/irtemp/button 참조, `Before=` 유닛명 오타(`guardx-rpia-publisher`→`rpia_publisher`) 수정.
- `install.sh` 재작성: 오버레이 dtc 컴파일+설치+config.txt 추가, enable 목록 갱신, SPI/I2C 멱등 활성화.
- `RUN.md`, `README.md`(rpi_a) 실 하드웨어 기준 갱신.
- `test/` 스크립트(01/04/05/06)+`TEST_GUIDE.md` 재작성(gas/spark 제거, irtemp 추가, adc는 시뮬 불가 명시).

---

## 5. 📋 TODO (남은 작업)

### ⓪ Pi 재초기화 + 크로스 컴파일 배포 워크플로우 — **진행 중**
> Pi를 새로 flash하고, Ubuntu에서 커널 모듈을 크로스 컴파일해 필요한 파일만
> scp하는 방식으로 전환. **가이드: `CROSS_COMPILE.md`** (헤더 rsync 방식 + App은
> Pi 네이티브 빌드로 확정). 드라이버 Makefile은 `ARCH`/`CROSS_COMPILE`/`KDIR`
> 오버라이드를 받도록 수정됨.
- [ ] Pi 초기화(64-bit) + do_spi/do_i2c + kernel-headers/libmosquitto-dev 설치
- [ ] Ubuntu: aarch64 툴체인 + Pi 헤더 rsync + `make ARCH=arm64 ...`
- [ ] vermagic 일치 확인 후 scp → Pi에서 App 네이티브 빌드 → insmod 4종 검증
- [ ] (커널 버전 바뀌면 헤더 재rsync + 재빌드 필수 — vermagic 불일치 주의)

### ① 시스템 환경 / 드라이버 자동 적재 (systemd 배포) — **⓪ 이후**
> 사용자 지시: "드라이버 자동 적재는 진짜 최종에 올리고". systemd 유닛은 **수정/준비 완료됐지만 Pi에서 배포·테스트 미완.**
> ⚠️ 현 `install.sh`는 **네이티브 빌드 전제**(Pi에서 make)라, 크로스 워크플로우에선
> "빌드" 단계를 빼고 산출물 배치+enable만 쓰도록 손봐야 함 (CROSS_COMPILE.md 6절).
- [ ] Pi에서 `install.sh` 실행 (오버레이 설치 → 재부팅 필요 확인)
- [ ] `dtoverlay=rpia-adc` 적용 후 재부팅 시 probe 바인딩 확인
- [ ] `systemctl enable` 5종(adc/temphum/irtemp/button/publisher) + 부팅 순서(After/Requires) 실검증
- [ ] 부팅 후 자동으로 `/dev/rpia_*` 생성되고 publisher가 발행하는지 E2E

### ② RPi B 착수 (RPi B 시작 시)
- [ ] 새 JSON 스키마 파서 (`gas_raw`/`spark_raw`/`irtemp_ambient`/`irtemp_object`)
- [ ] gas raw→ppm 환산 + 불꽃/가스 임계값 판단 로직
- [ ] 프로토콜 문서 동기화:
  - `GuardX_Protocol_Convention.md` (현재 80,83줄에 구 `gas_ppm`/`spark_detected` 잔존 — 의도적 보류, RPi B 때 일괄 갱신. `rpi_a/README.md:183`에 명시됨)
  - `TEST_GUIDE.md` 센서 스키마
- [ ] RPi B → RPi C 제어 커맨드 경로

### ③ (선택) 드라이버 리팩터링 — 사용자 "의견만" 요청, 미확정
- Option A: char-device 보일러플레이트를 `guardx_chrdev.h` static inline으로 추출해 4개 드라이버 중복 축소. (착수 전 사용자 확인 필요)

---

## 6. ⚠️ 헷갈리기 쉬운 함정 (인수인계 주의)

1. **`rpia_gas_*` / `rpia_spark_*` 함수는 살아있는 코드다.** 삭제된 건 커널 *드라이버*(`rpia_gas.ko`)뿐. HAL의 동명 함수들은 `/dev/rpia_adc`에서 채널 읽는 App 인터페이스라 유지.
2. **MCP3008 채널: CH0=불꽃, CH1=가스** (직관과 반대일 수 있음).
3. **GPIO 번호는 +512** — 코드에 BCM23이 아니라 535로 들어감.
4. **rpia_adc는 simulate 모드 없음** — 오버레이+실 SPI 하드웨어 필수. 그래서 App 전체 파이프라인은 adc 하드웨어 없이 못 돌림(temphum/irtemp/button만 부분 시뮬 가능).
5. **SPI 불통 시 하드웨어 먼저 의심하지 말 것** — config.txt 오버레이 잔재 → raspi-config로 SPI 리셋이 초반 체크리스트.
6. **App 빌드 시 그냥 `make`는 프로덕션 mTLS(8883)** — 로컬 테스트는 반드시 `EXTRA_CFLAGS` 오버라이드.

---

## 7. 주요 경로

```
rpi_a/
├─ README.md              # 시스템 구조/드라이버/빌드 (갱신됨)
├─ HANDOFF.md             # (이 문서)
└─ rpia_app/
   ├─ drivers/
   │  ├─ src/             # rpia_adc.c, rpia_temphum.c, rpia_irtemp.c, rpia_button.c
   │  ├─ include/         # 대응 헤더
   │  ├─ rpia-adc-overlay.dts
   │  └─ Makefile
   ├─ hal/                # guardx_hal.c/.h (rpia_*_open/read/close 래퍼)
   ├─ app/                # json_builder.c, mqtt_pub.*, device_registry.c, Makefile
   ├─ systemd/            # rpia_{adc,temphum,irtemp,button,publisher}.service
   ├─ install.sh          # 오버레이+빌드+enable 일괄
   ├─ RUN.md
   └─ test/               # 01_load_modules / 04_inject / 05_driver_smoke / 06_cleanup / TEST_GUIDE.md
```

관련 메모리(`~/.claude/.../memory/`): `guardx_spi_config_reset_fix.md`, `guardx_kernel_api_landscape.md`.
```
```
