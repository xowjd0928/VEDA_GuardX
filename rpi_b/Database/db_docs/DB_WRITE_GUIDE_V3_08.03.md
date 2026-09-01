# GuardX DB 가이드 — 하드웨어(RPi A/B/C) 축

> **v3** · 2026-08-03
> 대상: RPi B `rpib_app` 담당 · DB 를 처음 보는 팀원
> 기준: `rpi_b/Database/schema.sql`(카메라 16) + `rpi_b/Database/fire_schema.sql`(화재 9)

**v2에서 뭐가 바뀌었나**: v2는 하드웨어 데이터를 `device_logs` 한 테이블에
`metric` 컬럼을 붙여 넣자는 *계획서*였다. 실제로는 `fire_schema.sql`(07-31)이
정규화된 9개 테이블로 다시 설계됐고 엔진도 그쪽으로 구현됐다. **v2의 §4·§5는
전부 폐기**고, 이 문서가 실제 구현을 기준으로 다시 쓴 것이다.

---

## 0. 30초 요약

**한 사이클 = 봉투 1장 + 값 6장.**

RPi A 가 1초마다 센서 6개 값을 보낸다. DB 에는 이렇게 들어간다:

```
sensor_reading   ← 봉투 1행 (언제, 몇 번째, 종합 위험도 몇 점)
   └ sensor_value  ← 값 6행 (채널별 숫자 + 유효 여부)
```

그리고 **불이 났다고 확정되면** 그때만 추가로:

```
fire_event            ← 사건 1행 (확정/해제, 원인 채널)
   └ fire_event_command  ← 그 사건에서 내보낸 액추에이터 명령들
```

값을 세로로 쌓는 게 핵심이다. 센서가 값을 하나 더 내도 **테이블 구조를 안
바꾼다** — `sensor_channel` 에 행 하나만 추가하면 된다.

---

## 1. 지금 어디까지 됐나

```
RPi A   센서 읽기 → MQTT 발행                    ✅
   ↓ guardx/sensor/rpia
RPi B   수신 → 퍼지 판단 → DB 기록 → 명령 발행    ✅
   ↓ guardx/actuator/rpic
RPi C   액추에이터 동작                          ✅
```

`rpib_app` 이 실제로 쓰는 테이블(코드에서 확인):

| 테이블 | 쓰기 | 읽기 |
|---|---|---|
| `sensor_reading` · `sensor_value` | ✅ 매 사이클 | |
| `fire_event` · `fire_event_command` | ✅ 전이 시 | |
| `button_event` | ✅ 버튼 눌림 | |
| `actuator_command` | | ✅ 명령 카탈로그 |
| `fire_threshold` | | ✅ 기동 시 + 핫리로드 |

`manual_command` 는 아직 아무도 안 쓴다 (VMS 수동 제어가 붙으면 그때).

---

## 2. 전체 스키마 한눈에 — 두 축, 25테이블

DB 는 **하나**(`guardx`)인데 스키마 파일은 둘이다. 서로 FK 를 걸지 않는다.

### 카메라 축 (16) — `schema.sql`

```
cameras ─ camera_credentials
zones ─┬─ zone_thresholds        정원·혼잡 임계   ← VMS SETTINGS 가 고침
       ├─ zone_geometry_history
       ├─ zone_occupancy         분 단위 인원
       ├─ zone_flow
       └─ congestion_prediction  예측
detections   사람 감지 (일 파티션, 최대 테이블)
faces · line_flow · tracks · track_path
devices · device_logs
incidents ─ alerts               혼잡 경보
```

### 화재 축 (9) — `fire_schema.sql`

```
[마스터]  sensor_channel      측정 채널 6종 정의
          actuator_command    제어 명령 카탈로그

[설정]    fire_threshold      판단 임계 (이력 보존형)

[시계열]  sensor_reading ─ sensor_value
          fire_event ─ fire_event_command
          button_event
          manual_command
```

### 두 축이 안 겹치는 이유

화재 축은 `zones`·`devices` 에 FK 를 안 건다. 나중에 구역별 화재 감지가
필요해지면 `sensor_reading` 에 `zone_id` 컬럼을 더해 **논리 연결**로 붙이면
된다 (물리 FK 없이). 지금 묶으면 카메라 스키마가 바뀔 때마다 화재 쪽이 끌려간다.

