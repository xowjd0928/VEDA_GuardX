# VMS 조회 서비스 분리 — v1(프로세스) · v2(모듈)

> 브랜치: `VEDA-155-MQTT-폴러-조회-서비스-분리`
> 작성: 2026-08-03
> 관련: `POLLER_VMS_CHANGES.md`(v7 도입 당시), `vms/docs/DB_LINK_AND_MQTT_MIGRATION.md`(토픽 규약), `DB_ACCESS_HARDENING.md`

| 단계 | 내용 | 상태 |
|---|---|---|
| **v1** | 실행 파일 분리 (`guardx_mqttd` 신설) | ✅ 실기 검증 (2026-08-03) |
| **v2** | 모듈 분리 (`MqttDb`·`Mqtt` 신설) | ✅ 코드 반영 |

---

## 0. 한 줄 요약

`guardx_poller` 하나가 하던 일을 **두 프로세스로 나눴다.** 카메라 폴링이 멈춰도
VMS의 조회·정원 편집은 계속 동작한다. **토픽·페이로드 규약은 그대로라 VMS 코드는
한 줄도 바뀌지 않는다.**

```
[전]  guardx_poller ─┬─ 카메라 폴링 → DB 쓰기
                     └─ VMS 통로 (조회 응답 + 상태 발행)   ← 카메라가 죽으면 같이 죽음

[후]  guardx_poller ─── 카메라 폴링 → DB 쓰기 + 경보 판정
      guardx_mqttd  ─── VMS 통로 (조회 응답 + 상태 발행)   ← 카메라와 무관하게 상시 실행
```

**v1 검증 결과 (2026-08-03)**: `systemctl stop guardx-poller` 상태에서
`guardx_mqttd` 만으로 VMS의 CROWD 히트맵 조회가 정상 동작하는 것을 확인했다.
카메라 폴러와 mqttd 를 모두 끄면 조회가 실패하고, mqttd 만 켜면 다시 살아난다 —
조회 경로가 실제로 이 프로세스에 있다는 뜻이다.

---

## 1. 왜 했나

`task_vms.cpp`(VMS 조회 대체, v7)는 카메라와 아무 상관이 없는 코드다. CROWD 날짜
조회·히트맵·ZONE SETTINGS 정원 편집·열린 incident 복원은 **과거 데이터와 설정을
다루는 기능**이라 카메라가 꺼져 있어도 되어야 한다.

그런데 한 프로세스에 얹혀 있어서, 카메라 쪽 사정으로 폴러가 죽으면 VMS 화면 절반이
같이 멎었다. 실제로 죽는 경로가 여럿이다:

| 위치 | 상황 |
|---|---|
| `poller_main.cpp:49` | `pqxx::connection` 생성 실패 시 예외 미처리 → 프로세스 즉사 |
| `poller_main.cpp` 존 매핑 | 폴링 가능한 채널이 없으면 `return 1` |
| 카메라 비밀번호 없음 | `return 1` |

증상은 VMS 쪽에서 이렇게 보였다 (2026-08-02 로그):

```
[MqttLink] 응답 없음(타임아웃): "guardx/db/rpib/query/heatday"
[CrowdPage] 집계 실패 QDate("2026-07-31") "응답 없음 (타임아웃)"
```

이때 `zones`는 정상 수신됐는데, **retained 메시지라 브로커가 들고 있던 과거 값**이라
폴러 생존의 증거가 아니었다.

---

## 2. 무엇을 바꿨나

### 2-1. 신규 파일

| 파일 | 역할 |
|---|---|
| `src/mqtt_main.cpp` | `guardx_mqttd` 진입점 (`main` → `runMqttService()` 호출만) |
| `src/MqttDb/mqtt_service.cpp` | 본체 — 구독 등록 + 상태 발행 루프 + DB 재연결 |
| `include/MqttDb/mqtt_service.hpp` | 위 선언 |
| `systemd/guardx-mqttd.service` | 유닛 파일 |

`src/main.cpp` ↔ `src/Poller/poller_main.cpp` 의 대칭 구조를 그대로 따랐다.

