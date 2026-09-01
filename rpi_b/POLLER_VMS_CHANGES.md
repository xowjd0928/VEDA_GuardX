# RPi B 폴러 — VMS 연동을 위해 추가·변경한 구조

> 대상: RPi B 폴러 담당 · 코드 리뷰어
> 기준: 태그 `mqtt-v9` · 브랜치 `VEDA-138-VMS-DB-연동-MQTT-전환`
> 관련: `vms/docs/DB_LINK_AND_MQTT_MIGRATION.md`(설계), `DB_ACCESS_HARDENING.md`(v8)
> 작성: 2026-07-29

---

## 0. 왜 폴러를 건드렸나

VMS(Qt)가 Postgres에 **직접 붙어** 화면 데이터를 읽고 있었다. 그래서

- 운영자 PC마다 DB 계정이 배포되고
- Postgres를 외부에 열어둬야 했다 (`Database/schema.sql` 이 `camera_credentials`
  비밀번호를 평문 저장하며 근거로 든 "DB는 RPi B localhost 전용" 전제가 깨진 상태)

**조회 주체를 폴러로 옮기면** VMS는 MQTT만 쓰게 되고 DB 자격이 RPi B 안에만 남는다.
폴러는 이미 DB에 붙어 있고 MQTT도 쓰고 있었으므로, 새 프로세스 없이 태스크를
하나 추가하는 것으로 끝났다.

---

## 1. 변경 요약

> ⚠ 아래 경로는 **이 작업(v7) 당시 기준**이다. VEDA-155 v2에서 모듈이 갈리면서
> `Poller/task_vms.*` → `MqttDb/task_vms.*`, `Poller/mqtt_pub.*` → `Mqtt/mqtt_pub.*`
> 로 옮겨졌고 실행 파일도 `guardx_mqttd` 로 분리됐다. 현재 구조는
> `MQTT_SERVICE_SPLIT.md` 를 볼 것.

| 파일 | 상태 | 내용 |
|---|---|---|
| `include/Poller/task_vms.hpp` | **신규** | 인터페이스 2개 |
| `src/Poller/task_vms.cpp` | **신규** | 발행·조회·쓰기 전부 |
| `include/Poller/mqtt_pub.hpp` | 수정 | 함수 3개 추가 |
| `src/Poller/mqtt_pub.cpp` | 수정 | 구독 기능 + retained 발행 |
| `src/Poller/poller_main.cpp` | 수정 | 2곳 (INIT 1줄, LOOP 1줄) |
| `CMakeLists.txt` / `Makefile` | 수정 | 소스 등록 |

**기존 로직은 하나도 안 건드렸다.** `task_detections`·`task_alert` 등은 그대로다.

---

## 2. `mqtt_pub` — 발행 전용에서 양방향으로

### 2-1. 원래 있던 것

```cpp
bool mqttInit(const Config& cfg);
void mqttPublishAlert(int zone_id, ..., const char* severity, ...);
```

혼잡 경보(`guardx/alert/rpib`)를 **발행만** 했다. `retain=false`, 구독 기능 없음.

### 2-2. 추가한 것

```cpp
void mqttPublishRetained(const std::string& topic, const std::string& payload);
void mqttPublishReply(const std::string& topic, const std::string& payload);
void mqttSubscribe(const std::string& topic,
                   std::function<void(const std::string&)> handler);
```

**`retain=true` 가 이 프로젝트에 없던 패턴이다.**

| | alert | VMS 상태 토픽 |
|---|---|---|
| 성격 | **사건** — 지나간 걸 뒤늦게 받아봐야 의미 없음 | **상태** — "지금 값이 얼마인가"에 언제든 답해야 함 |
| retain | `false` | **`true`** |

retained 라야 VMS가 언제 켜지든 구독 즉시 현재 값을 받는다.

### 2-3. 내부 구현에서 주의한 것

**콜백 등록**
```cpp
mosquitto_connect_callback_set(g_m, onConnect);
mosquitto_message_callback_set(g_m, onMessage);
```

