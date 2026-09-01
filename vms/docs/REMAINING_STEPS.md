# 남은 단계 — 무엇이 끝났고 무엇이 남았나

> 대상: 팀 전체 · 다음 작업자
> 기준: 태그 `mqtt-v9` · 브랜치 `VEDA-138-VMS-DB-연동-MQTT-전환`
> 관련: `DB_LINK_AND_MQTT_MIGRATION.md`(설계) · `rpi_b/POLLER_VMS_CHANGES.md`(폴러 변경)
>       · `rpi_b/DB_ACCESS_HARDENING.md`(v8 절차) · `VMS_CODE_MAP.md`(VMS 구조)
> 작성: 2026-07-29

---

## 0. 한 장 요약

**VMS의 DB 직결 제거는 코드상 완료됐다.** 남은 것은 배포·운영·보안 정리다.

```
[DB] ──✅──> [RPi B 폴러] ──✅──> [브로커] ──✅──> [VMS]
  ↑                                              DB 쿼리 0개
  └── 아직 LAN에 열려 있음 (v8 미적용)
```

| 구분 | 항목 | 상태 |
|---|---|---|
| 코드 | VMS 4개 쿼리 → 0개 | ✅ `mqtt-v6` |
| 코드 | 폴러가 조회 대행 | ✅ `mqtt-v7` 실기 검증 |
| 코드 | VMS에서 정원 편집 | ✅ `mqtt-v9` 실기 검증 |
| **운영** | **폴러 systemd 정리** | ⬜ **가장 급함** |
| **운영** | **RPi C `receive.sh` systemd 미등록** (08-10 확인) | ⬜ 같은 부류 — 창 닫으면 방송 수신이 멎고 재부팅 후 안 올라온다. 담당자 확인 필요 |
| **운영** | **RPi A 센서 발행 정지** (08-10 실측) | ⬜ MQTT 40초 청취에 `guardx/db/rpib/sensors` 실시간 **0건**(retained만). VMS 는 `RPi A 점: 정상 → 끊김` 으로 정확히 표시 중 |
| 절차 | PR·규약 등재 | ⬜ |
| 보안 | Postgres localhost 바인딩 (v8) | ⬜ **차후** |
| 보안 | MQTT mTLS 전환 | ⬜ 팀 전체 일정 |

---

## 1. 🔴 지금 급한 것 — 폴러 systemd 정리

### 문제

폴러가 **터미널에서 수동 실행 중**이면 창을 닫는 순간 전부 멎는다.
데모·시연 중에 끊길 수 있고, 재부팅하면 안 올라온다.

게다가 계정이 둘이다:

| 계정 | 상태 |
|---|---|
| `juan` | 기존 systemd 서비스 (`guardx-poller`) |
| `bangjunhan` | 현재 작업·수동 실행 중 |

**둘이 동시에 돌면 같은 카메라를 두 번 폴링해 `detections` 가 2배로 쌓인다.**

### 확인

```bash
systemctl cat guardx-poller | grep -E "User|ExecStart|WorkingDirectory"
systemctl is-active guardx-poller
```

### 조치 (둘 중 하나)

**A. 기존 서비스에 코드를 맞춘다** — 서비스가 `/home/juan/...` 를 본다면
그쪽에 배포하고 재시작. 계정 하나로 유지되어 단순하다.

**B. 새 유닛을 만들고 기존 것을 끈다**

```ini
# /etc/systemd/system/guardx-poller.service
[Unit]
Description=GuardX RPi B Poller
After=network-online.target postgresql.service mosquitto.service

[Service]
Type=simple
User=bangjunhan
WorkingDirectory=/home/bangjunhan/7th_VEDA_GROUP2/rpi_b
ExecStart=/home/bangjunhan/7th_VEDA_GROUP2/rpi_b/build/guardx_poller
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now guardx-poller
journalctl -u guardx-poller -f
```

⚠ `WorkingDirectory` 가 중요하다 — 폴러는 `config.env` 와 `state/` 를
**상대경로**로 찾는다.

**어느 쪽이든 반드시 하나만 남길 것.**

---

## 2. 🟡 절차 정리

### 2-1. PR 생성

브랜치 `VEDA-138-VMS-DB-연동-MQTT-전환` → `main`.
Jira **VEDA-138** 과 연결.

RPi B 쪽(v7·v9)을 **별도 티켓으로 나누는 것**도 고려. 팀이 모듈 단위로
티켓을 끊는 편이라(VEDA-137 `RPIB_POLLER_V1.0` 등) 그쪽이 맞을 수 있다.