### 2-2. v1에서는 `task_vms.cpp`를 옮기지 않았다

동작 분리와 자리 이동을 한 커밋에 섞으면 diff가 커져 실제 변경이 리뷰에서
안 보인다. **어느 프로세스에 속하는가는 파일 위치가 아니라 어느 실행 파일에
링크되느냐로 정해지므로**, v1은 CMake 타깃만 나누고 파일은 `src/Poller/` 에
그대로 뒀다. 자리 정리는 v1 검증이 끝난 뒤 v2로 분리했다 (4절).

### 2-3. 폴러에서 뺀 것 (`poller_main.cpp`)

```
- startVmsQueryService(cfg);      → guardx_mqttd 로 이사
- publishVmsState(db);            → guardx_mqttd 로 이사
+ subscribeZoneChangedSignal();   → 임계 변경 신호만 받는다 (3-2 참조)
```

### 2-4. MQTT client_id 분리

```cpp
- g_m = mosquitto_new("rpib-poller", true, nullptr);   // 하드코딩
+ bool mqttInit(const Config& cfg, const std::string& client_id);
```

`guardx_poller` = `rpib-poller`, `guardx_mqttd` = `rpib-mqttd`.

**이게 이번 작업에서 제일 중요한 한 줄이다.** 브로커는 client id 하나당 연결
하나만 허용해서, 같은 id로 두 프로세스가 붙으면 서로를 끊어내며 무한 재접속에
빠진다. 로그에는 "붙었다 끊겼다"만 반복돼 원인 찾기가 오래 걸린다.

### 2-5. 조회 커넥션 재연결 (`MqttDb/task_vms.cpp`)

전에는 시작할 때 한 번 연결하고, 실패하면 **구독 자체를 안 하고 return** 했다.
폴러에 얹혀 있을 땐 프로세스가 같이 죽고 systemd가 되살려서 가려졌던 문제인데,
상시 서비스로 독립하면 정면으로 드러난다 — Postgres가 한 번 재시작되면 그 뒤로
영영 침묵한다.

```cpp
+ bool ensureQdb();   // 없거나 끊겼으면 다시 연다. 핸들러 진입 시마다 확인
```

DB가 안 붙어 있어도 **구독은 건다.** 침묵하면 VMS는 타임아웃까지 기다리기만
하지만, 구독해 두면 최소한 `ok:false + error` 로 이유를 답할 수 있다
(`task_vms.cpp` 의 "실패해도 반드시 응답을 보낸다" 원칙과 같은 이유).

---

## 3. 설계 판단 두 가지

### 3-1. 상태 발행 주체는 한 곳 (guardx_mqttd)

같은 retained 토픽을 두 프로세스가 쏘면 **어느 쪽 값이 최종인지 알 수 없다.**
지금은 둘 다 같은 DB를 읽어 값이 같으니 실질적 해는 없지만, 한쪽만 배포가 밀리는
순간(실제로 이번에 juan/bangjunhan 체크아웃이 갈렸다) 옛 코드가 쏜 값이 새 값을
덮어쓴다. 그래서 `zones`·`dates`·`incidents` 발행은 전부 mqttd 로 넘겼다.

> 참고: `publishIfChanged` 의 "바뀐 것만 발행"은 **지금 작동하지 않는다.**
> `queryZones` 등이 payload 에 `timestamp`(현재 시각)를 넣어 매 주기 문자열이
> 달라지기 때문이다. 결과적으로 30초마다 그냥 재발행되는데, retained 덮어쓰기라
> 무해하고 VMS 동작에도 영향이 없다. 고칠 거면 비교 대상에서 `timestamp` 를
> 빼야 한다 (이번 범위 밖).

`guardx/alert/rpib`(경보)은 **카메라 판정 결과**라 폴러에 남는다. 즉 폴러도 MQTT
발행은 계속 한다 — 분리한 건 "MQTT 사용"이 아니라 "VMS 질의응답 책임"이다.

### 3-2. set_zone 즉시 재판정 — 유일하게 끊겼던 연결선

