# GuardX DB 적재 가이드 — 하드웨어(RPi A/B/C) 축

> 대상: RPi B `rpib_app` 의 `db_writer.c` 를 구현하는 사람
> 기준 스키마: `database/schema.sql` (v2, 2026-07-20)
> 기준 프로토콜: `GuardX_Protocol_Convention.md` (센서 payload 2026-07-27 개정판)
> 작성: DB 담당 · 최종 갱신 2026-07-27

---

## 0. 30초 요약 — 딱 이것만 이해하면 된다

**센서가 보낸 숫자 하나 = DB 한 줄.** 그게 전부다.

RPi A 가 1초마다 이런 걸 보낸다:

```
"가스 484, 불꽃 1022, 온도 26.6, 습도 60.2, 주변IR 26.6, 대상IR 29.0"
```

숫자가 6개니까 → `device_logs` 테이블에 **6줄**이 쌓인다. 끝.

| 누가 (device_id) | 뭘 (metric) | 얼마 (value) | 언제 (ts) |
|---|---|---|---|
| 가스센서 | gas_raw | 484 | 10:00:00 |
| 불꽃센서 | spark_raw | 1022 | 10:00:00 |
| 온습도센서 | temperature | 26.6 | 10:00:00 |
| 온습도센서 | humidity | 60.2 | 10:00:00 |
| IR센서 | irtemp_ambient | 26.6 | 10:00:00 |
| IR센서 | irtemp_object | 29.0 | 10:00:00 |

그리고 **불이 났다고 판단되면** 그때만 추가로:
- `incidents` 에 사건 1건 만들고 → 사건번호를 받는다
- 그 뒤에 켠 펌프·팬 기록에 **그 사건번호를 달아서** `device_logs` 에 넣는다
- 나중에 "이 펌프는 왜 켜졌지?" → 사건번호 따라가면 원인이 나온다

이게 전부다. 아래는 이걸 실제 SQL 로 어떻게 쓰는지의 세부사항이다.

---

## 1. 먼저 알아야 할 사실

**RPi A 는 DB 를 만지지 않는다.** A 의 일은 MQTT 로 JSON 을 쏘는 것까지고
이미 끝났다. DB 에 넣는 건 전적으로 RPi B `rpib_app` 의 일이다.

```
RPi A  센서 읽기 → JSON 발행          [완료]
   ↓ MQTT (guardx/sensor/rpia)
RPi B  수신 → 파싱 → 판단             [완료]
       DB 기록                        [← 이 문서가 다루는 부분, 미구현]
   ↓ MQTT (guardx/actuator/rpic)
RPi C  액추에이터 동작                 [미착수]
```

**지금까지 하드웨어 데이터가 DB 에 들어간 적이 한 번도 없다.**
`db_writer.c` 가 프로토타입 스텁이라 Postgres 가 아니라 텍스트 파일에 쓴다:

```c
#define DB_WRITER_PATH "rpib_events.jsonl"
fp = fopen(DB_WRITER_PATH, "a");     // ← Postgres 아님
```

현재 guardx DB 에 실제로 쓰고 있는 건 카메라 Poller 하나뿐이다.

---

## 2. 지금 DB 상태를 직접 확인하는 법

이 문서가 맞는지 의심되면 아래를 직접 돌려보면 된다.

```bash
psql -h localhost -U guardx_reader -d guardx
```

```sql
-- 하드웨어 데이터가 정말 0인지
SELECT count(*) FROM device_logs;    -- 0 이어야 함
SELECT count(*) FROM incidents;      -- 0 이어야 함

-- 현재 등록된 디바이스 명단
SELECT device_id, device_role, device_type, rpi_host FROM devices ORDER BY device_id;

-- 테이블에 실제로 있는 컬럼 확인 (이 문서와 대조용)
\d device_logs
\d devices
\d incidents
```

### ⚠ 이 문서가 "현재 DB 그대로"는 아니다

`\d device_logs` 를 돌리면 **`metric` 컬럼이 없을 것이다.** 위 §0 의 표는
`metric` 이 있다고 전제하고 그린 것이다. §4 에서 왜 필요한지, 어떻게 할지 다룬다.

| 이 문서의 내용 | 현재 DB 상태 |
|---|---|
| §3 테이블 4종·컬럼 목록 | ✅ 현재 그대로 |
| §4 지적한 불일치 3건 | ⚠️ 아직 안 고쳐짐 |
| §5 INSERT 문 (metric 사용) | ⚠️ §4 를 먼저 반영해야 동작 |

