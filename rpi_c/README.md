# GuardX - RPi C (액추에이터 노드)

RPi B의 판단 로직이 MQTT로 내리는 제어 명령(`guardx/actuator/rpic`, QoS 1)을
받아 액추에이터를 구동하는 노드.

> **현재 상태: 코드 구현 완료, 실기 미검증.** 실물 액추에이터가 아직 없어
> RPi A와 동일한 시뮬레이션 모드로 전 경로(MQTT 수신 → 파싱 → 디스패치 →
> 드라이버 write)를 검증하는 단계까지 준비됨. 실물 배선 후 `[SIMULATION MODE]`
> 블록 제거 + 핀/각도 실측 보정이 필요하다.

---

## 1. 하드웨어 구조

```
                         5V 4A Adapter ══ 5V 라인 (1000uF 안정화)
                                              ║
RPi B ──MQTT(QoS1)──→ RPi C ─I2C─→ PCA9685 ──╫──→ Servo 문      (CH0)
      guardx/actuator/rpic  │      (0x40)    ╙──→ Servo 가스밸브 (CH1)
                            ├─HW PWM(GPIO12)───────→ DC팬
                            ├─GPIO(5/6/16/26)─→ ULN2003 ══→ 28BYJ-48 스텝모터
                            ├─GPIO(23/24)──→ HG7881 ═════→ Water Pump
                            └─I2S──→ MAX98357A ──────────→ 스피커 (경보음, ALSA 별도 경로)
```

> **실물 검증 반영(config 변경):** 초기 설계는 서보2+DC팬을 전부 PCA9685로
> 돌리고 오디오는 3.5mm 아날로그(LM386)로 냈으나, 실물 배선/검증 과정에서
> `dtparam=audio=off` + `dtoverlay=pwm,pin=12,func=4`(팬 HW PWM) +
> `dtoverlay=max98357a,sdmode-pin=4`(I2S 앰프) 구성으로 바뀌었다. 그 결과
> 팬이 PCA9685에서 GPIO12 HW PWM으로 빠지고, 앰프는 I2S로 이동해
> SD_MODE(GPIO4)를 커널이 재생 상태에 맞춰 제어하며,
> 스텝모터(28BYJ-48)가 새로 추가됐다.

- **서보 2개는 PCA9685(I2C, 0x40)로 구동한다.** 팬이 빠지면서 채널 주파수
  공유 제약(50Hz)이 사라져 서보 전용 50Hz로 쓴다. 펄스폭은 실물 검증치
  0.6~2.4ms(≈123~492 tick). *검증 시 개체 특성상 실제 가동 범위가 ~160도에서
  물리적으로 멈추는 것을 확인 - 기구 조립 후 펄스폭 재보정 여지.*
- **DC팬은 RPi 내장 하드웨어 PWM(GPIO12)으로 25kHz 구동한다.** 3.5mm 아날로그
  오디오를 끄면서(`dtparam=audio=off`) 비게 된 내장 PWM 자원을 사용
  (`dtoverlay=pwm,pin=12,func=4`). 25kHz라 가청 소음이 없다.
  > 팬만 커널 모듈이 아니라 App(hal/)이 sysfs로 직접 제어한다 - 커널 6.8+에서
  > 레거시 in-kernel PWM API(`pwm_request` 등)가 제거돼, 검증 코드(pwm.c)가
  > 쓴 안정적인 sysfs(`/sys/class/pwm/pwmchip0`) 경로를 그대로 쓴다. HAL
  > 인터페이스(`rpic_fan_*`)는 동일해 앱/registry는 이 차이를 모른다.
- **셔터 = 스텝모터(28BYJ-48)**. ULN2003 경유, GPIO 4핀(5/6/16/26) 하프스텝
  8상 구동. MQTT 명령은 `shutter CLOSE/OPEN/STOP`이고 App이 이를
  실물 방향(닫기=CCW, 열기=CW)으로 바꾼다. 정지 명령 또는 그 방향 리밋
  감지까지 계속 돌린다. 회전은 커널
  스레드가 백그라운드로 돌려 write()는 블로킹하지 않는다.
- **방향별 리밋 리드센서 2개**는 스텝모터 드라이버에 통합. `rpic_stepper`가
  IRQ로 감시해 **CW(+)로 돌 때 GPIO17 감지 시 정지, CCW(-)로 돌 때 GPIO27
  감지 시 정지**(방향별 엔드/홈 스톱). 반대 방향으로는 그 리밋에 걸려 있어도
  빠져나갈 수 있다. 원본 파이썬(gpiozero) 리드센서를 커널로 이관.
  풀업 필요(config.txt `gpio=17=ip,pu` + `gpio=27=ip,pu`, 또는 온보드 풀업).
