# RPi B 테스트 가이드

RPi A/C 실기 없이 한 대에서 **센서 수신 → 판단 → 명령 발행 → DB 기록**
전 경로를 검증한다. A 역할은 `02_fake_rpia.sh`, C 역할은 `03_watch_rpic.sh`가
대신한다.

## 사전 준비

```bash
sudo apt install mosquitto mosquitto-clients libmosquitto-dev
```

## 1. 기본 시나리오 (화재 확정 → 해제)

터미널 4개:

```bash
# T1: 브로커 확보 (이미 떠 있으면 그대로 쓰라고 알려줌)
cd rpib_app/test
./01_start_broker.sh

# T2: 판단 엔진
cd ../app && make && ./rpib_engine
# "mqtt: subscribed to guardx/sensor/rpia (qos0), ... /button (qos2)" 확인

# T3: C 역할 모니터
cd ../test && ./03_watch_rpic.sh

# T4: A 역할로 풀 시나리오 주입
./02_fake_rpia.sh fire_demo
```

**기대 결과 (시간 순)**
1. 정상 2사이클: T2/T3 조용함 (T2에 로그 없음이 정상)
2. 5채널 동시 상승 3사이클: **3번째에** T2가
   `!!! FIRE CONFIRMED (...)`를 출력하고, T3에 명령 5건
   (`servo_1 SET`, `shutter CLOSE`, `fan SET`, `water_pump ON`,
   `sound SET`)이 연달아 표시
3. 정상 10사이클: **10번째에** T2가 `situation recovered` + T3에 정지 3건
   (`water_pump OFF`, `sound SET 0`, `fan SET 0`).
   **가스밸브와 셔터는 되돌아오지 않음** (수동 복구 정책)
4. `cat ../app/rpib_events.jsonl`: sensor 줄들 사이에
   `fire_confirmed`/`recovered` 전이 기록 확인

## 2. 오탐 방지 검증 (연속 카운트)

```bash
./02_fake_rpia.sh gas 2        # 2사이클만 초과
./02_fake_rpia.sh normal 1     # 정상으로 복귀 -> 카운터 리셋
./02_fake_rpia.sh gas 2        # 다시 2사이클
```

기대: FIRE 미발동 (N_CONFIRM=3 연속이 아니므로). T3에 아무 명령 없음.

## 3. 센서 장애 동결 검증

```bash
./02_fake_rpia.sh gas 2        # 카운터 2까지 적립
./02_fake_rpia.sh invalid 5    # gas valid=false 5사이클 - 카운터 동결
./02_fake_rpia.sh gas 1        # 유효값 1사이클 추가 -> 3 도달
```

기대: invalid 구간에서 리셋되지 않고, 마지막 1사이클에 FIRE 확정.
(동결 정책: valid=false는 올리지도 내리지도 않는다)

## 4. 버튼 로그 경로 (QoS 2)

```bash
./02_fake_rpia.sh button
```

기대: T2에 `emergency button logged`, jsonl에 `"type":"button"` 1줄.
**액추에이터 명령은 안 나감** (규약 4-2: 로깅 전용, 제어는 하드웨어 인터락).

## 5. 워치독 검증

주입을 5초 이상 멈추면 T2에 `WARNING - no sensor data for 5000 ms` 1회.
다시 `normal`을 쏘면 경고 리셋.

## 6. 크로스 노드 (실기 A/C 연동)

- RPi A: `mqtt_pub.h`의 HOST를 B의 IP로 → A의 실제 발행이 T2에 잡히는지
- RPi C: `mqtt_sub.h`의 HOST를 B의 IP로 → `fire_demo` 대신 실제 C의
  dmesg에 명령이 도달하는지 (`03_watch_rpic.sh` 불필요)
- 이때 B에서는 `01_start_broker.sh` 대신 `../broker/guardx_broker.conf`를
  시스템 mosquitto에 설치해 쓰는 것을 권장 (install.sh가 해줌)

## 7. PHASE 4 핫리로드 검증

사전조건: DB 접속 정보를 PG* 환경변수로 export (엔진과 동일):
```bash
export PGHOST=localhost PGUSER=guardx_writer PGDATABASE=guardx
export PGPASSWORD=...   # 직접 입력, 스크립트/커밋에 남기지 말 것
```

