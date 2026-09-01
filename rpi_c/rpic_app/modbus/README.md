# RPi C ↔ STM32 Modbus RTU 통신 시험 도구

STM32(Modbus RTU **Server**, Slave ID 1)와 Raspberry Pi(Modbus RTU **Client/Master**)가
UART로 통신하는지 검증하는 유저스페이스 CLI(`modbus_test`).

> STM32 펌웨어는 FreeRTOS·Modbus·HUB75가 통합된 상태다. `PANEL_BRIGHTNESS`와
> `PANEL_REFRESH_LEVEL`은 HUB75 드라이버에 즉시 반영되고, `LED_COMMAND`는
> PA4 단일 GPIO LED를 제어한다.
>
> 설계 근거: `GuardX_Modbus_RTU_설계_프로토타입_전체프레임시각화.xlsx` v0.2.

MQTT 액추에이터 구독자(`../app/rpic_subscriber`)와는 **독립**이다. 외부 라이브러리
의존이 없다(표준 C + termios만).

---

## 1. 배선 (RS-485 반이중)

3.3V TTL 점대점에서 RS-485 반이중으로 바꿨다. **RTU 프레임/CRC는 그대로**이고
소프트웨어에서 바뀐 것은 시리얼 장치 경로 하나뿐이다 — 아래 "코드가 안 바뀌는
이유" 참조.

```
   STM32F401RE                SZH-CVBE-010                SZH-CVBE-008        RPi C
  USART6_TX PC6  ──TXD──▶ ┌──────────────┐            ┌──────────────┐
  USART6_RX PA12 ◀──RXD── │ TTL to RS485 │ A ──────── A│ USB to RS485 │──USB──▶ /dev/ttyUSB0
  3V3 / GND      ──────── │  자동 흐름제어  │ B ──────── B│  자동 흐름제어  │
                          └──────────────┘  (꼬임쌍선)  └──────────────┘
```

| 구간 | 연결 |
|---|---|
| STM32 → 010 | PC6(USART6_TX) → 모듈 TXD, PA12(USART6_RX) ← 모듈 RXD, 3V3, GND |
| 010 ↔ 008 | **A↔A, B↔B** (차동 한 쌍, 꼬임쌍선 권장) |
| 008 → RPi C | USB 포트 (전원도 USB에서 나오므로 별도 급전 불필요) |

- **A/B 극성이 바뀌면 통신이 안 된다.** 증상이 "무응답"뿐이라 제일 먼저 의심할 것.
- **TX↔RX 교차는 STM32 ↔ 010 구간에만 해당**한다. A/B는 교차하지 않는다(A는 A로).
- STM32와 010 모듈은 **공통 접지**가 필요하다. 반대로 008은 RPi C의 USB에서
  전원을 받으므로, 두 보드의 전원 핀을 서로 잇지 않는다.
- 선이 길거나(수 m 이상) 통신이 불안정하면 양 끝단 **120Ω 종단저항**을 확인한다
  (모듈에 내장된 경우가 많다 — 점퍼/스위치로 켜고 끈다).
- **GPIO14/15(물리 8·10번)는 이제 쓰지 않는다.** RPi C가 USB로 붙으므로 그
  핀들과 `/dev/serial0`이 비게 된다.

### 코드가 안 바뀌는 이유 (자동 흐름제어)

RS-485는 반이중이라 보내기 전에 드라이버를 켜고(DE), 다 보낸 뒤 꺼서 수신으로
돌려야 한다. **두 모듈 모두 "자동 흐름제어"라 그 전환을 모듈이 알아서 한다** —
TX 신호를 감지해 스스로 방향을 바꾸므로 RPi C도 STM32도 DE/RE용 GPIO를 쓰지
않는다.

이게 중요한 이유: **STM32F401의 USART에는 하드웨어 Driver-Enable 기능이 없다**
(그 기능은 F0/F3/F7/L4 세대부터다). 수동 모듈이었다면 STM32 쪽에서 전송완료(TC)
인터럽트를 잡아 GPIO를 직접 토글해야 했고, 타이밍을 놓치면 응답 앞뒤가 잘리는
디버깅하기 고약한 문제가 생긴다. 자동 모듈을 고른 덕에 그 작업이 통째로 없어졌다.

### Raspberry Pi 준비

USB 변환기라 `raspi-config`로 시리얼을 켤 필요가 없다(GPIO UART를 안 쓴다).
꽂고 나서 장치만 확인한다.

```bash
lsusb                     # 변환기 칩(CH340/CP2102/FT232 등)이 보이는지
ls -l /dev/ttyUSB*        # 보통 /dev/ttyUSB0
dmesg | tail              # 어느 ttyUSB에 붙었는지
```

