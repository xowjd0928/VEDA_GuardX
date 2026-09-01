# GuardX DB 스키마 변경 이력

> **이 문서의 목적**
> 실제 운영 DB는 스키마 파일 하나가 아니라 **최초 생성본 + 마이그레이션 여러 개의
> 합**이다 (`DB_WRITE_GUIDE_V3_08.03.md` §9). 어느 파일도 현재 DB를 통째로
> 설명하지 못한다.
>
> **DB를 총정리할 때(= `init.sql` · `schema.sql` 재작성) 이 문서를 위에서부터
> 훑으면 무엇을 본문에 흡수해야 하는지 알 수 있다.** 총정리는 프로그램이 완성된
> 뒤로 미뤄져 있으므로, 그때까지 변경은 전부 여기에 날짜와 함께 쌓는다.
>
> 새 마이그레이션을 만들면 **여기에 한 항목을 추가하는 것까지가 작업이다.**
> 빠뜨리면 다음 재구축에서 조용히 누락되고, 증상은 대개 "기능이 아무 반응 없음"이라
> 원인 찾기가 오래 걸린다.

---

## 적용 현황 요약

| 마이그레이션 | 날짜 | RPi B 운영 DB | 총정리 시 흡수 대상 |
|---|---|---|---|
| `migration_v15_feeds.sql` | ~2026-07 | ✅ | `schema.sql` |
| `migration_prediction_feedback4.sql` | ~2026-07 | ✅ | `schema.sql` |
| `migration_track_handover_fields.sql` | ~2026-07 | ✅ | `schema.sql` |
| `migration_zones_multich.sql` / `migration_zones_unique.sql` | ~2026-07 | ✅ | `schema.sql` |
| 손으로 준 `juan` GRANT | 2026-08-03 | ✅ | `fire_schema.sql` (블록 자체가 없음) |
| `migration_season_threshold.sql` | **2026-08-04** | ✅ 2026-08-04 적용 | `fire_schema.sql` |
| `history/fire/20260813_02_fan_auto_command.sql` | **2026-08-13** | ⬜ 미적용 | `fire_schema.sql` (반영 완료) |
| `history/cleanup/20260813_03_drop_led_matrix_cmd.sql` | **2026-08-13** | ⬜ 미적용 | `fire_schema.sql` (반영 완료) |

> 위쪽 5개는 `DB_WRITE_GUIDE_V3_08.03.md` §9에서 옮겨 온 기존 항목이다.
> 상세 경위는 그쪽을 볼 것. 아래 상세 항목은 이 문서를 만든 시점(2026-08-04)
> 이후 변경분만 적는다.

---

## 2026-08-04 — `season_threshold` 추가 (계절 임계 프리셋)

**파일**: `rpi_b/Database/migration_season_threshold.sql`
**티켓**: VEDA-158 후속
**작성**: 이병규

### 무엇을

화재 판단 임계(`fire_threshold`)를 계절별로 미리 정해두고, VMS SETTINGS 화면에서
버튼 한 번으로 폼에 채울 수 있게 하는 **읽기 전용 카탈로그 테이블**을 추가했다.

- 신규 테이블 `season_threshold` 1개 (고정 5행: `default` / `spring` / `summer` / `autumn` / `winter`)
- 컬럼은 `fire_threshold`의 값 컬럼 22개 + `season_key` / `season_name` / `sort_order` / `updated_at` / `updated_by`
- CHECK 제약은 `fire_threshold`와 동일하게 복제 (가중치 합 1.0 포함)

### 기존 테이블 영향

**없다.** `ALTER` 없음, FK 없음, 데이터 변화 없음, 폴러·mqttd 재시작 불필요.
롤백은 `DROP TABLE season_threshold;` 한 줄이고 다른 곳에 영향이 없다.

### 왜 `fire_threshold`에 행을 더하지 않았나

두 테이블은 수명주기가 정반대다.

| | `fire_threshold` | `season_threshold` |
|---|---|---|
| 쓰기 | INSERT only (append-only) | UPDATE (고정 5행) |
| 목적 | 이력 · 사후 감사 | 카탈로그 · 템플릿 |
| 활성 개념 | `is_active` 1행 | 없음 |