---

## 3. 관련 테이블 4개 — 뭐하는 애들인가

하드웨어 축이 쓰는 건 이 넷뿐이다. 나머지(`detections`, `zones`,
`congestion_prediction` 등)는 카메라 축 소유라 건드리지 않는다.

| 테이블 | 쉽게 말하면 | 언제 쓰나 |
|---|---|---|
| `devices` | **주소록** — 누가 있는지 | 처음 한 번 등록 (시드) |
| `device_logs` | **일지** — 매 순간 뭘 했는지 | 초당 6줄, 계속 |
| `incidents` | **사건 접수대장** — 큰일 났을 때만 | 화재·가스·버튼 |
| `alerts` | **알림 발송 기록** | 사건 알릴 때 (선택) |

### 실제 컬럼 (현재 DB 그대로)

```sql
devices(device_id PK, device_role, device_type, zone_id FK, rpi_host, status)
device_logs(log_id PK, device_id, incident_id, value, action, triggered_by, ts)
incidents(incident_id PK, zone_id FK, incident_type, source_type, source_id,
          severity, status, snapshot_path, detected_at)
alerts(alert_id PK, incident_id FK, message, broadcast_channel, created_at)
```

`device_logs` 에는 FK 가 없다 (D7 원칙 — 대용량 INSERT 성능 + 보존주기 상이).
`device_id` / `incident_id` 값이 실제로 존재하는지는 **넣는 쪽이 책임진다.**

---

## 4. 지금 안 맞는 것 — 이걸 먼저 고쳐야 §5 가 동작한다

### 4-A. `devices` 명단이 실제 하드웨어와 다르다 ⚠️

| | 현재 DB 시드 | 실제 (프로토콜 규약 기준) |
|---|---|---|
| 센서 | gas, temp_hum, spark, **vibration**, button | gas, temp_hum, spark, **irtemp**, button |
| 액추에이터 | lcd, amp, fan_a, motor_a, fan_b, motor_b | led, water_pump, amp, fan, servo_1, servo_2, led_matrix |

- **센서**: `vibration`(진동)은 실물에 없다. 대신 `irtemp`(MLX90614)가 DB 에 없다.
- **액추에이터**: 확정 목록 7종과 `amp` 하나만 일치. `water_pump` 는 DB 에 아예
  없고, `motor_a/b` 는 `servo_1/2` 의 옛 이름으로 보인다.

→ 이대로면 값을 넣을 때 **매칭할 `device_id` 를 못 찾는다.** 시드 교체가 선행.

교체안:
```sql
DELETE FROM devices;
INSERT INTO devices (device_role, device_type, zone_id, rpi_host) VALUES
    ('sensor',   'gas',        1, 'rpi_a'),
    ('sensor',   'spark',      1, 'rpi_a'),
    ('sensor',   'temp_hum',   1, 'rpi_a'),
    ('sensor',   'irtemp',     1, 'rpi_a'),
    ('button',   'button',     1, 'rpi_a'),
    ('actuator', 'led',        1, 'rpi_c'),
    ('actuator', 'water_pump', 1, 'rpi_c'),
    ('actuator', 'amp',        1, 'rpi_c'),
    ('actuator', 'fan',        1, 'rpi_c'),
    ('actuator', 'servo_1',    1, 'rpi_c'),
    ('actuator', 'servo_2',    1, 'rpi_c'),
    ('actuator', 'led_matrix', 1, 'rpi_c');
```
`device_type` 문자열을 프로토콜의 `command` 값과 **똑같이** 맞추는 게 핵심이다.
그래야 액추에이터 명령을 받았을 때 문자열 그대로 조회해서 device_id 를 찾을 수 있다.

### 4-B. 센서 하나가 값을 여러 개 낸다 → 구분할 방법이 없다 ⚠️

물리 센서는 4개인데 값은 6개다:

| 물리 센서 | 값 2개 |
|---|---|
| SHT30 (`temp_hum`) | `temperature`, `humidity` |
| MLX90614 (`irtemp`) | `irtemp_ambient`, `irtemp_object` |

`device_logs.value` 는 `REAL` 하나뿐이라, `device_id=temp_hum` 인 행이 두 개
들어가면 **어느 게 온도고 어느 게 습도인지 알 수 없다.**

**방법 A (권장) — `metric` 컬럼 추가**
```sql
ALTER TABLE device_logs ADD COLUMN metric TEXT;
```
- `devices` 는 물리 칩 단위 유지 → RPi A 의 `valid` 플래그(칩 단위)와 1:1 대응
- 센서가 값을 더 내도 `devices` 는 안 건드림
- 이 문서 §5 는 이 방식 기준

