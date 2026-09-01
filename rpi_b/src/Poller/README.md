# GuardX 라파 B 폴러 (rpi_b)

Wisenet 카메라(`juan_application` + WiseAI)에서 검출·예측·형상 데이터를 HTTPS로
폴링해 PostgreSQL/PostGIS(`guardx`)에 적재하고, **혼잡을 판정해 incidents/alerts
발화 + MQTT로 통지**하는 C++17 폴러. (v16, 2026-07-28)

```
카메라 (juan_application) ──HTTPS(digest+공개키 핀)──▶ 폴러 ──▶ PostgreSQL (guardx)
                                                        │
                                                        └─MQTT──▶ guardx/alert/rpib
                                                                  (transmission layer → RPi C 액추에이터)
```

- **풀 모델**: 카메라는 push하지 않음 — 폴러가 주기적으로 당겨온다.
- **판단은 rpi_b, 예측은 카메라**: 카메라가 분위수(p50/p10/p90)를 주면
  폴러가 `zone_thresholds`와 비교해 경보한다. 액추에이터 선택은
  transmission layer 소관 (역할 분리 — `MQTT_ALERT_INTERFACE.md`).
- **존 판정은 DB 몫**: 폴러는 픽셀 원시 점만 적재, `ST_Contains`는 소비자가.

---

## 1. 태스크 (7종, 1초 틱 단일 스레드)

| 태스크 | 주기 | 소스 → 대상 | 비고 |
|---|---|---|---|
| task_detections | 2s | `/detections?since=` → `detections` | Human+Face/Head(category 1/2/3, parent_id), 커서 증분 |
| task_faces | 10s | `/faces?since=` → `faces` | BestShot (사람 object_id + bbox + JPEG 경로) |
| task_prediction | 60s | `/prediction?channel=` → `congestion_prediction` | horizon {5,30,60,180} × p50/p10/p90/p_over_capacity(-1→NULL)/warmup |
| task_occupancy | 60s | `/occupancy?channel=` → `zone_occupancy` | 60분치 멱등 upsert + **현재 인원(now_smoothed) 반환** |
| **task_alert** | 60s | DB → `incidents`+`alerts` + MQTT | 아래 §2 |
| task_flow | 60s | `/events` 누적 델타 → `line_flow` | 카메라 재시작 리셋은 재기준선 |
| task_config_sync | 30s | WiseAI CGI → `zones`+이력 | 형상 해시 변경 감지 → epoch 버전업 |

공통 방어: 미래 ts 레코드(카메라 시계 글리치 ~0.1%)는 적재·커서 전진 모두
스킵, 시작 시 오염 커서 자가 치유. `CHANNELS=0,1,2,3` 다채널 (0-기반,
존 없는 채널은 부팅 시 경고 후 제외).

## 2. 혼잡 경보 파이프라인 (task_alert)

**3신호 중 나쁜 쪽 채택**: 현재 인원(now_smoothed, ≈1초 창 — 급증 즉시) ·
분 중앙값(zone_occupancy — 체류, false-clear 방지 바닥) · 예측 p50(최근접
horizon, warmup 제외). 비율 = count / `zone_thresholds.capacity_limit`.

**상태 전이 시만 발화** (hysteresis):

```
없음 ──warn(≥75%)──▶ 주의 ──critical(≥90%)──▶ 위험
  ▲                    │ (강등 없음)              │
  └── clear (< warn×0.9 해제 밴드) ◀──────────────┘
```

전이마다: `incidents` INSERT/UPDATE + `alerts` INSERT (원자 커밋) → 커밋 후
MQTT `guardx/alert/rpib` QoS 1 발행 (payload·구독법: `MQTT_ALERT_INTERFACE.md`).
capacity NULL 존은 skip(비무장). 현 운용: 전 존 capacity 10 → warn 8명/critical 9명.

## 3. 의존성

```bash
sudo apt install -y cmake libcurl4-openssl-dev nlohmann-json3-dev libpqxx-dev libmosquitto-dev
```

| 라이브러리 | 용도 |
|---|---|
| libcurl | HTTPS GET (digest 인증, thread_local persistent handle, 공개키 핀) |
| nlohmann/json | JSON 파싱 |
| libpqxx | PostgreSQL 접속 |
| libmosquitto | 경보 MQTT 발행 (브로커는 localhost mosquitto) |

