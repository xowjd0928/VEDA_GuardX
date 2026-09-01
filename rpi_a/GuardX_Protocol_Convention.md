# GuardX 통신 프로토콜 규약 (MQTT)

> 상태: 초안. `!!! 미확정 !!!` 표시된 항목은 팀 합의 전까지 잠정치.
> 관련 코드:
> - 토픽/QoS/액추에이터 command: `common/include/guardx_protocol.h`
> - **센서 payload 필드명의 실제 출처**: `rpia_app/app/src/json_builder.c`
>   (`CREATE_JSON` / `CREATE_BUTTON_JSON`). 4-1·4-2절은 이 코드와 반드시 동기화.

---

## 0. 원칙

1. 디바이스 인스턴스가 항상 1개인 노드는 토픽에 인스턴스 번호를 넣지 않는다.
2. QoS는 "정확히 1회 전달이 필요한가(로그/이벤트)" vs "유실 허용 또는 중복 허용되는 멱등 동작인가"로 결정한다.
   - 유실 허용(다음 주기에 새 값이 옴) → QoS 0
   - 멱등 동작(중복 수신돼도 결과 동일) → QoS 1
   - 정확히 1회만 기록/발동돼야 함(로그 중복 금지 등) → QoS 2
3. 모든 payload는 JSON이며 최소 공통 필드(`node_id`, `timestamp`, `seq`)를 포함한다.
4. 토픽 구조가 바뀌면 `guardx_protocol.h` 한 곳만 수정하고, 이 문서를 함께 갱신한다.

---

## 1. 노드 ID

| 노드 | ID | 역할 |
|---|---|---|
| RPi A | `rpia` | 센서 퍼블리셔 |
| RPi B | `rpib` | 브로커 + 판단 로직 + DB 라이터 |
| RPi C | `rpic` | 액추에이터 서브스크라이버 |

MQTT client id는 노드 ID와 동일하게 사용한다 (mTLS 적용 시 인증서 CN과도 일치).

---

## 2. 토픽 / QoS 일람

| 토픽 | 방향 | QoS | 상태 |
|---|---|---|---|
| `guardx/sensor/{node_id}` | RPi A → RPi B | 0 | 확정 (구현·실기검증 완료). payload 스키마는 센서 교체로 개정됨 — 4-1절 참조 |
| `guardx/sensor/{node_id}/button` | RPi A → RPi B | 2 | 확정 (구현·실기검증 완료) |
| `guardx/actuator/{node_id}` | RPi B → RPi C | 1 | 확정 (헤더 정의 완료, 실기 미검증) |
| `guardx/actuator/{node_id}/ack` | RPi C → RPi B | 1 | 잠정 (사용 여부 미정, 자리만 예약) |
| `guardx/camera/{camera_id}` | 카메라 → RPi B | `!!! 미정 !!!` | `!!! 미확정 !!!` — 카메라 담당자 확인 대기 |

### 2-1. QoS 결정 근거 상세

- **센서(`sensor`, QoS 0)**: 1초 주기 재발행되므로 한 번 유실돼도 다음 값이 바로 온다.
- **비상 버튼 로그(`sensor/.../button`, QoS 2)**: 이 토픽은 로깅 전용이다. 실제 제어(RPi A → RPi C)는 MQTT를 타지 않고 하드웨어 GPIO/릴레이 인터락으로 즉시 처리되므로, 이 토픽의 QoS는 순수하게 "DB에 눌림 이벤트가 중복 기록되지 않아야 한다"는 요구에서 나온 것이다.
- **액추에이터 제어(`actuator`, QoS 1)**: ON/OFF 같은 명령은 멱등하다. 같은 명령이 두 번 도착해도 최종 상태는 동일하므로 중복은 허용하되, 유실은 안 된다(꺼야 하는데 못 끄면 안전 문제). QoS 2의 핸드셰이크 오버헤드를 낼 이유가 없다.

---

## 3. 공통 payload envelope

모든 발행 메시지 공통 필드:

```json
{
  "node_id": "rpia",
  "timestamp": 1752300000123,
  "seq": 42
}
```

| 필드 | 타입 | 설명 |
|---|---|---|
| `node_id` | string | 1번 표의 노드 ID |
| `timestamp` | integer | epoch 밀리초. 노드 간 시각 동기화(NTP) 전제 |
| `seq` | integer | 발행 프로세스 기준 단조 증가. 프로세스 재시작 시 0으로 리셋 허용 |

---

## 4. 메시지별 스키마

### 4-1. 센서 데이터 (`guardx/sensor/{node_id}`, QoS 0)

