BEGIN;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version    TEXT PRIMARY KEY,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    note       TEXT
);

COMMENT ON TABLE schema_migrations IS
  '스키마 변경 적용 이력. 새 마이그레이션은 파일 끝에서 여기 INSERT 할 것.';

INSERT INTO schema_migrations (version, applied_at, note) VALUES
  ('20260701_90_camera_credentials',      '2026-07-01', 'backfill — archive/, schema.sql에 통합됨'),
  ('20260701_91_endpoint_contract_tier1', '2026-07-01', 'backfill — archive/, schema.sql에 통합됨'),
  ('20260701_92_endpoint_contract_tier2', '2026-07-01', 'backfill — archive/, schema.sql에 통합됨'),
  ('20260727_00_init',                    '2026-07-27', 'backfill — init.sql (롤/권한)'),
  ('20260727_01_schema',                  '2026-07-27', 'backfill — schema.sql 본체 v2'),
  ('20260727_02_zones_multich',           '2026-07-27', 'backfill'),
  ('20260727_03_zones_unique',            '2026-07-27', 'backfill'),
  ('20260727_04_v15_feeds',               '2026-07-27', 'backfill — faces/line_flow'),
  ('20260728_01_fire_schema',             '2026-07-28', 'backfill — fire_schema.sql 화재 10종'),
  ('20260728_02_prediction_feedback4',    '2026-07-28', 'backfill'),
  ('20260803_01_track_handover_fields',   '2026-08-03', 'backfill — 컬럼 23개, schema.sql 미반영분'),
  ('20260804_01_season_threshold',        '2026-08-04', 'backfill'),
  ('20260810_01_trajectory_segments',     '2026-08-10', 'backfill — 테이블 + 뷰 3'),
  ('20260811_01_endpoints',               '2026-08-11', 'backfill'),
  ('20260811_02_vms_auth',                '2026-08-11', 'backfill — vms_user/vms_session'),
  ('20260811_03_vms_pw_change',           '2026-08-11', 'backfill'),
  ('20260811_04_trajectory_grants',       '2026-08-11', 'backfill'),
  ('20260811_05_fire_retention',          '2026-08-11', 'backfill'),
  ('20260812_01_site_config',             '2026-08-12', 'backfill')
ON CONFLICT (version) DO NOTHING;

COMMIT;
