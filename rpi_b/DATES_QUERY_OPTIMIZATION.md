# task_vms dates 쿼리 최적화 (VEDA-146)

측정 2026-07-31 · RPi B · PostgreSQL 17.10 (Debian) · 폴러 가동 중 3회 반복

---

## 1. 문제

CROWD 화면 우측 날짜 목록을 만드는 쿼리가 `detections` 전 행을 스캔했다.

```sql
SELECT DISTINCT (ts AT TIME ZONE 'Asia/Seoul')::date FROM detections
```

`detections`는 일 단위 RANGE 파티션 테이블이지만 `DISTINCT ts`는 인덱스로
줄일 수 없어, 날짜 11개를 얻으려고 전 파티션의 모든 행을 읽는다.

**행이 쌓이는 만큼 그대로 나빠진다:**

| 날짜 | detections 행 수 | Execution |
|---|---|---|
| 07-29 | 1,340,000 | 880 ms |
| 07-30 | — | 1,247 ms |
| 07-31 | **2,677,518** | **1,694 ms** |

이틀 만에 행이 **2.00배**, 시간이 **1.92배**. 선형 관계가 실측으로 확인된다.
보존이 14일이므로 정상 상태(현재는 11일치만 쌓인 상태)에서는 더 나빠진다.

### 왜 이게 문제인가

이 쿼리는 폴러 **메인 루프의 30초 틱**에서 실행된다
(`poller_main.cpp` — `cfg_interval_s` 분기의 `publishVmsState`).

```
1초 틱 루프 (단일 스레드)
 ├ 2초마다  pollDetections    ← 감지 수집. VMS 화면 데이터의 원천
 ├ 10초마다 pollFaces
 ├ 60초마다 pollPrediction / pollOccupancy / pollAlert
 └ 30초마다 syncConfig + publishVmsState   ← 여기서 1.7초 정지
```

단일 스레드라 이 쿼리가 도는 동안 **루프 전체가 멈춘다.** 30초마다
1.7초씩 감지 수집이 밀린다. 쿼리 성능 문제가 아니라 **수집 지연 문제**다.

---

## 2. 해결

날짜는 **일 파티션 이름이 이미 갖고 있다.**

```
detections_p20260720   ← 이름 자체가 "7월 20일"
detections_p20260727
detections_p20260730
```

본체를 스캔하지 않고 카탈로그(`pg_inherits` + `pg_class`)에서 이름만 읽는다.
행 스캔 0.

```sql
SELECT day FROM (
  SELECT to_date(substring(c.relname FROM '\d{8}$'), 'YYYYMMDD') AS day
  FROM pg_inherits i
  JOIN pg_class c ON c.oid = i.inhrelid
  JOIN pg_class p ON p.oid = i.inhparent
  WHERE p.relname = 'detections'
    AND c.relname ~ '^detections_p\d{8}$'
) s
WHERE day <= current_date
  AND EXISTS (SELECT 1 FROM detections
              WHERE ts >= day::timestamptz AND ts < (day + 1)::timestamptz)
ORDER BY 1
```

### 두 필터가 필수인 이유 (실측 근거)

**`day <= current_date`** — `detections_ensure_partitions(days_ahead DEFAULT 7)`가
오늘부터 7일 뒤까지 파티션을 **미리 만든다.** 안 거르면 데이터가 한 줄도 없는
미래 날짜 7개가 목록에 뜨고, VMS가 그걸 골라 빈 히트맵을 띄운다.

**`EXISTS (...)`** — 폴러가 멈춰 있던 날은 파티션은 있고 행은 0이다
(실측: 07-23 ~ 07-26 **4일**). 날짜 필터만으로는 못 거른다.
파티션 프루닝으로 해당 파티션 하나만 보고 첫 행에서 멈추므로 비용이 무시할
수준이다.

**`pg_inherits` 조인** — 이름 패턴만 맞는 남의 테이블이 섞이지 않게.
`detections_drop_old`가 쓰는 것과 같은 패턴이다.

**스키마 변경·마이그레이션 없음** — 쿼리 문자열만 교체했다.

---

## 3. 측정 결과

측정 시점 데이터 규모: `detections` **2,677,518행** / 파티션 18개
(데이터 있는 날 11일 · 빈 과거 파티션 4일 · 선생성 미래 7일)

폴러 가동 중, 20초 간격으로 3회 반복 (30초 틱과 다른 지점에 걸리도록).