PostgreSQL + PostGIS + `guardx` DB가 localhost에 있어야 함 (§7).

## 4. 빌드

```bash
cmake -B build && cmake --build build -j      # configure(최초 1회) + 컴파일
```

| 산출물 | 설명 |
|---|---|
| `build/guardx_poller` | 메인 폴러 |
| `build/db_smoke` | DB 연결 + lookupZoneId 검증 |
| `build/det_smoke` | detections INSERT 계약 검증 (rollback, 오염 없음) |
| `build/test_parsing` | 파싱 검증 (오프라인, 픽스처 기반) |

## 5. 설정 (config.env)

폴러가 루트의 `config.env`를 직접 읽음. **git 제외** — `config.env.example`
복사해 작성, `chmod 600`. 우선순위: 환경변수 > config.env > 기본값.

| 키 | 기본 | 설명 |
|---|---|---|
| `CAM_HOST` | 192.168.0.3 | 카메라 주소 |
| `CAM_USER`/`CAM_PASS` | admin / — | **폴백** — 평소엔 DB `camera_credentials`에서 로드 |
| `CAM_INSECURE` | 0 | 1 = TLS 검증 끔 (**테스트 전용**) |
| `CAM_PINNED_KEY` | — | `sha256//…` 공개키 핀 — **운영 표준** (setup_security.sh가 설정) |
| `CAM_CAINFO` | — | CA 고정 (호스트명 일치 시에만 — 핀이 있으면 무시됨) |
| `PGCONN` | localhost guardx | libpq 접속 문자열 |
| `CHANNELS` | (CHANNEL 단일) | 폴링 채널 목록, 예 `0,1,2,3` |
| `MQTT_HOST`/`MQTT_PORT` | localhost / 1883 | 경보 발행 브로커 |
| `STATE_PATH` | ./state/poller_state.json | 커서·해시 영속 |
| `*_INTERVAL_S` | 2/10/60/30 | det/face/pred/cfg 주기 |

> ⚠ 인라인 주석 금지 (`KEY=1  # 주석` → 값이 깨짐). 주석은 자기 줄에.

## 6. 실행·운영

상시 가동은 systemd (`guardx-poller.service`, 샌드박스 드롭인 포함):

```bash
sudo systemctl status guardx-poller
sudo journalctl -u guardx-poller -f          # 실시간 로그
```

수동 실행은 **rpi_b 루트에서** (`config.env`·`state/` 상대경로):
`./build/guardx_poller`. 정상 로그:

```
[cred] camera 1 credentials loaded from DB (user=admin)
poller map: ch 0 -> zone 2 …                  # 채널→존 매핑
[det] +N rows, cursor=…                       # 2초 증분 (백로그 첫 회 전량)
[face] +N rows (skipped 1 future-ts), …       # 미래 ts 가드 동작 시 표기
[pred] ch1 zone 1 … rows=4 model=hw_damped_v1 # warmup이면 "(warmup)" 접미
[flow] +N rule-buckets                        # 통과 있던 분에만
[alert] zone 1 OPEN critical (…)              # 상태 전이 시만
[mqtt] alert/rpib <- zone 1 critical (9/10)   # 발행 성공
[state] …커서 미래 오염 → 재설정               # 자가 치유 (있을 때만)
# occupancy 적재는 무로그가 정상. [cfg] baseline은 state 리셋 직후 1회만.
```

## 7. DB 구축·보존

```bash
sudo -u postgres psql -f Database/init.sql               # 최초 1회 (계정·DB·PostGIS)
sudo -u postgres psql -d guardx -f Database/schema.sql   # 전체 재구축 (데이터 삭제 주의)
```

- `schema.sql` = 신규 구축의 단일 진실원천 (v16: 4존 시드, capacity 10,
  피드백 4호 컬럼, UNIQUE 제약 포함).
- `migration_*.sql` = 기존 DB 따라잡기용 적용 이력. `archive/`는 실행 금지.
- **보존 정책 전제**: cron 하루 1회 —
  `5 0 * * * sudo -u postgres psql -d guardx -c "SELECT guardx_maintain();"`
  (detections 일 파티션 14일 / pred·flow 180일 / faces 30일 / occupancy 365일.
  파티션 7일 선생성 — cron 7일 정지 시 INSERT 실패)