> **장치 번호는 고정이 아니다.** USB 시리얼이 여러 개면 `ttyUSB0`/`ttyUSB1`이
> 부팅마다 바뀔 수 있다. 고정하려면 udev 규칙으로 별칭을 만든다:
> ```bash
> # /etc/udev/rules.d/99-guardx-rs485.rules
> SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", SYMLINK+="guardx-rs485"
> ```
> (`lsusb`로 본 실제 VID:PID로 바꿀 것) 그다음 `/dev/guardx-rs485`를 쓰면 된다 —
> `modbus_test -d /dev/guardx-rs485`, 데몬은 `GUARDX_MATRIX_DEV=/dev/guardx-rs485`.

---

## 2. 빌드

```bash
cd rpic_app/modbus
make                                  # modbus_test 생성 (이 폴더 밑)
# 크로스컴파일: make CC=aarch64-linux-gnu-gcc
```

STM 쪽은 STM32CubeIDE에서 `rpi_c/stm32/Led_Matrix_Test` 프로젝트를 빌드/플래시한다.
부팅 후 Modbus 태스크는 UART 프레임을 처리하고, UI/Render 태스크는 레지스터 값을
HUB75 화면에 반영한다.

---

## 3. 사용법

```bash
./modbus_test                     # UART 연결을 유지하는 대화형 미니쉘
./modbus_test [옵션] <명령> [인자] # 자동화용 단발 실행
```

옵션: `-d <dev>`(기본 `/dev/ttyUSB0`) · `-b <baud>`(기본 115200) · `-s <id>`(기본 1)
· `-t <ms>`(응답 타임아웃, 기본 200) · `-r <n>`(재시도, 기본 2) · `-v`(프레임 hex 출력)

| 명령 | 설명 |
|---|---|
| `read <addr> <count>` | FC03 Holding Register 읽기 |
| `write <addr> <value>` | FC06 단일 레지스터 쓰기 |
| `wmulti <addr> <v1> [v2..]` | FC10 다중 레지스터 쓰기 |
| `led <on\|off>` | `LED_COMMAND`(0) — PA4 LED |
| `brightness <0-255>` | `PANEL_BRIGHTNESS`(1) — HUB75 밝기 즉시 반영 |
| `refresh <1-4>` | `PANEL_REFRESH_LEVEL`(2) — HUB75 주사율 즉시 반영 |
| `screen <0-3>` | `SCREEN_SELECT`(130) |
| `zones <t1 h1>` | Zone 1 온도×10/습도만 갱신, Zone 2~4는 유지 |
| `zones <t1 h1 ... t4 h4>` | Zone 1~4 온도×10/습도 전체 갱신 |
| `fire <1-4\|off>` | 해당 Zone 화재 화면 표시/해제 |
| `target <x1> <y1> <x2> <y2>` | 현재점 A와 방향점 B 갱신, 각 좌표 0~1000 |
| `target off` | 두 점 즉시 제거 |
| `status` | 제어+상태+진단 블록(0~7) 요약 |
| `dump` | 정의된 모든 레지스터 라벨과 함께 출력 |
| `monitor [ms]` | 상태 주기 폴링(기본 500 ms) |
| `raw <hex..>` | CRC 제외 프레임 바이트 입력 → CRC 붙여 전송 |
| `selftest` | 설계 T01~T16 골든 벡터로 STM 검증 |

### 예시

```bash
./modbus_test
GuardX Modbus shell - /dev/ttyUSB0, 115200 bps, Slave 1
명령 목록은 help, 종료는 exit 또는 quit
modbus> status
modbus> status -v
modbus> debug on
modbus> status
modbus> debug off
modbus> led on
modbus> brightness 128
modbus> screen 1
modbus> zones 250 45
modbus> zones 250 45 260 50 270 55 280 60
modbus> screen 2
modbus> target 200 400 350 450
modbus> target off
modbus> fire 3
modbus> fire off
modbus> exit

./modbus_test -v status          # 프레임까지 보면서 상태 읽기
./modbus_test led on             # PA4 LED ON  (01 06 00 00 00 01 ...)
./modbus_test write 1 128        # 밝기 레지스터 128
./modbus_test wmulti 0 1 128 2   # LED=ON, 밝기=128, 주사율=2 (한 프레임)
./modbus_test target 200 400 350 450  # A(200,400), B(350,450)
./modbus_test read 0 8           # 0~7번 레지스터 8개 읽기
./modbus_test raw 01 03 00 00 00 03   # 임의 PDU 손수 전송(CRC 자동)
./modbus_test selftest           # T01~T16 전체 검증
```

미니쉘에서는 명령 뒤에 `-v`를 붙이면 해당 명령의 TX/RX 프레임만 출력한다.
예: `status -v`, `read 0 8 -v`, `led on -v`.

미니쉘은 `Tab` 명령어 자동완성, `↑/↓` 명령 기록, `←/→` 커서 이동,
중간 문자 삽입과 `Backspace`/`Delete` 편집을 지원한다. 이 기능은 터미널에서
직접 실행할 때만 켜지고, `Ctrl+C`로 미니쉘을 종료한다. 파이프나 스크립트 입력은
기존 줄 단위 입력을 유지한다.