```json
{
  "node_id": "rpia",
  "timestamp": 1752300000123,
  "seq": 4821,
  "values": {
    "gas_raw": 312,
    "temperature": 23.5,
    "humidity": 60.2,
    "spark_raw": 78,
    "irtemp_ambient": 23.1,
    "irtemp_object": 31.4
  },
  "valid": {
    "gas": true,
    "temphum": true,
    "spark": true,
    "irtemp": true
  }
}
```

#### `values` 필드 정의 (RPi A 기준)

| 필드 | 타입 | 범위/단위 | 출처 | 비고 |
|---|---|---|---|---|
| `gas_raw` | integer | 0~1023 (10bit raw) | MQ-2 AO → MCP3008 **CH1** (`/dev/rpia_adc`) | **ppm 아님.** 환산은 RPi B 몫 |
| `spark_raw` | integer | 0~1023 (10bit raw) | TS0226 AO → MCP3008 **CH0** (`/dev/rpia_adc`) | **0/1 아님.** 임계값 판단은 RPi B 몫 |
| `temperature` | number | °C, 소수 1자리 | SHT30 (`/dev/rpia_temphum`) | 드라이버 x10 정수 → App에서 실수 변환 |
| `humidity` | number | %RH, 소수 1자리 | SHT30 (`/dev/rpia_temphum`) | 상동 |
| `irtemp_ambient` | number | °C, 소수 1자리 | MLX90614 `Tamb`(reg 0x06) (`/dev/rpia_irtemp`) | 센서 **주변** 온도 |
| `irtemp_object` | number | °C, 소수 1자리 | MLX90614 `Tobj1`(reg 0x07) (`/dev/rpia_irtemp`) | 비접촉 **대상** 온도 |

#### `valid` 필드

| 키 | 커버하는 `values` 필드 |
|---|---|
| `gas` | `gas_raw` |
| `spark` | `spark_raw` |
| `temphum` | `temperature`, `humidity` |
| `irtemp` | `irtemp_ambient`, `irtemp_object` |

- `valid`는 해당 사이클에서 읽기에 성공했는지 여부. `false`면 대응 `values` 필드는 신뢰할 수 없다(직전 값이거나 0/기본값일 수 있음). **필드 자체는 항상 존재한다** — `false`여도 키가 빠지지는 않는다.
- `gas`/`spark`는 물리적으로 같은 칩(MCP3008)에서 오지만 채널이 달라 `valid`는 별도로 관리한다.
- 온도/습도/IR온도는 드라이버가 x10 정수로 넘기고 App(`json_builder.c`)에서만 실수로 변환한다 (드라이버 컨벤션 3-1 참조).

#### ★ raw 값 정책 (RPi B 담당자 필독)

RPi A는 **환산·판정을 일절 하지 않는다.** ADC에서 읽은 10bit raw를 그대로 싣는다.

- 이유: MQ-2의 ppm 환산은 예열 시간·`R0` 캘리브레이션·부하저항에 의존해서 센서 개체마다 달라지고, 이 보정 파라미터를 들고 있어야 할 곳은 판단 로직이 있는 RPi B다. 불꽃 임계값도 마찬가지로 설치 환경(주변광)마다 튜닝 대상이라 발행 노드에 박아 넣으면 재배포 없이는 못 고친다.
- 따라서 **`ppm` 환산, 불꽃 감지 여부(bool) 판정, 히스테리시스/디바운스는 전부 RPi B 책임**이다.

`!!! 미확정 !!!` MQ-2 `R0` 캘리브레이션 절차와 ppm 환산식, 불꽃 감지 임계값(raw 기준) — RPi B 착수 시 확정.

#### 스키마 변경 이력 (구 필드 → 신 필드)

배선이 DO(디지털 출력) 기반에서 **AO + MCP3008(SPI ADC)** 기반으로 바뀌고 MLX90614(IR온도)가 추가되면서 아래와 같이 변경됐다.

| 구 필드 (폐기) | 신 필드 | 변경 사유 |
|---|---|---|
| `gas_ppm` (int, ppm) | `gas_raw` (int, 0~1023) | ppm 환산 책임을 RPi B로 이관 |
| `spark_detected` (int, 0/1) | `spark_raw` (int, 0~1023) | DO(0/1) → AO(아날로그). 임계값 판정을 RPi B로 이관 |
| — | `irtemp_ambient`, `irtemp_object` | MLX90614 신규 추가. Tamb/Tobj 2값 |
| `valid` 3키 | `valid` 4키 (`irtemp` 추가) | 상동 |

> ⚠️ **RPi B 미반영 상태**: `rpi_b/rpib_app/app/src/sensor_parser.c`는 아직 구 필드(`gas_ppm`/`spark_detected`)를 파싱하고 있어 현재 RPi A 발행 payload를 못 읽는다. RPi B 착수 시 `sensor_parser.{c,h}`·`decision.c`·`db_writer.c`·`test/02_fake_rpia.sh`를 신 스키마로 일괄 갱신할 것.