한 테이블에 섞으면 ① 감사 조회에 "적용된 적 없는 행"이 끼어들고 ② append-only
규칙이 깨지며 ③ 모든 이력 쿼리에 프리셋 제외 필터가 필요해진다.

### 권한

앱 계정은 **SELECT만**. 프리셋 수정은 `guardx_admin`이 `psql`로만 한다 —
운영 중 실수로 덮일 경로 자체가 없다.

```sql
GRANT SELECT ON season_threshold TO guardx_reader, guardx_writer;
-- juan 계정이 있으면 같이 (존재 확인 후)
```

`season_key`가 TEXT PK라 **SERIAL이 없어 시퀀스 권한이 불필요**하다.
2026-08-03 권한 사고의 후반부가 시퀀스 누락이었는데, 이 테이블은 그 함정을 비껴간다.

### ⚠ 시드 값은 아직 실측이 아니다

**2026-08-04 현재 5행 전부 `fire_threshold` 시드(threshold_id 1)와 같은 값이다.**
프로젝트가 7월 시작이라 여름 데이터밖에 없어서, 버튼과 배선을 먼저 검증하려고
값은 동일하게 두고 넣었다. `updated_by`에 `계절값 미정, 현재 기본값과 동일`로
표시해 두었으므로 아래 쿼리로 미결 상태를 확인할 수 있다.

```sql
SELECT season_key, updated_by FROM season_threshold
 WHERE updated_by LIKE '%미정%' ORDER BY sort_order;
```

**계절차를 둘 컬럼은 6개뿐이다** — 대기 온습도와 표면온도의 baseline만 계절을
탄다. 가중치·확정 사이클은 센서 신뢰도와 대응 정책의 문제라 계절과 무관하고,
가스/불꽃 ADC는 2026-07-31 실측 반영분이라 근거 없이 건드리면 안 된다.

```
temp_min_c, temp_max_c
humi_safe_percent, humi_danger_percent      ← 계절 영향 가장 큼
irtemp_min_c, irtemp_max_c
```

특히 습도는 내림차순 퍼지화라(`fire_schema.sql` 참조) `safe` 이상이면 위험도 0인데,
겨울 실내 습도는 20~30%다. 현재 `safe=50`이면 **겨울 내내 습도 채널이 위험도를
상시 가산**한다. 겨울 프리셋에서 이 값을 내리는 것이 이 기능의 핵심 목적이다.

값을 정할 때 쓸 쿼리 — 평상시(화재 없음) 분포를 보고 잡는다:

```sql
SELECT sc.channel_key, sc.unit,
       round(percentile_cont(0.05) WITHIN GROUP (ORDER BY sv.value)::numeric, 1) AS p05,
       round(percentile_cont(0.50) WITHIN GROUP (ORDER BY sv.value)::numeric, 1) AS p50,
       round(percentile_cont(0.95) WITHIN GROUP (ORDER BY sv.value)::numeric, 1) AS p95,
       count(*) AS n
FROM sensor_value   sv
JOIN sensor_reading sr USING (reading_id)
JOIN sensor_channel sc USING (channel_id)
WHERE sv.is_valid
  AND sr.received_at >= now() - interval '7 days'
  AND (sr.composite_score IS NULL OR sr.composite_score < 30)
GROUP BY sc.channel_key, sc.unit
ORDER BY sc.channel_key;
```

읽는 법 — 습도 `safe`는 `p05` 부근(평상시 하위 5%보다 아래여야 상시 가산이 안 생김),
온도·표면온도 `min`은 `p95`보다 위(평상시 최고보다 높아야 오경보가 없음).

### 연동된 코드 변경

| 파일 | 변경 |
|---|---|
| `rpi_b/src/MqttDb/task_vms.cpp` | `guardx/db/rpib/fire_threshold` 발행 payload에 `seasons` 배열 추가 |
| `vms/zone_settings_page.{h,cpp}` | SETTINGS 화면 우측에 계절 프리셋 버튼 |

발행 쿼리는 `to_regclass`로 테이블 존재를 먼저 확인한다 — **마이그레이션과 코드
배포 순서가 어느 쪽이어도 깨지지 않게** 하기 위해서다. 이 가드가 없으면 코드가
먼저 올라갔을 때 `relation "season_threshold" does not exist`로 발행 쿼리 전체가
죽어 SETTINGS 화면이 통째로 비어버린다.