- **워터펌프**는 HG7881(L9110S) 모터 드라이버 경유 ON/OFF (GPIO 2핀 23/24, IA/IB)
- **앰프(MAX98357A)**는 I2S 디지털 앰프다. SD_MODE를 BCM GPIO4(물리 핀 7번)에
  연결하고 `dtoverlay=max98357a,sdmode-pin=4`로 커널이 활성/종료 상태를 관리한다.
  부팅 오버레이가 ALSA 카드로 잡아주며, 경보음 재생은 App 레벨
  `rpic_audio`(ALSA)가 담당하고,
  **`sound` 명령 때만 상황음**을 내도록 구독자에 통합됨(`audio_event`).
  화재·거수자는 실행 중인 구독자 위치 기준 `../assets/audio/`의 WAV를
  재생하고 나머지는 내장 톤을 사용한다.
  > 옛 `rpic_amp`(LM386 GPIO 전원스위치) 커널 드라이버와 `amp` MQTT 명령은
  > **삭제됨**. MAX98357A의 SD_MODE는 별도 앱 명령이 아니라 ASoC 드라이버가 관리한다.
- 서보/팬/스텝모터/펌프 전원은 5V 4A 어댑터 라인, RPi와 공통 GND 필수
  (그라운드 미연결 시 서보 각도가 튄다 - 검증 중 확인된 흔한 실수)

### 지원 명령 (프로토콜 규약 4-3)

| `command` | action | 구동 경로 | 상태 |
|---|---|---|---|
| `servo_1` (가스밸브) | SET(0~180) | PCA9685 CH0 | 구현 (단위 검증 완료) |
| `fan` | ON/OFF/SET(0~100%) | HW PWM GPIO12 (25kHz) | 구현 (단위 검증 완료) |
| `shutter` | CLOSE/OPEN/STOP | GPIO 5/6/16/26 → ULN2003 (닫기=CCW/GPIO27, 열기=CW/GPIO17 리밋 정지) | 구현 |
| `water_pump` | ON/OFF | GPIO 23/24 → HG7881 | 구현 (단위 검증 완료) |
| `sound` (스피커) | SET(0 기본/1 화재/2 강도/3 비상)·ON | MAX98357A I2S → rpic_audio(ALSA) | 신규 (상황음, 톤; 파일 교체 가능) |
| `led` | - | **미배선** | 수신 시 경고 로그 + 무시 |

"단위 검증 완료" = 각 액추에이터를 독립 유저스페이스 테스트 코드로 실물
구동 확인함(핀/펄스폭/주파수 실측). MQTT→드라이버 전 경로 통합 검증은
시뮬레이션까지 확인, 실물 통합은 조립 후 예정.

MQTT에는 장치 역할인 `shutter`와 논리 동작 `CLOSE/OPEN/STOP`만 노출한다.
RPi C가 이를 실물 방향(닫기=CCW/BOTTOM, 열기=CW/TOP)으로 변환한다.
스텝모터 방향값은 HAL 아래에서만 사용한다.

---

## 2. 소프트웨어 구조 (RPi A와 대칭)

```
rpi_c/
├── README.md
├── common/                         # rpi_a/common/의 복사본 (3노드 공유분)
│   ├── include/guardx_protocol.h   # !!! rpi_a 쪽과 반드시 동기화 유지 !!!
│   └── certs/                      # mTLS 인증서 발급 스크립트 (CN=rpic 발급용)
└── rpic_app/
    ├── drivers/                    # 커널 모듈 3종 (팬·앰프는 커널 모듈 아님, 아래 참조)
    │   ├── rpic_pca9685            #   /dev/rpic_pca9685 (서보×2,      ioctl 0xC1)
    │   ├── rpic_pump               #   /dev/rpic_pump    (HG7881,      ioctl 0xC2)
    │   └── rpic_stepper            #   /dev/rpic_stepper (셔터 28BYJ-48 + 리밋 GPIO17/27, ioctl 0xC5)
    │   # 팬: 커널 6.8+ 레거시 PWM API 제거 → hal/이 sysfs(pwmchip0)로 제어
    │   # 앰프: MAX98357A(I2S)는 오버레이가 ALSA 카드로 잡아줌 → 드라이버 없음.
    │   #       소리는 app/의 rpic_audio(ALSA)가 낸다. (옛 rpic_amp는 삭제)
    ├── hal/                        # 드라이버 wrapper (/dev 경로는 여기만)
    ├── app/                        # MQTT 서브스크라이버 (rpic_subscriber)
    │   ├── mqtt_sub                #   구독 + 재접속/재구독 (on_connect에서 구독)
    │   ├── cmd_parser              #   JSON → {command, action, value}
    │   ├── actuator_registry       #   command → HAL 디스패치 + 안전 상태 + 알림음 트리거
    │   ├── rpic_audio              #   I2S 오디오 재생(ALSA, 톤/raw PCM/WAV API)
    │   ├── audio_event             #   액추에이터 켜질 때 알림음 1회(워커 스레드)
    │   └── audio_test              #   오디오 단독 검증 CLI (make tools)
    ├── systemd/, install.sh        # 배포 자동화 (드라이버 3 + App 1)
    ├── test/                       # 시뮬레이션 기반 테스트 스크립트/가이드
    └── modbus/                     # STM32 Modbus RTU 통신 시험 (신규, MQTT와 독립)
        ├── modbus_rtu              #   Modbus RTU Master (termios /dev/ttyUSB0 RS-485, FC 03/06/10)
        └── modbus_test             #   시험 CLI (read/write/status/selftest ...)
```

