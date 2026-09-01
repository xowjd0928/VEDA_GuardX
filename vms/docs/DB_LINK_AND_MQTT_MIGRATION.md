# VMS ↔ DB 연동을 MQTT로 전환 — 설계와 진행 기록

> 대상: VMS(Qt) 담당 · RPi B 담당 · 프로토콜 규약 관리자
> 기준 코드: `vms/` (태그 `mqtt-v9`), `rpi_b/` (origin/main 머지분)
> 관련 문서: `GuardX 통신 프로토콜 규약 (MQTT)`, `rpi_b/MQTT_ALERT_INTERFACE.md`
> 최초 작성 2026-07-28 · 개정 2026-07-29 (v5 구조 · v7 이식 · v9 편집 반영)

---

## 0. 30초 요약

**목표**: VMS에서 `192.168.0.3:5432` 직결과 Postgres 계정을 없앤다.

**핵심**: SQL이 사라지는 게 아니라 **실행 위치가 운영자 PC → RPi B로 이사**한다.

```
전:  [VMS] ──TCP 5432──> [Postgres]        VMS가 DB 계정을 들고 있음
후:  [VMS] <──MQTT── [브로커] <── [RPi B 폴러] ──localhost──> [Postgres]
```

**진행**: VMS·RPi B 양쪽 완료. DB 쿼리 4개 → **0개**.
남은 것은 `v8`(Postgres localhost 바인딩) 하나 — **그때까지 목표는 미달성**이다.

| 태그 | 내용 | 상태 |
|---|---|---|
| `mqtt-back_up` | DB 직결 기준점 | ✅ |
| `mqtt-v1` | MqttLink 배관 (기능 변화 없음) | ✅ |
| `mqtt-v2` | 정원/임계 → MQTT 구독 | ✅ |
| `mqtt-v3` | 슬라이더 구간 → MQTT 구독 | ✅ (v5에서 폐기) |
| `mqtt-v4` | 히트맵 → 요청-응답 | ✅ (v5에서 재설계) |
| `mqtt-v5` | 히트맵을 **날짜 단위 로딩**으로 재설계 | ✅ |
| `mqtt-v6` | VMS에서 DB 흔적 완전 제거 | ✅ |
| `mqtt-v7` | 조회를 RPi B 폴러로 이식 (셸 스크립트 폐기) | ✅ 실기 검증 |
| `mqtt-v9` | VMS에서 정원/임계 편집 (`cmd/set_zone`) | ✅ 실기 검증 |
| v8 | Postgres를 localhost 바인딩 | ⬜ |

> `v9`를 `v8`보다 먼저 한 이유: `v8`로 5432를 닫으면 DBeaver로 정원을 못 고치게
> 된다. 편집 경로를 먼저 만들어두지 않으면 값을 바꿀 방법이 사라진다.

---

# 1부 — 전환 전 구조 (기준점)

## 1.1 VMS가 던지던 SQL 4개

| # | 위치 | 쿼리 | 용도 | 현재 |
|---|---|---|---|---|
| a | `zone_config.cpp` | `zones JOIN zone_thresholds` | OCC 배지 분모(정원) | v2에서 대체 |
| b | `crowd_page.cpp` | `SELECT min(ts), now()` | 슬라이더 눈금 | v3 → v5에서 폐기 |
| c | `crowd_page.cpp` | detections 격자 집계 | 히트맵 | v4 → v5에서 재설계 |
| d | `live_viewer.cpp` | 객체 rect 대조 | **디버그 로그** | v6에서 삭제 |

VMS는 DB에 **읽기만** 했다. INSERT/UPDATE는 한 줄도 없었다.

## 1.2 라이브 오버레이는 원래 DB를 안 거친다 (전환 범위 밖)

가장 오해하기 쉬운 지점이다. 화면의 바운딩 박스와 OCC **분자**는 DB에서 오지 않는다.

`detection_feed.cpp` 가 카메라 OpenSDK(`/opensdk/juan_application/detections`)를
**200ms 주기로 직접 폴링**한다. DB가 죽어도 박스는 그대로 나온다.

**이 경로는 이번 전환에서 건드리지 않았다.**

## 1.3 전환 근거

| # | 문제 |
|---|---|
| 1 | VMS가 `psql`·DBeaver와 동급의 Postgres 클라이언트 → **운영자 PC마다 DB 계정 배포** |
| 2 | 그래서 Postgres를 외부에 열어둬야 함 (`schema.sql:86` 은 "DB는 RPi B localhost 전용"을 전제로 카메라 비밀번호를 평문 저장 중인데, 이 전제가 깨져 있었다) |
| 3 | 설정을 바꿔도 **VMS 재시작 전까지 반영 안 됨** (a는 기동 시 1회 조회) |
| 4 | 모든 쿼리가 GUI 스레드 동기 실행 → DB 지연이 곧 UI 프리즈 |

---

# 2부 — 동작이 어떻게 달라지는가

## 2.1 MQTT 기본 4개

| 용어 | 뜻 |
|---|---|
| 토픽 | 메시지 주소 (`guardx/db/rpib/zones`) |
| publish | 그 주소로 보냄 |
| subscribe | 그 주소를 구독 |
| broker | 중계 서버 (RPi B의 mosquitto) |

**브로커는 아무것도 만들어내지 않는다.** 누군가 publish 해야만 메시지가 존재한다.
"브로커가 RPi B에 있으니 DB를 읽어줄 것"이라는 기대는 성립하지 않는다 —
mosquitto와 postgres는 같은 기계에서 도는 서로 다른 프로그램이고, 둘 사이에서
누군가는 `SELECT` 를 실행해야 한다.

## 2.2 `retained` — 이 설계의 핵심

```
retain=false : 지금 접속해 있는 구독자에게만 배달. 늦게 오면 못 받음
retain=true  : 브로커가 마지막 메시지 1개를 보관. 나중에 구독해도 즉시 받음
```

RPi B가 기존에 쓰던 토픽(`guardx/alert/rpib` 등)은 전부 `retain=false` 다 —
"사건"이라 지나간 걸 뒤늦게 받아봐야 의미가 없기 때문.

반면 우리가 필요한 건 **"상태"** 다. "지금 정원이 얼마인가"는 언제 물어도 답이
있어야 한다. 그래서 `retain=true` 를 쓴다. **이 프로젝트에 없던 새 패턴이다.**

## 2.3 시나리오 — 운영자가 정원을 20 → 30으로 바꾼다

**전**
```
14:00:00  DB에서  UPDATE zone_thresholds SET capacity_limit = 30
14:00:01  VMS 화면:  OCC 3/20     ← 안 바뀜
18:00:00  VMS 화면:  OCC 3/20     ← 안 바뀜
          → VMS를 껐다 켜야 30이 된다
```