**7-1. 리로드 전 기준선** — 종합 55.4점짜리 `mid`는 임계 65에서는 화재가 아니다.

```bash
./02_fake_rpia.sh mid 3
```
기대: T2/T3 조용함 (55.4 < 65).

**7-2. 임계값 낮추기 (핫리로드)**

```bash
./05_reload_threshold.sh demo      # fire_score_threshold 65->50 활성화 + 신호
```
기대: 엔진 **재시작 없이** T2 stdout에 두 줄이 즉시 뜬다.
```
main: config reload signal received
threshold: applied (fire_score>=50.0 OR spark>=70.0&irtemp>=70.0, confirm=3 recover=10 relax=60 cycles)
```

**7-3. 새 임계값이 실제 판정에 반영됐는지** — 여기가 핵심이다. 로그만
바뀌고 판정은 옛 값으로 도는 경우를 걸러낸다.

```bash
./02_fake_rpia.sh mid 3
```
기대: 같은 페이로드인데 이번엔 **3번째 사이클에 FIRE 확정**
(`cause=spark`, 기여 17.5로 최대) + T3에 명령 5건.

**7-4. 원복**

```bash
./02_fake_rpia.sh normal 10        # FIRE -> NORMAL 복귀 (n_recover=10)
./05_reload_threshold.sh restore   # 65로 복귀
./02_fake_rpia.sh mid 3            # 다시 조용해야 정상 (7-1과 동일 상태)
```
순서 주의: `restore`를 먼저 해도 되지만, FIRE 상태로 남겨두면 다음 테스트의
기준선이 어긋난다. 상태를 NORMAL로 되돌린 뒤 임계값을 올리는 편이 헷갈리지
않는다.

DB 연결이 아예 안 되는 상황도 확인해볼 것: `PGHOST`를 존재하지 않는
호스트로 바꾸고 엔진을 기동하면 `threshold load failed, using
compiled-in defaults` 경고 후에도 정상 기동해야 한다(폴백 정책 검증).

## 8. FIRE 동결 완화 검증 (영구 잠금 방지)

FIRE 상태에서 센서가 무효면 해제 판정이 동결되는데, 영구 고장이면
영원히 못 빠져나오던 문제를 3단 구조로 고쳤다. 검증할 것은 두 가지고,
**둘 다 봐야 의미가 있다** - 완화가 되는 것과 되면 안 될 때 안 되는 것.

기본값 `freeze_relax_cycles=60`이면 매번 1분씩 기다려야 하므로 핫리로드로
낮춰서 시작한다:

```bash
./05_reload_threshold.sh fastfreeze   # relax 60 -> 5
```
기대: `threshold: applied (... recover=10 relax=5 cycles)`

**8-1. 완화 허용 — 생존 가중치 0.80**

```bash
./02_fake_rpia.sh combo 3      # FIRE 확정
./02_fake_rpia.sh invalid 8    # 가스만 무효 (생존 0.80)
```
기대: 5번째 사이클에 완화 로그.
```
main: freeze relax threshold reached (5 cycles) - recovery resumes on surviving channels if their weight >= 0.50
```
`relax`(5) < `n_recover`(10)이라 "recovery frozen" 경고는 뜨지 않는다 -
완화가 먼저 걸려서 애초에 동결 상태로 오래 머물지 않기 때문. 기본값
(relax 60 / recover 10)에서는 10사이클째 frozen 경고가 먼저 뜨고,
60사이클째 완화 로그가 뜬다.

이어서:
```bash
./02_fake_rpia.sh invalid 12   # 가스 무효 유지, 나머지는 안전값
```
기대: **`situation recovered`** + 정지 3건. 완화 전이었다면 이 이벤트는
영원히 나오지 않는다. 이게 이번 수정의 핵심 증거다.

**8-2. 완화 거부 — 생존 가중치 0.40 (더 중요)**

```bash
./02_fake_rpia.sh combo 3      # 다시 FIRE 확정
./02_fake_rpia.sh crippled 20  # 불꽃+표면온도 무효 (생존 0.40)
```
기대: 경고는 뜨지만 **`situation recovered`가 끝까지 안 나와야 정상.**
`crippled`의 센서값은 전부 안전구간이라, 완화가 잘못 발동하면 즉시 해제가
성립해버려서 실패가 눈에 띈다.