**방법 B — `devices` 를 값 단위로 쪼개기 (DDL 불필요)**
`temp_hum` 대신 `temperature`, `humidity` 를 각각 별도 행으로 등록.
- 스키마 변경 없이 시드만 고치면 됨 → **당장 시연해야 하면 이쪽이 빠름**
- 단점: `valid` 플래그가 칩 단위(`temphum` 하나)라 매핑이 어긋나고,
  `devices.status`(ONLINE/OFFLINE) 관리가 부자연스러워짐

### 4-C. `seq` 를 넣을 자리가 없다 (권장 사항)

프로토콜 §3 은 `seq`(발행 순번)를 공통 필수 필드로 정의하고, `rpib_app` 구조체도
이미 들고 있는데, `device_logs` 에는 넣을 컬럼이 없다.

특히 **버튼(QoS 2)은 "중복 기록 금지"가 존재 이유**인데, `seq` 를 저장하지 않으면
프로세스 재시작 후 같은 눌림이 다시 와도 중복인지 판단할 수 없다.

```sql
ALTER TABLE device_logs ADD COLUMN seq INT;   -- 권장
```

---

## 5. 실제 적재 방법 — 복붙용 SQL

### 5-1. 시작할 때 한 번: device_id 알아내기

`device_id` 는 `SERIAL`(자동증가)이라 코드에 숫자를 박으면 안 된다.
프로세스 시작 시 1회 조회해서 메모리에 캐시한다.

```sql
SELECT device_id, device_type, zone_id FROM devices;
```

```c
/* device_type 문자열 → device_id 매핑 테이블을 들고 있는다.
 *   "gas" → 1, "spark" → 2, "water_pump" → 7 ...
 * 조회 실패나 항목 누락 = 시드 불일치 → 기동 중단할 것.
 * (device_id 를 모른 채로 적재하면 나중에 어느 센서 값인지 복구 불가) */
```

> `zone_id` 도 같이 읽어둔다. `incidents.zone_id` 가 NOT NULL 이라 사건을
> 만들 때 필요한데, 별도 출처가 없으므로 **그 디바이스가 속한 zone** 을 쓴다.
> 현재는 존이 1개(`zone_id=1`)뿐이라 사실상 고정값이다.

### 5-2. 센서 관측 (1초마다, 상시)

받는 것 (`guardx/sensor/rpia`, QoS 0):
```json
{ "node_id":"rpia", "timestamp":1752300000123, "seq":4821,
  "values": { "gas_raw":484, "spark_raw":1022,
              "temperature":26.6, "humidity":60.2,
              "irtemp_ambient":26.6, "irtemp_object":29.0 },
  "valid":  { "gas":true, "spark":true, "temphum":true, "irtemp":true } }
```

→ `device_logs` 에 최대 6행. 값 하나당 한 번씩 실행:

```sql
INSERT INTO device_logs (device_id, metric, value, seq, incident_id,
                         action, triggered_by, ts)
VALUES ($1, $2, $3, $4, NULL, NULL, NULL,
        to_timestamp($5::bigint / 1000.0));
```

| 파라미터 | 넣을 값 |
|---|---|
| `$1 device_id` | §5-1 캐시에서 조회 (`gas_raw` → gas 의 device_id) |
| `$2 metric` | `'gas_raw'` / `'temperature'` / … (JSON 키 이름 그대로) |
| `$3 value` | 숫자 값 |
| `$4 seq` | payload 의 `seq` |
| `$5` | payload 의 `timestamp` (epoch ms) |

**지켜야 할 규칙**

1. **`ts` 는 반드시 payload 의 `timestamp` 를 쓴다.** `now()` 를 쓰면 네트워크
   지연·처리 지연이 관측 시각으로 오염된다.
2. **`valid` 가 `false` 인 값은 아예 넣지 않는다.** 프로토콜 §4-1 에 따르면
   `false` 여도 필드는 존재하지만 값은 직전값이거나 0 이다. 이걸 넣으면
   나중에 통계·그래프가 오염된다. 스킵했다는 사실만 로그로 남길 것.
   - `valid.temphum` 이 false → `temperature`, `humidity` **둘 다** 스킵
   - `valid.irtemp` 이 false → `irtemp_ambient`, `irtemp_object` **둘 다** 스킵