**후**
```
14:00:00  DB에서  UPDATE zone_thresholds SET capacity_limit = 30
14:00:05  폴러가 감지 → guardx/db/rpib/zones 에 발행 (retain)
14:00:05  브로커 → 구독 중인 VMS 전부
14:00:05  VMS 화면:  OCC 3/30     ← 자동으로 바뀜
```

| | 전 | 후 |
|---|---|---|
| 누가 시작하나 | VMS가 **물어본다** (pull) | RPi B가 **알려준다** (push) |
| 안 물어보면 | 영원히 모름 | 상관없음 |
| 문제 #3 | 있음 | **사라짐** |

## 2.4 코드 모양의 변화

**전** — 함수가 리턴하면 값이 준비돼 있다.
```cpp
bool load() {
    q.exec("SELECT ...");
    while (q.next()) g_zones[ch].capacity = ...;   // 여기서 채워짐
    return true;                                    // 리턴 = 준비 완료
}
```

**후** — 두 조각으로 쪼개진다.
```cpp
void init() {
    MqttLink::instance()->subscribe("guardx/db/rpib/zones", apply, 1);
}                                    // ★ 리턴해도 값이 없다

static void apply(const QByteArray &p) {   // 나중에, 여러 번 불림
    g_zones[ch].capacity = ...;
}
```

**"함수가 리턴하면 값이 준비돼 있다"가 깨지는 것** — 이게 로직상 가장 큰 변화다.
`ZoneConfig` 는 `Theme::channel_cap()` 폴백이 이미 있어서 안전했지만, 폴백이
없는 화면(히트맵·슬라이더)은 "값이 아직 없는 상태"를 새로 다뤄야 했다.

---

# 3부 — 최종 설계

## 3.1 데이터를 두 부류로 나눈다

| 부류 | 판별 기준 | 방식 | 난이도 |
|---|---|---|---|
| **A. 상태** | 사용자가 뭘 고르든 답이 같다 | 발행/구독 (retained) | 쉬움 |
| **B. 질의** | 사용자 선택에 따라 답이 달라진다 | 요청/응답 | 어려움 |

> 앞으로 기능을 추가할 때도 이 기준으로 먼저 나누면 된다.
> "자동 점유율 갱신"은 A, "객체 추적 조회"는 B다.

## 3.2 토픽 일람

토픽 이름은 규약 형식 `guardx/{도메인}/{노드ID}/{하위}` 를 따른다
(`guardx/sensor/rpia/button` 과 같은 구조).

| 토픽 | 방향 | QoS | retain | 부류 |
|---|---|---|---|---|
| `guardx/db/rpib/zones` | RPi B → VMS | 1 | ✓ | A |
| `guardx/db/rpib/dates` | RPi B → VMS | 1 | ✓ | A |
| `guardx/db/rpib/query/heatday` | VMS → RPi B | 1 | ✗ | B (요청) |
| `guardx/db/rpib/cmd/set_zone` | VMS → RPi B | 1 | ✗ | **C (쓰기)** |
| `guardx/db/rpib/fire_threshold` | RPi B → VMS | 1 | ✓ | A |
| `guardx/db/rpib/cmd/set_fire_threshold` | VMS → RPi B | 1 | ✗ | **C (쓰기)** |
| `guardx/config/rpib` | RPi B → 화재 엔진 | 1 | ✗ | 신호 |
| `guardx/db/{client_id}/result` | RPi B → VMS | 1 | ✗ | B·C (응답) |
| `guardx/db/rpib/endpoints` | RPi B → VMS | 1 | ✓ | A (08-11 신설) |
| `guardx/db/rpib/cmd/login` | VMS → RPi B | 1 | ✗ | B (요청, 08-11 신설) |
| `guardx/db/rpib/cmd/session_check` | VMS → RPi B | 1 | ✗ | B (요청, 08-11 신설) |
| `guardx/db/rpib/cmd/logout` | VMS → RPi B | 1 | ✗ | **C (쓰기, 08-11 신설)** |
| `guardx/db/rpib/cmd/change_password` | VMS → RPi B | 1 | ✗ | **C (쓰기, 08-11 신설)** |
| `guardx/db/rpib/cmd/create_user` | VMS → RPi B | 1 | ✗ | **C (쓰기, 08-11 신설)** |
| `guardx/db/rpib/cmd/set_user_enabled` | VMS → RPi B | 1 | ✗ | **C (쓰기, 08-12 신설)** |
| `guardx/db/rpib/cmd/list_users` | VMS → RPi B | 1 | ✗ | B (요청, **admin 전용**, 08-12 신설) |
| `guardx/db/rpib/site_config` | RPi B → VMS | 1 | ✓ | A (08-12 신설) |
| `guardx/db/rpib/cmd/set_site_config` | VMS → RPi B | 1 | ✗ | **C (쓰기, 08-12 신설)** |

⚠ `cmd/list_users` 는 **부류 B(질의)인데 admin 토큰을 요구하는 유일한 경우**다.
기존 B(`heatday` 등)는 토큰이 없다 — 그쪽은 집계값이지만 이쪽은 계정 명부라서다.

### QoS 근거 (규약 0절 원칙 2)

- **QoS 0을 쓰지 않는 이유**: QoS 0의 안전성은 "다음 주기에 새 값이 온다"에서
  나온다(센서 토픽이 그렇다). `zones`·`dates` 는 **변경 시에만** 발행하므로
  한 번 유실되면 영영 틀린 값이 남는다.
- **QoS 2를 쓰지 않는 이유**: 전부 덮어쓰기(멱등)라 중복 수신이 무해하다.
  QoS 2의 4-way 핸드셰이크 비용을 낼 이유가 없다. 규약에서 QoS 2는 비상 버튼
  하나뿐이고 근거가 "DB 중복 기록 금지"다.
- **구독 쪽 QoS도 1로 줘야 한다.** 실제 전달 QoS는 `min(발행, 구독)` 이다.

### client_id 규칙 — 규약의 예외

규약 0절은 "client id = node id"지만 실제로는 이미 깨져 있다. MQTT는 client id가
겹치면 기존 세션을 강제로 끊기 때문이다.

| 프로그램 | client id |
|---|---|
| 폴러 | `rpib-poller` |
| transmission layer | `rpib-txlayer` |
| **VMS** | **`vms-{hostname}`** (여러 대가 동시에 뜰 수 있음) |

## 3.3 payload

### `guardx/db/rpib/zones`

```jsonc
{ "node_id":"rpib", "timestamp":1785228000000,
  "zones":[ { "zone_id":2, "channel":0, "capacity_limit":30,
              "warn_ratio":0.75, "critical_ratio":0.90 } ] }
```

