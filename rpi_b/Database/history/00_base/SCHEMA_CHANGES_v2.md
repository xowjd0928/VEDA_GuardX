# GuardX DB 스키마 변경 안내 — v2 (2026-07-20)

수신: 팀 (특히 DB·백엔드·기능3 동선추적 담당)
작성: 카메라/폴러 사이드
대상 DB: `guardx` (RPi B, PostgreSQL + PostGIS)
원본: `rpi_b/database/schema.sql` (이 문서는 그 요약·해설)

> ✅ **실물 DB 대조 완료 (2026-07-22)**: 이 문서의 모든 표는 라이브 DB
> introspection(pg_catalog/information_schema)과 대조해 검증됨. 실측 스냅샷은
> 문서 말미 §8 "라이브 실측값" 참조 — 테이블 16개, detections 358k행(이틀치
> 온전 축적), congestion_prediction 6,620행, cron 2건 가동 확인.

---

## 0. 세 줄 요약

1. **구 마이그레이션 3본(tier1·tier2·camera_credentials)이 본체 스키마에 통합**됐다.
   신규 구축은 이제 `schema.sql` **하나**로 끝. `migration_*.sql`은 **기존 DB용
   이력 자료**로만 남는다 (신규 구축엔 실행하지 말 것).
2. **`detections`가 일 단위 파티션 + 14일 자동 보존**으로 바뀌었다. 조회 방식은
   그대로지만, **`guardx_maintain()` cron이 안 돌면 7일 뒤 INSERT가 실패**한다.
3. **예측 계약이 v2(hw_damped_v1)로 확정**됐다 — `congestion_prediction`의
   해석이 바뀌었다 (아래 §3).

**여러분 코드에 영향 있는가?** — 컬럼을 SELECT만 하는 쿼리는 **거의 무영향**
(기존 컬럼 다 유지, 추가만 있음). 영향 지점은 §5의 체크리스트 3개뿐.

---

## 1. 테이블 전체 지도 (v2 기준)

```
코어(공간·원천)          기능1 군중예측           기능2 재난          기능3 동선      수렴·출력
─────────────────       ──────────────          ──────────         ──────────     ──────────
cameras                 zone_occupancy          devices            tracks         incidents
camera_credentials ★    zone_flow               device_logs        track_path     alerts
zones                   congestion_prediction ◆
zone_geometry_history ★
zone_thresholds ★
detections ◆ (파티션)
```

`★` = v2에서 통합/신설된 테이블 · `◆` = v2에서 구조·해석이 바뀐 테이블

---

## 2. 무엇이 바뀌었나 — 항목별

### 2-A. 마이그레이션 3본 → 본체 통합 (구조 동일, 위치만 이동)

세 마이그레이션이 만들던 것이 이제 `schema.sql`에 처음부터 들어 있다.
**이미 마이그레이션을 적용한 DB라면 구조는 동일** — 재구축한 DB와 같은 모양이
된다. 세 파일이 하던 일:

| 구 파일 | 만든 것 | 지금 위치 |
|---|---|---|
| `tier1` | `congestion_prediction.config_version` 컬럼 | 본체 (§3 참조) |
| `tier2` | `zone_thresholds`, `zone_geometry_history` 신설 + `zones.config_version` | 본체 |
| `camera_credentials` | `camera_credentials` 테이블 | 본체 |

### 2-B. `detections` — 일 파티션 + 14일 보존 (⚠ 운영 주의)

**왜**: 실측 초당 7.41행(→ 실운영 ~50행/s) → 연 39GB. SD카드 쓰기 증폭 없이
오래된 데이터를 버리려면 파티션 drop이 배치 DELETE보다 우월.

**구조 변화**:
- `PARTITION BY RANGE (ts)` — 하루당 `detections_pYYYYMMDD` 자식 테이블 1개.
- **PK가 `(detection_id, ts)` 복합**으로 바뀜 (파티션 키 포함 규칙). 단일
  `detection_id` PK를 전제한 코드가 있으면 확인.
- **DEFAULT 파티션 없음** — 의도적. 파티션 없는 날짜의 INSERT는 즉시 에러로
  드러나게 함 (조용한 오적재 방지).

