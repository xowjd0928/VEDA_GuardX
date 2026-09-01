# Postgres 접근 통제 (localhost 바인딩) — 목적·절차·영향

> 대상: RPi B 운영자 · DB를 쓰는 모든 팀원
> 관련: `vms/docs/DB_LINK_AND_MQTT_MIGRATION.md` (VMS의 DB 직결 제거), `Database/schema.sql`
> 상태: **미적용** — 적용 시점은 팀 합의 필요 (아래 5절 롤아웃 계획)
> 작성: 2026-07-29

---

## 0. 한 장 요약

`postgresql.conf` 의 한 줄을 바꾸는 작업이다.

```conf
listen_addresses = '*'          # 지금 — LAN의 누구든 5432로 붙을 수 있다
listen_addresses = 'localhost'  # 목표 — RPi B 자기 자신에서만
```

| | 지금 | 적용 후 |
|---|---|---|
| RPi B의 폴러 → DB | ✅ | ✅ (localhost라 무관) |
| VMS → DB | 이미 안 붙음 | 이미 안 붙음 |
| **DBeaver·psql (다른 PC)** | ✅ | ❌ **차단** → SSH 터널로 대체 |
| 같은 LAN의 제3자 | ✅ 붙을 수 있음 | ❌ 차단 |

**코드 변경은 없다.** 설정 한 줄 + 재시작이고, 되돌리기도 같은 방식이다.

---

## 1. 왜 해야 하나

### 1-1. 스키마가 이미 그 전제로 쓰여 있다

`Database/schema.sql` 의 `camera_credentials` 주석:

> 비밀번호 평문 저장 — **DB는 RPi B localhost 전용** + digest 인증은 원문 필요.
> 접근 통제는 DB 권한으로 (§6에서 reader 의 SELECT 를 명시적으로 회수).
> **외부 노출 DB로 바뀌면 이 판단 재검토.**

카메라 admin 비밀번호를 **평문으로 저장한 근거가 "localhost 전용"** 이다.
그런데 실제로는 `listen_addresses = '*'` 라 LAN에 열려 있다.

**즉 스키마가 세운 전제가 현재 거짓이다.** 이 작업은 새 제약을 거는 게 아니라
**원래 있어야 했던 상태로 되돌리는 것**이다.

### 1-2. VMS의 DB 직결을 없앤 작업의 마지막 조각

| 단계 | 내용 | 상태 |
|---|---|---|
| `v6` | VMS 코드·설정에서 DB 자격 제거 | ✅ |
| `v7` | RPi B 폴러가 대신 조회해 MQTT로 발행 | ✅ |
| `v9` | VMS에서 정원 편집 (`cmd/set_zone`) | ✅ |
| **`v8`** | **Postgres를 localhost로 묶기** | ⬜ |

**v8 없이는 절반만 달성된 상태다.** VMS 소스에서 계정을 뺐어도, 계정 문자열을
아는 사람은 여전히 어디서든 붙을 수 있다. 실제로 통제가 걸리는 지점이 v8이다.

---

## 2. 무엇이 바뀌고, 누가 영향을 받나

### 2-1. 영향 없음

| 대상 | 이유 |
|---|---|
| RPi B 폴러 (`guardx_poller`) | `PGCONN` 이 `host=localhost` — 그대로 붙는다 |
| VMS (Qt) | v6부터 DB에 안 붙는다. MQTT만 쓴다 |
| RPi A / RPi C | DB를 쓰지 않는다 |
| MQTT 브로커 | 별개 프로세스·별개 포트(1883) |

### 2-2. 영향 있음 — **끊긴다**

| 대상 | 대체 방법 |
|---|---|
| 다른 PC의 **DBeaver** | SSH 터널 (4-3절) |
| 다른 PC의 `psql` | 〃 |
| 외부에서 도는 스크립트·리포트 도구 | 〃 또는 RPi B로 이전 |

**정원·임계 수정은 이미 대체됐다** — VMS의 `ZONE SETTINGS` 화면(v9)에서
바꾸면 폴러가 DB를 갱신한다. DBeaver가 없어도 운영에 지장이 없다.

### 2-3. ⚠ 오해하기 쉬운 것 — "MQTT가 DB 접근을 대체한다"가 아니다