---

## 3. 화재 축 9테이블 — 뭐하는 애들인가

| 테이블 | 쉽게 말하면 | 빈도 |
|---|---|---|
| `sensor_channel` | **측정 항목 사전** — gas_raw 는 몇 번인가 | 고정 6행 |
| `actuator_command` | **명령 사전** — water_pump 는 몇 번인가 | 고정 6행 |
| `fire_threshold` | **판단 기준** — 몇 점부터 불인가 | 바꿀 때마다 1행 |
| `sensor_reading` | **사이클 봉투** — 언제 받았나, 몇 점인가 | 초당 1행 |
| `sensor_value` | **값** — 채널별 숫자 | 초당 6행 |
| `fire_event` | **사건** — 확정/해제 순간만 | 드물게 |
| `fire_event_command` | **그 사건이 내린 명령들** | 사건당 여러 행 |
| `button_event` | **비상 버튼 눌림 로그** | 드물게 |
| `manual_command` | **VMS 수동 제어 로그** | 미사용 |

### 컬럼

```sql
sensor_channel(channel_id PK, channel_key, device, unit, value_kind, raw_min, raw_max)
actuator_command(command_id PK, command_key, actuator, kind)

sensor_reading(reading_id PK, sensor_seq, composite_score, received_at)
sensor_value(reading_id+channel_id PK, value, is_valid)

fire_event(event_id PK, event_type, cause_channel_id FK, trigger_seq, occurred_at)
fire_event_command(event_id+command_id PK, action, value, published_seq)

button_event(button_event_id PK, sensor_seq, press_count, occurred_at)
manual_command(manual_command_id PK, command_id FK, action, value, source,
               published_seq, issued_at)
```

### 마스터 시드 — 코드가 이 숫자를 참조한다

```
sensor_channel                       actuator_command
 1 gas_raw        MQ-2      adc       1 servo_1     가스밸브     set
 2 spark_raw      TS0226    adc       2 shutter     화재셔터     both
 3 temperature    SHT30     °C        3 fan         배연팬       both
 4 humidity       SHT30     %         4 water_pump  소화펌프     onoff
 5 irtemp_ambient MLX90614  °C        5 sound       경보음       both
 6 irtemp_object  MLX90614  °C        7 led_matrix  LED매트릭스  both
```

`channel_key` 는 RPi A `json_builder.c` 의 values 키와, `command_key` 는
`guardx_protocol.h` 의 `GUARDX_CMD_*` 문자열과 **1:1로 같아야 한다.**
문자열이 어긋나면 조회가 조용히 0행을 돌려주고 그때부터 기록이 사라진다.

---

## 4. 적재 방법 — 복붙용 SQL

### 4-1. 시작할 때 한 번: 마스터를 메모리에 캐시

`channel_id`·`command_id` 는 코드에 숫자를 박지 말고 기동 시 1회 조회한다.

```sql
SELECT channel_id, channel_key FROM sensor_channel;
SELECT command_id, command_key FROM actuator_command;
```

조회 실패나 항목 누락 = 시드 불일치 → **기동을 중단할 것.** 매핑을 모른 채
적재하면 나중에 어느 센서 값인지 복구할 수 없다.

### 4-2. 센서 관측 (1초마다)

받는 것 (`guardx/sensor/rpia`, QoS 0):

```json
{ "node_id":"rpia", "timestamp":1752300000123, "seq":4821,
  "values": { "gas_raw":484, "spark_raw":1022, "temperature":26.6,
              "humidity":60.2, "irtemp_ambient":26.6, "irtemp_object":29.0 },
  "valid":  { "gas":true, "spark":true, "temphum":true, "irtemp":true } }
```

**한 트랜잭션에서** 봉투 1행 + 값 6행:

```sql
-- ① 봉투
INSERT INTO sensor_reading (sensor_seq, composite_score, received_at)
VALUES ($1, $2, to_timestamp($3::bigint / 1000.0))
RETURNING reading_id;

-- ② 값 (채널마다 반복)
INSERT INTO sensor_value (reading_id, channel_id, value, is_valid)
VALUES ($1, $2, $3, $4);
```

