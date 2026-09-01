-- ============================================================================
-- 02_schema.sql — GuardX DB 스키마 본체 (v2 — 2026-07-20 전면 재구축)
--
-- 실행 위치 : guardx DB 내부
-- 실행 계정 : guardx_admin
-- 실행 횟수 : DB를 밀고 재구축할 때마다 반복 실행 가능 (테스트 사이클용)
-- 실행 방법 : psql -U guardx_admin -d guardx -f schema.sql
--
-- v2 변경 요약 (2026-07-20)
--   * 구 마이그레이션 3본(tier1·tier2·camera_credentials)을 본체에 통합.
--     신규 구축은 이 파일 하나로 끝 — migration_*.sql 은 기존 DB용 이력 자료.
--   * detections 를 일 단위 파티션으로 전환 + 14일 sliding window 보존.
--     (실측 초당 7.41행 → 연 39GB. 파티션 drop 은 배치 DELETE 와 달리
--      SD카드 쓰기 증폭이 없음. 14일 정상상태 ≈ 3.8GB)
--   * congestion_prediction 180일 · device_logs 90일 배치 삭제 (소량이라 DELETE 로 충분).
--   * 시드를 현 운용에 맞춤: 채널 1 단일 존, capacity 20 (카메라 앱 상수와 일치).
--
-- ⚠ 보존 정책은 guardx_maintain() 을 하루 1회 실행해야 동작한다 (폴러 재가동 전제):
--     sudo crontab -e  →  5 0 * * * sudo -u postgres psql -d guardx -c "SELECT guardx_maintain();"
--   파티션은 7일치 선생성 — cron 이 7일 넘게 죽으면 detections INSERT 가
--   "no partition" 으로 실패한다 (폴러는 크래시 없이 로그만 남기고 계속).
--
-- 설계 원칙
--   1) 세 기둥: ZONES(공간 허브) - DETECTIONS(원천) - INCIDENTS(사건 수렴점)
--   2) D7  대용량 시계열 테이블(detections, device_logs, track_path,
--          zone_occupancy, zone_flow)은 FK 제약 없이 생성.
--          정합성 검증은 각 컬럼 값을 채우는 애플리케이션(라파 B) 책임.
--          대신 JOIN에 쓰이는 컬럼은 인덱스를 명시적으로 생성한다.
--   3) 점선 관계(ERD 기준, FK 아님)
--        - zones <-> detections : ST_Contains 공간조인
--        - tracks <-> detections : object_id 논리연결 (보관주기 상이)
-- ============================================================================

-- 재실행용 리셋: 테이블만 삭제 (참조 역순).
-- DROP SCHEMA public CASCADE는 public에 설치된 PostGIS까지 삭제하므로 금지
DROP TABLE IF EXISTS alerts                CASCADE;
DROP TABLE IF EXISTS incidents             CASCADE;
DROP TABLE IF EXISTS device_logs           CASCADE;
DROP TABLE IF EXISTS devices               CASCADE;
DROP TABLE IF EXISTS trajectory_segments   CASCADE;
DROP TABLE IF EXISTS track_path            CASCADE;
DROP TABLE IF EXISTS tracks                CASCADE;
DROP TABLE IF EXISTS congestion_prediction CASCADE;
DROP TABLE IF EXISTS zone_flow             CASCADE;
DROP TABLE IF EXISTS zone_occupancy        CASCADE;
DROP TABLE IF EXISTS detections            CASCADE;   -- 파티션도 함께 삭제됨
DROP TABLE IF EXISTS faces                 CASCADE;
DROP TABLE IF EXISTS line_flow             CASCADE;
DROP TABLE IF EXISTS camera_credentials    CASCADE;
DROP TABLE IF EXISTS zone_thresholds       CASCADE;
DROP TABLE IF EXISTS zone_geometry_history CASCADE;
DROP TABLE IF EXISTS zones                 CASCADE;
DROP TABLE IF EXISTS cameras               CASCADE;

DROP FUNCTION IF EXISTS guardx_maintain(int, int, int);
DROP FUNCTION IF EXISTS guardx_maintain(int, int, int, int, int, int);
DROP FUNCTION IF EXISTS detections_ensure_partitions(int);
DROP FUNCTION IF EXISTS detections_drop_old(int);