```
MQTT      = VMS가 화면에 쓸 데이터를 받는 경로 (읽기 + 정원 편집)
SSH 터널  = 사람이 DB를 조회·관리하는 경로 (임의 SQL)
```

MQTT로는 **미리 정해둔 것만** 주고받는다(`zones`·`dates`·히트맵·`set_zone`).
"detections 몇 건 쌓였나", "파티션이 어디까지 있나" 같은 **임의 조회는 여전히
DB에 붙어야 하고**, 그건 SSH 터널로 한다.

---

## 3. 선행 조건 (전부 충족돼야 한다)

- [x] VMS가 DB에 붙지 않음 (`mqtt-v6`)
- [x] 폴러가 조회를 대행 (`mqtt-v7`) — 실기 검증
- [x] VMS에서 정원 편집 가능 (`mqtt-v9`) — 실기 검증
- [ ] **팀 확인**: 외부에서 5432에 붙는 다른 도구·사람이 없는가 (6절 체크리스트)
- [ ] 폴러가 systemd로 상시 실행 중 (수동 실행이면 터미널을 닫는 순간 멎는다)

마지막 두 개가 남았다.

---

## 4. 실행 절차

### 4-1. 현재 값 확인

```bash
sudo -u postgres psql -c "SHOW listen_addresses;"
```

`*` 또는 IP 목록이면 외부에 열려 있는 상태다.

지금 누가 붙어 있는지도 함께 본다 (끊길 대상을 미리 파악):

```bash
sudo -u postgres psql -c "SELECT usename, client_addr, application_name, state FROM pg_stat_activity WHERE client_addr IS NOT NULL;"
```

`client_addr` 가 `127.0.0.1` 이 아닌 행 = **v8 후 끊긴다.**

### 4-2. 변경

```bash
sudo nano /etc/postgresql/*/main/postgresql.conf
```

```conf
listen_addresses = 'localhost'
```

```bash
sudo systemctl restart postgresql
```

> `reload` 로는 안 된다 — `listen_addresses` 는 재시작이 필요한 항목이다.

### 4-3. DBeaver를 계속 쓰려면 — SSH 터널

접속하려는 PC에서:

```bash
ssh -L 5432:localhost:5432 <계정>@<RPi B 주소>
```

이 창을 **켜둔 채로** DBeaver를 `localhost:5432` 로 연결한다.

- 트래픽이 SSH로 암호화된다
- **SSH 계정이 있는 사람만** 접근할 수 있다 (LAN의 아무나가 아니라)
- 로컬 5432가 이미 쓰이면 `-L 15432:localhost:5432` 처럼 다른 포트로

DBeaver에는 SSH 터널 기능이 내장돼 있어 연결 설정의 **SSH 탭**에서 직접
설정해도 된다 (별도 터미널 불필요).

---

## 5. 검증

### 5-1. 안에서는 되어야 한다 (RPi B)

```bash
psql -h localhost -U guardx_reader -d guardx -c "SELECT count(*) FROM zones;"
```

### 5-2. 밖에서는 막혀야 한다 (다른 PC)

```bash
timeout 3 bash -c "echo > /dev/tcp/<RPi B 주소>/5432" && echo "아직 열림 ❌" || echo "차단됨 ✅"
```

### 5-3. 폴러가 계속 돌아야 한다

```bash
journalctl -u guardx-poller -n 20 --no-pager | grep -E "det|vms"
```

`[det] +N rows` 와 `[vms] ... 발행` 이 계속 나오면 정상.

### 5-4. VMS 전 화면이 동작해야 한다

| 화면 | 확인 |
|---|---|
| LIVE | OCC 배지의 분모가 표시됨 |
| CROWD | 날짜 목록·히트맵 정상 |
| ZONE SETTINGS | 정원을 바꾸고 [적용] → 1초 안에 OCC 반영 |

**이 셋이 되면 v8 완료다.**

---

## 6. 롤백

문제가 생기면 되돌리는 데 30초면 된다.

```bash
sudo sed -i "s/^listen_addresses = 'localhost'/listen_addresses = '*'/" /etc/postgresql/*/main/postgresql.conf
sudo systemctl restart postgresql
```

**데이터에 영향이 없다.** 접속 허용 범위만 바뀌는 설정이라 스키마도 행도
건드리지 않는다.

---

## 7. 롤아웃 계획 (권장)

지금 바로 닫지 않고 단계를 두는 편이 안전하다.