⚠ **`zone_id ≠ channel` 이다** (실측: zone1→ch1, zone2→ch0). 반드시 `zones` 를
조인해 `channel` 을 실어야 한다 — VMS는 `channel` 로만 판단한다.
`capacity_limit` 은 스키마상 NULL 가능이며 그대로 `null` 로 보낸다
(VMS는 "값 없음"으로 보고 기본값 유지).

### `guardx/db/rpib/dates`

```jsonc
{ "node_id":"rpib", "dates":["2026-07-20","2026-07-21","2026-07-28"] }
```

`detections` 에 행이 있는 날짜만. 날짜 경계는 **표시 타임존(Asia/Seoul)** 기준으로
자르며, 쿼리 서비스와 반드시 같은 TZ를 써야 한다.

### `guardx/db/rpib/query/heatday` (요청)

```jsonc
{ "node_id":"vms-desk01", "req_id":"a3f1…", "reply_to":"guardx/db/vms-desk01/result",
  "query":"heatday", "date":"2026-07-28",
  "cell":60, "slot_min":10, "category":1, "min_likelihood":0.30 }
```

### `guardx/db/{client_id}/result` (응답)

```jsonc
{ "node_id":"rpib", "req_id":"a3f1…", "date":"2026-07-28", "ok":true,
  "cells":[[84,0,21,7,133],[84,1,29,7,91]] }
```

`cells` = `[slot, channel, gx, gy, count]` 배열. 하루치가 5천 셀 규모라
키 있는 객체 대신 배열을 쓴다(전송량 1/3).

실패 시:
```jsonc
{ "req_id":"a3f1…", "date":"2026-07-28", "ok":false, "error":"…" }
```

### `guardx/db/rpib/cmd/set_zone` (쓰기, v9)

```jsonc
{ "node_id":"vms-desk01", "req_id":"b7e2…", "reply_to":"guardx/db/vms-desk01/result",
  "cmd":"set_zone", "zone_id":2,
  "capacity_limit":30, "warn_ratio":0.75, "critical_ratio":0.90 }
```

VMS가 DB에 직접 붙지 않고도 `zone_thresholds` 를 고치는 유일한 경로다.
QoS 1 — `capacity = 30` 은 멱등이라 중복 수신이 무해하다.

**폴러는 UPDATE 직후 `zones` 를 즉시 재발행한다.** 그래서 다음 틱(기본 30초)을
기다리지 않고 1초 안에 VMS 화면이 바뀌고, **그 갱신 자체가 성공 확인**이 된다
(별도 ack UI가 필요 없다).

⚠ **쓰기는 읽기보다 위험하다.** 브로커가 현재 평문·익명 허용이라 같은 LAN의
누구든 이 토픽에 쏠 수 있다. 폴러가 반드시 값을 검증한다:

| 항목 | 범위 | 안 막으면 |
|---|---|---|
| `capacity_limit` | 1 ~ 10000 | 0이면 VMS에서 `OCC n/0` — 나눗셈이 깨진다 |
| `warn_ratio` | 0 초과 1 미만 | 색 임계가 무의미해짐 |
| `critical_ratio` | warn 초과 1 이하 | 위험이 주의보다 낮아짐 |
| `zone_id` | `affected_rows()==0` 이면 거부 | 없는 존에 조용히 성공 응답 |

이 검증이 **브로커 ACL 또는 mTLS 전환 전까지의 유일한 방어선**이다.

### `guardx/db/rpib/fire_threshold` (상태, retained)

```jsonc
{ "node_id":"rpib", "timestamp":1754…,
  "threshold": { "threshold_id":7, "gas_raw_min":650, …,
                 "updated_at":"2026-08-03T05:12:44Z", "updated_by":"vms vms-3-15" } }
```

활성 행(`is_active`) **하나를 통째로** 싣는다. `fire_threshold` 는 "1행 = 설정
스냅샷 전체" 구조라(`rpi_b/Database/fire_schema.sql`) 부분 전송이 의미가 없고,
VMS 편집 폼도 22개 값을 다 채워야 한다.

활성 행이 없으면 `"threshold": null` 로 나간다 — VMS는 그때 [적용]을 막는다.
빈 폼을 그대로 보내면 0 투성이 설정이 활성화되기 때문이다.

### `guardx/db/rpib/cmd/set_fire_threshold` (쓰기)

```jsonc
{ "node_id":"vms-3-15", "req_id":"c9d4…", "reply_to":"guardx/db/vms-3-15/result",
  "cmd":"set_fire_threshold",
  "gas_raw_min":650, "gas_raw_max":800, … ,
  "weight_gas":0.20, "weight_spark":0.35, … ,
  "fire_score_threshold":65, "n_confirm":3, "n_recover":10,
  "updated_by":"vms vms-3-15" }
```

**22개 값을 전부 보낸다.** RPi B는 이걸로 새 행을 INSERT 하고 `is_active` 를
넘긴다 — 이전 행은 남으므로 "언제 누가 무엇을 바꿨나"가 이력으로 보존된다.

```
UPDATE fire_threshold SET is_active=FALSE WHERE is_active;
INSERT INTO fire_threshold (…, is_active) VALUES (…, TRUE);
```

⚠ **두 문장이 한 트랜잭션이어야 한다.** `uq_fire_threshold_active` 부분 유니크
인덱스 때문에, 비활성화 없이 INSERT 하면 유니크 위반으로 죽는다.

**성공 후 `guardx/config/rpib` 로 리로드 신호를 쏜다.** 이게 없으면 DB만 바뀌고
화재 엔진(`rpib_app`)은 옛 임계로 계속 판단한다 — 겉보기엔 저장이 된 것 같은데
동작이 안 바뀌는, 찾기 어려운 상태가 된다. 엔진은 페이로드를 보지 않는다
(`"reload"` 문자열은 사람이 로그에서 알아보라고 넣은 것).

검증은 DB CHECK 와 같은 조건을 RPi B가 **먼저** 한다. CHECK 에 걸리면
`new row violates check constraint "chk_weight_sum"` 같은 Postgres 원문이 그대로
VMS 화면에 뜨는데, 그걸로는 어느 값이 왜 틀렸는지 알 수 없다.

| 항목 | 조건 | 안 막으면 |
|---|---|---|
| 가중치 5개 합 | `1.0 ± 0.001` | DB CHECK 거부 (원문 에러) |
| `gas_raw_min` | `< gas_raw_max` | 퍼지 구간이 뒤집힘 |
| `spark_raw_safe` | `> spark_raw_danger` | 내림차순 채널이라 방향이 반대 |
| `humi_safe_percent` | `> humi_danger_percent` | 〃 |
| `fire_score_threshold` | 0 ~ 100 | 화재가 영영 확정 안 되거나 항상 확정 |
| `n_confirm`·`n_recover`·`freeze_relax_cycles` | 1 이상 | 0이면 판정 로직이 깨진다 |

