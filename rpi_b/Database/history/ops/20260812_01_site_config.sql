-- ============================================================
-- GuardX — 전역 설정 site_config (SITE 문구·캘리브레이션)   2026-08-12
--
-- 실행:  sudo -u postgres psql -v ON_ERROR_STOP=1 -d guardx \
--            < ~/7th_VEDA_GROUP2/rpi_b/Database/migration_site_config.sql
--
-- ⚠ `-f 파일경로` 가 아니라 `< 파일경로` 다 (sudo -u postgres 는 개발자 홈을
--   못 읽는다 — migration_season_threshold.sql 머리말 참조).
-- ⚠ ON_ERROR_STOP=1 필수. psql 기본값은 "에러 나도 계속"이라 GRANT 가 실패해도
--   exit code 0 을 준다 (fire_schema.sql 머리말 §10).
--
-- 선행: 없음 (독립 테이블). 추가 전용 · 멱등 · 무중단.
--
-- ── 왜 필요한가 ──
--   08-12 합의 ④: 캘리브레이션과 SITE 문구는 전역이다 — 배포 시 모든 사용자가
--   같은 환경을 본다. retained 만 믿으면 브로커 재시작에 날아가므로 DB 가
--   진실원천이고, 폴러가 기동 시 DB → retained 로 재발행한다.
--   계약: vms/docs/DB_LINK_AND_MQTT_MIGRATION.md §3.3 (site_config, 08-12 신설)
--
--   endpoints 와 같은 key-value 꼴이다 — 다음 "전역 설정"이 나와도 스키마 변경
--   없이 행 하나로 끝난다. 지금 키는 2개뿐이며, 키를 늘리려면 계약 문서의
--   화이트리스트 표를 먼저 고친다:
--     site_name    jsonb 문자열   ("GuardX 시연장")
--     calibration  jsonb 객체     (VMS 소유 스키마 — 서버는 통짜 보관만)
-- ============================================================

BEGIN;

SET ROLE guardx_admin;   -- 소유자 고정 (migration_season_threshold.sql §34-41)

CREATE TABLE IF NOT EXISTS site_config (
    key         TEXT PRIMARY KEY,
    value       JSONB NOT NULL,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_by  TEXT
);

COMMENT ON TABLE site_config IS
    '전역 설정 (SITE 문구·캘리브레이션 등). 서버는 value 를 해석하지 않는다 — '
    '통짜 보관 + retained 발행만. 허용 키와 payload 상한(16KB)은 '
    'vms/docs/DB_LINK_AND_MQTT_MIGRATION.md §3.3 이 정본이다.';

-- 쓰기 롤: 기본권한(arw)이 이미 주는 조합이지만, 기본권한이 바뀌어도 이 파일만으로
-- 성립하도록 명시한다. DELETE 는 안 준다 — 계약에 "삭제 개념 없음"(빈 값
-- 덮어쓰기가 지움)으로 못 박았고, 안 주면 뚫린 mqttd 가 행을 지울 수도 없다.
GRANT SELECT, INSERT, UPDATE ON site_config TO guardx_writer;

-- 읽기 롤: 비밀이 아니다(현장 이름·화면 캘리브레이션). vms_user 처럼 회수할
-- 이유가 없어 기본권한 그대로 두되 명시한다.
GRANT SELECT ON site_config TO guardx_reader;

RESET ROLE;

COMMIT;

-- ── 확인 ──
--   -- 소유자가 guardx_admin 이어야 한다
--   SELECT tablename, tableowner FROM pg_tables WHERE tablename = 'site_config';
--
--   -- t / t / f 여야 한다 (DELETE 가 t 면 이 마이그레이션은 실패한 것이다)
--   SELECT has_table_privilege('guardx_writer','site_config','INSERT') AS w_ins,
--          has_table_privilege('guardx_writer','site_config','UPDATE') AS w_upd,
--          has_table_privilege('guardx_writer','site_config','DELETE') AS w_del;
--
--   -- 폴러 재시작 뒤 (저장된 키가 있으면 즉시 수신돼야 한다):
--   mosquitto_sub -t 'guardx/db/rpib/site_config' -v

-- ── 롤백 ──
--   BEGIN;
--     DROP TABLE IF EXISTS site_config;
--   COMMIT;
--   -- ⚠ retained 는 브로커에 남는다 — 지우려면 빈 payload 로 한 번 발행:
--   --   mosquitto_pub -t 'guardx/db/rpib/site_config' -r -n