| 회차 | 기존 Planning | 기존 Execution | 신규 Planning | 신규 Execution |
|---|---|---|---|---|
| 1 | 14.069 ms | 1693.725 ms | 7.468 ms | 3.931 ms |
| 2 | 14.355 ms | 1755.987 ms | 6.708 ms | 3.345 ms |
| 3 | 14.158 ms | 1680.512 ms | 6.323 ms | 3.520 ms |
| **중앙값** | **14.158** | **1693.725** | **6.708** | **3.520** |

폴러가 실제로 부담하는 것은 Planning + Execution 합계다.

| | 합계 |
|---|---|
| 기존 (`DISTINCT ts`) | **1,708 ms** |
| 신규 (파티션 카탈로그) | **10.2 ms** |
| | **약 167배** |

Execution만 보면 1,694 ms → 3.5 ms (약 481배).

### 정확성 — 이게 성능보다 먼저다

신·구 쿼리가 **같은 날짜 집합**을 내놓는지 FULL JOIN으로 대조했다.

```
### 불일치 검사 (0 rows 여야 정상) ###
 old_q | new_q
-------+-------
(0 rows)
```

**3회 모두 불일치 0건.**

### 특성 변화

신규 쿼리는 Planning(6.7 ms)이 Execution(3.5 ms)보다 크다. 읽을 데이터가
거의 없어 계획 수립이 지배적이다. 보존 14일이라 파티션 수가 20개 안쪽으로
묶여 있으므로, **데이터가 아무리 쌓여도 10 ms 근처를 유지한다.**
기존 방식은 행 수에 비례해 계속 나빠졌다.

---

## 4. 주의 — 깨질 수 있는 조건

**파티션 명명 규약 `detections_pYYYYMMDD`에 의존한다.**
같은 규약을 쓰는 곳이 세 군데다:

- `schema.sql` — `detections_ensure_partitions`
- `schema.sql` — `detections_drop_old`
- `task_vms.cpp` — 이 쿼리

규약을 바꾸면 정규식이 아무것도 못 잡아 **에러 없이 빈 목록**이 된다.
조용히 실패하므로 세 곳을 반드시 함께 고칠 것.

**서버 타임존이 `Asia/Seoul`이어야 한다.**
파티션 경계는 서버 TZ 기준 자정이고 화면 날짜는 KST다. 지금은 일치하지만
UTC로 바꾸면 9시간 어긋나 자정 근처 데이터의 날짜가 틀어진다.
`schema.sql`에 "경계 일관성 위해 TZ 변경 금지" 명시.

**`pg_stat_user_tables.n_live_tup`으로 빈 파티션을 판정하지 말 것.**
추정치라 stale하다 — 실측에서 12만·16만 행이 있는 파티션(`p20260720`,
`p20260722`)이 `0`으로 나왔다. 그래서 `EXISTS`로 실제 행을 본다.

---

## 5. 재현 방법

측정 스크립트를 `/tmp`에 만들고(레포 안에 두면 `git status`가 더러워진다)
3회 반복한다.

```bash
for i in 1 2 3; do
  sudo -u postgres psql -d guardx -f /tmp/dates_bench.sql
  sleep 20
done | tee ~/bench.txt
```

```bash
grep -E "###|Execution Time|Planning Time" ~/bench.txt
```

`EXPLAIN ANALYZE`는 `SELECT`라 데이터를 바꾸지 않는다. 폴러를 멈추고 재면
경합이 없어 **기존 쿼리가 실제보다 빠르게 나오므로**, 운영 조건을 보려면
폴러를 켠 채로 잰다.

---

## 6. 측정 시 확인된 별건

**`[det]` 로그는 폴링 주기를 나타내지 않는다.**
`task_detections.cpp`에 `if (arr.empty()) return;`이 있어 **감지가 0건이면
로그를 남기지 않는다.** 즉 `[det]` 간격은 폴러가 밀렸는지가 아니라 사람이
얼마나 지나갔는지를 보여준다. 루프 지연 측정에 쓸 수 없다.

**폴러 이중 가동 → MQTT 재접속 루프.**
측정 중 `guardx-poller`(juan, systemd)와 수동 실행(bangjunhan)이 동시에
돌고 있었다. 같은 client_id로 붙어 브로커가 서로를 끊어내면서
`[mqtt] 구독`이 2초마다 반복됐다. 재접속 사이에 도착한 `set_zone`·히트맵
요청이 유실될 수 있다. 운영 전에 하나로 정리할 것.