**이 값은 화재 판단 기준이다.** 잘못 들어가면 경보가 안 울린다 — `set_zone`
보다 검증이 더 중요한 이유다.

**MQTT 5의 ResponseTopic/CorrelationData 대신 payload에 넣는 이유**: RPi B의
mosquitto 버전 의존을 없애기 위해서다 (1.6.x는 MQTT 5 미지원). payload 방식은
3.1.1에서도 그대로 돈다.

### `guardx/db/rpib/endpoints` (상태, retained) — 08-11 신설

```jsonc
{ "node_id":"rpib", "rpic_rtp_host":"172.20.33.114",
  "updated_at":"2026-08-11T10:22:31Z" }
```

RPi C RTP 방송 목적지를 컴파일 상수(`shared/broadcast_protocol.h`
`GUARDX_BROADCAST_RTP_HOST`)에서 DB로 옮긴 것 — 지금은 IP가 바뀌면 VMS·RPi C
**양쪽 재빌드**다. VMS 쪽 수신은 단계 0d.

- **표의 key 가 최상위 필드명**이다. 주소가 늘어도 payload 모양과 파서가 안 바뀐다.
  봉투 필드와의 충돌은 서버의 테이블 CHECK 이 막는다(`node_id`·`timestamp`·
  `updated_at` 은 key 로 못 쓴다).
- ⚠ **`timestamp` 가 없다.** 다른 상태 토픽처럼 now() 를 실으면 내용이 안 바뀌어도
  payload 문자열이 매 틱 달라져 `publishIfChanged` 가 항상 실패한다(30초마다 무조건
  재발행). 주소는 사이트를 옮기지 않는 한 안 바뀌는 값이라 신선도는 `updated_at`
  (= 주소가 **마지막으로 바뀐** 시각)으로 본다.
- **표가 비면 발행하지 않는다** — 빈 payload 를 보내면 VMS 가 캐시를 지운다.
- VMS 폴백 3단: 수신 → 레지스트리 캐시 → 컴파일 상수. 브로커가 죽어도 방송이
  도는 08-10 원칙이 캐시로 보존된다.

### `guardx/db/rpib/cmd/login` · `session_check` · `logout` — 08-11 신설

로그인 3종. 요청-응답 규약(`req_id`·`reply_to`)은 기존 그대로다.

| 토픽 | params | 성공 응답 |
|---|---|---|
| `cmd/login` | `{username, password, device?}` | `{ok, role, display_name, token, expires_at}` |
| `cmd/session_check` | `{token}` | `{ok, username, display_name, role, expires_at}` |
| `cmd/logout` | `{token}` | `{ok}` |

```jsonc
// 요청
{ "node_id":"vms-3-11", "req_id":"7b2e…", "reply_to":"guardx/db/vms-3-11/result",
  "username":"admin", "password":"…", "device":"vms vms-3-11" }
// 응답
{ "req_id":"7b2e…", "ok":true, "role":"admin", "display_name":"홍길동",
  "token":"…64hex…", "expires_at":"2026-09-10T01:22:31Z" }
```

- `device` 는 **선택**이다(감사용 — `vms_session.device`). 형식은 `updated_by` 와
  같은 `"vms {client_id}"`. 안 보내면 세션에 기기 표기가 안 남을 뿐이다.
- 비밀번호가 **평문으로 실린다.** mTLS(8883) 이후에만 유효한 설계다.
  절체는 08-11 에 끝났고, 그와 함께 **`auth/stub` 기본값을 꺼짐으로 뒤집었다** —
  이제 스텁은 명시적으로 켜야 도는 개발용 우회로다
  (`HKCU\Software\GuardX\VMS\auth` `stub`=1). ⚠ 1883 평문 브로커에 이 요청을
  다시 흘리지 말 것.

⚠ **실패 응답의 `reason` 과 `error` 는 다른 것이다** (08-11 확정).

| 필드 | 성격 | 값 | 누가 읽나 |
|---|---|---|---|
| `error` | 사람이 읽는 진단 | `e.what()` 원문 (자유 형식) | 개발자·로그 |
| `reason` | 기계가 분기하는 열거값 | 아래 목록 (고정) | VMS 코드 |

**분기는 `reason` 으로만 한다.** `error` 로 분기하면 예외 문구가 바뀔 때마다
화면이 조용히 깨진다. 로그인 3종은 `error` 를 아예 싣지 않는다 — DB 오류 문구로
스키마가 새고 계정 존재 여부가 새기 때문에 서버가 응답 경로를 따로 뒀다.

| reason | 뜻 |
|---|---|
| `bad_credentials` | 계정 없음 **+** 비밀번호 틀림 (일부러 하나로 덮는다) |
| `locked` | 실패 누적 잠금. `retry_after_s` 동반 (5회→60, 10회→600) |
| `disabled` | 계정 비활성 |
| `expired` | 세션 만료 **+** 없는 토큰 (일부러 하나로 덮는다) |
| `forbidden` | 토큰은 유효하나 역할이 admin 이 아님 — **쓰기 명령**의 거절 사유 |

### `cmd/change_password` · `cmd/create_user` — 08-11 저녁 신설

| 토픽 | params | 성공 응답 |
|---|---|---|
| `guardx/db/rpib/cmd/change_password` | `{token, old_password, new_password}` | `{ok, token, expires_at}` |
| `guardx/db/rpib/cmd/create_user` | `{token, username, display_name, role, password}` | `{ok, user_id}` |

`reason` 추가분: `weak_password`(**빈 값 전용** — 08-12 정책 폐지) ·
`duplicate`(이미 있는 username).
`bad_credentials` 는 `change_password` 에서 **현재 비밀번호가 틀림**을 뜻한다.

- ⭐ **변경 성공 시 그 사용자의 기존 세션을 전부 무효화**한다 — 비밀번호를 바꾸는
  이유의 절반이 "샜을지도 모른다"라서, 다른 기기의 세션이 살아 있으면 바꾼 의미가
  없다. 대신 **응답에 새 토큰**을 실어 방금 바꾼 본인은 재로그인하지 않는다
- `create_user` 는 **admin 만**(`forbidden`). 새 계정은 `must_change_pw=TRUE` 로
  만들어진다 — 만든 사람이 초기 비밀번호를 알고 있으므로 본인이 첫 로그인에서 바꾼다
