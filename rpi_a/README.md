# GuardX

3노드 라즈베리파이 기반 화재/비상상황 감지·대응 IoT 시스템

> **현재 상태: 프로토타입 단계, RPi A 실 하드웨어 통합 진행 중.** RPi A(센서 노드)는 센서 4종을 실물로 배선하고 센서값 실측까지 검증 완료. 이에 맞춰 커널 드라이버를 실 하드웨어용으로 재작성/개정했으며, **드라이버 단독 재검증 → 시스템 환경 구성 → App 재검증** 순으로 진행 중이다. RPi B/C는 통신 규약만 설계된 상태이며 구현은 아직 없음. 아래 "구현 현황"에서 축별로 상세히 구분함.

---

## 1. 시스템 구조

```
RPi A (센서 노드)          RPi B (브로커+판단)          RPi C (액추에이터)
  가스 (MQ-2)                                              LED
  불꽃 (TS0226)             Mosquitto MQTT 브로커          워터펌프
  온습도 (SHT30)                판단 로직                   AMP
  IR온도 (MLX90614)            PostgreSQL DB                DC팬
  비상 버튼                                                 서보 ×2
    │  │                                                    LED Matrix
    │  │                                                    (STM32 경유 UART 브릿지)
    │  │       센서 데이터           제어 명령
    │  └──────── MQTT ────→ [ ] ──────── MQTT ────────→ [ ]
    │          (QoS0/QoS2)          (QoS1)
    │
    └── 비상버튼 하드웨어 직결 (GPIO/릴레이, MQTT 미경유) ──────→ RPi C

                    카메라 (엣지 AI, RTSP/HTTPS)
                         │
                         └──── MQTT(예정) ────→ RPi B
```

- **센서 데이터**: RPi A ──MQTT──→ RPi B (`guardx/sensor/{node}`, 버튼은 QoS2)
- **제어 명령**: RPi B ──MQTT──→ RPi C (`guardx/actuator/{node}`, QoS1) — 판단 로직이 화재 확정 시 액추에이터 동작 명령 발행
- **비상 인터락**: RPi A ──하드웨어 직결──→ RPi C (버튼 즉시 차단, MQTT 미경유)

- **RPi A**: 센서 5종(가스·불꽃·온습도·IR온도·버튼)을 커널 드라이버로 읽어 1Hz로 MQTT 발행. 비상 버튼은 RPi C로의 즉시 제어(하드웨어 인터락)와 RPi B로의 로깅(MQTT)이 분리된 이중 경로.
- **RPi B**: MQTT 브로커 역할 + 센서/카메라 데이터를 받아 화재 여부 판단 + 액추에이터 제어 명령 발행 + DB 기록. (미착수)
- **RPi C**: 액추에이터 6종 제어. LED Matrix만 STM32에 연결되어 있어 RPi C가 UART로 중계. (미착수)
- **STM32**: 프로젝트 포함 여부 자체가 미확정이라, 포함/미포함 두 케이스를 모두 고려해 설계 중. 현재는 LED Matrix 제어 경로에서만 확정적으로 필요.

---

## 2. 구현 현황

### 🚧 RPi A — 실 하드웨어 통합 진행 중

**커널 드라이버 4종** (`rpia_app/drivers/`) — 물리 칩 1개 = 커널 모듈 1개 원칙(컨벤션 1-1):

| 모듈 | 칩 | 버스 | 노출 |
|---|---|---|---|
| `rpia_adc` | MCP3008 10bit ADC | **SPI0/CE0** | 불꽃(CH0)·가스(CH1) raw |
| `rpia_temphum` | SHT30 | I2C 0x44 | 온도·습도 (x10) |
| `rpia_irtemp` | MLX90614 | I2C 0x5A | 주변온도(Tamb)·대상온도(Tobj) (x10) |
| `rpia_button` | 기계식 버튼 | GPIO23 인터럽트 | 눌림 카운트 (poll 기반) |