-- ============================================================================
-- 1. 코어 — 공간 기반 + 원천 데이터
-- ============================================================================

-- ----------------------------------------------------------------------------
-- cameras : 카메라 채널 명세. 채널 1개 = 1행
-- ----------------------------------------------------------------------------
CREATE TABLE cameras (
    camera_id     SERIAL PRIMARY KEY,
    camera_name   TEXT NOT NULL,
    resolution_w  INT NOT NULL,
    resolution_h  INT NOT NULL,
    status        TEXT NOT NULL DEFAULT 'ONLINE'
);

-- ----------------------------------------------------------------------------
-- camera_credentials : 카메라 접속 계정 (구 migration_camera_credentials.sql)
--   비밀번호 평문 저장 — DB는 RPi B localhost 전용 + digest 인증은 원문 필요.
--   접근 통제는 DB 권한으로 (§6에서 reader 의 SELECT 를 명시적으로 회수).
--   외부 노출 DB로 바뀌면 이 판단 재검토.
-- ----------------------------------------------------------------------------
CREATE TABLE camera_credentials (
    camera_id  INT PRIMARY KEY REFERENCES cameras (camera_id),
    cam_user   TEXT NOT NULL,
    cam_pass   TEXT NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ----------------------------------------------------------------------------
-- zones : 공간 허브. 운영자가 그린 ROI. camera_id는 정적 데이터라 FK 유지.
--   config_version = 현재 유효 roi_polygon 의 형상 epoch (구 tier2).
--   capacity 는 zone_thresholds 로 분리 (단일 진실원천).
-- ----------------------------------------------------------------------------
CREATE TABLE zones (
    zone_id         SERIAL PRIMARY KEY,
    zone_name       TEXT NOT NULL,
    camera_id       INT NOT NULL REFERENCES cameras (camera_id),
    channel         INT NOT NULL,
    roi_polygon     geometry(Polygon, 0) NOT NULL,
    zone_type       TEXT NOT NULL DEFAULT 'normal',   -- normal / entrance / restricted
    config_version  INT NOT NULL DEFAULT 1
);

CREATE INDEX idx_zones_roi ON zones USING GIST (roi_polygon);
-- 채널당 존 1개 보장 (폴러 lookupZoneId 가정의 무결성 장치, v2.3).
-- 채널 내 존 세분화 도입 시 이 제약을 풀고 "대표 존" 개념과 함께 재설계.
CREATE UNIQUE INDEX uq_zones_camera_channel ON zones (camera_id, channel);

COMMENT ON COLUMN zones.config_version IS
    '현재 유효한 roi_polygon 의 형상 epoch. detections 태그와 대조되는 기준.';

-- ----------------------------------------------------------------------------
-- zone_geometry_history : 형상 버저닝 (구 tier2). zones.zone_id 는 영구 고정 →
--   형상 변경 = 구 행 valid_to 마감 + 신 행 추가. zones.roi_polygon 은 현재 캐시.
-- ----------------------------------------------------------------------------
CREATE TABLE zone_geometry_history (
    geom_id        BIGSERIAL PRIMARY KEY,
    zone_id        INT NOT NULL REFERENCES zones (zone_id),
    roi_polygon    geometry(Polygon) NOT NULL,
    config_version INT NOT NULL,
    valid_from     TIMESTAMPTZ NOT NULL DEFAULT now(),
    valid_to       TIMESTAMPTZ,                       -- NULL = 현재 유효 행
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_zgh_zone       ON zone_geometry_history (zone_id);
CREATE INDEX idx_zgh_zone_valid ON zone_geometry_history (zone_id, valid_from, valid_to);
CREATE INDEX idx_zgh_geom       ON zone_geometry_history USING GIST (roi_polygon);
-- 존당 '열린'(현재) 형상 행은 최대 1개 — 마감 안 한 채 새 행 추가하는 실수 방지
CREATE UNIQUE INDEX uq_zgh_open ON zone_geometry_history (zone_id) WHERE valid_to IS NULL;

-- ----------------------------------------------------------------------------
-- zone_thresholds : 운영 설정 (구 tier2). 혼잡비 = predicted_count / capacity_limit
-- ----------------------------------------------------------------------------
CREATE TABLE zone_thresholds (
    zone_id        INT PRIMARY KEY REFERENCES zones (zone_id),
    capacity_limit INT,
    warn_ratio     REAL NOT NULL DEFAULT 0.75,    -- 주의 단계 임계
    critical_ratio REAL NOT NULL DEFAULT 0.90,    -- 위험 단계 임계
    updated_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT chk_ratio_order CHECK (warn_ratio < critical_ratio),
    CONSTRAINT chk_ratio_range CHECK (warn_ratio > 0 AND critical_ratio <= 1)
);

-- ----------------------------------------------------------------------------
-- detections : 원천 데이터. 초당 다건 유입 → FK 없음 (D7).
--   v2: 일 단위 RANGE 파티션 + 14일 보존 (guardx_maintain 이 drop).
--   파티션 테이블은 PK에 파티션 키가 포함돼야 함 → (detection_id, ts).
--   DEFAULT 파티션은 두지 않는다 — 잔여 행이 이후 파티션 생성을 막아
--   복구가 수동이 되기 때문. 파티션 없는 날짜의 INSERT 는 즉시 에러로 드러난다.
-- ----------------------------------------------------------------------------
CREATE TABLE detections (
    detection_id  BIGSERIAL,
    camera_id     INT NOT NULL,          -- cameras 논리 참조 (FK 없음, D7)
    channel       INT NOT NULL,
    object_id     INT NOT NULL,          -- 채널 간 유지 여부 미확정 (O6)
    category      SMALLINT NOT NULL,     -- 1=Human, 2=Face, 3=Head (v15)
    parent_id     INT,                   -- Face/Head의 부모(사람) object_id. Human은 NULL
    likelihood    REAL,
    rect_sx       INT,
    rect_sy       INT,
    rect_ex       INT,
    rect_ey       INT,
    geom          geometry(Point, 0) NOT NULL,
    ts            TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (detection_id, ts)
) PARTITION BY RANGE (ts);

-- 조회 패턴: 채널/시간 범위 필터, 공간조인(geom) → 3종 인덱스 (파티션에 자동 전파)
CREATE INDEX idx_detections_camera  ON detections (camera_id);
CREATE INDEX idx_detections_ts      ON detections (ts);
CREATE INDEX idx_detections_geom    ON detections USING GIST (geom);
CREATE INDEX idx_detections_objid   ON detections (object_id, ts);

-- ── 파티션 관리 함수 (서버 timezone 기준 일 단위 — 경계 일관성 위해 TZ 변경 금지) ──

-- 오늘부터 days_ahead 일까지 detections_pYYYYMMDD 파티션 선생성. 반환 = 생성 수
CREATE FUNCTION detections_ensure_partitions(days_ahead INT DEFAULT 7)
RETURNS INT LANGUAGE plpgsql AS $$
DECLARE
    d date; nm text; created int := 0;
BEGIN
    FOR i IN 0..days_ahead LOOP
        d  := now()::date + i;
        nm := 'detections_p' || to_char(d, 'YYYYMMDD');
        IF NOT EXISTS (SELECT 1 FROM pg_class WHERE relname = nm) THEN
            EXECUTE format(
                'CREATE TABLE %I PARTITION OF detections FOR VALUES FROM (%L) TO (%L)',
                nm, d::timestamptz, (d + 1)::timestamptz);
            created := created + 1;
        END IF;
    END LOOP;
    RETURN created;
END $$;

-- retain_days 보다 오래된 일 파티션 drop. 반환 = drop 수
CREATE FUNCTION detections_drop_old(retain_days INT DEFAULT 14)
RETURNS INT LANGUAGE plpgsql AS $$
DECLARE
    part record; dropped int := 0;
    cutoff date := now()::date - retain_days;
BEGIN
    FOR part IN
        SELECT c.relname
        FROM pg_inherits i
        JOIN pg_class c ON c.oid = i.inhrelid
        JOIN pg_class p ON p.oid = i.inhparent
        WHERE p.relname = 'detections'
          AND c.relname ~ '^detections_p\d{8}$'
          AND to_date(substring(c.relname FROM '\d{8}$'), 'YYYYMMDD') < cutoff
    LOOP
        EXECUTE format('DROP TABLE %I', part.relname);
        dropped := dropped + 1;
    END LOOP;
    RETURN dropped;
END $$;

-- ----------------------------------------------------------------------------
-- faces : face bestshot 이벤트 (v15). object_id = 사람 object_id — detections
--         와의 조인 키. 얼굴이 안 잡힌(뒷모습) 사람은 행이 없음 = "빈칸".
-- ----------------------------------------------------------------------------
CREATE TABLE faces (
    face_id     BIGSERIAL PRIMARY KEY,
    camera_id   INT NOT NULL,          -- cameras 논리 참조 (FK 없음, D7)
    channel     INT NOT NULL,
    object_id   INT NOT NULL,          -- 사람 object_id (detections.object_id)
    likelihood  REAL,
    rect_sx     INT,                   -- 얼굴 bbox (2592×1520 픽셀, ImageRefShape)
    rect_sy     INT,
    rect_ex     INT,
    rect_ey     INT,
    image_ref   TEXT,                  -- 카메라 내 JPEG 경로 (/download/chN/…)
    ts          TIMESTAMPTZ NOT NULL
);

CREATE INDEX idx_faces_objid ON faces (object_id, ts);
CREATE INDEX idx_faces_ts    ON faces (ts);

-- ----------------------------------------------------------------------------
-- line_flow : 라인 통과량 분 단위 원장 (v15). /events 누적 카운트의 폴러 델타.
--             rule = 라인 이름 (채널 매핑은 소비자 몫 — 라인은 옮겨 다님).
--             0인 분은 행 없음 (공백 = 0 해석). 카메라 재시작 생존 기록.
-- ----------------------------------------------------------------------------
CREATE TABLE line_flow (
    rule        TEXT NOT NULL,
    action      TEXT NOT NULL,         -- Right / Left
    bucket_ts   TIMESTAMPTZ NOT NULL,  -- 분 단위 절단
    flow_count  INT NOT NULL,
    PRIMARY KEY (rule, action, bucket_ts)
);

CREATE INDEX idx_line_flow_ts ON line_flow (bucket_ts);

-- 일일 유지보수 진입점 — cron 에서 이것 하나만 부르면 됨 (파일 서두 참조)
CREATE FUNCTION guardx_maintain(
    p_detections_days  INT DEFAULT 14,    -- detections  : 파티션 drop
    p_prediction_days  INT DEFAULT 180,   -- congestion_prediction : 배치 DELETE (소량)
    p_device_log_days  INT DEFAULT 90,    -- device_logs : 배치 DELETE
    p_faces_days       INT DEFAULT 30,    -- faces       : 배치 DELETE (소량)
    p_flow_days        INT DEFAULT 180,   -- line_flow   : 배치 DELETE (소량)
    p_occupancy_days   INT DEFAULT 365)   -- zone_occupancy : 배치 DELETE (극소량)
RETURNS TEXT LANGUAGE plpgsql AS $$
DECLARE
    n_created int; n_dropped int; n_pred bigint; n_dlog bigint;
    n_face bigint; n_flow bigint; n_occ bigint;
BEGIN
    n_created := detections_ensure_partitions(7);
    n_dropped := detections_drop_old(p_detections_days);
    DELETE FROM congestion_prediction
        WHERE predicted_at < now() - make_interval(days => p_prediction_days);
    GET DIAGNOSTICS n_pred = ROW_COUNT;
    DELETE FROM device_logs
        WHERE ts < now() - make_interval(days => p_device_log_days);
    GET DIAGNOSTICS n_dlog = ROW_COUNT;
    DELETE FROM faces
        WHERE ts < now() - make_interval(days => p_faces_days);
    GET DIAGNOSTICS n_face = ROW_COUNT;
    DELETE FROM line_flow
        WHERE bucket_ts < now() - make_interval(days => p_flow_days);
    GET DIAGNOSTICS n_flow = ROW_COUNT;
    DELETE FROM zone_occupancy
        WHERE bucket_ts < now() - make_interval(days => p_occupancy_days);
    GET DIAGNOSTICS n_occ = ROW_COUNT;
    RETURN format('partitions +%s/-%s, predictions -%s, device_logs -%s, '
                  'faces -%s, line_flow -%s, zone_occupancy -%s',
                  n_created, n_dropped, n_pred, n_dlog, n_face, n_flow, n_occ);
END $$;


-- ============================================================================
-- 2. 기능1 — 군중예측 (구역 축, detections 집계 산출물)
-- ============================================================================

-- ----------------------------------------------------------------------------
-- zone_occupancy : 구역별 인원수 스냅샷. (zone_id, bucket_ts) 복합 PK
-- ----------------------------------------------------------------------------
CREATE TABLE zone_occupancy (
    zone_id       INT NOT NULL,          -- zones 논리 참조 (FK 없음, D7)
    bucket_ts     TIMESTAMPTZ NOT NULL,
    person_count  INT NOT NULL,
    PRIMARY KEY (zone_id, bucket_ts)
);

CREATE INDEX idx_occupancy_ts ON zone_occupancy (bucket_ts);

-- ----------------------------------------------------------------------------
-- zone_flow : 구역 간 이동 인원. object_id 전역 유지를 전제로 계산 (O6 확인 후 조정)
-- ----------------------------------------------------------------------------
CREATE TABLE zone_flow (
    from_zone_id  INT NOT NULL,
    to_zone_id    INT NOT NULL,
    bucket_ts     TIMESTAMPTZ NOT NULL,
    flow_count    INT NOT NULL,
    PRIMARY KEY (from_zone_id, to_zone_id, bucket_ts)
);

CREATE INDEX idx_flow_ts ON zone_flow (bucket_ts);

-- ----------------------------------------------------------------------------
-- congestion_prediction : 예측 모델 출력. predicted_at(생성 시각)과
--                         target_ts(대상 시각)를 분리해 정확도 평가 가능하게 함.
--   v2 계약: 폴러가 60초마다 카메라 /prediction 의 p50 을 horizon
--   {5,30,60,180}분 × 4행으로 적재 (model_version='hw_damped_v1').
--   스키마 피드백 4호 (2026-07-28 반영): p10/p90/p_over_capacity/warmup 신설.
--   ⚠ p_over_capacity: 카메라 -1(불명)은 NULL 적재 (확률 0 아님).
--   ⚠ warmup 행도 적재 (플래그 구분) — 소비자는 warmup IS NOT TRUE 로 걸러라.
-- ----------------------------------------------------------------------------
CREATE TABLE congestion_prediction (
    prediction_id    BIGSERIAL PRIMARY KEY,
    zone_id          INT NOT NULL REFERENCES zones (zone_id),
    predicted_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    target_ts        TIMESTAMPTZ NOT NULL,
    predicted_count  INT NOT NULL,
    p10              REAL,              -- 예측 하한 (10분위, 명 단위 소수)
    p90              REAL,              -- 예측 상한 (90분위)
    p_over_capacity  REAL,              -- 용량 초과 확률 0..1 (-1 불명 → NULL)
    warmup           BOOLEAN,           -- true = 워밍업 중 예측 (평가·경보 제외)
    model_version    TEXT,
    config_version   INT                -- v9~v12 epoch 태그 이력용. v13부터 예측은
                                        -- epoch 비의존 → 폴러는 NULL 적재
);

CREATE INDEX idx_prediction_target ON congestion_prediction (zone_id, target_ts);

COMMENT ON COLUMN congestion_prediction.config_version IS
    'v9~v12 형상 epoch 태그 (이력 호환). v13(hw_damped_v1)부터 예측은 epoch 비의존 — NULL.';


-- ============================================================================
-- 3. 기능3 — 동선추적 (사람 축, detections 재구성)
-- ============================================================================

-- ----------------------------------------------------------------------------
-- tracks : 객체(사람) 1명 = 1행. detections와는 object_id로 논리 연결 (FK 없음,
--          보관주기가 서로 달라 물리적 FK를 걸면 detections 삭제가 막힘)
-- ----------------------------------------------------------------------------
CREATE TABLE tracks (
    track_id        BIGSERIAL PRIMARY KEY,
    object_id       INT NOT NULL,
    first_seen_at   TIMESTAMPTZ NOT NULL,
    last_seen_at    TIMESTAMPTZ NOT NULL,
    is_suspect      BOOLEAN NOT NULL DEFAULT FALSE,
    risk_level      TEXT,
    attributes      JSONB                -- cloth / face / access 요약
);

CREATE INDEX idx_tracks_objid ON tracks (object_id);

-- ----------------------------------------------------------------------------
-- track_path : 객체의 시간순 발자국. FK 없음 (D7) — tracks와 같은 이유
-- ----------------------------------------------------------------------------
CREATE TABLE track_path (
    track_id  BIGINT NOT NULL,
    ts        TIMESTAMPTZ NOT NULL,
    geom      geometry(Point, 0) NOT NULL,
    zone_id   INT,
    PRIMARY KEY (track_id, ts)
);

CREATE INDEX idx_track_path_geom ON track_path USING GIST (geom);
CREATE INDEX idx_track_path_zone ON track_path (zone_id);

-- ----------------------------------------------------------------------------
-- trajectory_segments : CAP analytics/trajectories segment data.
-- This table is used for business analytics such as dwell time and zone flow.
-- It intentionally stores anonymous segment-level records instead of long-term
-- person identities.
-- ----------------------------------------------------------------------------
CREATE TABLE trajectory_segments (
    camera_id       INT NOT NULL,
    segment_id      BIGINT NOT NULL,

    global_id       BIGINT NOT NULL,
    display_id      BIGINT,
    object_id       INT NOT NULL,

    channel         INT NOT NULL,
    raw_channel     INT NOT NULL,
    zone_id         INT,

    start_ms        BIGINT NOT NULL,
    end_ms          BIGINT NOT NULL,
    dwell_ms        BIGINT NOT NULL,

    confidence      REAL NOT NULL,
    is_reliable     BOOLEAN NOT NULL,
    state           TEXT NOT NULL,

    segment_ts      TIMESTAMPTZ NOT NULL,
    served_at       TIMESTAMPTZ NOT NULL DEFAULT now(),

    PRIMARY KEY (camera_id, segment_id, start_ms)
);

CREATE INDEX idx_trajectory_segments_global_start
ON trajectory_segments (camera_id, global_id, start_ms);

CREATE INDEX idx_trajectory_segments_zone_reliable
ON trajectory_segments (zone_id, is_reliable, segment_ts);

CREATE INDEX idx_trajectory_segments_served
ON trajectory_segments (served_at);

CREATE INDEX idx_trajectory_segments_display
ON trajectory_segments (display_id, segment_ts);

CREATE OR REPLACE VIEW reliable_trajectory_segments AS
SELECT *
FROM trajectory_segments
WHERE is_reliable = true;

CREATE OR REPLACE VIEW trajectory_zone_dwell_summary AS
SELECT
    zone_id,
    COUNT(*) AS visit_count,
    AVG(dwell_ms) AS avg_dwell_ms,
    SUM(dwell_ms) AS total_dwell_ms,
    AVG(confidence) AS avg_confidence
FROM trajectory_segments
WHERE is_reliable = true
GROUP BY zone_id;

CREATE OR REPLACE VIEW trajectory_zone_transition_summary AS
WITH ordered_segments AS (
    SELECT
        camera_id,
        global_id,
        zone_id,
        start_ms,
        LEAD(zone_id) OVER (
            PARTITION BY camera_id, global_id
            ORDER BY start_ms
        ) AS next_zone_id
    FROM trajectory_segments
    WHERE is_reliable = true
      AND zone_id IS NOT NULL
)
SELECT
    zone_id AS from_zone_id,
    next_zone_id AS to_zone_id,
    COUNT(*) AS transition_count
FROM ordered_segments
WHERE next_zone_id IS NOT NULL
  AND zone_id <> next_zone_id
GROUP BY zone_id, next_zone_id;


-- ============================================================================
-- 4. 기능2 — 재난대처 (디바이스 그룹)
-- ============================================================================

-- ----------------------------------------------------------------------------
-- devices : 센서 · 액추에이터 · 비상버튼 통합 명세 (role 컬럼으로 구분)
-- ----------------------------------------------------------------------------
CREATE TABLE devices (
    device_id    SERIAL PRIMARY KEY,
    device_role  TEXT NOT NULL,     -- sensor / actuator / button
    device_type  TEXT NOT NULL,     -- gas / temp_hum / spark / vibration /
                                     -- button / lcd / amp / fan_a / fan_b /
                                     -- motor_a / motor_b
    zone_id      INT NOT NULL REFERENCES zones (zone_id),
    rpi_host     TEXT NOT NULL,     -- rpi_a / rpi_c
    status       TEXT NOT NULL DEFAULT 'ONLINE'
);

-- ----------------------------------------------------------------------------
-- device_logs : 센서 측정값(입력) + 액추에이터 동작(출력) 통합 기록
--               incident_id는 액추에이터 반응 시에만 채워짐 (nullable)
--               보존: guardx_maintain() 이 90일 지난 행 삭제
-- ----------------------------------------------------------------------------
CREATE TABLE device_logs (
    log_id       BIGSERIAL PRIMARY KEY,
    device_id    INT NOT NULL,      -- devices 논리 참조 (FK 없음, D7)
    incident_id  INT,               -- incidents 논리 참조 (FK 없음, D7)
    value        REAL,
    action       TEXT,
    triggered_by TEXT,              -- auto / manual
    ts           TIMESTAMPTZ NOT NULL
);

CREATE INDEX idx_device_logs_device   ON device_logs (device_id);
CREATE INDEX idx_device_logs_incident ON device_logs (incident_id);
CREATE INDEX idx_device_logs_ts       ON device_logs (ts);


-- ============================================================================
-- 5. 수렴점 + 출력
-- ============================================================================

-- ----------------------------------------------------------------------------
-- incidents : 사건 수렴점. source_type/source_id는 다형성 참조라 FK를 걸지
--             않음 (센서/버튼/탐지/예측 어디서든 사건이 발생할 수 있음)
-- ----------------------------------------------------------------------------
CREATE TABLE incidents (
    incident_id    SERIAL PRIMARY KEY,
    zone_id        INT NOT NULL REFERENCES zones (zone_id),
    incident_type  TEXT NOT NULL,    -- fire / gas / theft / congestion / button
    source_type    TEXT NOT NULL,    -- sensor / button / detection / prediction
    source_id      BIGINT,
    severity       TEXT,
    status         TEXT NOT NULL DEFAULT 'open',  -- open / acknowledged / resolved
    snapshot_path  TEXT,             -- best-shot 이미지 파일 경로 (D6, O5)
    detected_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_incidents_zone ON incidents (zone_id);
CREATE INDEX idx_incidents_ts   ON incidents (detected_at);

-- ----------------------------------------------------------------------------
-- alerts : 알림 발송 이력
-- ----------------------------------------------------------------------------
CREATE TABLE alerts (
    alert_id           SERIAL PRIMARY KEY,
    incident_id        INT NOT NULL REFERENCES incidents (incident_id),
    message            TEXT NOT NULL,
    broadcast_channel  TEXT,         -- vms_popup / amp_broadcast
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);


-- ============================================================================
-- 6. 권한 부여 (D9 — 최소 권한 원칙)
--
-- ALTER DEFAULT PRIVILEGES를 함께 지정해, 이 스크립트 실행 이후 새로
-- 추가되는 테이블에도 동일 권한이 자동 적용되도록 함
-- (detections 일 파티션은 guardx_admin 소유 함수가 만들므로 이 기본권한을 상속.
--  단, 파티션 INSERT/SELECT 권한 검사는 부모 테이블 기준이라 사실상 무관)
-- ============================================================================

GRANT USAGE ON SCHEMA public TO guardx_writer, guardx_reader;

GRANT INSERT, UPDATE, SELECT ON ALL TABLES IN SCHEMA public TO guardx_writer;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO guardx_writer;

GRANT SELECT ON ALL TABLES IN SCHEMA public TO guardx_reader;
-- 카메라 계정은 read API 로 새어나가면 안 됨 — 폴러(writer)만 읽는다
REVOKE ALL ON camera_credentials FROM guardx_reader;

ALTER DEFAULT PRIVILEGES IN SCHEMA public
    GRANT INSERT, UPDATE, SELECT ON TABLES TO guardx_writer;
ALTER DEFAULT PRIVILEGES IN SCHEMA public
    GRANT SELECT ON TABLES TO guardx_reader;

-- juan 계정(피어 인증 로컬 운영)이 있으면 폴러와 동급 권한 부여 (없으면 조용히 스킵)
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'juan') THEN
        GRANT USAGE ON SCHEMA public TO juan;
        GRANT INSERT, UPDATE, SELECT ON ALL TABLES IN SCHEMA public TO juan;
        GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO juan;
    END IF;
END $$;


-- ============================================================================
-- 7. 시드 데이터 — 현 운용 기준 (2026-07-20): 채널 1 단일, 라인 1개, IVA 영역 없음
-- ============================================================================

-- 카메라 1대 (192.168.0.3, 2592×1520)
INSERT INTO cameras (camera_name, resolution_w, resolution_h)
VALUES ('main_camera', 2592, 1520);

-- 카메라 접속 계정 (운영 시 폴링 전용 계정으로 교체 권장)
INSERT INTO camera_credentials (camera_id, cam_user, cam_pass)
VALUES (1, 'admin', 'qkdwnsgks123!');

-- 존 4개 — 채널 0~3 전체 화면 (v2.2: **실측으로 0-기반 확정**, 2026-07-27 —
-- MetadataManager_N = 채널 N, WiseAI 설정과 동일 번호). 운용 센서 ch1을
-- 첫 행으로 유지 → zone_A(ch1)=zone_id 1 불변 (db_smoke·기존 데이터 정합).
INSERT INTO zones (zone_name, camera_id, channel, roi_polygon)
SELECT v.zname, 1, v.ch, ST_MakeEnvelope(0, 0, 2592, 1520, 0)
FROM (VALUES ('zone_A', 1), ('zone_B', 0), ('zone_C', 2), ('zone_D', 3))
     AS v(zname, ch);

-- 형상 이력 첫 버전 (config_version=1)
INSERT INTO zone_geometry_history (zone_id, roi_polygon, config_version)
SELECT zone_id, roi_polygon, 1 FROM zones;

-- 운영 임계 — 전 존 capacity 10 (2026-07-28 운용 확정: warn 8명 / critical 9명).
-- 카메라 앱 상수(kCapacityLimit=20)와 불일치는 무해 — p_over_capacity를 경보에
-- 안 쓰기로 결정(역할 분리: 카메라=분위수, rpi_b=임계 판정). capacity NULL 존은
-- task_alert가 판단 불가로 skip (비무장).
INSERT INTO zone_thresholds (zone_id, capacity_limit)
SELECT z.zone_id, 10 FROM zones z;

-- devices 시드 — 라파 A 센서 5종 + 라파 C 액추에이터 6종 (전부 zone 1 임시 배정)
INSERT INTO devices (device_role, device_type, zone_id, rpi_host) VALUES
    ('sensor',   'gas',       1, 'rpi_a'),
    ('sensor',   'temp_hum',  1, 'rpi_a'),
    ('sensor',   'spark',     1, 'rpi_a'),
    ('sensor',   'vibration', 1, 'rpi_a'),
    ('button',   'button',    1, 'rpi_a'),
    ('actuator', 'lcd',       1, 'rpi_c'),
    ('actuator', 'amp',       1, 'rpi_c'),
    ('actuator', 'fan_a',     1, 'rpi_c'),
    ('actuator', 'motor_a',   1, 'rpi_c'),
    ('actuator', 'fan_b',     1, 'rpi_c'),
    ('actuator', 'motor_b',   1, 'rpi_c');

-- detections 파티션 초기 생성 (오늘 ~ +7일). 이후는 cron 의 guardx_maintain() 몫
SELECT detections_ensure_partitions(7);


-- ============================================================================
-- 8. 설치 확인
-- ============================================================================

-- 본 테이블 17개 + detections_pYYYYMMDD 일 파티션 8개가 나와야 정상
SELECT table_name FROM information_schema.tables
WHERE table_schema = 'public' ORDER BY table_name;

-- 시드 데이터 확인 (1 / 4 / 11 / 4 / 1 이 나와야 정상)
SELECT 'cameras' AS table_name, count(*) FROM cameras
UNION ALL
SELECT 'zones', count(*) FROM zones
UNION ALL
SELECT 'devices', count(*) FROM devices
UNION ALL
SELECT 'zone_thresholds', count(*) FROM zone_thresholds
UNION ALL
SELECT 'camera_credentials', count(*) FROM camera_credentials;

-- 유지보수 함수 동작 확인 (요약 문자열 반환)
SELECT guardx_maintain();