### 2-2. 통신 규약 문서에 토픽 등재

`guardx/db/*` 5개가 아직 팀 규약에 없다.

```
guardx/db/rpib/zones            QoS1 retained   상태
guardx/db/rpib/dates            QoS1 retained   상태
guardx/db/rpib/query/heatday    QoS1            질의(요청)
guardx/db/rpib/cmd/set_zone     QoS1            쓰기(명령)
guardx/db/{client_id}/result    QoS1            응답
```

⚠ `rpi_b/MQTT_ALERT_INTERFACE.md` §9에 **미결 3건이 이미 쌓여 있다.**
(alert 토픽 정식 등재, node_id 의미, camera 토픽) — **함께 올려야** 또
미결로 남지 않는다.

함께 명시할 것:
- **client_id 예외**: 규약은 "client id = node id"지만 VMS는 여러 대가 뜰 수
  있어 `vms-{hostname}` 을 쓴다. 폴러도 이미 `rpib-poller` 로 예외다.
- **retained 사용**: 이 프로젝트에서 처음 쓴 패턴. "상태"와 "사건"의 구분.

### 2-3. 로그 버퍼링 (한 줄)

`poller_main.cpp` 맨 앞:
```cpp
std::cout << std::unitbuf;
```

journald가 파이프로 받아 로그가 4KB씩 뭉쳐 늦게 나온다. 장애 볼 때 불편하다.

---

## 3. 🟢 차후 — Postgres localhost 바인딩 (v8)

**지금 하지 않는다.** 절차·영향·롤백은 `rpi_b/DB_ACCESS_HARDENING.md` 에 따로
정리해뒀다. 여기서는 "언제, 왜 미루는가"만 적는다.

### 무엇인가

```conf
# postgresql.conf
listen_addresses = 'localhost'   # 지금은 '*'
```

RPi B 안에서만 Postgres에 붙을 수 있게 한다. **설정 한 줄이고 코드 변경이 없다.**

### 왜 해야 하나

`Database/schema.sql` 이 `camera_credentials` 비밀번호를 평문 저장하며 근거로
"DB는 RPi B localhost 전용"을 든다. **실제로는 LAN에 열려 있어 그 전제가 거짓이다.**
v8은 새 제약이 아니라 원래 전제를 사실로 만드는 작업이다.

### 왜 지금 미루나

| 이유 | 설명 |
|---|---|
| **main 머지 후가 안전** | 브랜치 상태에서 닫으면 롤백 시 코드와 설정을 함께 되돌려야 해 상태가 얽힌다 |
| **팀 확인 필요** | 다른 팀원의 DBeaver·스크립트가 끊긴다. `pg_stat_activity` 로 실제 접속자를 먼저 파악해야 한다 |
| **폴러 안정화가 선행** | systemd로 상시 실행되는 상태에서 닫아야 문제 원인을 가릴 수 있다 |

### 선행 조건 (체크리스트)

- [x] VMS가 DB에 안 붙음 (`v6`)
- [x] 폴러가 조회 대행 (`v7`)
- [x] VMS에서 정원 편집 가능 (`v9`) ← **이게 없으면 v8 후 값을 못 바꾼다**
- [ ] 폴러가 systemd로 상시 실행 (1절)
- [ ] 팀 확인 — 외부에서 5432에 붙는 도구·사람 파악
- [ ] main 머지 완료

### 끊기는 것과 대체

| 대상 | 대체 |
|---|---|
| 다른 PC의 DBeaver·psql | SSH 터널 (`ssh -L 5432:localhost:5432 ...`) |
| 정원·임계 수정 | **VMS의 ZONE SETTINGS** (v9) — 이미 대체됨 |
| 폴러·VMS | 영향 없음 |

---

## 4. 🟢 차후 — MQTT 브로커 보안 (mTLS)

### 현재 상태가 위험하다

`rpi_b/rpib_app/broker/guardx_broker.conf`:
```conf
listener 1883 0.0.0.0
allow_anonymous true
```

파일 주석이 스스로 경고한다:
> ⚠ 테스트 전용 — **같은 LAN의 누구든 액추에이터 명령을 쏠 수 있는 상태**

**v9로 쓰기 경로가 생겨 위험이 커졌다.** 이제 같은 LAN에서 이것도 가능하다:

```bash
mosquitto_pub -h <RPi B> -t guardx/db/rpib/cmd/set_zone -m '{...}'
```