**지켜야 할 규칙**

1. **`received_at` 은 payload 의 `timestamp` 를 쓴다.** `now()` 를 쓰면 네트워크
   지연이 관측 시각으로 오염된다.
2. **`is_valid=false` 인 값도 넣되 값은 `NULL`.** v2에서는 "아예 스킵"이었는데,
   그러면 "센서가 죽어 있었다"와 "그 사이클을 놓쳤다"를 구분할 수 없다.
   `valid.temphum` 이 false → `temperature`·`humidity` **둘 다** false.
3. **환산하지 말 것.** raw 그대로 저장한다. 퍼지 환산은 `decision.c` 안에서만
   쓴다 — 환산식이 바뀌어도 과거 데이터를 다시 계산할 수 있어야 한다.
4. `composite_score` 는 그 사이클의 종합 위험도(0~100). **판단을 안 한
   사이클은 NULL** 이다. "0점"과 "계산 안 함"은 전혀 다른 의미다.

> 초당 7행 = 하루 약 60만 행. 파티션이 아니라 통짜 테이블이라, 보존 정책을
> 나중에 반드시 정해야 한다.

### 4-3. 화재 확정 → 사건 + 명령

전이 순간에만 1행. 확정과 해제 둘 다 남긴다.

```sql
-- ① 사건
INSERT INTO fire_event (event_type, cause_channel_id, trigger_seq, occurred_at)
VALUES ($1, $2, $3, now())
RETURNING event_id;          -- ★ 이 번호를 ②에서 쓴다

-- ② 그 사건이 내보낸 명령들 (명령마다 반복)
INSERT INTO fire_event_command (event_id, command_id, action, value, published_seq)
VALUES ($1, $2, $3, $4, $5);
```

| 항목 | 값 |
|---|---|
| `event_type` | `'fire_confirmed'` / `'recovered'` |
| `cause_channel_id` | 확정을 유발한 채널. **해제는 원인이 없으니 NULL** |
| `action` | `'ON'` `'OFF'` `'SET'` / 셔터만 `'OPEN'` `'CLOSE'` `'STOP'` |
| `value` | SET 일 때 각도·듀티, 아니면 NULL |

**`event_id` 가 "왜 이 펌프가 켜졌나"를 잇는 유일한 연결고리다.** 빼먹으면
사후 추적이 불가능하다.

### 4-4. 비상 버튼 (QoS 2 — 중복 기록 금지)

```sql
INSERT INTO button_event (sensor_seq, press_count, occurred_at)
VALUES ($1, $2, to_timestamp($3::bigint / 1000.0));
```

- QoS 2 의 존재 이유가 중복 방지다. **직전 처리한 `sensor_seq` 를 기억해
  같으면 스킵**한다. `sensor_seq` 는 RPi A 재시작 시 리셋되므로 전역 유일이
  아니다 — PK/FK 로 쓰지 말고 중복 판별 단서로만 쓴다.
- 이 경로는 **로깅 전용**이다. 실제 차단은 RPi A→C 하드웨어 인터락이 이미
  처리한 뒤다. 여기서 액추에이터 명령을 내지 않는다.

---

## 5. 판단 임계 (`fire_threshold`) — 재시작 없이 바꾸기

엔진은 `is_active = true` **한 행만** 읽어 런타임 변수로 적재한다.

### 이력 보존형이다

값을 바꿀 때 UPDATE 하지 않고 **새 행을 넣고 활성 표시를 넘긴다.**

```sql
BEGIN;
UPDATE fire_threshold SET is_active = FALSE WHERE is_active;
INSERT INTO fire_threshold (…, is_active) VALUES (…, TRUE);
COMMIT;
```

⚠ **두 문장이 한 트랜잭션이어야 한다.** `uq_fire_threshold_active` 부분 유니크
인덱스 때문에, 비활성화 없이 INSERT 하면 유니크 위반으로 죽는다.

덕분에 "언제 누가 무엇으로 바꿨나"가 `updated_at`·`updated_by` 와 함께 남는다.
튜닝 이력이 곧 감사 기록이다.

### 바꾼 뒤에는 반드시 신호를 쏜다