3. 평상시 관측이므로 `incident_id`, `action`, `triggered_by` 는 전부 NULL.
4. **환산하지 말 것.** RPi A 가 보낸 raw 그대로 저장한다. ppm 환산이나 불꽃
   판정 결과는 `decision.c` 안에서만 쓰고 DB 에는 원본을 남긴다
   (환산식이 바뀌어도 과거 데이터를 다시 계산할 수 있어야 하므로).

> ⚠️ 초당 6행 = 하루 약 52만 행. `device_logs` 는 `guardx_maintain()` 이
> 90일 보존으로 배치 DELETE 한다 (detections 처럼 파티션이 아니다).
> 부담되면 파티션 전환 또는 보존기간 단축을 검토할 것.

### 5-3. 비상 버튼 (QoS 2 — 중복 기록 금지)

받는 것 (`guardx/sensor/rpia/button`):
```json
{ "node_id":"rpia", "timestamp":1752300001000, "seq":4823,
  "event":"emergency_button", "press_count":1 }
```

2단계로 넣는다. 로그를 먼저 쓰고, 그 `log_id` 를 사건의 출처로 건다.

```sql
-- ① 눌림 기록
INSERT INTO device_logs (device_id, metric, value, seq, action, triggered_by, ts)
VALUES ($1, 'press_count', $2, $3, 'press', 'manual',
        to_timestamp($4::bigint / 1000.0))
RETURNING log_id;

-- ② 사건 생성 (source_id = ①에서 받은 log_id)
INSERT INTO incidents (zone_id, incident_type, source_type, source_id,
                       severity, status, detected_at)
VALUES ($1, 'button', 'button', $2, 'high', 'open',
        to_timestamp($3::bigint / 1000.0));
```

**규칙**
- QoS 2 의 존재 이유가 "중복 기록 금지"다. 브로커 재전송으로 같은 메시지가
  두 번 올 수 있으므로, **직전에 처리한 `seq` 를 기억해 같으면 스킵**한다.
- 이 경로는 **로깅 전용**이다. 실제 차단은 RPi A→C 하드웨어 인터락이 이미
  처리한 뒤다. 여기서 액추에이터 명령을 내지 않는다.

### 5-4. 화재/가스 확정 → 사건 만들기

`DECISION_FEED()` 가 `DECISION_EVENT_FIRE` 를 반환한 순간 딱 한 번.

```sql
INSERT INTO incidents (zone_id, incident_type, source_type, source_id,
                       severity, status, detected_at)
VALUES ($1, $2, 'sensor', $3, 'critical', 'open', now())
RETURNING incident_id;          -- ★ 이 번호를 5-5 에서 쓴다
```

| 판단 원인 (`decision_cause_t`) | `incident_type` |
|---|---|
| `DECISION_CAUSE_GAS` | `'gas'` |
| `DECISION_CAUSE_TEMP` | `'fire'` |
| `DECISION_CAUSE_SPARK` | `'fire'` |

- `source_type` 은 `'sensor'` 고정 (버튼발 사건은 §5-3 에서 `'button'`).
- `source_id` 에는 판단을 유발한 `device_logs.log_id` 를 넣는다.
- 상황 해제(`DECISION_EVENT_RECOVER`) 시:
  ```sql
  UPDATE incidents SET status = 'resolved' WHERE incident_id = $1;
  ```

> ⚠️ **함수 시그니처 변경 필요**: 현재 `db_write_transition()` 은 에러코드만
> 반환한다. §5-5 에서 `incident_id` 가 필요하므로 out-param 으로 돌려주도록
> 바꿔야 한다.

### 5-5. 액추에이터 동작 기록

**현재 `db_writer.h` 에 이 함수가 없다 — 신설 대상.**
`main.c` 의 `pub_onoff()` / `pub_set()` 이 MQTT 발행만 하고 DB 에 안 남긴다.

```sql
INSERT INTO device_logs (device_id, incident_id, value, action, triggered_by, ts)
VALUES ($1, $2, $3, $4, 'auto', now());
```

| 발행한 명령 | device_id | action | value |
|---|---|---|---|
| `servo_2` SET 90 | servo_2 | `'SET'` | 90 |
| `water_pump` ON | water_pump | `'ON'` | NULL |
| `amp` OFF | amp | `'OFF'` | NULL |
| `fan` SET 100 | fan | `'SET'` | 100 |

**규칙**
- `incident_id` 는 §5-4 에서 받은 번호. **이게 "왜 이 펌프가 켜졌나"를 잇는
  유일한 연결고리다.** 빼먹으면 나중에 원인 추적이 불가능하다.