- ⭐ **비밀번호 정책은 폐지됐다 (08-12).** 길이 규칙이 없다 — 서버가 거부하는
  것은 **빈 값 하나뿐**이고 그때만 `weak_password` 가 온다.
  (08-11 의 "최소 8바이트" 규칙은 여기서 끝났다. VMS 쪽 선검사
  `password_policy_error()` 도 함께 지운다 — 화면만 더 엄격하면 서버가 받아 줄
  비밀번호를 거부하면서 이유는 서버 탓으로 보인다.)
  - 빈 값만 막는 근거: 로그인 핸들러가 빈 password 를 `bad_credentials` 로
    거절하므로, 허용하면 **"만들 수는 있는데 로그인은 안 되는 계정"** 이 생긴다.
    `weak_password` reason 자체는 계약에 남는다 — VMS 분기 코드를 지울 필요는 없다
  - ⚠ **공백을 깎지 않는다.** 앞뒤 공백도 비밀번호의 일부이고 **개수까지 보존**된다.
    08-12 실기 확인: 공백 3개로 변경 → 같은 3개로 재로그인 성공, 1개는 실패.
    서버 전 경로 실사로도 확인했다(login·change_password·create_user) —
    `jgetStr` 은 JSON 이스케이프만 풀고 공백은 건드리지 않는다.
    ⚠ 어느 쪽에서도 `trim`/`trimmed()` 를 걸지 말 것 — 한쪽만 깎으면 방금 바꾼
    본인이 못 들어오고, 원인은 로그 어디에도 안 남는다
    (아이디·표시 이름은 반대다 — 그쪽은 깎는 것이 정상이다)
- ⚠ **`must_change_password`(reason)** — 강제 변경 대상의 토큰으로 쓰기 명령
  (`set_zone`·`set_actuator`·`set_fire_threshold`·`create_user`)이 오면 서버가
  거절한다(08-11 저녁 서버 추가). 강제 변경을 UI 로만 막으면 **공개된 시드
  비밀번호로 로그인해 액추에이터를 조작**할 수 있어 §5b 가 화면 연출이 된다.
  `change_password` 는 이 검사를 안 탄다 — 유일한 탈출구다.
  VMS 는 이 사유를 받으면 상태를 되돌려 **변경 화면으로 보낸다**
- `login`·`session_check` 응답에 **`must_change`(bool)** 가 추가됐다.
  ⚠ **없으면 false 로 읽는다** — 서버가 아직 안 실어도 기존 동작이 안 깨진다

### `cmd/set_user_enabled` — 08-12 신설 (서버 배포·검증 완료)

| 토픽 | params | 성공 응답 |
|---|---|---|
| `guardx/db/rpib/cmd/set_user_enabled` | `{token, username, enabled}` | `{ok, username, enabled}` |

⚠ 응답에 **`username`·`enabled` 가 함께 온다**(실서버 실측). 화면이 응답만 보고
행 상태를 갱신할 수 있게 반영된 값을 되돌려 준다.

**계정 삭제는 없다. 비활성화가 삭제다.** 행을 지우면 그 계정이 남긴
`updated_by`·`vms_session.device` 기록이 가리킬 곳을 잃는다 — 사고 조사에서 가장
먼저 보는 것이 "누가 언제 무엇을 바꿨나"라, 지우는 쪽이 잃는 것이 더 크다.

서버 쪽 근거가 하나 더 있다: `guardx_writer` 에 `vms_user` DELETE 권한이 **없고,
안 주는 것이 설계다** — "mqttd 가 뚫려도 공격자가 계정을 못 지운다"
(`migration_vms_auth.sql`). 비활성화는 UPDATE 라 그 통제를 유지한 채 구현된다
(그래서 이 기능에는 마이그레이션이 필요 없었다).

`reason` 추가분:

| reason | 뜻 | 서버가 막는 이유 |
|---|---|---|
| `self_target` | 자기 계정을 자기가 비활성화 | 누른 순간 자기 세션이 죽는다 |
| `last_admin` | 마지막 관리자를 비활성화 | 아무도 계정을 관리할 수 없는 상태가 된다 |
| `not_found` | 그런 username 이 없다 | — |
| `forbidden` | admin 아님 | `create_user` 와 같다 |

- ⚠ **비활성 계정도 `username` 을 계속 점유한다** — `create_user` 가 그 아이디에
  `duplicate` 를 준다. 같은 사람이 돌아오면 **재활성(`enabled:true`)이 정상 경로**다.
  화면은 `duplicate` 를 만났을 때 이 경로를 가리켜야 한다(안 그러면 관리자가
  `user2`·`user3` 을 만들기 시작한다)
- 비활성 계정의 로그인은 기존대로 `disabled` 로 거절된다
- ⭐ **끌 때 대상의 `vms_session` 을 전부 지운다.** 안 지우면 이미 발급된 토큰이
  **최대 30일** 살아서 다른 PC 의 로그인이 그대로 유지된다 — "계정을 껐는데 그
  사람 화면은 멀쩡한" 상태가 된다. 실측 확인: 비활성 직후 그 계정의 기존 토큰으로
  명령을 보내면 `expired`
- **멱등이다** — 이미 그 상태여도 `ok:true` (덮어쓰기 무해 원칙). 화면이 같은
  버튼을 두 번 눌러도 안전하다
- ⚠ `enabled` 필드는 **`true`/`false` 불리언이어야 한다.** 누락하거나 문자열
  `"false"` 를 보내면 `reason` 이 아니라 **`error`** 로 거절된다(규격 위반은 기계
  분기 대상이 아니다). 서버가 누락을 `false` 로 읽지 않는 이유는, 그러면 오타
  하나로 계정이 꺼지기 때문이다

### `cmd/list_users` — 08-12 신설 (**서버 구현 완료 · 배포 대기**)

계정 화면이 "아이디를 손으로 정확히 입력"에 기대지 않으려면 목록이 필요하다.
**VMS 가 요청한 형태를 그대로 받았다** — 아래 필드·규칙은 서버 확정본이다.
(배포 시점은 §5 "배포 상태" 참조)

| 토픽 | params | 성공 응답 |
|---|---|---|
| `guardx/db/rpib/cmd/list_users` | `{token}` | `{ok, users:[…]}` |

```jsonc
{ "req_id":"…", "ok":true, "users":[
  { "username":"admin", "display_name":"홍길동", "role":"admin",
    "enabled":true,  "must_change_pw":false, "last_login_at":"2026-08-12T04:11:02Z" },
  { "username":"kim",   "display_name":"김운영", "role":"operator",
    "enabled":false, "must_change_pw":true,  "last_login_at":null }
]}
```