전에는 `handleSetZone`(네트워크 스레드)이 메모리 큐에 zone_id를 넣고 폴러 루프가
`takeRealertZones()` 로 꺼내 갔다. 프로세스가 갈리면 이 큐가 안 통한다.

내부 토픽으로 대체했다:

```
guardx/db/rpib/evt/zone_changed   {"node_id":"rpib","zone_id":N}   retain=false
   guardx_mqttd (발행)  →  guardx_poller (구독 → 큐 → 다음 1초 틱에 재판정)
```

- **retained 하지 않는다** — 지나간 신호를 재기동 때 다시 실행하면 안 된다
- 폴러가 꺼져 있으면 신호는 사라지지만, 그때는 재판정할 대상 자체가 없어 무해하다
- VMS는 이 토픽을 모른다 (노드 간 내부 신호)

---

## 4. v2 — 모듈 분리

v1은 실행 파일만 갈랐고 파일은 전부 `Poller/` 에 있었다. v2는 폴더와 라이브러리를
갈라 **디렉터리 구조가 실제 구조를 말하도록** 했다. 동작 변경은 없다.

### 4-1. 새 모듈 배치

```
include/ src/
  Config/    Database/  Storage/    설정·DB 헬퍼·커서 (기존)
  Mqtt/      ← 신규     브로커 전송 계층 (mqtt_pub, topics)   ─ 양쪽 공용
  Poller/               카메라 → DB 적재 + 경보 판정          ─ guardx_poller
  MqttDb/    ← 신규     DB ↔ VMS 질의응답 + 상태 발행         ─ guardx_mqttd
```

| 옮긴 것 | 전 | 후 |
|---|---|---|
| 조회 서비스 | `Poller/task_vms.*` | `MqttDb/task_vms.*` |
| mqttd 본체 | `Poller/mqtt_service.*` | `MqttDb/mqtt_service.*` |
| MQTT 전송 | `Poller/mqtt_pub.*` | `Mqtt/mqtt_pub.*` |
| 진입점 | `src/mqtt_main.cpp` | 그대로 (실행 파일 진입점은 `src/` 에 모아 둔다) |

### 4-2. 재판정 신호의 절반을 폴러로 되돌렸다

v1에서는 신호를 받는 쪽(`subscribeZoneChangedSignal`·`takeRealertZones`)이
`task_vms.cpp` 에 남아 있었다. 그러면 **guardx_poller 가 VMS 조회 서비스 전체를
링크해야 한다** — 안 쓰는 코드가 폴러 바이너리에 딸려오고, CMake 의존 그래프가
"폴러는 MqttDb 에 의존한다"는 거짓말을 하게 된다.

큐를 소비하는 것은 `pollAlert` 이고 그건 카메라 폴러의 일이므로, 받는 절반을
`Poller/realert_signal.{hpp,cpp}` 로 옮겼다. 양쪽이 같은 토픽 문자열을 봐야 해서
상수는 공용 모듈인 `Mqtt/topics.hpp` 에 둔다.

```
MqttDb/task_vms.cpp  ── 발행 ──► TOPIC_ZONE_CHANGED ── 구독 ──► Poller/realert_signal.cpp
                                (Mqtt/topics.hpp)
```

결과로 **`poller_main.cpp` 는 이제 MqttDb 를 include 하지 않는다.**

### 4-3. 라이브러리 3분할

```cmake
mqtt_core     ← mosquitto                    (양쪽 공용)
camera_core   ← curl + pqxx + mqtt_core      → guardx_poller
mqttdb_core   ← pqxx + mqtt_core             → guardx_mqttd   # curl 없음
```

`camera_core` 와 `mqttdb_core` 는 **서로를 참조하지 않는다.** 둘 사이의 유일한
대화는 브로커를 지나는 신호 하나뿐이고, 링크 그래프가 그 사실을 그대로 보여준다 —
나중에 누가 실수로 다시 붙이면 빌드가 막아준다.

---

## 5. 배포 절차 (RPi B)