> [!warning] cron 필수 (✅ 현재 등록됨 — 2026-07-22 확인)
> `guardx_maintain()`이 **하루 1회** 파티션을 7일치 선생성 + 14일 초과분 drop.
> 안 돌면 7일 뒤 `detections` INSERT가 "no partition found"로 실패한다
> (폴러는 크래시 없이 로그만 남기고 계속 — 즉 **조용히 데이터가 안 쌓인다**).
> 현재 root crontab에 등록되어 가동 중:
> ```
> 5 0 * * * sudo -u postgres psql -d guardx -c "SELECT guardx_maintain();" >> /var/log/guardx_maintain.log 2>&1
> ```
> (같은 crontab에 리포트 생성 `*/10 * * * * guardx-gen-report.sh`도 실재.)

- 조회는 그대로 — 부모 `detections`에 SELECT하면 파티션 프루닝은 PG가 자동.
  기존 `WHERE ts BETWEEN ...` 쿼리 무수정.
- 실측: 현재 파티션 **10개**(07-20~07-29), detections **358,370행**,
  범위 07-20 17:35 ~ 현재 — 이틀치가 유실 없이 온전. 폴러 주기(2초)가 링
  10분 상한 안이라 §7의 "10분 유실"은 실운영에서 발생 안 함.

### 2-C. `zones` 관심사 3분할

`zones` 한 테이블에 섞여 있던 것을 셋으로 나눔 (단일 진실원천):

| 테이블 | 담당 | 핵심 컬럼 |
|---|---|---|
| `zones` | **정체성** — zone_id 영구 고정 | `roi_polygon`(현재 형상 캐시), `config_version` |
| `zone_geometry_history` | **형상 이력** — 라인·ROI 변경 추적 | `valid_from`/`valid_to`(NULL=현재), `config_version` |
| `zone_thresholds` | **운영 설정** | `capacity_limit`, `warn_ratio`(0.75), `critical_ratio`(0.90) |

- `zones.zone_id`는 (camera_id, channel)당 **영구 고정** → 기존 FK 안 깨짐.
- 형상 변경 = history 구 행 `valid_to` 마감 + 신 행 추가. `zones.roi_polygon`은
  ST_Contains 핫패스용 현재 캐시.
- **`capacity_limit`은 이제 `zone_thresholds`에 있다** (구 `zones.capacity_limit`
  아님). 혼잡비 = `predicted_count / zone_thresholds.capacity_limit`.

### 2-D. `camera_credentials` 신설

카메라 접속 계정을 DB에 (폴러가 digest 인증에 원문 비번 필요 → 평문 저장).
접근 통제는 DB 권한으로: **`guardx_reader`는 이 테이블 SELECT 회수됨**
(`REVOKE`). 폴러(`guardx_writer`)만 읽는다. DB가 외부 노출로 바뀌면 재검토.

---

## 3. `congestion_prediction` — 예측 v2 계약 (해석 변경)

테이블 구조는 tier1 이후 동일하지만 **채워지는 의미가 v13에서 확정**됐다.

```sql
CREATE TABLE congestion_prediction (
    prediction_id   BIGSERIAL PRIMARY KEY,
    zone_id         INT NOT NULL REFERENCES zones(zone_id),
    predicted_at    TIMESTAMPTZ NOT NULL DEFAULT now(),  -- 예측을 "만든" 시각
    target_ts       TIMESTAMPTZ NOT NULL,                -- 예측이 "겨냥한" 시각
    predicted_count INT NOT NULL,                        -- p50 (반올림)
    model_version   TEXT,                                -- 'hw_damped_v1'
    config_version  INT                                  -- v13부터 NULL
);
```

| 컬럼 | v2 계약 |
|---|---|
| `predicted_at` / `target_ts` | **분리 저장** — horizon = `target_ts − predicted_at` (분). 이 분리 덕에 "그때 예측 vs 실측" 사후 정확도 평가가 가능 |
| `predicted_count` | 카메라 `/prediction`의 **p50(중앙값 예측)**. horizon {5,30,60,180}분 × 4행을 폴러가 **60초마다** 적재 |
| `model_version` | `'hw_damped_v1'` 고정 |
| `config_version` | **v13부터 NULL** — 예측이 형상 epoch에 비의존 (점유는 라인 형상과 무관). v9~v12 이력 호환용으로 컬럼만 존치 |