```bash
mosquitto_pub -h localhost -q 1 -t guardx/config/rpib -m reload
```

**이게 없으면 DB 만 바뀌고 엔진은 옛 임계로 계속 판단한다.** 겉보기엔 저장이
된 것 같은데 동작이 안 바뀌는, 제일 찾기 어려운 상태가 된다. 엔진은 페이로드를
보지 않고 "오면 다시 읽는다"만 한다.

### 손으로 하지 말고 VMS 를 쓸 것

VMS **SETTINGS** 화면에 편집 폼이 있다 (2026-08-03). 위의 트랜잭션과 리로드
신호를 알아서 처리하고, 값 범위·가중치 합(1.00)도 보내기 전에 검사한다.

```
[VMS] --guardx/db/rpib/cmd/set_fire_threshold--> [guardx_mqttd] --INSERT--> [DB]
                                                      └--guardx/config/rpib--> [엔진]
```

DBeaver 로 직접 고치면 `is_active` 플립과 리로드 신호를 사람이 챙겨야 한다.
규약은 `vms/docs/DB_LINK_AND_MQTT_MIGRATION.md`.

---

## 6. 접속 · 권한

### 계정

| 용도 | 계정 | 권한 |
|---|---|---|
| 적재 | `guardx_writer` | INSERT / UPDATE / SELECT |
| 조회 | `guardx_reader` | SELECT |
| 로컬 운영 | `juan` | writer 와 동급 (`schema.sql` 에 전용 블록) |

```
host=localhost dbname=guardx user=guardx_writer password=<config.env 에서>
```

비밀번호를 소스에 박지 말 것. `config.env` 는 `.gitignore` 대상이고 권한은 600.

### ⚠ 화재 테이블 권한 함정 (실사고 2026-08-03)

`schema.sql` 에는 `juan` 계정에 권한을 주는 블록이 있는데 **`fire_schema.sql`
에는 없다.** 게다가 그 블록은 `ALL TABLES IN SCHEMA public` 이라 **실행 시점에
있던 테이블에만** 적용된다 — 나중에 만든 화재 테이블에는 소급되지 않는다.

증상: 카메라 테이블은 멀쩡한데 화재 테이블만 `permission denied for table
fire_threshold` (SQLSTATE 42501).

```bash
sudo -u postgres psql -d guardx -c \
  "GRANT INSERT, UPDATE, SELECT ON ALL TABLES IN SCHEMA public TO juan;
   GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO juan;"
```

**시퀀스 권한을 같이 줘야 한다.** SERIAL 컬럼에 INSERT 하려면 테이블 권한과
별개로 `nextval()` 권한이 필요해서, 빠뜨리면 **조회는 되는데 INSERT 만**
`permission denied for sequence` 로 죽는다.

접속 계정이 의심되면 추측하지 말고 확인한다:

```sql
SELECT current_user;
SELECT usename, client_addr FROM pg_stat_activity WHERE datname='guardx';
```

### DB 가 죽었을 때

**기록 실패가 판단·명령 발행을 막으면 안 된다.** INSERT 실패는 로그만 남기고
계속 진행하되, 연결이 끊겼으면 재연결을 시도한다.

---

## 7. SQL Injection — 문자열을 이어붙이지 말 것

```c
/* ❌ 절대 금지 */
sprintf(sql, "INSERT INTO sensor_value VALUES (%d, %f)", ch, val);
PQexec(conn, sql);

/* ✅ 값은 반드시 파라미터로 */
const char *params[3] = { rid, chid, valstr };
PQexecParams(conn,
    "INSERT INTO sensor_value (reading_id, channel_id, value) VALUES ($1,$2,$3)",
    3, NULL, params, NULL, NULL, 0);
```

같은 저장소의 카메라 폴러(C++)가 이 원칙을 지키고 있다 —
`rpi_b/src/Poller/task_detections.cpp` 의 `pqxx::params{…}` 참조.

---

## 8. 잘 들어갔는지 확인하는 쿼리