- `triggered_by`: 판단 로직 발동은 `'auto'`, 운영자 수동 조작은 `'manual'`.
- `metric` 은 NULL (액추에이터는 측정값이 아니다).
- 이 기록은 **"명령을 보냈다"**이지 **"실제로 동작했다"**가 아니다.
  RPi C 의 ack 토픽은 규약상 자리만 예약된 미확정 상태라 실동작 확인은 불가능하다.

---

## 6. SQL Injection 방지 — 반드시 지킬 것

**문자열을 이어붙여서 SQL 을 만들지 말 것.** 현재 스텁이 `fprintf` 로 JSON 을
직접 조립하는데, 그 습관을 SQL 에 그대로 옮기면 취약점이 된다.

```c
/* ❌ 절대 금지 */
sprintf(sql, "INSERT INTO device_logs VALUES (%d, '%s')", id, metric);
PQexec(conn, sql);

/* ✅ 값은 반드시 파라미터로 분리 */
const char *params[3] = { id_str, metric, value_str };
PQexecParams(conn,
    "INSERT INTO device_logs (device_id, metric, value) VALUES ($1, $2, $3)",
    3, NULL, params, NULL, NULL, 0);
```

같은 저장소의 카메라 Poller(C++)가 이미 이 원칙을 지키고 있으니 참고할 것
(`rpi_b/Poller/task_detections.cpp` 의 `pqxx::params{...}`).

---

## 7. 접속 정보 · 장애 처리

| 용도 | 계정 | 권한 |
|---|---|---|
| `rpib_app` 적재 | `guardx_writer` | INSERT / UPDATE / SELECT |
| 조회 전용 | `guardx_reader` | SELECT (`camera_credentials` 제외) |

```
host=localhost dbname=guardx user=guardx_writer password=<config.env 에서>
```

비밀번호는 소스에 박지 말고 `Config/` 의 env 파일에서 읽는다 (Poller 와 동일 방식).

**DB 가 죽었을 때**: 기록 실패가 판단·명령 발행을 막으면 안 된다
(`db_writer.h` 주석의 "기록 실패 != 판단 중단" 원칙). INSERT 실패는 로그만
남기고 계속 진행하되, 연결이 끊겼으면 재연결을 시도할 것.

---

## 8. 잘 들어갔는지 확인하는 쿼리

```sql
-- 센서가 들어오고 있나 (최근 1분)
SELECT d.device_type, l.metric, l.value, l.ts
FROM device_logs l JOIN devices d USING (device_id)
WHERE l.ts > now() - interval '1 minute'
ORDER BY l.ts DESC LIMIT 20;

-- 센서별 최신값 한 눈에
SELECT DISTINCT ON (device_id, metric)
       device_id, metric, value, ts
FROM device_logs
WHERE metric IS NOT NULL
ORDER BY device_id, metric, ts DESC;

-- 사건과 그에 대한 액추에이터 반응 (인과 추적)
SELECT i.incident_id, i.incident_type, i.detected_at,
       d.device_type, l.action, l.value
FROM incidents i
LEFT JOIN device_logs l ON l.incident_id = i.incident_id
LEFT JOIN devices d ON d.device_id = l.device_id
ORDER BY i.detected_at DESC;
```

---

## 9. 작업 순서

| # | 할 일 | 담당 |
|---|---|---|
| 1 | `devices` 시드 교체 (§4-A) | DB |
| 2 | `device_logs.metric` 컬럼 추가 (§4-B) | DB |
| 3 | `device_logs.seq` 컬럼 추가 (§4-C, 권장) | DB |
| 4 | `sensor_parser.{c,h}` 를 신 스키마로 갱신 | RPi B |
| 5 | `db_writer.c` 를 libpq 로 재구현 (§5·§6) | RPi B |
| 6 | `db_write_actuator()` 신설 + `main.c` 에서 호출 | RPi B |
| 7 | `db_write_transition()` 이 `incident_id` 반환하도록 변경 | RPi B |

1~3 은 스키마 작업이라 선행. 4~7 은 C 코드 작업.

> 4번(`sensor_parser`)이 5번보다 먼저인 이유: 현재 파서는 구 스키마
> (`gas_ppm`/`spark_detected`)를 읽고 있어서 **RPi A 가 지금 보내는 payload 를
> 아예 파싱하지 못한다.** 이게 안 고쳐지면 db_writer 에 값이 도달하지 않는다.