**재접속 시 재구독** — `clean_session=true` 라 브로커가 세션을 기억하지 않는다.
`onConnect` 에서 등록된 토픽을 일괄 재구독한다.

**핸들러 예외 차단** — 콜백이 예외를 던지면 **mosquitto 네트워크 스레드가 죽고
폴러 전체가 조용히 멎는다.** `try/catch` 로 막았다.

```cpp
try { h(payload); }
catch (const std::exception& e) {
  std::cerr << "[mqtt] 핸들러 예외 (" << msg->topic << "): " << e.what() << "\n";
}
```

**뮤텍스** — 핸들러 맵을 메인 스레드가 등록하고 네트워크 스레드가 읽는다.

---

## 3. `task_vms` — 신규 태스크

### 3-1. 인터페이스

```cpp
void publishVmsState(pqxx::connection& db);   // 주기 호출 (메인 루프)
void startVmsQueryService(const Config& cfg); // INIT 1회
```

### 3-2. 다루는 토픽 7개

| 토픽 | 방향 | 성격 | 언제 |
|---|---|---|---|
| `guardx/db/rpib/zones` | 발행 (retained) | 상태 | 값이 바뀔 때만 |
| `guardx/db/rpib/dates` | 발행 (retained) | 상태 | 값이 바뀔 때만 |
| `guardx/db/rpib/incidents` | 발행 (retained) | 상태 | 값이 바뀔 때만 (07-30 신설) |
| `guardx/db/rpib/query/heatday` | 구독 | 질의 | 요청 올 때 |
| `guardx/db/rpib/query/occseries` | 구독 | 질의 | 요청 올 때 (07-30 신설) |
| `guardx/db/rpib/query/incidents` | 구독 | 질의 | 요청 올 때 (07-30 신설) |
| `guardx/db/rpib/cmd/set_zone` | 구독 | **쓰기** | 명령 올 때 |

> **`incidents`(retained)가 왜 필요한가** (07-30): 경보(`guardx/alert/rpib`)는
> 상태가 *바뀔 때만* retain=false로 나간다. VMS는 `clean_session=true`라
> 경보 발생 뒤에 켜지면 이미 열린 critical을 **영영 모른다** — 화면은 평온한데
> 현장은 critical. 이 토픽이 "지금 열려 있는 것 전부"를 접속 즉시 건네준다.
> (transmission layer는 `clean_session=false` 권장이라 이 문제가 없다 —
> `MQTT_ALERT_INTERFACE.md` §2)
>
> ⚠ 이 스냅샷은 설정 틱(기본 30초)에만 재발행되므로 라이브 경보보다 **낡을 수
> 있다**. 수신 측은 payload의 `timestamp`를 채널별 마지막 라이브 수신 시각과
> 비교해 낡은 스냅샷을 무시해야 한다 (VMS `AlertFeed`가 그렇게 한다).

> **예측(predictions) 토픽은 의도적으로 없다** (2026-07-29 결정): VMS의
> 라이브 예측 표시는 detections 박스와 동일하게 **카메라 `/prediction`
> HTTP 직접 폴링** (실시간=카메라 직결, DB=이력·경보·MAE — detection_feed와
> 같은 패턴). MQTT 경유는 DB 자격 제거가 목적이지 카메라 접근 제거가 아님.
> 과거 예측 이력이 필요해지면 그때 부류 B(heatday식 질의)로 추가.

응답은 요청에 실려 온 `reply_to`(= `guardx/db/{client_id}/result`)로 보낸다.

### 3-3. payload를 Postgres가 만든다

```cpp
pqxx::result r = tx.exec(
    "SELECT json_build_object("
    "  'node_id', 'rpib', ..."
    "  'zones', coalesce(json_agg(json_build_object(...)), '[]'::json))::text"
    " FROM zones z JOIN zone_thresholds t USING (zone_id)");
```

