-- ============================================================
-- GuardX 엔드포인트 계약 정합 — Tier 2
--   [A] zone_thresholds     : 운영 설정 분리 (capacity + 혼잡 위험도 단계)
--   [B] zone_geometry_history: 형상 버저닝 (자식 이력 테이블, FK 안전)
-- 대상 DB: guardx  /  실행 권한: guardx_admin 또는 postgres
-- 실행:  sudo -u postgres psql -d guardx -f migration_endpoint_contract_tier2.sql
--
-- 설계 결정 (형상 버저닝):
--   zones.zone_id 는 (camera_id,channel)당 영구 고정 → 기존 FK 절대 안 깨짐.
--   형상 변경 = zone_geometry_history 구 행 valid_to 마감 + 신 행 추가 (이력 보존).
--   zones.roi_polygon 은 "현재 형상 캐시"로 유지 → ST_Contains 핫패스에 이력 조인 불필요.
--   관심사 3분할: zones(정체성) / zone_geometry_history(형상이력) / zone_thresholds(운영설정).
--
-- 선행: Tier 1 (congestion_prediction.config_version) 먼저 적용 권장.
-- 멱등: 재실행 안전(IF NOT EXISTS / WHERE NOT EXISTS / ON CONFLICT).
-- ============================================================

BEGIN;

-- ── [B-1] zones 현재 형상 epoch 태그 ──────────────────────
-- 현재 유효 형상의 버전. 형상 변경 시 폴러가 version++ 하고 여기에 기록.
ALTER TABLE zones
    ADD COLUMN IF NOT EXISTS config_version integer NOT NULL DEFAULT 1;

COMMENT ON COLUMN zones.config_version IS
    '현재 유효한 roi_polygon 의 형상 epoch. detections/prediction 태그와 대조되는 기준.';

-- ── [B-2] 형상 버전 이력 테이블 ───────────────────────────
CREATE TABLE IF NOT EXISTS zone_geometry_history (
    geom_id        bigserial PRIMARY KEY,
    zone_id        integer          NOT NULL REFERENCES zones(zone_id),
    roi_polygon    geometry(Polygon) NOT NULL,
    config_version integer          NOT NULL,
    valid_from     timestamptz      NOT NULL DEFAULT now(),
    valid_to       timestamptz,                       -- NULL = 현재 유효 행
    created_at     timestamptz      NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_zgh_zone       ON zone_geometry_history(zone_id);
CREATE INDEX IF NOT EXISTS idx_zgh_zone_valid ON zone_geometry_history(zone_id, valid_from, valid_to);
CREATE INDEX IF NOT EXISTS idx_zgh_geom       ON zone_geometry_history USING gist(roi_polygon);
-- 존당 '열린'(현재) 형상 행은 최대 1개 — 마감 안 한 채 새 행 추가하는 실수 방지
CREATE UNIQUE INDEX IF NOT EXISTS uq_zgh_open ON zone_geometry_history(zone_id) WHERE valid_to IS NULL;

-- 기존 zones 4행의 현재 형상을 이력 첫 버전(config_version=1)으로 시드
INSERT INTO zone_geometry_history (zone_id, roi_polygon, config_version, valid_from)
SELECT z.zone_id, z.roi_polygon, 1, now()
FROM zones z
WHERE NOT EXISTS (
    SELECT 1 FROM zone_geometry_history h
    WHERE h.zone_id = z.zone_id AND h.valid_to IS NULL
);

-- ── [A] 운영 설정 테이블 (capacity + 위험도 단계) ─────────
-- 혼잡비 = predicted_count / capacity_limit.
-- warn_ratio(주의) / critical_ratio(위험): 비율(0~1)이라 capacity와 무관하게 기본값 안전.
CREATE TABLE IF NOT EXISTS zone_thresholds (
    zone_id        integer PRIMARY KEY REFERENCES zones(zone_id),
    capacity_limit integer,                       -- 사이트별 실제 수용인원 (아래 UPDATE로 채움)
    warn_ratio     real NOT NULL DEFAULT 0.75,    -- 주의 단계 임계
    critical_ratio real NOT NULL DEFAULT 0.90,    -- 위험 단계 임계
    updated_at     timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT chk_ratio_order CHECK (warn_ratio < critical_ratio),
    CONSTRAINT chk_ratio_range CHECK (warn_ratio > 0 AND critical_ratio <= 1)
);

-- 4개 존 행 시드 (위험도 단계는 기본값, capacity는 아직 미정 → NULL)
INSERT INTO zone_thresholds (zone_id)
SELECT zone_id FROM zones
ON CONFLICT (zone_id) DO NOTHING;

-- zones.capacity_limit 제거 → 단일 진실원천(zone_thresholds)으로 이관.
-- 현재 4행 전부 NULL이라 데이터 손실 없음.
ALTER TABLE zones DROP COLUMN IF EXISTS capacity_limit;

COMMIT;

-- ── 검증 ─────────────────────────────────────────────────
-- \d zones                    → config_version 有, capacity_limit 無
-- \d zone_geometry_history    → 테이블·인덱스 확인
-- \d zone_thresholds          → 테이블·CHECK 확인
-- SELECT z.zone_name, z.config_version, t.capacity_limit, t.warn_ratio, t.critical_ratio,
--        h.valid_from, h.valid_to
--   FROM zones z
--   JOIN zone_thresholds t USING (zone_id)
--   JOIN zone_geometry_history h ON h.zone_id=z.zone_id AND h.valid_to IS NULL
--   ORDER BY z.channel;
--   → 4행, valid_to 전부 NULL(현재), config_version=1 이면 정상.

-- ============================================================
-- 남은 입력 1개 — capacity 값 (지어낼 수 없음)
-- ============================================================
-- 각 존 실제 수용인원을 알면 (예시 숫자 반드시 교체):
--   UPDATE zone_thresholds SET capacity_limit=20, updated_at=now()
--     WHERE zone_id=(SELECT zone_id FROM zones WHERE camera_id=1 AND channel=0);  -- zone_A
--   ... (channel 1/2/3 = zone_B/C/D)
-- capacity가 NULL인 동안 혼잡비는 계산 불가 → 소비자는 'unknown'으로 처리.
-- 대안: 카메라 /prediction 의 capacity 를 config sync 때 폴러가 동기화(카메라=capacity 마스터).

-- ── 폴러 트랙 후속 (DB 아님, 참고) ───────────────────────
-- 형상 변경 감지 시(해시 diff) 폴러가 트랜잭션으로:
--   1) UPDATE zone_geometry_history SET valid_to=now()
--        WHERE zone_id=? AND valid_to IS NULL;
--   2) INSERT INTO zone_geometry_history(zone_id, roi_polygon, config_version, valid_from)
--        VALUES(?, <new_poly>, <N>, now());
--   3) UPDATE zones SET roi_polygon=<new_poly>, config_version=<N> WHERE zone_id=?;
-- 예측/검출은 zones.config_version 을 태그로 실어 저장(→ epoch 정합 MAE).

-- ── 롤백 (Tier 2) ────────────────────────────────────────
-- BEGIN;
--   ALTER TABLE zones ADD COLUMN IF NOT EXISTS capacity_limit integer;  -- 원복(값은 전부 NULL이었음)
--   DROP TABLE IF EXISTS zone_thresholds;
--   DROP TABLE IF EXISTS zone_geometry_history;
--   ALTER TABLE zones DROP COLUMN IF EXISTS config_version;
-- COMMIT;