- ⚠ **retained 발행이 아니라 요청-응답(`cmd/`)이다.** 계정 명부를 retained 토픽에
  두면 브로커에 붙는 누구에게나 전 직원 아이디·역할이 상시 노출된다.
  구독으로 새는 것과 명령으로 묻는 것은 보안이 다른 사건이다
- **admin 만**(`forbidden`). 토큰 없으면 `expired` — 나머지 쓰기 명령과 같다
- `enabled:false` 행도 **반드시 포함**한다. 재활성 대상이 목록에서 사라지면
  `set_user_enabled(enabled:true)` 를 부를 방법이 화면에 없다
- 결과가 0건이어도 `users` 키를 **생략하지 않는다**(`[]`). 키 없음과 빈 목록을
  구분하려면 VMS 가 응답 형식을 의심해야 한다
- 정렬은 서버가 `username ASC`. 계정 수가 수십 단위라 페이징은 두지 않는다
- `must_change_pw` 는 선택이지만 있으면 화면이 "초기 비밀번호 미변경"을 표시한다.
  ⚠ **비밀번호 해시·salt 는 어떤 필드로도 싣지 않는다**

**서버 확정 사항** (08-12, 요청에 대한 답):

- **필드는 요청한 6개를 그대로 싣는다** — `username` · `display_name` · `role` ·
  `enabled` · `must_change_pw` · `last_login_at`. 선택이라던 뒤 둘도 포함했다
  (같은 행에 이미 있어 비용이 0이고, 없으면 화면이 "초기 비밀번호 미변경"을
  못 그린다). `user_id` 는 **싣지 않는다** — 모든 명령이 `username` 으로 도는데
  내부 식별자를 밖으로 낼 이유가 없다
- `last_login_at` 은 한 번도 로그인하지 않았으면 **`null`**. 형식은 프로젝트 규약
  대로 ISO8601 UTC `Z` (아래 "시각 표기 규칙")
- ⚠ **`must_change_password` 검사를 건다** (VMS 가 "안 걸어도 된다"고 했지만
  서버는 거는 쪽을 택했다). 근거: ①모든 admin 명령이 같은 가드를 타야 "어디는
  되고 어디는 안 되는" 예외가 안 생긴다 ②계정 명부는 전 직원 아이디·역할이라
  시드 비밀번호를 아는 사람에게 열어 줄 것이 아니다. **정상 흐름에서는 안 뜬다** —
  강제 변경 상태의 VMS 는 이 화면에 들어오지 않으므로
- 해시·salt 차단은 SQL 에서 **컬럼을 하나씩 나열**해 구조적으로 막았다
  (`to_jsonb(row)` 를 쓰면 `pw_hash`·`pw_salt` 가 통째로 실린다 — `fire_threshold`
  발행이 그 방식이라 같은 손버릇이 나오기 쉬운 자리다)
- 조회인데도 **쓰기 커넥션(`rwDb`)으로 읽는다** — `vms_user` 는 `guardx_reader`
  에서 SELECT 가 회수돼 있어(해시 노출 방지) 조회 커넥션으로는 아예 못 읽는다

### `guardx/db/rpib/site_config` (상태, retained) · `cmd/set_site_config` — 08-12 신설

08-12 합의 ④: 캘리브레이션과 SITE 문구는 **전역**이다 — 배포 시 모든 사용자가
같은 환경을 본다. 서버가 단일 진실원천으로 저장하고 retained 로 뿌린다.
항목이 2개뿐이라 토픽을 하나로 묶었다 — 다음 "전역 설정"이 나와도 키만 늘린다.

**상태** (`fire_threshold`·`endpoints` 와 같은 규약 — 변경 시에만 retained 발행):

```jsonc
{ "node_id":"rpib",
  "site_name":"GuardX 시연장",
  "calibration": { /* VMS 소유 스키마 — 서버는 통짜 보관·발행만 한다 */ },
  "updated_at":"2026-08-12T05:00:00Z" }
```

- `timestamp` 는 싣지 않는다 — `endpoints` 와 같은 이유(매 틱 달라져 dedup 이
  깨진다). `updated_at` = 저장된 키들의 max(updated_at)
- 저장은 DB 테이블(key-value·JSONB)이다 — retained 만 믿으면 브로커 재시작에
  날아간다. 폴러 기동 시 DB → retained 재발행 (기존 상태 토픽 패턴 그대로).
  **실측: retained 를 지우고 mqttd 를 재시작하니 5초 만에 DB 에서 복원됐다**
- ⚠ 저장된 키가 하나도 없으면 발행하지 않고, **저장 안 된 키는 payload 에서
  빠진다** — VMS 는 없는 키를 "설정 안 됨"으로 읽어야 한다 (`site_name` 만
  저장된 상태면 `calibration` 필드 자체가 없다)

**쓰기**:

| 토픽 | params | 성공 응답 |
|---|---|---|
| `guardx/db/rpib/cmd/set_site_config` | `{token, site_name?, calibration?}` | `{ok}` |

- **부분 갱신** — 실린 키만 저장한다. `site_name` 만 보내면 `calibration` 은
  안 변한다 (실측 확인)
- 허용 키는 **화이트리스트 2개**: `site_name`(문자열) · `calibration`(JSON 객체
  통짜). 그 외 키는 무시한다 — 봉투 필드(`req_id` 등)와 설정 키가 같은 평면에
  있어 화이트리스트 추출이 유일한 방식이다. 키를 늘리려면 **이 표를 먼저 고친다**
- `calibration` 내용을 서버는 해석하지 않는다. 스키마 정본은 VMS
  (`calibration_store`) — 뭘 넣든 그대로 저장·재발행된다. JSON 문법이 틀리면
  `error` 로 거절된다
- "삭제" 개념은 없다 — 빈 값(`""` / `{}`)을 덮어쓰는 것이 "지움"이다
- **admin 전용** — 쓰기 명령 재검증 그대로
- ⚠ **요청 payload 전체 상한 16 KB** (calibration 은 수 KB 예상). 초과는
  `too_large` 로 거부한다. 상태 토픽도 자연히 같은 상한 아래에 있게 된다
- ⚠ `calibration` 안에 `token`·`reply_to` 같은 키가 있어도 **바깥 값이 이긴다** —
  서버가 `calibration` 객체를 먼저 잘라낸 뒤 봉투 필드를 읽는다(공격 payload 로
  실측 확인). 그래도 VMS 는 봉투 필드명을 `calibration` 안에 쓰지 않는 편이 좋다

`reason` 추가분: `too_large`(payload 상한 초과).

### 시각 표기 규칙 (프로젝트 전역, 08-11 확정)

| 필드 이름 | 형식 | 예 |
|---|---|---|
| `timestamp` | epoch **밀리초 정수** | `1785228000000` |
| `*_at` (`expires_at`·`updated_at`) | **ISO8601 · UTC · `Z` · 초 단위** | `"2026-09-10T01:22:31Z"` |