### `season_key` 는 코드가 참조하는 식별자다 — 바꾸지 말 것

`season_key`에 CHECK 제약이 걸려 있다. **소문자 5개만 허용**한다.

```sql
CHECK (season_key IN ('default','spring','summer','autumn','winter'))
```

DBeaver 등에서 `spring` → `Spring` 으로 고치면 이렇게 거부된다:

```
SQL Error [23514]: new row for relation "season_threshold"
violates check constraint "season_threshold_season_key_check"
```

**이건 버그가 아니라 의도된 방어다.** `task_vms.cpp` 발행 쿼리가
`WHERE s.season_key <> 'default'` 로 이 값을 직접 비교하므로, 대소문자가
바뀌면 필터가 조용히 어긋나 '기본' 행이 계절 버튼으로 튀어나온다.

- **화면 글자를 바꾸고 싶다** → `season_name` 을 고친다 (VMS 버튼은 이 값을 쓴다)
- **임계값을 바꾸고 싶다** → 온습도·표면온도 6개 컬럼을 고친다
- **`season_key` 를 바꾸고 싶다** → CHECK 와 `task_vms.cpp` 를 함께 고쳐야 한다.
  얻는 것이 없으므로 하지 않는 것을 권한다.

### ⚠ 부수 발견 — `init.sql` 의 비밀번호가 실제와 다르다 (2026-08-04)

이 마이그레이션을 적용하다 드러났다. `guardx_admin` 으로 접속을 시도하니
`FATAL: password authentication failed` 가 났다.

```
rpi_b/Database/init.sql
  IF NOT EXISTS (...) THEN CREATE ROLE guardx_admin LOGIN PASSWORD '...';
```

`IF NOT EXISTS` 안에 있어서 **역할이 이미 있으면 그 비밀번호는 적용되지
않는다.** 즉 이 파일의 값은 "최초 생성 시에만 유효"한데 실제 DB는 그 뒤로
바뀌었고, 현재 값은 어디에도 기록돼 있지 않다.

- 영향: 폴러·mqttd 는 무관하다 (`config.env` 는 `guardx_writer` 를 쓴다).
  막히는 것은 **스키마 변경 작업**뿐이다.
- 이번 회피: 마이그레이션이 `SET ROLE guardx_admin` 을 쓰므로 `postgres`
  peer 인증으로 실행하면 비밀번호 없이 올바른 소유자가 된다.
- 총정리 때 할 일: 비밀번호를 `\password` 로 다시 정하고, `init.sql` 에
  평문으로 박아두는 방식 자체를 재검토한다 (세 계정 모두 해당).

### 총정리 때 할 일

1. `fire_schema.sql` 본문에 `season_threshold` CREATE + 시드 + GRANT를 흡수
2. 그 시점의 실측 시드 값을 반영 (위 "미정" 표시가 남아 있으면 아직인 것)
3. `fire_schema.sql`은 `DROP TABLE`로 시작하므로 **운영 DB에서 재실행 금지**
   — 이 테이블을 추가하려고 그 파일을 다시 돌리면 센서·사건 이력이 전부 사라진다
4. `init.sql` 의 계정 비밀번호 3종을 실제 값과 맞추거나, 평문 보관을 없앤다
   (위 "부수 발견" 참조)

---

## 2026-08-13 — `fan_auto` 명령 등록 (팬 자동 제어)

**파일**: `rpi_b/Database/history/fire/20260813_02_fan_auto_command.sql`
**티켓**: VEDA-195
**작성**: Claude

### 무엇을

`actuator_command` 카탈로그에 `fan_auto` 한 행을 추가했다 (`command_id = 6`,
`kind = 'onoff'`). 기존 시드가 1~5·7을 쓰고 6이 비어 있었다.

VMS DEVICE 화면의 팬 `AUTO` 토글이 이 명령을 기존 `set_actuator` 경로로 보낸다.

### 왜 필요한가 — 없으면 버튼이 조용히 막힌다

`guardx_mqttd` 의 `handleSetActuator`(`task_vms.cpp`)는 `command_key` 를
`actuator_command` 에서 찾지 못하면 **"카탈로그 미등록"으로 거부**한다.
행이 없으면 VMS 에서 AUTO 를 눌러도 RPi C 까지 가지 못하고, 화면에는 아무
반응이 없다. 이 문서 머리말이 경고하는 "기능이 아무 반응 없음"의 전형이다.