> [!note] 알려진 한계 (스키마 피드백 4호로 제안 예정)
> `predicted_count`가 **INT**라 p50이 반올림 저장된다 (±0.5 양자화). 또
> 구간(p10/p90)·용량초과확률(p_over_capacity)·warmup 플래그는 카메라가 주지만
> **아직 컬럼이 없어 버려진다**. 소수 p50 + 이 4개 컬럼 추가를 제안할 것 —
> 반영 전까지는 p50만 정수로 적재.

### 정확도 평가 쿼리 예 (실측과 대조)

```sql
-- horizon별 실전 MAE. 실측 = detections의 0패딩 분 중앙값(모델 관측과 동일 정의)
WITH fc AS (SELECT date_trunc('minute',ts) m, ts, count(*)::int c
            FROM detections WHERE ts > now()-interval '7 days' GROUP BY 1,2),
     mm AS (SELECT m, CASE WHEN count(*)<=150 THEN 0
                           ELSE (array_agg(c ORDER BY c))[count(*)-150] END actual
            FROM fc GROUP BY m),
     p  AS (SELECT round(extract(epoch FROM(target_ts-predicted_at))/60.0)::int h,
                   date_trunc('minute',target_ts) m, predicted_count pc
            FROM congestion_prediction WHERE target_ts < now()-interval '1 minute')
SELECT p.h, count(*),
       round(avg(abs(p.pc-coalesce(mm.actual,0)))::numeric,3) AS mae
FROM p LEFT JOIN mm USING(m) GROUP BY 1 ORDER BY 1;
```