⚠ "ISO8601 UTC"라고만 정하면 부족하다 — Postgres 는 `timestamptz` 를 json 에
넣을 때 **접속 세션의 TimeZone 설정대로** 렌더해서 `+09:00` 이 섞여 나온다.
서버가 `to_char(… AT TIME ZONE 'UTC', …)` 로 형식을 코드에 못 박는다.

### 쓰기 명령의 `token` (단계 6)

`cmd/set_zone` · `cmd/set_actuator` · `cmd/set_fire_threshold` · `cmd/create_user`
· `cmd/set_user_enabled` · `cmd/set_site_config` · `cmd/list_users` 에 `token`
필드가 붙는다. 서버는 ①세션 조회 ②만료 확인 ③`role='admin'` 확인 후에만 실행하고,
거부는 `reason:"forbidden"`(만료·비활성은 `expired`/`disabled` 를 그대로 준다 —
VMS 가 "다시 로그인"과 "권한 없음"을 갈라야 한다).

- 과도기 스위치는 서버의 `REQUIRE_TOKEN`(기본 0). ⚠ **관대해지는 것은 "토큰
  없음"뿐이다** — 틀린 토큰은 이 값과 무관하게 언제나 거부된다. 그래서 스텁으로
  개발하는 동안에는 `token` 을 **아예 빼는 편이 낫다**(빈 문자열도 "없음"으로 친다).
- 🔴 **08-12: `REQUIRE_TOKEN=1` 을 실서버에 켰다.** 이제 **토큰 없는 쓰기 명령은
  전부 `forbidden`** 이다(무토큰 `create_user` 구멍도 함께 닫혔다). 위의 "스텁
  개발 중에는 토큰을 빼라"는 조언은 **더 이상 유효하지 않다** — 실서버에 붙는
  경로라면 토큰을 반드시 실어야 한다. `cmd/login`·`session_check`·`logout` 은
  면제라 토큰 없이 계속 동작한다
- ⚠ 과도기(`REQUIRE_TOKEN=0`)에는 요청자 신원이 없어 `set_user_enabled` 의
  `self_target` 검사가 성립하지 않았다. 켠 뒤로는 항상 성립한다
- `cmd/track_display` 는 **면제** — 저위험 자동 발행(추적 LED·keepalive)이라
  로그인 전에도 죽으면 안 된다. ACL 로만 묶는다.
- ⚠ `guardx/cmd/rpib/clear_fire` 는 **아직 토큰 검증이 없다.** 그건 `guardx_mqttd`
  가 아니라 `rpib_engine`(C)이 받는데 그 프로세스가 Pi 에 배포돼 있지 않아
  보류됐다. 당분간 통제는 브로커 ACL 뿐이다 — **결정이지 누락이 아니다.**

## 3.4 히트맵을 "날짜 단위"로 받는 이유 (v5의 핵심)

v4까지는 슬라이더를 움직일 때마다 집계를 요청했다. 그 결과:

- 드래그 한 번에 요청이 수십 개 발행
- 응답이 보낸 순서대로 오지 않아 화면이 엉뚱한 구간으로 튐
  (구 버전은 `q.exec()` 가 GUI를 멈춰 **순서가 공짜로 지켜지고 있었다**)
- 이를 막으려면 `req_id` 가드·디바운스·타임아웃을 전부 직접 만들어야 함

**요청 단위를 "날짜"로 바꾸면 그 복잡도의 근원이 사라진다.**

```
서버: 하루치를 10분 슬롯으로 집계해 한 번에 전송
VMS : 슬라이더 = 하루 안의 시각(00:00~23:5x, 144칸 고정)
      날짜는 우측 목록에서 다중 선택
      슬라이더·누적·프리셋·날짜선택 = 전부 캐시 위 계산 (네트워크 0)
```

크기 실측 (2026-07-28):

| | 값 |
|---|---|
| 하루치 | **5,341셀 ≈ 134 KB** |
| 보존기간 전체(14일)를 10분 단위로 받는다면 | ~2 MB (참고) |
| 5일 동시 선택 시 메모리 | ~27,000셀 |

요청이 **날짜당 1회**로 줄어 순서 역전 문제가 사실상 사라졌다. 여러 날짜를 동시에
요청해도 `req_id → 날짜` 대응만 들고 있으면 각 응답이 자기 날짜 캐시로 들어가므로
순서가 상관없다.

### 누적 토글의 의미 확장

별도의 "전체 보기" 모드를 두지 않고 기존 토글을 확장했다.

| 누적 | 날짜 | 슬라이더 | 결과 |
|---|---|---|---|
| OFF | 1일 | 14:00 | 그 10분 |
| OFF | N일 | 14:00 | **N일치 14:00 합산** (요일별 패턴 비교) |
| ON | 1일 | 14:00 | 그날 00:00~14:10 |
| ON | N일 | **끝** | **고른 날짜들의 하루 전체** |

네 조합이 모두 의미를 가진다.

**색 정규화는 손대지 않았다.** `FloorCanvas` 가 화면 최댓값 기준으로 정규화하므로
날짜를 겹쳐 카운트가 N배가 되어도 색 분포가 유지된다 — 평균을 따로 낼 필요가 없다.

---

# 4부 — VMS 코드 구조 (현재)

## 4.1 파일별 역할

| 파일 | 역할 |
|---|---|
| `mqtt_link.{h,cpp}` | 브로커 접속·재접속·구독 관리 (싱글턴) |
| `zone_config.{h,cpp}` | `zones` 구독 → 정원/임계 캐시 |
| `crowd_page.{h,cpp}` | `dates` 구독 + `heatday` 요청-응답 + 날짜별 캐시 |
| `credentials.{h,cpp}` | `[camera]` / `[mqtt]` — **`[database]` 없음** |
| `tools/db_zones_bridge.sh` | RPi B 대역: `zones`·`dates` 발행 |
| `tools/heatmap_query_service.sh` | RPi B 대역: `heatday` 요청 처리 |

## 4.2 `MqttLink` — 스레드를 만들지 않는다

```cpp
// QTimer 20ms — GUI 스레드에서 논블로킹 호출
mosquitto_loop(m_mosq, 0, 1);
```

콜백이 이 호출 안에서 동기 실행되므로 **핸들러가 GUI 스레드에 떨어진다.**
위젯을 건드리는 코드를 `QMetaObject::invokeMethod` 마샬링 없이 그대로 쓸 수 있다.