> **`modbus/` (신규):** STM32(Modbus RTU **Server**, Slave ID 1)와 UART로 통신하는지
> 검증하는 유저스페이스 CLI. LED 매트릭스 **표시**(`guardx/display/rpic/…`의
> 온습도·화재·트래킹)를 STM32로 중계하는 `app/matrix_link`의 하위 계층이다. MQTT
> 구독자(`app/`)와는 독립이고 외부 라이브러리 의존도 없다. 배선·사용법·레지스터
> 맵은 `rpic_app/modbus/README.md` 참조. STM 펌웨어는 `rpi_c/stm32/Led_Matrix_Test`
> (`Core/Src/modbus_slave.c`, `main.c`의 `APP_ENABLE_MODBUS` 스위치).

데이터 흐름:

```
RPi A(센서) → RPi B(브로커+판단) → [MQTT QoS1] → mqtt_sub(on_message)
  → cmd_parser(PARSE_CMD_JSON) → actuator_registry(DISPATCH_CMD)
  → guardx_hal(write) → /dev/rpic_* → 커널 드라이버 → I2C/GPIO
```

설계 결정 (RPi A 컨벤션 계승 + 액추에이터 특성 반영):
- **OPEN_ALL은 하나라도 실패하면 기동 실패** - "팬은 도는데 밸브를 못 잠그는"
  부분 동작 상태로 기동하지 않는다
- **명령 개별 실패는 격리** - 잘못된 payload/범위 초과/드라이버 실패는 로그만
  남기고 프로세스 유지 (RPi A READ_ALL의 valid=false 정책과 동일)
- **멱등이라 중복 제거 없음** - QoS 1 중복 수신은 같은 결과이므로 seq 검사 안 함
- **안전 상태 이중 보장** - 드라이버 로드/언로드 시(커널), App 시작/종료 시
  (유저스페이스) 각각 팬/펌프 OFF + 스텝모터 정지 + 서보 안전 각도 적용
- **ACK(4-4)는 규약대로 미사용** - `mqtt_sub.h`의 `RPIC_ENABLE_ACK`로 자리만 예약

---

## 3. 빌드/실행 - 시뮬레이션 모드 (실물 액추에이터 연결 X)

```bash
cd rpic_app/drivers && make
sudo insmod rpic_pca9685.ko simulate=1
sudo insmod rpic_stepper.ko simulate=1
sudo insmod rpic_pump.ko simulate=1
# 팬은 커널 모듈이 아님. pwmchip0가 없으면 App이 자동으로 팬 no-op(soft)
# 모드로 뜨므로 시뮬레이션에 별도 준비 불필요.

cd ../app && make
sudo ./rpic_subscriber
```

명령 주입 (RPi B 역할 흉내):

```bash
cd rpic_app/test
./03_publish_cmds.sh demo      # 또는 servo1 90 / fan 70 / pump on ...
```

상세 절차는 `rpic_app/RUN.md`, 시나리오별 테스트는 `rpic_app/test/TEST_GUIDE.md` 참조.

---

## 4. 알려진 미확정 사항

- **부팅 설정 의존**: 팬(HW PWM)은 `dtparam=audio=off` +
  `dtoverlay=pwm,pin=12,func=4`, 앰프는
  `dtoverlay=max98357a,sdmode-pin=4`(SD_MODE↔BCM GPIO4),
  PCA9685는 `dtparam=i2c_arm=on`이 config.txt에 있어야 한다. 커널 모듈은
  이 오버레이가 등록한 자원(pwmchip0 등)에 붙는다.