### 4-2. 비상 버튼 로그 (`guardx/sensor/{node_id}/button`, QoS 2)

```json
{
  "node_id": "rpia",
  "timestamp": 1752300001000,
  "seq": 4823,
  "event": "emergency_button",
  "press_count": 1
}
```

- `press_count`: 마지막 발행 이후 발생한 눌림 횟수 (드라이버가 카운터를 들고 있다가 read 시 리셋).
- 이 메시지는 **로깅용**이며, 실제 제어는 이 토픽과 무관하게 하드웨어로 이미 처리된 뒤다.

### 4-3. 액추에이터 제어 명령 (`guardx/actuator/{node_id}`, QoS 1)

**이진 제어** (LED, 워터펌프, AMP 전원/뮤트):
```json
{
  "node_id": "rpic",
  "timestamp": 1752300000123,
  "seq": 42,
  "command": "water_pump",
  "action": "ON"
}
```

**숫자값 제어** (서보 각도, 팬 속도):
```json
{
  "node_id": "rpic",
  "timestamp": 1752300000123,
  "seq": 43,
  "command": "servo_1",
  "action": "SET",
  "value": 90
}
```

| 필드 | 타입 | 설명 |
|---|---|---|
| `command` | string | 대상 액추에이터 식별자 (아래 표) |
| `action` | string | `"ON"` \| `"OFF"` \| `"SET"`(value 필드 동반) |
| `value` | number | `action:"SET"`일 때만 사용, 의미는 command마다 다름 |

#### 확정된 액추에이터 목록

| `command` | 제어 방식 | action/value | 비고 |
|---|---|---|---|
| `led` | GPIO | ON/OFF | |
| `water_pump` | GPIO (relay) | ON/OFF | |
| `amp` | GPIO (전원/뮤트 핀) | ON/OFF | 오디오 신호 자체는 이 프로토콜 범위 밖 (I2S/ALSA 별도 경로) |
| `fan` | GPIO/PWM | ON/OFF 또는 SET(value=0~100, 듀티 %) | |
| `servo_1` | PWM | SET(value=0~180, 각도) | |
| `servo_2` | PWM | SET(value=0~180, 각도) | |

`led_matrix` 액추에이터 명령은 삭제했다 — RPi C↔STM32 브릿지가 구현되지 않아 받아도 무시만 했다. LED 매트릭스는 액추에이터가 아니라 **표시 장치**로만 쓴다(`guardx/display/rpic/{zones/N,fire,track}` → RPi C `matrix_link` → Modbus RTU → STM32).

`!!! 미확정 !!!` `fan`의 SET 세부 범위(팬 최소 구동 듀티), 서보 각도 물리적 제한(기구 구조상 0~180 전체를 못 쓸 수도 있음).

### 4-4. 액추에이터 ACK (`guardx/actuator/{node_id}/ack`, QoS 1) — 미사용, 자리만 예약

```json
{
  "node_id": "rpic",
  "timestamp": 1752300000456,
  "seq": 7,
  "command": "relay_1",
  "result": "ok"
}
```

`!!! 미확정 !!!` 실제로 필요한지, 필요하다면 `result` 값 목록(`ok`/`fail`/...) 논의 필요.

### 4-5. 카메라 메타데이터 (`guardx/camera/{camera_id}`) — 전체 미확정

`!!! 미확정 !!!` DETECTIONS 테이블 연계 예정. 카메라 담당자 확인 후 이 섹션 작성.

---

## 5. 변경 이력

| 날짜 | 내용 |
|---|---|
| (오늘) | 최초 작성. sensor/button 토픽은 RPi A 구현·실기검증 완료 기준으로 확정. actuator 토픽 QoS 1로 신규 확정. camera는 미확정 상태 명시 |
| (오늘, 추가) | 액추에이터 목록 확정(LED/워터펌프/AMP/DC팬/서보×2/LED Matrix). 이진(ON/OFF) 외 숫자값(SET+value) 스키마 추가. LED Matrix는 RPi C 직접 GPIO 제어가 아니라 기존 UART Protocol v1.0을 통한 STM32 브릿지 구조로 확정 |
| 2026-07-27 | **센서 payload 스키마 개정 (4-1절).** 배선이 DO → AO+MCP3008(SPI ADC)로 바뀌고 MLX90614(IR온도)가 추가된 것을 반영: `gas_ppm`→`gas_raw`(0~1023), `spark_detected`(0/1)→`spark_raw`(0~1023), `irtemp_ambient`/`irtemp_object` 신설, `valid`에 `irtemp` 추가. ppm 환산·불꽃 임계값 판정 책임을 RPi A → **RPi B로 이관** 명문화. RPi A 실기검증 완료(`rpia_app/VERIFY.md`) 기준이며, RPi B 파서는 아직 구 스키마라 갱신 필요 |