```sql
-- 센서가 들어오고 있나 (최근 1분)
SELECT r.received_at, c.channel_key, v.value, v.is_valid
FROM sensor_reading r
JOIN sensor_value v USING (reading_id)
JOIN sensor_channel c USING (channel_id)
WHERE r.received_at > now() - interval '1 minute'
ORDER BY r.received_at DESC, c.channel_id
LIMIT 20;

-- 위험도 추이 (판단이 도는지)
SELECT received_at, sensor_seq, composite_score
FROM sensor_reading
WHERE received_at > now() - interval '10 minutes'
ORDER BY received_at DESC LIMIT 20;

-- 사건과 그 반응 (인과 추적)
SELECT e.event_id, e.event_type, c.channel_key AS cause, e.occurred_at,
       a.command_key, ec.action, ec.value
FROM fire_event e
LEFT JOIN sensor_channel c ON c.channel_id = e.cause_channel_id
LEFT JOIN fire_event_command ec USING (event_id)
LEFT JOIN actuator_command a USING (command_id)
ORDER BY e.occurred_at DESC;

-- 현재 판단 기준 + 바뀐 이력
SELECT threshold_id, fire_score_threshold, n_confirm, n_recover,
       is_active, updated_at, updated_by
FROM fire_threshold ORDER BY threshold_id DESC;

-- 안 쓰는 테이블 찾기 (정리 판단 근거)
SELECT relname, n_live_tup, seq_scan + idx_scan AS reads, n_tup_ins AS inserts
FROM pg_stat_user_tables ORDER BY inserts DESC;
```

---

## 9. 파일 지도 · 알려진 불일치

### 실제 DB = 파일 여러 개의 합

```
rpi_b/Database/schema.sql              카메라 16테이블 (최초 생성본)
  + migration_v15_feeds.sql              detections.parent_id
  + migration_prediction_feedback4.sql   congestion_prediction p10·p90·…
  + migration_track_handover_fields.sql  tracks·detections 동선 컬럼
  + migration_zones_multich/unique.sql   zones 제약
rpi_b/Database/fire_schema.sql         화재 9테이블 (별도 실행)
  + 손으로 준 juan GRANT (§6)
  + migration_season_threshold.sql       season_threshold (2026-08-04)
```

**변경 이력은 `database/SCHEMA_CHANGELOG.md` 에 날짜와 함께 쌓는다.** 2026-08-04
이후 새 마이그레이션은 그 문서에 항목을 추가하는 것까지가 작업이다 — 총정리 때
그 문서를 위에서부터 훑으면 무엇을 본문에 흡수해야 하는지 알 수 있다.

**어느 한 파일도 현재 DB 를 통째로 설명하지 못한다.** 구조를 알아야 하면
파일이 아니라 DB 에 직접 물어보는 게 빠르다:

```bash
sudo -u postgres psql -d guardx -c "\dt"
sudo -u postgres psql -d guardx -c "\d fire_threshold"
```

### ⚠ 알려진 불일치 3건

| # | 내용 | 영향 |
|---|---|---|
| 1 | **`database/schema.sql` 이 낡음** — `rpi_b/Database/schema.sql` 에 있는 `faces`·`line_flow` 가 없다 | 이 사본으로 DB 를 만들면 카메라 축 2테이블이 빠진다. `rpi_b/Database/` 쪽이 기준이다 |
| 2 | **`fire_schema.sql` 에 juan 블록 없음** | DB 재구축 때마다 §6 권한 사고가 재발한다 |
| 3 | **`fire_schema.sql` 은 `DROP TABLE` 로 시작** | 운영 DB 에서 재실행하면 센서·사건 이력이 전부 사라진다. 임계를 고치려고 이 파일을 다시 돌리지 말 것 — 그건 VMS SETTINGS 의 일이다 |

### 정리 계획

테이블 정리(안 쓰는 것 삭제 + `init.sql`·`schema.sql` 재작성)는 **프로그램이
완성된 뒤**로 미룬다. 지금 손대면 통합 중에 깨진다.

대신 근거는 지금부터 모은다 — §8 마지막 쿼리로 접근 통계를 주기적으로 남기면,
나중에 "몇 주 동안 한 번도 안 건드린 테이블"을 감이 아니라 숫자로 고를 수 있다.
삭제할 때도 `DROP` 대신 `ALTER TABLE … RENAME TO zz_unused_…` 로 며칠 관찰한
뒤 지우는 편이 안전하다.