현재 방어선은 **폴러의 값 검증뿐**이고, 그건 "잘못된 값"은 막아도 "권한 없는
사람"은 못 막는다.

### 보안 단계

```
현재    DB 열림 + MQTT 열림          ← 아무나 다 됨
v8      DB 닫힘 + MQTT 열림          ← DB는 막았으나 MQTT로 우회 가능
mTLS    DB 닫힘 + MQTT 인증서 필요    ← 실질적 통제
```

**v8은 필요하지만 충분하지 않다.**

### 준비는 되어 있다

`rpi_b/common/certs/guardx_mtls.conf`:
```conf
listener 8883
require_certificate true
use_identity_as_username true
```

전환하면 VMS도 `mosquitto_tls_set()` 과 인증서 3종(ca/cert/key)이 필요하고,
`credentials.ini` 의 `[mqtt]` 에 경로를 추가해야 한다.

⚠ **RPi A/B/C/VMS가 함께 넘어가야 하는 작업**이라 한 사람이 결정할 수 없다.
팀 일정에 올려야 한다. (`VEDA-120` 에서 A→B 구간은 이미 mTLS를 적용했으므로
그 방식을 확장하는 형태가 될 것)

### 중간 단계 대안

전면 mTLS가 부담이면 **브로커 사용자/ACL** 만 먼저 넣을 수도 있다:

```bash
mosquitto_passwd -c /etc/mosquitto/passwd vms
# + acl_file 로 토픽별 read/write 분리
```

`cmd/set_zone` 만 인증 필요로 걸어도 위험이 크게 준다.

---

## 5. 미규명 — 언젠가 봐야 할 것

**2026-07-28 16:19에 `detections` 적재가 멎었던 원인.**

날짜 목록에 `07-23~26`이 비어 있는 것도 같은 문제일 수 있다.
**파티션 소진이 유력하다** — `Database/schema.sql` 주석:

> 파티션은 7일치 선생성 — cron 이 7일 넘게 죽으면 `detections` INSERT 가
> "no partition" 으로 실패한다 (**폴러는 크래시 없이 로그만 남기고 계속**).

이 성질 때문에 "서비스는 `active` 인데 데이터가 안 쌓이는" 상태가 된다.

```bash
sudo crontab -l | grep guardx_maintain
sudo -u postgres psql -d guardx -c \
  "SELECT relname FROM pg_class WHERE relname LIKE 'detections_%' ORDER BY relname DESC LIMIT 5;"
```

cron이 없으면 등록:
```
5 0 * * * sudo -u postgres psql -d guardx -c "SELECT guardx_maintain();"
```

---

## 6. ⚪ 새 기능 — 붙이기 전에 분류부터

새 기능은 **먼저 두 부류로 나눈다.** 그러면 어디를 고칠지가 정해진다.

| 부류 | 판별 기준 | RPi B | VMS |
|---|---|---|---|
| **A. 상태** | 사용자가 뭘 고르든 답이 같다 | `publishVmsState` 에 쿼리 추가 | `subscribe` 한 줄 |
| **B. 질의** | 사용자 선택에 따라 답이 달라진다 | `handleHeatday` 복사 + SQL 교체 | 요청 발행 + 응답 핸들러 |

| 기능 | 부류 | 난이도 | 비고 |
|---|---|---|---|
| **자동 점유율 갱신** | A | 낮음 | `zone_occupancy` 주기 발행 → VMS 구독만 |
| **객체 추적 (objective tracing)** | **B** | 중 | v5 요청-응답 틀 재사용. 상세는 `VMS_CODE_MAP.md` |
| 캘린더 위젯 (날짜 목록 대체) | — | 낮음 | VMS만 |
| 사건(incident) 알림 | A | 낮음 | `guardx/alert/rpib` 을 VMS도 구독 |

---

## 7. 권장 순서

| # | 작업 | 소요 | 막히면 |
|---|---|---|---|
| 1 | 폴러 systemd 정리 | 30분 | 터미널 닫으면 전부 멎음 |
| 2 | 로그 버퍼링 한 줄 | 5분 | — |
| 3 | PR 생성 + 규약 토픽 등재 | 1시간 | — |
| 4 | main 머지 | — | 팀 리뷰 |
| 5 | **v8** (팀 확인 후) | 30분 | `DB_ACCESS_HARDENING.md` |
| 6 | 객체 추적 기능 | 1~2일 | `VMS_CODE_MAP.md` |
| 7 | mTLS (팀 일정) | — | 전 노드 동시 |

**1번을 먼저.** 나머지는 그 위에서 한다.