접속도 `mosquitto_connect_async()` 라 브로커가 꺼져 있어도 앱이 멈추지 않고,
실패 시 1s→2s→4s…30s 백오프로 재시도하며 재접속되면 등록된 토픽을 자동 재구독한다.

## 4.3 mosquitto 2.1 관련 함정 (실측)

| 증상 | 원인 | 대응 |
|---|---|---|
| `cjson/cJSON.h: No such file` | 2.1의 `<mosquitto.h>` 가 libcommon까지 끌어옴 | `<mosquitto/libmosquitto.h>` 만 포함 (`__has_include` 로 2.0 폴백) |
| `mosquitto_strerror` 사용 불가 | `libcommon_string.h` 가 단독 포함 안 되고 심볼도 별도 DLL | `mosq_err()` 매핑 함수를 직접 둠 |
| 실행 시 DLL 로드 실패 | 2.1은 common/cjson/pthread/OpenSSL로 쪼개짐 | POST_BUILD 복사 6종 |

## 4.4 빌드 시 주의

**헤더(`.h`)를 고쳤는데 이상하게 죽으면 클린 빌드부터 해볼 것.**
증분 빌드가 헤더 의존을 놓쳐 `mainwindow.cpp.obj` 가 옛 클래스 크기로 남으면
`new CrowdPage` 가 메모리를 덜 잡아 힙이 깨진다 (실제로 겪음).

```bash
cmake --build <builddir> --clean-first --parallel
```

빌드 전에 앱을 끌 것 — 실행 중이면 `LNK1104` 로 링크가 막힌다.

---

# 5부 — 남은 작업

## v8 — Postgres localhost 바인딩 ⬜ (마지막 단계)

> **상세 절차·영향 분석·롤아웃 계획은 `rpi_b/DB_ACCESS_HARDENING.md` 에 별도로
> 정리했다.** 여기서는 개요만 둔다.

```conf
# postgresql.conf
listen_addresses = 'localhost'
```

**완료 판정**: 외부에서 5432 접속이 막힌 상태로 VMS 전 화면이 동작.
여기까지 가야 `schema.sql:86` 의 전제("DB는 RPi B localhost 전용")가 지켜지고,
원래 목표인 "운영자 PC에서 DB 계정 제거"가 실제로 달성된다.

**선행 조건은 이미 갖춰졌다:**

| 무엇 | 어디서 해결 |
|---|---|
| VMS가 DB를 읽지 않음 | `v6` |
| 폴러가 대신 읽어 발행 | `v7` |
| 정원을 DBeaver 없이 고칠 수 있음 | `v9` ← **이게 없으면 v8 후 값을 못 바꾼다** |

⚠ **팀 확인 필요.** 다른 팀원이 DBeaver·스크립트로 그 DB에 붙고 있으면 전부
끊긴다. 필요하면 SSH 터널로 계속 볼 수 있다:

```bash
ssh -L 5432:localhost:5432 <user>@<RPi B> 
# 그 뒤 DBeaver를 localhost:5432 로 연결
```

## 운영 정리 (v8과 별개로 필요)

| 항목 | 왜 |
|---|---|
| 폴러를 systemd 서비스로 | 지금 수동 실행이면 터미널을 닫는 순간 전부 멎는다 |
| `juan` / `bangjunhan` 서비스 중복 제거 | 둘이 동시에 돌면 `detections` 가 2배로 쌓인다 |
| `std::cout << std::unitbuf` | journald도 파이프라 로그가 4KB씩 뭉쳐 늦게 나온다 |
| 규약 문서에 `guardx/db/*` 5개 등재 | `MQTT_ALERT_INTERFACE.md` §9 미결 3건과 **함께** 올릴 것 |

## 미규명

**2026-07-28 16:19에 `detections` 적재가 멎었던 원인.** 날짜 목록에 07-23~26이
비어 있는 것도 같은 문제일 수 있다. 파티션 소진이 유력하다 —
`guardx_maintain()` cron이 죽으면 INSERT가 "no partition" 으로 실패하는데
**폴러는 크래시 없이 로그만 남기고 계속 돈다**(`schema.sql` 주석).

```bash
sudo crontab -l | grep guardx_maintain
```

## 곁가지

| 항목 | 부류 | 난이도 |
|---|---|---|
| 캘린더 위젯 (날짜 목록 대체) | — | 낮음 |
| 자동 점유율 갱신 (`zone_occupancy`) | **A** | 낮음 — 폴러가 주기 발행, VMS는 구독만 |
| 객체 추적 조회 | **B** | 중 — v5의 요청-응답 틀 재사용 |
| 규약 문서에 `guardx/db/*` 4개 등재 | — | 낮음 |

> `MQTT_ALERT_INTERFACE.md` §9에 미결 3건이 이미 쌓여 있다. **우리 db 토픽도 함께
> 올려야** 토픽 네이밍이 제각각이 되지 않는다.

---

## 부록 A. 성능에 대한 정직한 정리

**조회형(히트맵·추적)은 MQTT가 DB 직결보다 빠르지 않다.** 브로커 중계·JSON
직렬화로 요청당 10~25ms 정도 느리다. LAN 왕복 횟수는 같다(추가 홉은 전부 RPi B
내부 localhost).

MQTT를 고른 이유는 성능이 아니다:

| 이유 | 성립 |
|---|---|
| 조회가 빨라진다 | ❌ |
| 운영자 PC에서 DB 계정 제거 | ✅ 원래 목표 |
| Postgres를 localhost로 묶을 수 있음 | ✅ |
| 자동 갱신이 폴링 없이 됨 | ✅ 구조적 이점 |
| 조회 인프라 재사용 (추적 등) | ✅ |

다만 v5의 날짜 단위 캐싱 덕에 **슬라이더 조작은 back_up보다 빠르다** —
back_up은 슬라이더를 움직일 때마다 무조건 DB를 다시 쳤다.

## 부록 B. 검증 명령

브릿지·쿼리서비스 실행 (각각 별도 터미널, `PGPASSWORD` 필요):

```bash
cd vms && ./tools/db_zones_bridge.sh
```

```bash
cd vms && ./tools/heatmap_query_service.sh
```

토픽 확인:

```bash
mosquitto_sub -h 127.0.0.1 -t 'guardx/db/#' -v
```

retained 메시지 삭제 (토픽 이름을 바꿨을 때 유령 값이 남지 않게):

```bash
mosquitto_pub -h 127.0.0.1 -t 'guardx/db/rpib/zones' -r -n
```

> ⚠ Windows PowerShell은 네이티브 exe에 인자를 넘길 때 문자열 안의 큰따옴표를
> 삼킨다. JSON을 `-m` 으로 넘기면 깨진 payload가 발행되므로 **Git Bash를 쓰거나
> `-f 파일` 을 쓸 것.**