(이 쿼리 기반 자동 리포트가 http://172.20.33.251:8088 에서 상시 갱신 — `rpi_b/report/`)

---

## 4. 유지보수 함수 (신규)

| 함수 | 역할 |
|---|---|
| `guardx_maintain(det=14, pred=180, dlog=90)` | **cron 진입점.** 파티션 7일 선생성 + det 14일 drop + prediction 180일·device_logs 90일 DELETE. 요약 문자열 반환 |
| `detections_ensure_partitions(days_ahead=7)` | 파티션 선생성 (maintain이 호출) |
| `detections_drop_old(retain_days=14)` | 오래된 파티션 drop (maintain이 호출) |

보존 기간을 바꾸려면 인자만: 예 `SELECT guardx_maintain(7);` = det 7일 보존.

---

## 5. 여러분이 확인할 것 — 체크리스트

- [ ] **`detections`를 INSERT하는 코드**가 있나 → PK가 `(detection_id, ts)`
      복합으로 바뀜. `ts` 없이 INSERT하면 실패. (폴러는 이미 대응)
- [ ] **`zones.capacity_limit`을 읽던 코드**가 있나 → `zone_thresholds.capacity_limit`로
      이동. 조인 한 번 추가.
- [ ] **`congestion_prediction`을 읽는 대시보드/기능** → `predicted_count`는
      정수 p50이고 `config_version`은 NULL임을 전제할 것. horizon은
      `target_ts − predicted_at`로 복원.
- [x] **`guardx_maintain()` cron** → ✅ 등록·가동 확인됨 (2026-07-22). 새로
      배포한 DB라면 재확인: `sudo crontab -l | grep guardx_maintain`.
- [ ] 기능3(동선추적) `tracks`/`track_path` — **구조 변경 없음.** `object_id`
      논리 연결·FK 없음 정책 그대로 (D7).

---

## 6. 권한 요약 (D9 최소 권한)

| 역할 | 권한 (2026-07-22 실측) |
|---|---|
| `guardx_writer` (폴러) | 전 테이블 INSERT/SELECT/UPDATE + 시퀀스 |
| `guardx_reader` | 전 테이블 SELECT — **단 `camera_credentials`는 회수됨** ✅ |
| `juan` (로컬 peer) | **전 테이블 풀권한** (INSERT/SELECT/UPDATE/DELETE/TRUNCATE/REFERENCES/TRIGGER) — 조건부 GRANT가 실제로 적용됨 |

> [!warning] 알려진 버그 — `guardx_admin` SELECT 불가 (✅ 실증됨)
> `SET ROLE guardx_admin; SELECT count(*) FROM detections;` →
> **`ERROR: permission denied for table detections`** (2026-07-22 재현 확인).
> v2 재구축 시 소유자 `guardx_admin`에 대한 명시적 GRANT가 빠진 것.
> **우회 2가지**: ① `sudo -u postgres psql` (슈퍼유저) ② `psql`을 juan 계정
> peer 인증으로 (juan은 전 테이블 SELECT 가능 — 위 표). 자동 리포트는 ①을 쓴다.
> **정식 수정**: `GRANT SELECT ON ALL TABLES IN SCHEMA public TO guardx_admin;`
> (RPi B 세션 처리 예정 — 미결 항목)

---

## 7. 재구축 방법 (참고)

```bash
# 신규/재구축 — 이 파일 하나
psql -U guardx_admin -d guardx -f schema.sql   # migration_*.sql 실행하지 말 것

# 기존 DB에 부분 적용해야 하면 — migration_*.sql (멱등, 재실행 안전)
```

---

## 8. 라이브 실측값 (2026-07-22 introspection)

DB introspection으로 확인한 실제 상태 — 이 문서의 표들이 사실과 일치함을 보증.

### 테이블 (본 테이블 16개 + detections 일 파티션 10개)

`spatial_ref_sys`는 PostGIS 시스템 테이블(우리가 만든 게 아님). 나머지 15개가
우리 스키마: cameras, camera_credentials, zones, zone_geometry_history,
zone_thresholds, detections(partitioned), zone_occupancy, zone_flow,
congestion_prediction, tracks, track_path, devices, device_logs, incidents,
alerts.

### FK 실측 (8개 — D7대로 시계열 테이블엔 없음)

```
alerts.incident_id            → incidents.incident_id
camera_credentials.camera_id  → cameras.camera_id
congestion_prediction.zone_id → zones.zone_id
devices.zone_id               → zones.zone_id
incidents.zone_id             → zones.zone_id
zone_geometry_history.zone_id → zones.zone_id
zone_thresholds.zone_id       → zones.zone_id
zones.camera_id               → cameras.camera_id
```
확인: detections·zone_occupancy·zone_flow·tracks·track_path·device_logs에는
FK **없음** (D7 — 보존주기 상이 + 대용량 INSERT 성능).

### detections 파티션 (10개, 서버 TZ=KST 경계)

```
detections_p20260720 : 07-20 00:00+09 ~ 07-21 00:00+09
... (일 단위) ...
detections_p20260729 : 07-29 00:00+09 ~ 07-30 00:00+09
```
- **PK = (detection_id, ts)** 복합 — 각 파티션에 자동 전파 확인.
- 파티션 경계가 `+09`(KST) — `detections_ensure_partitions`가 서버 TZ 기준
  일 경계를 씀 (일관성 위해 TZ 변경 금지).

### 행 수 (핵심 테이블)

| 테이블 | 행 수 | 비고 |
|---|---|---|
| detections | **358,370** | 07-20 17:35 ~ 현재 (이틀치, 유실 없음) |
| congestion_prediction | **6,620** | 60초 × 4 horizon 적재 누적 |
| devices | 11 | 시드 (센서 5 + 액추에이터 6) |
| cameras / zones / zone_thresholds / camera_credentials / zone_geometry_history | 각 1 | 단일 존 시드 |
| zone_occupancy / zone_flow / tracks / track_path | 0 | **아직 미적재** (기능1 집계·기능3 동선 담당 대기) |

> zone_occupancy·zone_flow·tracks가 0인 건 정상 — detections(원천)에서
> **집계·재구성해 채우는 건 각 기능 담당의 몫**(D7). 폴러는 원천만 적재한다.

### 함수 (3개, 기본 인자 확인)

```
guardx_maintain(p_detections_days=14, p_prediction_days=180, p_device_log_days=90) → text
detections_ensure_partitions(days_ahead=7) → int
detections_drop_old(retain_days=14) → int
```

### cron (2건 실측 — root crontab)

```
5 0 * * *    guardx_maintain()           # 보존 정책 (매일 00:05)
*/10 * * * * guardx-gen-report.sh         # 예측 리포트 (10분)
```

### CHECK 제약

```
zone_thresholds.chk_ratio_order : warn_ratio < critical_ratio
zone_thresholds.chk_ratio_range : warn_ratio > 0 AND critical_ratio <= 1
```

---

문의는 카메라/폴러 사이드로. 예측 계약 상세는 `juan_application/docs/CAMERA_API_v13.md`.