- 넷 다 "아래=하드웨어 서브시스템 클라이언트 + 위=캐릭터 디바이스(`/dev/rpia_*`)" 이중 구조.
- **`rpia_adc`(SPI)만 DT 오버레이 + probe 구조**: 최신 커널(6.18.34+rpt-rpi-v8, Pi4 실기)은 외부 모듈이 버스번호로 spi_controller를 얻는 API가 제거돼(`spi_busnum_to_master` 없음) init에서 직접 attach가 불가능하다. 그래서 `rpia-adc-overlay.dts`로 spi0/CE0에 장치를 선언하고 커널이 `probe()`를 호출하게 했다.
- **`rpia_temphum`·`rpia_irtemp`(I2C), `rpia_button`(GPIO)은 init/exit 직접 획득 유지**: I2C는 `i2c_get_adapter()`, GPIO는 `gpio_request()`가 정상 동작하므로 오버레이가 필요 없다. 단 이 커널은 pinctrl-bcm2711 gpiochip base가 512라, 버튼은 BCM 번호가 아니라 **BCM+512**(23→535)를 legacy API에 넘겨야 한다(`RPIA_BUTTON_GPIO`). temphum/irtemp는 실 센서 없이 테스트할 **시뮬레이션 모드**(모듈 파라미터 + debugfs)도 내장 — 기본값(`simulate=false`)으로 로드하면 실제 하드웨어 경로.
- 커널 6.4/6.18 시그니처 변경(`class_create()` 등) 대응 완료.

**App 레이어** (`rpia_app/app/`): HAL → device registry(OPEN_ALL/READ_ALL) → JSON 직렬화 → MQTT 발행(poll 기반 1Hz 이벤트 루프). IR온도 확장에 맞춰 JSON 스키마가 `irtemp` → `irtemp_ambient`/`irtemp_object` 2필드로 변경됨.

**검증 상태**:
- ✅ **하드웨어 배선 + 센서값 실측**: MCP3008 경유 가스·불꽃, SHT30 온습도, MLX90614 주변·대상 IR온도 전부 실물에서 유효값 확인(`HW_test/` 통합 리더). GPIO 핀 배치 확정(아래 4절).
- ✅ **과거 시뮬레이션 파이프라인**: 드라이버 로드, RPi A↔RPi B 크로스 노드 MQTT, 장애 복원력(부분 실패 격리·브로커 재접속·정상 종료)은 시뮬레이션 모드에서 검증 완료.
- 🚧 **실 하드웨어 드라이버 재검증**: spi_driver/오버레이로 재작성한 `rpia_adc` 및 2값으로 확장한 `rpia_irtemp`의 실기 로드 검증이 다음 단계.
- ⏸ **배포 자동화**: systemd 유닛 5종 + `install.sh` 존재. 오버레이 도입에 맞춘 갱신 및 실기 적용은 드라이버 재검증 이후.

> **SPI 트러블슈팅 교훈(2026-07)**: 가스·불꽃이 계속 0으로 읽히던 문제는 하드웨어/칩 결함이 아니라 `config.txt`에 남은 커스텀 오버레이(`dtoverlay=spi0-0cs`)와 `dtparam=spi=on`의 충돌이었다. `raspi-config`로 SPI를 기본값 리셋하니 즉시 해결. loopback 테스트가 통과해도 config 오버레이 문제로 실제 칩 통신은 안 될 수 있으므로, 원인불명 SPI 불통 시 인터페이스 설정 리셋을 우선 시도할 것.

### 🚧 통신 프로토콜 — 설계 확정, 실기 검증 일부만