| 단계 | 상태 | 기간 | 목적 |
|---|---|---|---|
| **1. 현행 유지** | 5432 열림 + MQTT 동작 | 지금 | VMS 경로를 실사용으로 검증 |
| **2. 공지 + 터널 안내** | 열림 | 며칠 | 팀원이 SSH 터널로 옮길 시간 |
| **3. main 머지 후 적용** | **localhost** | — | 브랜치가 정식화된 뒤 |
| 4. 브로커 보안 (mTLS) | — | 이후 | 8절 참조 |

**3단계를 main 머지 이후로 두는 이유**: 브랜치 상태에서 닫으면, 되돌릴 때
"코드 롤백 + 설정 롤백"을 같이 해야 해서 상태가 얽힌다. 코드가 정식화된 뒤
설정만 따로 바꾸는 편이 단순하다.

### 팀 확인 체크리스트

공지 시 물어볼 것:

- [ ] 외부에서 5432로 붙는 도구·스크립트가 있는가
- [ ] DBeaver를 쓰는 사람이 몇 명인가 (SSH 계정이 다 있는가)
- [ ] 정기 리포트·백업이 외부에서 도는가
- [ ] `camera_app/` 등 다른 모듈이 DB에 직접 붙는가

`pg_stat_activity` 조회(4-1절)로 **실제 붙어 있는 것**을 먼저 확인하면
누락이 줄어든다.

---

## 8. ⚠ v8을 해도 남는 위험 — MQTT 브로커

**정직하게 적어둔다. v8은 DB만 닫는다. MQTT는 여전히 열려 있다.**

`rpib_app/broker/guardx_broker.conf`:

```conf
listener 1883 0.0.0.0
allow_anonymous true
```

파일 주석도 스스로 이렇게 경고한다:

> ⚠ 테스트 전용 — **같은 LAN의 누구든 액추에이터 명령을 쏠 수 있는 상태.**
> 2단계 전환 전까지만

즉 v8 후에도 같은 LAN에서 이런 게 가능하다:

```bash
mosquitto_pub -h <RPi B> -t guardx/db/rpib/cmd/set_zone -m '{...}'   # 정원 변경
mosquitto_sub -h <RPi B> -t 'guardx/db/#'                            # 데이터 열람
```

**현재 방어선은 폴러의 값 검증뿐이다** (`task_vms.cpp` — capacity 범위,
ratio 대소, zone_id 존재 확인). 이건 "잘못된 값"은 막지만 "권한 없는 사람"은
못 막는다.

### 다음 단계 (별건)

`common/certs/guardx_mtls.conf` 에 2단계가 준비돼 있다:

```conf
listener 8883
require_certificate true       # 클라이언트도 인증서를 제시해야 함
use_identity_as_username true
```

전환하면 VMS도 `mosquitto_tls_set()` 과 인증서 3종(ca/cert/key)이 필요하다.
**RPi A/B/C/VMS가 함께 넘어가야 하는 작업**이라 팀 일정에 맞춰야 한다.

**정리하면 보안 단계는 이렇다:**

```
현재    DB 열림 + MQTT 열림          ← 아무나 다 됨
v8      DB 닫힘 + MQTT 열림          ← DB는 막았으나 MQTT로 우회 가능
mTLS    DB 닫힘 + MQTT 인증서 필요    ← 실질적 통제
```

v8은 **필요하지만 충분하지 않다.** 그렇다고 미룰 이유는 없다 — 공격면이
둘에서 하나로 줄고, 평문 비밀번호를 담은 테이블이 가려진다.

---

## 부록. 왜 방화벽이 아니라 `listen_addresses` 인가

둘 다 가능하지만 `listen_addresses` 가 낫다.

| | `listen_addresses='localhost'` | 방화벽(ufw/iptables) |
|---|---|---|
| Postgres가 포트를 여나 | **안 연다** | 연다 (방화벽이 앞에서 막음) |
| 방화벽 규칙이 지워지면 | 여전히 안 열림 | **노출됨** |
| 설정 위치 | DB 설정 한 곳 | 별도 시스템 설정 |
| 실수 여지 | 적음 | 규칙 순서·인터페이스 지정 등 |

**포트를 아예 열지 않는 쪽이 더 단순하고 덜 깨진다.** 방화벽은 추가 방어선으로
같이 쓸 수 있지만, 대체재는 아니다.
