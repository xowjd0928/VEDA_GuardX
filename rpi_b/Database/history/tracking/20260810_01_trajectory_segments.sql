-- ============================================================================
-- GuardX trajectory analytics migration
-- Source: CAP /analytics/trajectories
--
-- This table stores anonymous trajectory segments that passed through the
-- camera-side tracklet analytics layer. It is separated from track_path because
-- track_path is point-level VMS drawing data, while this table is segment-level
-- business analytics data.
-- ============================================================================

CREATE TABLE IF NOT EXISTS trajectory_segments (
    camera_id       INT NOT NULL,
    segment_id      BIGINT NOT NULL,

    global_id       BIGINT NOT NULL,
    display_id      BIGINT,
    object_id       INT NOT NULL,

    channel         INT NOT NULL,      -- VMS channel number, 1-based
    raw_channel     INT NOT NULL,      -- OpenSDK metadata channel, 0-based
    zone_id         INT,               -- resolved from zones(camera_id, channel)

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

CREATE INDEX IF NOT EXISTS idx_trajectory_segments_global_start
ON trajectory_segments (camera_id, global_id, start_ms);

CREATE INDEX IF NOT EXISTS idx_trajectory_segments_zone_reliable
ON trajectory_segments (zone_id, is_reliable, segment_ts);

CREATE INDEX IF NOT EXISTS idx_trajectory_segments_served
ON trajectory_segments (served_at);

CREATE INDEX IF NOT EXISTS idx_trajectory_segments_display
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
-- 권한 (2026-08-11 추가 — 신규 설치가 같은 함정에 빠지지 않도록)
--
-- 원래 이 파일에는 GRANT 절이 없었다. schema.sql §6 의 `ON ALL TABLES` 는
-- **실행 시점에 있던 테이블에만** 적용되고, `ALTER DEFAULT PRIVILEGES` 는
-- **그 문장을 실행한 롤이 만든 테이블에만** 적용된다. 그래서 이 파일을
-- guardx_admin·postgres 가 아닌 계정으로 돌리면 소유자 외에는 아무도 접근할
-- 수 없는 테이블이 조용히 만들어진다.
--
-- 실제로 그렇게 됐다 — 운영 DB 에서 이 테이블만 소유자 juan · relacl 비어
-- 있음. 폴러가 마침 juan 으로 붙어 있어서 6일간 드러나지 않았다
-- (기존 DB 의 정정은 migration_trajectory_grants.sql).
--
-- 명시 GRANT 는 누가 실행하든 같은 결과를 준다 — 소유자는 언제나 GRANT 할
-- 수 있기 때문이다. 그래서 이 방식으로 못 박는다.
-- ============================================================================

GRANT INSERT, UPDATE, SELECT ON trajectory_segments TO guardx_writer;
GRANT SELECT ON trajectory_segments TO guardx_reader;
GRANT SELECT ON reliable_trajectory_segments,
                trajectory_zone_dwell_summary,
                trajectory_zone_transition_summary
      TO guardx_reader, guardx_writer;