**C++에서 문자열을 조립하지 않는다.** 이스케이프 실수가 원천적으로 없고,
VMS가 받는 형식이 **SQL 한 곳에만** 적혀 있어 규격이 흩어지지 않는다.

### 3-4. 스레드 분리 — 커넥션이 두 개다

```
메인 루프 스레드 ──> db (poller_main의 커넥션) ──> publishVmsState()
mosquitto 스레드 ──> g_qdb (task_vms 전용)     ──> heatday / set_zone 핸들러
```

**`pqxx::connection` 은 스레드 안전하지 않다.** `mqttInit` 이 `loop_start` 를
걸어 콜백이 네트워크 스레드에서 불리므로, 조회 서비스는 전용 커넥션을 따로 연다.

발행 dedup 상태(`g_last_zones`/`g_last_dates`)는 양쪽이 함께 만지므로 뮤텍스로 보호.

### 3-5. 값이 바뀔 때만 발행

```cpp
void publishIfChanged(const char* topic, const std::string& payload, std::string& last) {
  if (payload.empty() || payload == last) return;
  ...
}
```

같은 값을 30초마다 계속 쏘면 브로커 로그만 지저분해지고 구독자도 얻는 게 없다.
**DB를 안 건드리면 트래픽이 0이다.**

### 3-6. 히트맵은 "하루 단위"로 응답한다

요청 하나에 **하루치를 10분 슬롯으로 집계**해 통째로 보낸다.

```sql
SELECT floor(extract(epoch FROM (ts AT TIME ZONE $2::text)
                     - (SELECT day FROM d)::timestamp) / ($3::int * 60))::int AS s,
       channel, floor(ST_X(geom)/$4::int)::int AS gx, ...
FROM detections
WHERE ts >= ((SELECT day FROM d)::timestamp AT TIME ZONE $2::text)
  AND ts <  (((SELECT day FROM d) + 1)::timestamp AT TIME ZONE $2::text)
  ...
```

**왜 하루 단위인가**: VMS가 슬라이더를 움직일 때마다 요청하면 드래그 한 번에
수십 개가 나가고 응답 순서가 뒤바뀐다. 날짜당 1회로 줄이면 그 문제가 사라지고,
VMS는 캐시 위에서 슬라이더·누적·다중선택을 계산한다(네트워크 0).

**실측**: 하루치 5,341셀 ≈ **107 KB**. 보존 14일 전체를 10분 단위로 보내면 ~2MB.

⚠ **SQL 캐스트를 전부 명시했다.** `AT TIME ZONE`·나눗셈의 파라미터는 Postgres가
타입을 못 정해 `could not determine data type of parameter` 로 죽는다.

⚠ **타임존** — 날짜 경계를 `Asia/Seoul` 기준으로 자른다. VMS가 보는 "하루"와
같아야 하므로 양쪽이 같은 TZ를 써야 한다.

### 3-7. `set_zone` — 유일한 쓰기 경로

VMS가 DB에 직접 붙지 않고 정원을 고치는 방법이다.

```
[VMS] --cmd/set_zone--> [폴러] --UPDATE--> [DB]
                           └---zones 즉시 재발행---> [VMS] 화면 갱신
```

**UPDATE 직후 바로 재발행**하므로 다음 틱(기본 30초)을 기다리지 않고 1초 안에
VMS 화면이 바뀐다. **그 갱신 자체가 성공 확인**이라 별도 ack UI가 필요 없다.

같은 트랜잭션에서 새 값을 읽어 payload를 만든다 — commit 전 읽기라 방금 쓴 값이
그대로 반영된다.

#### 값 검증 — 현재 유일한 방어선

```cpp
if (zone_id <= 0)                    → 거부
if (cap < 1 || cap > 10000)          → 거부
if (warn <= 0.0 || warn >= 1.0)      → 거부
if (crit <= warn || crit > 1.0)      → 거부
if (up.affected_rows() == 0)         → 거부 (없는 zone_id)
```