## 8. 배포·보안 (VM → Pi)

```bash
bash sync_to_rpi.sh              # 공유폴더에서! rsync + 빌드 + 재시작
bash sync_to_rpi.sh --dry-run    # 전송 목록 미리보기
sudo bash setup_security.sh      # (Pi) TLS 핀·권한 600·systemd 샌드박스 — 멱등
```

- sync는 Pi의 `config.env`·`camera.pem`·`state/`·`build/`를 건드리지 않는다.
- 보안 현황: TLS 공개키 핀(`CAM_INSECURE=0`) + 파일 600 + systemd 샌드박스
  (ProtectSystem=strict 등) 적용됨. 잔여(브로커 인증, 전용 계정 등)는 `TODO.md` §보안.

## 9. 테스트

```bash
./build/test_parsing        # 오프라인 (rpi_b 루트에서 — 픽스처 상대경로)
./build/db_smoke            # DB 연결 + lookupZoneId(1,1)==1
./build/det_smoke           # detections INSERT 계약
```

경보 통합 테스트(실기기): `mosquitto_sub -t 'guardx/alert/#' -v` 켜두고
zone 1 `capacity_limit=1`로 낮춘 뒤 ch1 앞에 사람 — 60초 내 critical 수신.
**끝나면 10으로 원복** (절차: `MQTT_ALERT_INTERFACE.md` §8).

## 10. 디렉토리 구조

```
rpi_b/                       (v16 정리 — include/src 분리)
├ CMakeLists.txt · Makefile  표준 빌드 (mqtt/camera/mqttdb 라이브러리 + 실행파일 2개)
├ config.env.example         설정 템플릿 (실파일 config.env는 git 제외, 600)
├ sync_to_rpi.sh             VM → Pi 배포 (rsync+빌드+재시작)
├ setup_security.sh          보안 프로비저닝 (핀·권한·샌드박스, 멱등)
├ MQTT_ALERT_INTERFACE.md    경보 MQTT 계약 (transmission layer 전달용)
├ include/   모듈별 헤더 — include 경로는 "Poller/x.hpp" 형태 그대로
│  ├ Config/   config.hpp · credentials.hpp
│  ├ Database/ db.hpp (lookupZoneId)
│  ├ Poller/   http_client · mqtt_pub · time_util · task_* (7종)
│  └ Storage/  state.hpp
├ src/       모듈별 구현 (include/ 미러)
│  ├ main.cpp                진입점 (runPoller 호출만)
│  └ Poller/  poller_main.cpp(폴러 본체: 초기화 + 1초 틱 루프) · README.md(이 문서)
├ Database/  schema.sql(단일 진실원천) · migration_*.sql(적용 이력) · archive/
├ report/    상주 리포트 (gen_report.sh cron + http.server :8088)
└ test/      test_parsing.cpp + fixtures/ · db_smoke · det_smoke · legacy_*.py
```

## 11. 개발 흐름

공유폴더(편집) → `sync_to_rpi.sh`(Pi 배포·검증) → VM 클론에 rsync → 브랜치
커밋 → GitHub PR. 커밋 전 `git status`에서 `config.env`/`camera.pem` 부재 확인
(`.gitignore` 등재됨).

```bash
git checkout main && git pull
git checkout -b VEDA-xxx-작업이름
git add -A rpi_b && git commit -m "feat(scope): 설명"
git push -u origin VEDA-xxx-작업이름
```

## 12. 참고

- 좌표계: 픽셀 공간 2592×1520, 원점 좌상단, y-down, SRID 0.
- 시간: 저장은 전부 UTC(ISO8601 `Z`) — 표시 변환은 소비자 몫. 커서 비교는
  문자열 사전순(=시간순).
- zone_id 하드코딩 금지 — `lookupZoneId(camera_id, channel)` 조회
  (채널당 존 1개는 `uq_zones_camera_channel`이 보장).
- 장애 거동: 카메라/브로커 연결 실패 시 크래시 없이 다음 틱 계속, 커서
  보존(유실 없음). 영속 실패는 `[state] save 실패` 로그로 드러남.
- 문서: `TODO.md`(백로그) · `MQTT_ALERT_INTERFACE.md`(경보 계약) ·
  카메라 쪽 `juan_application/docs/`(DATA_FLOW_MAP·CAMERA_API·작업 로그).