이게 안전성 증명이다. 불꽃과 표면온도가 동시에 죽는 건 "화재가 직접 증거
센서를 태워먹은" 패턴이고, 그 상태에서 남은 가스·온습도만으로 진화를
선언하면 안 된다. 여기서 빠져나오려면 사람이 개입해야 하며, 그 수동 해제
경로는 아직 없다(VMS 확장과 함께 결정).

**8-3. 원복**

```bash
./05_reload_threshold.sh restore   # relax 60, 임계 65로 복귀
```
8-2에서 엔진이 FIRE에 갇힌 채로 남아 있으므로, 다음 테스트 전에
엔진을 재시작하거나 `./02_fake_rpia.sh normal 10`으로 풀어줄 것.

> 참고: 엔진 재시작으로 푸는 방법은 B의 상태만 NORMAL로 되돌릴 뿐
> RPi C의 액추에이터에 OFF를 보내지 않는다. 실기에서는 펌프가 계속 도는
> 상태로 남는다는 뜻이다(상태 영속화 미구현 - 별도 TODO 항목).

## 9. PHASE 3 워커 스레드 + 종합 점수 검증

**9-1. 큐 단위 테스트** (브로커·DB 불필요, 제일 먼저)

```bash
cd ../app && make test
```
11개 항목 전부 `ok`여야 한다. 개발 PC에서 통과했더라도 **Pi에서 반드시
다시 돌릴 것** - pthread 구현과 코어 수가 달라 한쪽에서만 드러나는
경합이 있을 수 있다.

**9-2. 회귀** — 1~8절 시나리오가 전부 그대로 동작해야 한다. 기록이
비동기가 됐을 뿐 판단·발행 경로는 안 바뀌었으므로, 결과가 달라지면
그 자체가 버그다.

**9-3. 종합 점수 기록** — 이미 손계산으로 검증된 값들이라 좋은 대조군이다.

```bash
./02_fake_rpia.sh mid 3
tail -3 ../app/rpib_events.jsonl
```
각 줄에 `"score":55.4`. 다른 케이스의 기대값:

| 케이스 | 기대 score | 근거 |
|---|---|---|
| `normal` | 0.0 | 전 채널 안전구간 |
| `mid` | 55.4 | 임계 65/50 사이 |
| `combo` | 70.0 | 5채널 동시 상승 |
| `crossfire` | 42.0 | 가중합산은 미달, 오버라이드로 FIRE |
| `degraded` | 76.9 | 가스 무효 재정규화(보정 전 61.5) |

`crossfire`가 42점인데 `fire_confirmed`가 함께 남는 것이 정상이다 -
점수가 아니라 교차 확증으로 확정된 화재라는 기록이 된다.

**9-4. 동결 사이클은 null**

```bash
./02_fake_rpia.sh combo 3      # FIRE 확정
./02_fake_rpia.sh crippled 5   # 불꽃+표면온도 무효 -> 해제 판정 동결
grep '"score":null' ../app/rpib_events.jsonl | tail -3
```
동결 사이클에는 `fire_condition()`이 호출되지 않아 점수가 계산되지
않는다. 직전 값이 남아 "그 사이클의 점수"로 오해되면 안 되므로
`null`로 구분한다 - **`0`이 아니다.** 0점은 "안전하다고 판단함"이고
null은 "판단하지 않음"이라 의미가 정반대다.

**9-5. 종료 시 유실 없음** (워커 분리의 핵심 위험)

```bash
./02_fake_rpia.sh fire_demo
# 끝나자마자 엔진 터미널에서 Ctrl+C
tail -5 ../app/rpib_events.jsonl
```
마지막 `recovered` 줄까지 남아 있어야 한다. `db_writer_close()`가
종료 신호 → join → fclose 순서로 큐를 비우기 때문이다. 여기서 줄이
잘려 있으면 join이 빠졌거나 순서가 뒤집힌 것이다.

## 10. PHASE 5 PostgreSQL 실기록 검증

사전조건: PG* 환경변수 + `export PGCONNECT_TIMEOUT=3`. 이게 없으면
재연결 시도 한 번에 워커가 OS TCP 타임아웃(수십 초)까지 갇힌다.

