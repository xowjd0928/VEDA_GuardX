-- ============================================================
-- GuardX — 노드 엔드포인트 주소 (endpoints)   2026-08-11
--
-- 실행:  sudo -u postgres psql -v ON_ERROR_STOP=1 -d guardx \
--            < ~/7th_VEDA_GROUP2/rpi_b/Database/migration_endpoints.sql
--
-- ⚠ `-f 파일경로` 가 아니라 `< 파일경로` 다 (migration_season_threshold.sql
--   머리말과 같은 이유 — sudo -u postgres 는 개발자 홈을 못 읽는다).
--
-- 추가 전용 · 멱등 · 무중단.
--   기존 테이블에 ALTER 없음. FK 없음. 롤백은 DROP TABLE endpoints; 한 줄.
--   mqttd 는 30초 틱마다 이 테이블을 읽어 값이 바뀌었을 때만 재발행하므로
--   값을 고친 뒤 서비스를 재시작할 필요가 없다.
--
-- ⚠ ON_ERROR_STOP=1 을 반드시 붙일 것. psql 기본값은 "에러 나도 계속"이라
--   GRANT 가 실패해도 exit code 0 을 준다 (fire_schema.sql 머리말 §10).
--
-- ── 무엇을 해결하나 ──
--   RPi C 의 RTP 방송 수신 주소가 컴파일 상수였다
--   (shared/broadcast_protocol.h `GUARDX_BROADCAST_RTP_HOST`).
--   IP 가 바뀌면 VMS·RPi C 를 양쪽 다 재빌드해야 한다. DB 로 옮기고
--   guardx/db/rpib/endpoints 에 retained 로 실어 보내면 재빌드가 없어진다.
--
-- ── 왜 kv 한 장인가 (노드별 컬럼 테이블이 아니라) ──
--   담을 것이 지금 한 줄이고, 앞으로 늘어도 성격이 같은 "주소 한 개"다.
--   컬럼으로 두면 항목이 늘 때마다 ALTER + 코드 + VMS 파서가 함께 움직인다.
--   kv 면 INSERT 한 줄로 끝나고, 발행 SQL(json_object_agg)도 안 바뀐다.
--
-- ── 이 테이블은 앱에서 읽기 전용이다 ──
--   값 수정은 guardx_admin 이 psql 로만 (season_threshold 와 같은 규약).
--   VMS 가 주소를 고치는 경로는 만들지 않는다 — 방송 목적지를 원격에서
--   바꿀 수 있다는 것은 그 자체로 공격 표면이다.
--
-- ⚠ 이름 유래 주의: Database/archive/migration_endpoint_contract_tier1.sql
--   ·tier2.sql 은 이 테이블과 **무관하다.** 그쪽 "엔드포인트"는 카메라
--   HTTP API(/detections·/prediction·/config) 출력과 스키마의 정합을 뜻하고,
--   내용(congestion_prediction.config_version · zone_thresholds ·
--   zone_geometry_history)은 이미 schema.sql 본문에 흡수돼 archive 로
--   내려간 것이다. 되살릴 것도, 개념이 겹치는 것도 없다 (2026-08-11 확인).
-- ============================================================

BEGIN;

-- 소유자를 guardx_admin 으로 고정한다 (migration_season_threshold.sql §34-41).
-- postgres 로 실행하면 이 테이블만 소유자가 postgres 가 되어, 나중에
-- guardx_admin 이 ALTER/DROP 하려 할 때 막히고 DEFAULT PRIVILEGES 도 안 걸린다.
-- 이미 guardx_admin 으로 접속했다면 무해한 no-op 다.
SET ROLE guardx_admin;