`debug on`을 입력하면 이후 모든 명령의 TX/RX 패킷을 계속 출력하고,
`debug off`로 끈다. `debug`만 입력하면 ON/OFF가 전환되며 `verbose`도 같은 명령이다.

### selftest

STM을 **리셋한 직후** 실행하면 좋다(T01이 기본값 LED=off·밝기=255·주사율=1을 기대).
16개 벡터를 그대로 쏘고 응답을 **바이트 단위**로 대조하며, 끝에서 기본값을 복구해
재실행에 대비한다.

```
=== GuardX Modbus RTU selftest (T01~T16, 설계 골든 벡터) ===
  OK  T01  기본 레지스터 읽기   -> 01 03 06 00 00 00 FF 00 01 D0 85
  OK  T02  LED ON 단일 쓰기     -> 01 06 00 00 00 01 48 0A
  ...
  OK  T10  CRC 오류 무응답      -> (무응답)
  OK  T11  다른 Slave ID        -> (무응답)
  OK  T12  Broadcast 쓰기       -> (무응답)
  OK  T13  좌표 묶음 쓰기       -> 01 10 00 79 00 05 D1 D3
  ...
  OK  T16  좌표 즉시 제거       -> 01 06 00 79 00 00 58 13
==== 16개 중 16 통과, 0 실패 ====
```

---

## 4. 레지스터 맵 (설계 "레지스터 맵" 시트 · PDU 주소)

| PDU | 이름 | 접근 | 범위 | 비고 |
|---:|---|:---:|---|---|
| 0 | LED_COMMAND | RW | 0~1 | PA4 LED |
| 1 | PANEL_BRIGHTNESS | RW | 0~255 | matrix 연결 후 반영 |
| 2 | PANEL_REFRESH_LEVEL | RW | 1~4 | matrix 연결 후 반영 |
| 3 | DEVICE_STATUS | RO | — | bit0=UART bit1=HUB75 |
| 4 | UPTIME_SECONDS | RO | — | 초, 순환 |
| 5 | RX_FRAME_COUNT | RO | — | 정상 CRC+대상 프레임 |
| 6 | CRC_ERROR_COUNT | RO | — | CRC 불일치 |
| 7 | EXCEPTION_COUNT | RO | — | 예외 응답 수 |
| 100~107 | ZONE1~4 TEMP/HUMIDITY | RW | 온 0~65534 / 습 0~100 | 0xFFFF/0x00FF=없음 |
| 120 | FIRE_ZONE_BITMAP | RW | 0~15 | bit0~3=Zone1~4 |
| 121 | INTRUDER_TRACK_STATUS | RW | 0, 1, 3 | bit0=A, bit1=B 유효 |
| 122~123 | INTRUDER_CURRENT_X/Y | RW | 0~1000 | 현재점 A |
| 124~125 | INTRUDER_DIRECTION_X/Y | RW | 0~1000 | 방향점 B |
| 130 | SCREEN_SELECT | RW | 0~3 | 0=자동, 1~3=고정 |
| 200 | PROTOCOL_VERSION | RO | — | 0x0002=v0.2 |

> RPi 측 상수는 `include/guardx_modbus_regs.h`, STM 측은
> `stm32/Led_Matrix_Test/Core/Inc/modbus_slave.h`. **두 파일은 같은 표를 본다 —
> 한쪽을 바꾸면 반드시 다른 쪽도 맞출 것.**

침입자 추적 데이터는 FC10으로 `121~125` 다섯 레지스터를 반드시 한 프레임에
써야 한다. 좌표 레지스터의 FC06 단일 쓰기는 STM32가 예외 `0x03`으로 거부한다.
새 좌표가 10초 동안 도착하지 않으면 STM32는 상태를 0으로 내려 화면에서 두 점을
자동 제거한다. `target off`는 10초를 기다리지 않고 즉시 상태를 내린다.

---

## 5. 알려진 프로토타입 한계

- **프레임 종료 판정**: STM은 USART IDLE 이벤트(DMA)로, RPi는 첫 바이트
  타임아웃 + 바이트 간 5 ms 무수신으로 프레임 끝을 잡는다. 점대점 115200에서
  충분하나, RS-485 다중 슬레이브로 가면 t3.5 정밀 타이밍 재검토가 필요하다.
- **STM 응답이 인터럽트 문맥의 블로킹 송신**: 프레임이 작아(대개 8 bytes) 문제
  없지만, 대량 읽기(최대 255 bytes 응답)를 자주 하면 IDLE 콜백 지연이 커질 수
  있다. 필요 시 송신을 메인 루프로 옮기는 리팩터가 남는다.
- **쓰기 재시도**: 타임아웃 시 최대 `-r`회 재전송한다. 여기 레지스터 쓰기는
  절대값 설정(멱등)이라 중복 적용이 안전하다.
- **추적 대상 수**: 현재 프로토타입은 한 명(A/B 한 쌍)만 표현한다. 다중 대상은
  추후 레지스터 블록 반복 또는 별도 메시지 구조가 필요하다.