```bash
cd ~/7th_VEDA_GROUP2/rpi_b
cmake -B build && cmake --build build -j        # guardx_poller + guardx_mqttd
sudo cp systemd/guardx-mqttd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now guardx-mqttd
sudo systemctl restart guardx-poller            # client_id 변경분 반영
```

> `guardx-mqttd.service` 의 `WorkingDirectory`·`ExecStart` 경로는 실제 배포
> 경로에 맞춰 확인할 것. `Config::load()` 가 `config.env` 를 **상대경로**로
> 읽으므로 WorkingDirectory 가 틀리면 PGCONN 이 기본값(비밀번호 없음)으로
> 떨어져 DB 인증이 실패한다.

`guardx-poller.service.d/hardening.conf` drop-in 이 있다면 같은 내용을
`guardx-mqttd.service.d/` 에도 복사하는 것을 권장한다.

---

## 6. 검증

### 6-1. 두 프로세스가 서로 안 끊는지 (client_id)

```bash
journalctl -u guardx-mqttd -u guardx-poller -f | grep -E "client_id|구독|연결"
```

`[mqtt] client_id=rpib-mqttd` / `rpib-poller` 가 각각 한 번씩 뜨고, 재접속이
반복되지 않아야 한다.

### 6-2. **분리 성공 판정 — 카메라 폴러를 끄고 VMS 확인**

```bash
sudo systemctl stop guardx-poller
```

| 화면 | 기대 |
|---|---|
| CROWD | 날짜 목록·히트맵 정상 조회 |
| ZONE SETTINGS | 정원 변경 → 저장 성공 |
| LIVE | OCC 배지 분모(정원) 표시 |

**이 셋이 되면 v1 완료다.** (실시간 인원·경보는 카메라 폴러 소관이라 안 도는 게 정상)

```bash
sudo systemctl start guardx-poller
```

### 6-3. 즉시 재판정 신호

정원을 낮춰 경보가 뜨는 값으로 바꾼 뒤:

```bash
journalctl -u guardx-poller -n 20 | grep "realert"
```

`[realert] 임계 변경 신호 — zone N 즉시 재판정` 이 1초 안에 찍히면 정상.

### 6-4. DB 재연결

```bash
sudo systemctl restart postgresql
```

`[mqttd] DB 연결 실패 … 재시도` → `[mqttd] DB 연결됨` 으로 스스로 회복하고,
그 뒤 VMS 조회가 정상 동작해야 한다. (전에는 여기서 영영 침묵했다)

---

## 7. 롤백

```bash
sudo systemctl disable --now guardx-mqttd
git checkout backup/VEDA-155-before-split -- rpi_b/
cmake --build build -j && sudo systemctl restart guardx-poller
```

DB 스키마도 토픽 규약도 안 건드렸으므로 코드만 되돌리면 끝이다.

---

## 8. 남은 과제 (이번 범위 밖)

- **`poller_main.cpp:49` 의 예외 미처리** — `pqxx::connection` 생성 실패 시
  `terminate` 로 죽는다. 아래 `if (!db.is_open())` 검사에는 도달하지 못한다.
  mqttd 쪽은 이번에 재시도 루프를 넣었지만 폴러는 그대로다.
- ~~`poller_core` 분리~~ — **v2에서 완료** (4-3절)
- **`publishIfChanged` 의 timestamp** — 3-1절 참고. 중복 제거가 사실상 안 걸린다.
  고치려면 비교용 payload 에서 `timestamp` 를 빼야 한다.
- **브로커 인증** — `allow_anonymous true` 라 같은 LAN의 누구든
  `cmd/set_zone` 을 쏠 수 있다. 프로세스를 나눠도 이 문제는 그대로다
  (`DB_ACCESS_HARDENING.md` 8절, mTLS 전환 건).
- **DB 권한 분리** — mqttd 는 읽기 + `zone_thresholds` UPDATE 만 있으면 되므로
  폴러와 다른 계정을 줄 수 있게 됐다. PGCONN 을 프로세스별로 나누면 된다.