- `common/include/guardx_protocol.h` + `GuardX_Protocol_Convention.md`: 3노드 공통 MQTT 토픽/QoS/payload 스키마
- **실기 검증 완료**: `guardx/sensor/{node}`(QoS0), `guardx/sensor/{node}/button`(QoS2) — RPi A 실제 발행분
- **스키마 변경 반영 필요**: IR온도 2값화로 센서 payload가 바뀜(`irtemp_ambient`/`irtemp_object`). RPi B 파서가 이 새 필드를 받도록 맞춰야 함(아래 RPi B 항목).
- **설계만 확정, 실기 미검증**: `guardx/actuator/{node}`(QoS1) — RPi C가 없어 발행/구독 오간 적 없음
- **미확정**: `guardx/camera/{cam}` — 카메라 담당자 확인 대기

### 🚧 mTLS — 인증서 발급/클라이언트 통합까지, 브로커 실기 테스트 전

- `common/certs/gen_certs.sh`: CA + 노드별(rpia/rpib/rpic) 인증서 발급 스크립트, 체인 검증 완료
- RPi A 클라이언트(`mqtt_pub.c`)에 TLS 연동 완료 (인증서 로드 성공까지 확인)
- 브로커(RPi B) 쪽 mTLS 실기 적용은 다음 작업

### ❌ RPi B — 미착수

브로커는 지금까지 테스트용 평문 인스턴스로만 사용. 판단 로직(임계값/연속 카운트 기반 화재 확정), DB 스키마(ERD), 카메라 메타데이터 수신은 전부 설계 문서만 있고 코드 없음. **추가로, RPi A의 IR온도 2값화(`irtemp_ambient`/`irtemp_object`)에 맞춰 sensor_parser/decision/db_writer를 갱신해야 함(RPi A App 검증 후 착수).**

### ❌ RPi C — 미착수

액추에이터 6종(LED/워터펌프/AMP/DC팬/서보×2/LED Matrix) 목록과 MQTT 명령 스키마(`command`/`action`/`value`)만 확정. 드라이버·App 코드 없음. LED Matrix는 STM32 UART 브릿지가 필요해 다른 액추에이터와 구현 방식이 다름.

### ❌ 카메라 연동 — 미착수

엣지 AI 카메라의 detection 메타데이터를 MQTT로 어떻게 넘길지 카메라 담당 팀원과 협의 전.

---

## 3. 저장소 구조

```
rpi_a/
├── README.md
├── GuardX_Protocol_Convention.md   # MQTT 토픽/QoS/payload 규약 문서
├── common/
│   ├── include/guardx_protocol.h   # 위 문서에 대응하는 공통 헤더 (3노드 공유)
│   └── certs/                      # mTLS 인증서 발급 스크립트 + 브로커 설정 예시
└── rpia_app/                       # RPi A 전용 (드라이버+App)
    ├── drivers/                    # 커널 모듈 4종 (rpia_adc/temphum/irtemp/button)
    │   └── rpia-adc-overlay.dts    # MCP3008용 SPI 디바이스 트리 오버레이
    ├── hal/                        # 드라이버 wrapper
    ├── app/                        # MQTT 퍼블리셔
    ├── systemd/, install.sh        # 배포 자동화 (오버레이 대응 갱신 예정)
    └── test/                       # 시뮬레이션 기반 테스트 스크립트/가이드
```

`rpib_app/`, `rpic_app/`은 아직 존재하지 않음 — 각 노드 착수 시 `rpia_app`과 동일한 레벨의 형제 디렉토리로 추가 예정.

> 별도로 저장소 루트의 `HW_test/`에 실 센서 브링업용 독립 테스트 코드(spidev/i2c-dev 직접 접근, 커널 드라이버 무관)가 있다. 배선·센서 실측은 여기서 먼저 검증했다.

---

## 4. 빌드/실행 (RPi A, 실 센서 연결 기준)

### 배선 (RPi 40핀)

