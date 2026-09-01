-- ============================================================
-- GuardX v15 — 신규 적재 3종 (occupancy·faces·line_flow) 지원
-- 실행:  sudo -u postgres psql -d guardx -f Database/migration_v15_feeds.sql
--
-- 추가 전용 · 멱등 · 폴러 무중단.
--   [A] faces      : face bestshot 이벤트 (사람 object_id 링크 + bbox + JPEG 경로)
--   [B] line_flow  : 라인 통과량 분 단위 원장 (/events 누적 카운트의 델타)
--   [C] guardx_maintain 확장 : faces 30일 · line_flow 180일 · zone_occupancy 365일
--   zone_occupancy 는 기존 테이블 그대로 사용 (폴러가 채우기 시작).
-- ============================================================

BEGIN;

-- [0] detections 확장 — v15부터 category 1=Human, 2=Face, 3=Head (실측:
--     카메라가 매 프레임 Face/Head를 사람의 자식 객체로 송출 — VMS 블러 원천).
--     parent_id = 사람 object_id (Human 행은 NULL). 파티션 부모에 ALTER →
--     전 파티션 자동 전파.
ALTER TABLE detections ADD COLUMN IF NOT EXISTS parent_id INT;
COMMENT ON COLUMN detections.category IS '1=Human, 2=Face, 3=Head (v15)';
COMMENT ON COLUMN detections.parent_id IS 'Face/Head의 부모(사람) object_id. Human은 NULL';

-- [A] faces — 얼굴 1건 = 1행. object_id = 사람 object_id (detections 조인 키).
--     뒷모습 등으로 얼굴이 안 잡힌 사람은 행이 없음 = "빈칸" 의미론.
--     image_ref 는 카메라 내 JPEG 경로 — 카메라 보존 기간 내에서만 유효.
CREATE TABLE IF NOT EXISTS faces (
    face_id     BIGSERIAL PRIMARY KEY,
    camera_id   INT NOT NULL,          -- cameras 논리 참조 (FK 없음, D7)
    channel     INT NOT NULL,
    object_id   INT NOT NULL,          -- 사람 object_id (detections.object_id)
    likelihood  REAL,
    rect_sx     INT,                   -- 얼굴 bbox (2592×1520 픽셀, ImageRefShape)
    rect_sy     INT,
    rect_ex     INT,
    rect_ey     INT,
    image_ref   TEXT,                  -- /download/chN/….jpg
    ts          TIMESTAMPTZ NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_faces_objid ON faces (object_id, ts);
CREATE INDEX IF NOT EXISTS idx_faces_ts    ON faces (ts);

-- [B] line_flow — (룰, 방향, 분) 당 통과 수. 0인 분은 행 없음(공백=0 해석).
--     rule 이 라인 이름(name1=ch0 라인, name2=ch1 라인) — 채널 매핑은 소비자 몫
--     (라인은 옮겨 다니므로 채널을 굳지 않는다).
CREATE TABLE IF NOT EXISTS line_flow (
    rule        TEXT NOT NULL,
    action      TEXT NOT NULL,         -- Right / Left
    bucket_ts   TIMESTAMPTZ NOT NULL,  -- 분 단위 절단
    flow_count  INT NOT NULL,
    PRIMARY KEY (rule, action, bucket_ts)
);

CREATE INDEX IF NOT EXISTS idx_line_flow_ts ON line_flow (bucket_ts);

-- 권한: 이 마이그레이션은 postgres 로 실행되므로 admin 기본권한이 적용되지
-- 않는다 — 명시적으로 부여 (reader 조회 포함; 자격증명류 아님).
GRANT INSERT, UPDATE, SELECT ON faces, line_flow TO guardx_writer;
GRANT USAGE, SELECT ON SEQUENCE faces_face_id_seq TO guardx_writer;
GRANT SELECT ON faces, line_flow TO guardx_reader;
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'juan') THEN
        GRANT INSERT, UPDATE, SELECT ON faces, line_flow TO juan;
        GRANT USAGE, SELECT ON SEQUENCE faces_face_id_seq TO juan;
    END IF;
END $$;

-- [C] 유지보수 확장 — 시그니처가 바뀌므로 구판 제거 후 재생성
--     (cron 의 `SELECT guardx_maintain();` 호출은 그대로 동작)
--     신판 시그니처도 함께 DROP — 재실행 멱등성 (2026-07-27 2차 실행에서
--     "already exists" ROLLBACK 실측 후 보강)
DROP FUNCTION IF EXISTS guardx_maintain(int, int, int);
DROP FUNCTION IF EXISTS guardx_maintain(int, int, int, int, int, int);

CREATE FUNCTION guardx_maintain(
    p_detections_days  INT DEFAULT 14,    -- detections  : 파티션 drop
    p_prediction_days  INT DEFAULT 180,   -- congestion_prediction : 배치 DELETE
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

COMMIT;

-- 검증:
--   \d faces        \d line_flow
--   SELECT guardx_maintain();   → 7개 항목 요약 문자열이면 정상