| 안 막으면 | 결과 |
|---|---|
| `capacity = 0` | VMS에서 `OCC n/0` — 나눗셈이 깨진다 |
| `critical < warn` | 위험 단계가 주의보다 낮아짐 |
| 없는 `zone_id` | 조용히 성공 응답 후 아무 일도 안 일어남 |

⚠ **브로커가 평문·익명 허용이라 같은 LAN의 누구든 이 토픽에 쏠 수 있다.**
이 검증은 "잘못된 값"은 막지만 "권한 없는 사람"은 못 막는다. 실질적 통제는
mTLS 2단계(`common/certs/guardx_mtls.conf`)에서 온다.

---

## 4. `poller_main.cpp` — 2줄

### INIT
```cpp
if (zone_by_ch.empty()) { ... }

startVmsQueryService(cfg);   // ← 추가
```

### LOOP
```cpp
if (tick % cfg.cfg_interval_s == 0) {
  syncConfig(cfg, db, st, zone_by_ch);
  publishVmsState(db);       // ← 추가
}
```

`cfg_interval_s`(기본 30초) 틱을 `syncConfig` 와 공유한다. 정원·날짜목록은 거의
안 바뀌는 설정성 값이라 이 주기로 충분하다.

**정원 변경 반영 시간**: DB 수정 → 최대 30초(폴링) + ~1ms(MQTT) + 최대 0.8초
(VMS 오버레이 재그리기) = **평균 ~15초, 최악 ~31초**.
단 `set_zone` 으로 바꾸면 즉시 재발행이라 **1초 미만**이다.

빠르게 하려면 `config.env` 에 `export CFG_INTERVAL_S=10` — 다만 `syncConfig` 도
같은 틱이라 카메라 폴링이 함께 잦아진다.

---

## 5. 기동 시 확인할 로그

```
[vms] 조회 서비스 시작 — guardx/db/rpib/query/heatday
[vms] 설정 변경 수신 — guardx/db/rpib/cmd/set_zone
[vms] guardx/db/rpib/zones 발행 (461B)
[vms] guardx/db/rpib/dates 발행 (145B)
```

동작 중:
```
[vms] heatday 2026-07-28 응답 107339B
[vms] set_zone zone 2 -> cap 50 warn 0.75 crit 0.9
```

### ⚠ 로그가 안 보일 때 — 버퍼링

`std::cout` 은 출력이 **파이프**로 갈 때 라인 단위가 아니라 4KB 뭉치로 나간다.
`| tee` 나 journald(systemd)로 받으면 로그가 늦게 나온다.

```bash
./build/guardx_poller                       # 터미널 직접 — 즉시 보임
stdbuf -oL ./build/guardx_poller | tee log  # 파이프 + 라인 버퍼링
```

**권장 수정** (`poller_main.cpp` 맨 앞 한 줄):
```cpp
std::cout << std::unitbuf;
```

---

## 6. 빌드

```bash
cd ~/7th_VEDA_GROUP2/rpi_b && cmake -B build && cmake --build build -j
```

| 무엇을 바꿨나 | 필요한 것 |
|---|---|
| `.cpp` 내용만 | `cmake --build build -j` |
| 파일 추가/삭제, `CMakeLists.txt` | `cmake -B build` 부터 |

의존: `libpqxx-dev`, `libmosquitto-dev`, `libcurl4-openssl-dev`

---

## 7. 이 구조가 앞으로 쓰일 자리

새 기능을 붙일 때 **먼저 두 부류로 나눈다.**

| 부류 | 판별 | 방식 | 예 |
|---|---|---|---|
| **A. 상태** | 사용자가 뭘 고르든 답이 같다 | `mqttPublishRetained` + 주기 조회 | 자동 점유율 갱신(`zone_occupancy`) |
| **B. 질의** | 사용자 선택에 따라 답이 달라진다 | `mqttSubscribe` + `mqttPublishReply` | 객체 추적 조회 |

A는 `publishVmsState` 에 쿼리 하나 추가하면 끝이고,
B는 `handleHeatday` 를 복사해 SQL만 바꾸면 된다.
