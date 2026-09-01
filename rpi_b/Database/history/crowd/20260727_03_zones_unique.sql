-- ============================================================
-- GuardX — zones (camera_id, channel) UNIQUE 제약 (정규화 점검 후속)
-- 실행:  sudo -u postgres psql -d guardx -f Database/migration_zones_unique.sql
--
-- 근거: 폴러 lookupZoneId 가 "채널당 존 1개"를 가정하고 LIMIT 1 로 집는데,
-- 제약이 없으면 중복 존이 조용히 임의 선택된다 (무결성 구멍, 2026-07-27 점검).
-- 추후 채널 내 존 세분화(입구 존 등)를 도입할 때는 이 제약을 풀고
-- "대표 존" 개념과 함께 재설계할 것 — 그 전까지는 안전장치.
-- 멱등. 폴러 무중단.
-- ============================================================

CREATE UNIQUE INDEX IF NOT EXISTS uq_zones_camera_channel
    ON zones (camera_id, channel);

-- 검증: \d zones → uq_zones_camera_channel UNIQUE 인덱스 확인