| 핀(물리/BCM) | 기능 | 연결 |
|---|---|---|
| 1 (3V3) | 전원 | MCP3008 VDD·VREF, SHT30·MLX90614 VCC |
| 6 (GND) | 접지 | 공통 |
| 3 (GPIO2 SDA), 5 (GPIO3 SCL) | I2C | SHT30(0x44), MLX90614(0x5A) 병렬 |
| 19 (GPIO10 MOSI), 21 (GPIO9 MISO), 23 (GPIO11 SCLK), 24 (GPIO8 CE0) | SPI | MCP3008 |
| 16 (GPIO23) | 인터럽트 | 비상 버튼 (내부 풀업, 눌림=GND) |

### SPI/I2C 활성화 + 오버레이 설치

```bash
sudo raspi-config nonint do_spi 0     # SPI 기본값으로 활성화
sudo raspi-config nonint do_i2c 0     # I2C 활성화

cd rpia_app/drivers
dtc -@ -I dts -O dtb -o rpia-adc.dtbo rpia-adc-overlay.dts
sudo cp rpia-adc.dtbo /boot/firmware/overlays/
# /boot/firmware/config.txt 의 [all] 섹션에 아래 한 줄 추가 후 재부팅:
#   dtoverlay=rpia-adc
sudo reboot
```

### 드라이버 빌드 + 로드

`rpia_adc`(SPI)만 오버레이+probe라 `modprobe`, 나머지 3종은 `insmod`.

```bash
cd rpia_app/drivers && make           # rpia_adc/temphum/irtemp/button .ko 생성

sudo modprobe rpia_adc                # 오버레이가 선언한 SPI 장치에 probe 바인딩
sudo insmod rpia_temphum.ko
sudo insmod rpia_irtemp.ko
sudo insmod rpia_button.ko            # GPIO23(=base 512+23) legacy 요청

ls /dev/rpia_*                        # adc/temphum/irtemp/button 4개 확인
dmesg | grep -E "rpia_adc|rpia_irtemp|rpia_temphum|rpia_button"
ls /dev/spidev0.0                     # 없어야 정상 (CE0가 rpia_adc로 넘어감)
```

### App 빌드 + 실행

```bash
cd ../app && make
sudo ./rpia_publisher                 # 드라이버 로드 + MQTT 브로커 접속 가능 상태 전제
```

상세 절차는 `rpia_app/RUN.md`, 시뮬레이션 시나리오 테스트는 `rpia_app/test/TEST_GUIDE.md` 참조. (RUN.md/systemd 유닛은 오버레이·IR온도 변경 반영이 아직 필요한 상태.)

---

## 5. 알려진 미확정 / 남은 작업

- **RPi A 드라이버 재검증**: spi_driver/오버레이로 바뀐 `rpia_adc`, 2값 확장된 `rpia_irtemp`의 실기 로드 확인 (진행 예정).
- **RPi A 시스템 환경**: 모듈 자동 로드(`modules-load.d`/systemd), 오버레이 영구 적용, `/dev/rpia_*` 부팅 자동 생성.
- **RPi A App 재검증**: 새 JSON 스키마(`irtemp_ambient`/`irtemp_object`)로 OPEN_ALL/READ_ALL/발행 확인.
- **RPi B 파서 갱신**: IR온도 2값화 반영(sensor_parser/decision/db_writer).
- 가스/불꽃 환산·임계값: RPi A는 raw(0~1023)만 발행하고, ppm 환산·불꽃 감지 임계값 판단은 RPi B(판단 노드) 몫으로 넘김(미착수). MQ-2 예열/캘리브레이션도 그때 함께.
- 프로토콜 문서 동기화 필요: `GuardX_Protocol_Convention.md`·`test/TEST_GUIDE.md`의 센서 payload 스키마가 아직 옛 필드(`gas_ppm`/`spark_detected`/단일 `irtemp`) 기준 → 새 스키마(`gas_raw`/`spark_raw`/`irtemp_ambient`/`irtemp_object`)로 갱신 필요(RPi B 착수 시 일괄).
- STM32 보드 포함 여부, 카메라 메타데이터 MQTT 스펙, DB 스키마(ERD).