CREATE TABLE IF NOT EXISTS endpoints (
    -- 예약어 3개를 DB 단에서 막는다. 발행 payload 는 이 표의 key 를 최상위
    -- 필드명으로 그대로 펼치므로(json_object_agg), 봉투 필드와 같은 이름의
    -- 행이 하나 생기면 payload 가 조용히 뒤덮인다 — 코드에서 방어하면
    -- 잊어버리기 쉬운 종류라 제약으로 못 박는다.
    key        TEXT PRIMARY KEY
               CHECK (key <> '' AND key NOT IN ('node_id','timestamp','updated_at')),
    value      TEXT        NOT NULL CHECK (value <> ''),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_by TEXT,                        -- 'juan@psql' 등. 감사용
    note       TEXT                         -- 왜 이 값인지 (사이트 이전 등)
);

COMMENT ON TABLE endpoints IS
    '노드 간 네트워크 주소. 컴파일 상수를 대체한다. guardx/db/rpib/endpoints 로 retained 발행.';
COMMENT ON COLUMN endpoints.key IS
    '주소 항목 키. MQTT payload 의 필드명이 그대로 이 값이 된다.';

-- ── 시드 ──
-- 현재 컴파일 상수와 같은 값을 넣는다 (shared/broadcast_protocol.h:68).
-- 같은 값으로 출발해야 이 마이그레이션 자체가 동작을 바꾸지 않는다 —
-- 주소 이전은 값을 UPDATE 하는 별개의 순간에 일어난다.
INSERT INTO endpoints (key, value, updated_by, note) VALUES
    ('rpic_rtp_host', '172.20.33.114', 'migration_endpoints.sql',
     'shared/broadcast_protocol.h GUARDX_BROADCAST_RTP_HOST 초기값과 동일')
ON CONFLICT (key) DO NOTHING;

-- ============================================================
-- 권한 — §6 권한 함정(실사고 2026-08-03) 재발 방지
--   ALL TABLES 는 실행 시점 테이블에만 적용되므로 신규 테이블은 명시해야 한다.
--   TEXT PK 라 SERIAL 이 없다 → 시퀀스 권한 불필요 (그 사고의 후반부).
--
--   앱은 SELECT 만. 주소 수정은 guardx_admin 이 psql 로만 한다.
--   REVOKE 는 하지 않는다 — 주소는 비밀이 아니고, mqttd 의 조회 커넥션이
--   reader 로 분리되면(작업 B) reader 가 읽을 수 있어야 발행이 된다.
-- ============================================================
GRANT SELECT ON endpoints TO guardx_reader, guardx_writer;

-- juan 계정이 있으면 같이 (migration_v15_feeds.sql·season_threshold 와 동일 패턴).
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'juan') THEN
        GRANT SELECT ON endpoints TO juan;
    END IF;
END $$;

RESET ROLE;

COMMIT;

-- ── 확인 ──
--   \d endpoints
--   -- 소유자가 guardx_admin 이어야 한다
--   SELECT tableowner FROM pg_tables WHERE tablename = 'endpoints';
--   SELECT key, value, updated_at, updated_by FROM endpoints ORDER BY key;
--   -- 권한 (reader 가 읽을 수 있어야 발행이 된다)
--   SELECT has_table_privilege('guardx_reader', 'endpoints', 'SELECT');
--   -- 발행 확인 (최대 30초 = CFG_INTERVAL_S 후)
--   mosquitto_sub -h localhost -t 'guardx/db/rpib/endpoints' -v -C 1

-- ── 주소를 옮길 때 (RPi C 가 다른 IP 로 이사한 경우) ──
--   ⚠ updated_at 을 함께 갱신할 것. 이 리포엔 트리거가 하나도 없어서
--     자동으로 따라오지 않는다 — 빠뜨리면 payload 의 updated_at 이 거짓말을 한다.
--
--   UPDATE endpoints
--      SET value = '10.0.0.9', updated_at = now(),
--          updated_by = 'juan@psql', note = '2026-08-xx 사이트 이전'
--    WHERE key = 'rpic_rtp_host';
--
--   → 다음 30초 틱에 mqttd 가 재발행한다. 서비스 재시작 불필요.