- **팬은 sysfs로 제어(커널 모듈 아님)**: 커널 6.8+에서 레거시 in-kernel
  PWM API가 제거돼(실제로 6.18에서 `pwm_request`/`pwm_free`/`pwm_config`
  링크 실패 확인) 커널 모듈 대신 `hal/guardx_hal.c`가 sysfs
  (`/sys/class/pwm/pwmchip0/pwm0`)를 직접 연다. `pwmchip0`가 없으면
  (오버레이 미구성) 팬만 no-op(soft) 모드로 뜨고 노드는 계속 산다 -
  이때 경고가 stderr에 남으므로 실물이면 오버레이를 확인할 것.
  (in-kernel 방식으로 되돌리려면 DT 오버레이+플랫폼 드라이버 배선 필요)
- **GPIO 전역 base 의존**: 최신 커널(Pi4/BCM2711 + 6.x)은 메인 gpiochip
  전역 base가 0이 아니라 512다(`cat /sys/class/gpio/gpiochip*/base`로 확인).
  레거시 `gpio_request(BCM)`가 -517(EPROBE_DEFER)로 실패해서, stepper/pump
  드라이버는 `gpio_base`(기본 512) 모듈 파라미터를 두고 실효 번호를
  `base + BCM`으로 요청한다. 커널/보드가 base를 바꾸면
  `insmod ... gpio_base=NNN`으로 덮어쓴다. (장기적으로는 gpiod 디스크립터
  API로 이관하는 게 정석)
- **`shutter` 명령**: `CLOSE=닫기`, `OPEN=열기`, `STOP=정지`.
  App이 논리 동작을 실제 모터 방향으로 변환한다.
  write()는 방향만 설정하고 즉시 반환한다(회전은 커널 스레드).
- **방향별 리밋 정지(CW=GPIO17, CCW=GPIO27)**: 스텝모터 드라이버가 두 리드센서를
  IRQ로 감시해, CW로 돌 때 GPIO17·CCW로 돌 때 GPIO27이 감지되면(LOW) 그 방향
  회전을 멈춘다(방향별 엔드 스톱). **풀업 필요**(config.txt `gpio=17=ip,pu` +
  `gpio=27=ip,pu`, 또는 온보드 풀업). 없으면 플로팅으로 튄다. 어떤 리밋에 걸린
  뒤에도 반대 방향 명령은 먹어서 빠져나올 수 있다.
- **I2S 알림음(`rpic_audio` + `audio_event`)**: `sound SET 1`은
  `fire_alert.wav`, `sound SET 2`는 `intruder_alert.wav`를 재생하도록 구독자에
  통합. 대기 중에는 ALSA 장치를 열어두지 않고, 요청마다
  open → 재생 → close한다. 소리는 부가 기능이라 MAX98357A 카드가 없으면
  재생 요청 때 경고만 남기고 노드는 계속 산다. WAV 재생이 실패하면 기존 내장
  톤으로 대체한다. 소리만 따로
  확인하려면 `make tools`의 `audio_test`. 구독자 빌드에 `libasound2-dev` 필요.
- **혼잡 위험 경고음**: VMS와 동일한 `guardx/alert/rpib` 경보를 구독하고,
  채널이 `critical`로 전이하면 `crowd_alert.wav`를 한 번 재생한다. QoS 1 중복
  메시지는 채널별 마지막 단계로 제거한다.
- **`rpic_amp`(LM386) 삭제됨**: 하드웨어가 MAX98357A(I2S)로 바뀌어 옛
  전원스위치 커널 드라이버·HAL·systemd·`amp` MQTT 명령을 모두 제거했다.
  소리는 `rpic_audio`가 ALSA로 내고, MAX98357A의 SD_MODE는 BCM GPIO4를 통해
  ASoC 드라이버가 관리한다.
- **서보 안전 각도(SERVO*_SAFE_ANGLE)**: 문 힌지/밸브 레버 방향 확정 전까지
  잠정 0도. 서보 실측 가동 범위 ~160도 - 조립 후 펄스폭 재보정 여지.
- **팬 ON 기본 듀티**(현재 100%) / 최소 구동 듀티: 실측 후 확정
- **led**: 미배선 (수신 시 무시)
- **led_matrix 액추에이터 명령**: 삭제됨. STM32 UART 브릿지가 끝내 구현되지
  않아 받아도 무시만 했다. LED 매트릭스 자체는 표시 경로
  (`guardx/display/rpic/{zones/N,fire,track}` → `matrix_link` → Modbus → STM32)
  로 계속 쓴다 — 그쪽은 액추에이터 명령과 무관하다.
- **브로커 접속 정보**: 현재 로컬 테스트 기본값(평문/localhost). RPi B 실기
  연동 시 `app/include/mqtt_sub.h`에서 TLS=1 + 실제 IP로 교체
