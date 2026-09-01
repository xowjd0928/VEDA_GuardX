-- ============================================================
-- GuardX — fire_schema.sql 보존 정책 (guardx_fire_maintain)   2026-08-11
--
-- 실행:  sudo -u postgres psql -v ON_ERROR_STOP=1 -d guardx \
--            < ~/7th_VEDA_GROUP2/rpi_b/Database/migration_fire_retention.sql
--
-- ⚠ `-f 파일경로` 가 아니라 `< 파일경로` 다. sudo -u postgres 는 psql 을
--   postgres OS 계정으로 돌리는데, 이 파일은 개발자 홈(/home/xxx, 보통 700)에
--   있어서 그 계정이 못 읽는다 (`Permission denied`). 리다이렉션은 sudo 이전에
--   호출자 셸이 처리하므로 그 문제가 없다.
--
-- 추가 전용 · 멱등(CREATE OR REPLACE) · 무중단. 테이블 변경 없음, FK 없음,
-- rpib_ingest/decision/dispatch/guardx_mqttd 재시작 불필요.
-- 롤백은 DROP FUNCTION guardx_fire_maintain(int); 한 줄.
--
-- ⚠ ON_ERROR_STOP=1 을 반드시 붙일 것 (fire_schema.sql 머리말과 동일 이유).
--
-- ── 보존 정책 설계 (2026-08-10 결정, TODO 논의 참조) ──
--   sensor_reading + sensor_value : 30일만 보존. 1Hz(zone당 초당 1행)라 실측
--     하루 ~73MB(인덱스 제외) — RPi B 디스크(91G 여유)엔 문제없지만, 이건
--     "blackbox"(최근 운영 이력)로 쓰려는 목적에 맞춘 기간이다.
--     sensor_value는 sensor_reading(reading_id)에 ON DELETE CASCADE가 이미
--     걸려있어(fire_schema.sql:243) sensor_reading만 지우면 자동 정리된다 —
--     별도 DELETE문 불필요.
--   fire_event / fire_event_command / button_event / manual_command / fire_threshold :
--     **삭제 안 함.** 전부 저볼륨 감사·이력 기록(하루 몇 건 수준)이라 지울
--     이유가 약하고, fire_threshold는 애초에 "그 시점에 어떤 임계값이었는지"
--     역추적하는 이력 테이블이라 지우면 존재 목적이 깨진다. 그래서 이 5개
--     테이블용 파라미터는 아예 만들지 않는다 — "깜빡하고 안 지웠다"가 아니라
--     "설계상 이 함수가 손대지 않는다"를 시그니처로 명확히 한다.
-- ============================================================

BEGIN;

-- 소유자를 guardx_admin으로 고정 (migration_season_threshold.sql과 동일 이유 —
-- postgres로 실행하면 소유자가 postgres가 되어 나중에 guardx_admin이 이 함수를
-- ALTER/DROP하려 할 때 막힌다). 이미 guardx_admin으로 접속했다면 무해한 no-op.
SET ROLE guardx_admin;

-- 재실행 대비 (CREATE OR REPLACE로 충분하지만, 시그니처가 바뀌는 경우를 대비해
-- 명시적으로 지우고 새로 만든다 — guardx_maintain()도 schema.sql 머리말에서
-- 같은 이유로 DROP FUNCTION IF EXISTS를 여러 시그니처로 나열해둔다).
DROP FUNCTION IF EXISTS guardx_fire_maintain(int);

-- p_sensor_reading_days : sensor_reading(+cascade로 sensor_value) 보존 일수.
--   기본 30 — cron에서 인자 없이 호출하면 이 기본값이 적용된다.
CREATE FUNCTION guardx_fire_maintain(p_sensor_reading_days INT DEFAULT 30)
RETURNS TEXT LANGUAGE plpgsql AS $$
DECLARE
    n_reading bigint;
BEGIN
    DELETE FROM sensor_reading
        WHERE received_at < now() - make_interval(days => p_sensor_reading_days);
    GET DIAGNOSTICS n_reading = ROW_COUNT;

    RETURN format('sensor_reading -%s (sensor_value는 CASCADE로 함께 정리됨)',
                  n_reading);
END $$;

COMMENT ON FUNCTION guardx_fire_maintain(int) IS
    'fire_schema.sql 보존 정책. sensor_reading/sensor_value만 대상(30일 기본) — '
    'fire_event류 5개 감사·이력 테이블은 의도적으로 삭제 대상에서 제외 '
    '(2026-08-10 결정, 이 함수 헤더 주석 참조).';

RESET ROLE;

COMMIT;

-- ── 확인 ──
--   \df guardx_fire_maintain
--   SELECT guardx_fire_maintain();              -- 기본 30일로 즉시 1회 실행
--   SELECT guardx_fire_maintain(60);             -- 필요 시 다른 기간으로 테스트

-- ── cron 등록 (schema.sql 머리말이 안내하는 guardx_maintain()과 같은 crontab에) ──
--   sudo crontab -e 로 기존 항목을 찾아서 같은 줄에 이어붙이거나 새 줄 추가:
--     5 0 * * * sudo -u postgres psql -d guardx -c "SELECT guardx_maintain(); SELECT guardx_fire_maintain();"
--   (기존에 guardx_maintain()만 있던 줄을 이렇게 확장 — 매일 00:05 한 번에 카메라/화재
--   보존 정책 둘 다 실행됨)
