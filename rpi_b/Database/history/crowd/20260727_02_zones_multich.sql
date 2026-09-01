-- ============================================================
-- GuardX — v14 다채널 존 추가 (실측 채널 번호 기준)
-- 실행:  sudo -u postgres psql -d guardx -f Database/migration_zones_multich.sql
--
-- 실측 (2026-07-27, 카메라 v14): 채널 번호는 **0-기반** (센서 0~3,
-- MetadataManager_N = 채널 N). 운용 센서 = ch1 (기존 zone_A, zone_id=1 불변),
-- 통행 있는 두 번째 센서 = ch0. ch2·ch3은 현재 통행 없음(대비용 존).
-- ⚠ 폐기된 migration_zones_ch234.sql(1-기반 가정, 채널 2·3·4)을 대체한다.
--
-- 가동 중인 DB에 대한 **추가 전용** — 재구축 불필요, 폴러 무중단. 멱등.
-- capacity_limit: ch1=20(기존 유지). 나머지는 실공간 미확정 NULL.
-- ============================================================

BEGIN;

-- 채널 0·2·3 존 추가 (전체 화면 ROI)
INSERT INTO zones (zone_name, camera_id, channel, roi_polygon)
SELECT v.zname, 1, v.ch, ST_MakeEnvelope(0, 0, 2592, 1520, 0)
FROM (VALUES ('zone_B', 0), ('zone_C', 2), ('zone_D', 3)) AS v(zname, ch)
WHERE NOT EXISTS (
    SELECT 1 FROM zones z WHERE z.camera_id = 1 AND z.channel = v.ch
);

-- 형상 이력 첫 버전 (열린 행 없는 존만)
INSERT INTO zone_geometry_history (zone_id, roi_polygon, config_version)
SELECT z.zone_id, z.roi_polygon, z.config_version
FROM zones z
WHERE NOT EXISTS (
    SELECT 1 FROM zone_geometry_history h
    WHERE h.zone_id = z.zone_id AND h.valid_to IS NULL
);

-- 운영 임계 행 (capacity NULL, 위험도 비율 기본값)
INSERT INTO zone_thresholds (zone_id)
SELECT zone_id FROM zones
ON CONFLICT (zone_id) DO NOTHING;

COMMIT;

-- 검증:
--   SELECT z.zone_id, z.zone_name, z.channel, t.capacity_limit
--     FROM zones z JOIN zone_thresholds t USING (zone_id) ORDER BY z.channel;
--   → 4행: ch0 zone_B / ch1 zone_A(cap 20, zone_id=1) / ch2 zone_C / ch3 zone_D
--   db_smoke는 여전히 lookupZoneId(1,1)==1 (zone_A 불변).
--
-- 이후: config.env에 CHANNELS=0,1 (통행 채널만; 2·3은 추후 확장) →
--       폴러 재빌드·재시작 → [pred] ch0/ch1 두 줄씩 적재 확인.