**10-1. jsonl 회귀** — 기본 경로가 안 깨졌는지 먼저.
```bash
unset DB_BACKEND && ./rpib_engine
```
→ `db: backend=jsonl (fallback=rpib_events.jsonl)` 후 9절 시나리오 통과.

**10-2. pg 모드 기동**
```bash
DB_BACKEND=pg ./rpib_engine
```
→ `db: backend=pg` + `db: pg connected`

```bash
./02_fake_rpia.sh fire_demo
```

행 수 확인 — 센서 15사이클이면 reading 15 / value 90(=15×6):
```bash
psql -c "SELECT 'reading',count(*) FROM sensor_reading UNION ALL SELECT 'value',count(*) FROM sensor_value UNION ALL SELECT 'event',count(*) FROM fire_event UNION ALL SELECT 'command',count(*) FROM fire_event_command;"
```
`command`는 화재 5건 + 해제 3건 = 8이어야 한다.

**10-3. cause 매핑** ← 이번 작업에서 가장 조용히 틀리기 쉬운 곳

`decision_cause_t`와 `sensor_channel.channel_id`는 번호가 겹치면서
어긋난다(SPARK=3이지만 채널 3은 temperature). 잘못 넣어도 FK 제약을
통과하고 에러도 안 나므로, 반드시 조인해서 눈으로 봐야 한다.

```bash
./02_fake_rpia.sh crossfire 3   # cause=spark 기대
./02_fake_rpia.sh normal 10
./02_fake_rpia.sh degraded 3    # cause=irtemp 기대
psql -c "SELECT fe.event_type, sc.channel_key, fe.trigger_seq FROM fire_event fe LEFT JOIN sensor_channel sc ON sc.channel_id = fe.cause_channel_id ORDER BY fe.event_id;"
```
→ `spark_raw`, `irtemp_object`가 나와야 한다. `temperature`나
`irtemp_ambient`가 나오면 매핑이 틀린 것이다. `recovered` 행은
`channel_key`가 NULL이어야 정상(해제에는 원인이 없다).

**10-4. 명령 기록 연결**
```bash
psql -c "SELECT fe.event_type, ac.command_key, fec.action, fec.value FROM fire_event_command fec JOIN fire_event fe USING (event_id) JOIN actuator_command ac USING (command_id) ORDER BY fe.event_id, ac.command_id;"
```
→ `fire_confirmed`에 servo_1/servo_2/fan/water_pump/amp 5건,
`recovered`에 fan/water_pump/amp 3건. ON/OFF 명령의 `value`는 NULL.

**10-5. 연결 끊김 폴백** ← 이번 작업의 안전장치

엔진을 띄워둔 채로:
```bash
sudo systemctl stop postgresql
./02_fake_rpia.sh combo 5
```
기대: `db: pg ... failed` 경고가 나오지만 **판단과 명령 발행은 계속**
되고(T3에 명령 5건), 기록은 JSONL로 흘러간다.
```bash
tail -3 ../app/rpib_events.jsonl
```

```bash
sudo systemctl start postgresql
./02_fake_rpia.sh normal 10
```
기대: 5초 안에 `db: pg reconnected` 후 다시 DB로 기록.

**10-6. 백필**
```bash
psql -v ON_ERROR_STOP=1 -v path="$(realpath ../app/rpib_events.jsonl)" \
     -f ../../Database/backfill_jsonl.sql
```
10-5의 장애 구간이 DB에 들어가야 한다. 마지막에 나오는 테이블별 건수로
확인. 백필이 끝나면 JSONL을 비우거나 옮겨둘 것 — 다시 돌리면 중복
방지가 완벽하지 않다(A 재시작 시 seq가 리셋되므로).

정리: `./04_cleanup.sh`

---
**참고**: 위 1~3절은 PHASE 1(단순 임계값) 시절 문구가 남아 있어
PHASE 2 퍼지 융합 기준으로는 부정확하다(예: "가스만으로 FIRE 확정"은
더 이상 성립하지 않음 - 단독 최대 기여 20점). 이 가이드 전체를
PHASE 2/4 기준으로 다시 쓰는 작업은 별도로 진행 필요.