### `kind` 를 `onoff` 로 둔 이유

AUTO 는 켜고 끄는 것뿐이고 값이 없다. `set` 이나 `both` 로 열어두면
`set_actuator` 의 `kind` 검증은 `value` 를 통과시키는데 RPi C 의
`handle_fan_auto` 가 그걸 거절한다 — "명령은 받아들여졌는데 아무 일도
일어나지 않는" 구간이 생긴다.

### 기존 테이블 영향

**없다.** `ALTER` 없음, 기존 행 변화 없음. `ON CONFLICT (command_key) DO UPDATE`
라 여러 번 돌려도 안전하다. 롤백은
`DELETE FROM actuator_command WHERE command_key='fan_auto';` 한 줄이지만,
`manual_command` 에 이미 기록이 쌓였다면 FK 때문에 그 행부터 지워야 한다.

### 배포 순서

DB 먼저, 코드 나중이 안전하다. 반대로 하면 새 VMS 가 AUTO 를 보내는데 RPi B 가
거부하는 구간이 생긴다(기능이 안 될 뿐 다른 명령에는 영향 없음).

---

## 2026-08-13 — `led_matrix` 명령 제거 (이력 포함)

**파일**: `rpi_b/Database/history/cleanup/20260813_03_drop_led_matrix_cmd.sql`
**티켓**: VEDA-195
**작성**: Claude

### 무엇을

`actuator_command` 의 `led_matrix` 행과, 그것을 참조하는 `manual_command` ·
`fire_event_command` 의 **과거 기록까지** 삭제한다.

### 왜 이력까지 지우나

`led_matrix` 는 RPi C → STM32 UART 브릿지를 전제로 카탈로그에만 있었고 그
브릿지는 구현되지 않았다. RPi C 는 명령을 받아도 경고만 찍고 버렸다
(`actuator_registry.c` 의 `handle_unwired`). 즉 **한 번도 동작한 적이 없다.**

남은 기록은 "이 버튼을 눌렀다"는 흔적일 뿐 무엇도 일어나지 않았고, 그대로
두면 나중에 감사 로그를 읽는 사람이 실제로 동작한 명령과 구분하지 못한다.
운영 판단으로 함께 지운다.

### 삭제 순서와 범위

FK 때문에 자식 → 부모 순이다.

1. `manual_command`      (운영자 수동 명령 로그)
2. `fire_event_command`  (화재 자동 대응 이력)
3. `actuator_command`    (카탈로그 본체)

셋 다 `WHERE command_id = <led_matrix 의 id>` 로만 좁혀져 있다. **다른 명령의
기록은 건드리지 않는다.**

### 안전장치

실행 시 `pg_constraint` 를 조회해 `actuator_command` 를 참조하는 표가 위 둘
말고 더 있는지 본다. 있으면 이름을 찍고 `RAISE EXCEPTION` 으로 멈춘다 —
그냥 두면 FK 위반으로 트랜잭션이 통째로 되감기며 원인만 안 보인다.

### LED 매트릭스 표시 기능과는 무관하다

온습도·화재·트래킹 표시는 `guardx/display/rpic/{zones/N,fire,track}` →
RPi C `matrix_link` → Modbus → STM32 경로다. **DB 를 지나지 않으므로** 이
마이그레이션의 영향을 받지 않는다.

### 코드 쪽 동반 변경 (같은 브랜치)

| 파일 | 변경 |
|---|---|
| `vms/device_control_page.cpp` | `ACTUATOR_FIELDS` 에서 `led_matrix` 행 제거 (화면에서 버튼이 사라짐) |
| `rpi_c/.../actuator_registry.c` | 디스패치 표에서 제거 |
| `guardx_protocol.h` ×3 | `GUARDX_CMD_LED_MATRIX` 상수 제거 |
| `history/00_base/…_fire_schema.sql` | 시드에서 제거 (신규 DB 구축용) |

### 총정리 때 할 일

`fire_schema.sql` 본문에는 이미 반영해 두었다(시드에서 뺌 + `fan_auto` 추가).
별도로 흡수할 것은 없다.
