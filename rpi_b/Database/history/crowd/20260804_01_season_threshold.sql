-- ============================================================
-- GuardX — 계절 임계 프리셋 (season_threshold)   2026-08-04
--
-- 실행:  sudo -u postgres psql -v ON_ERROR_STOP=1 -d guardx \
--            < ~/7th_VEDA_GROUP2/rpi_b/Database/migration_season_threshold.sql
--
-- ⚠ `-f 파일경로` 가 아니라 `< 파일경로` 다. sudo -u postgres 는 psql 을
--   postgres OS 계정으로 돌리는데, 이 파일은 개발자 홈(/home/xxx, 보통 700)에
--   있어서 그 계정이 못 읽는다 (`Permission denied`). 리다이렉션은 sudo 이전에
--   호출자 셸이 처리하므로 그 문제가 없다.
--   guardx_admin 비밀번호를 아는 경우엔 아래도 같다 (파일을 본인이 읽으므로 -f 가능):
--     psql -v ON_ERROR_STOP=1 -h localhost -U guardx_admin -d guardx -f <경로>
--
-- 추가 전용 · 멱등 · 무중단.
--   기존 30개 테이블에 ALTER 없음. FK 없음. 폴러·mqttd 재시작 불필요.
--   롤백은 DROP TABLE season_threshold; 한 줄이고 다른 곳에 영향이 없다.
--
-- ⚠ ON_ERROR_STOP=1 을 반드시 붙일 것. psql 은 기본값이 "에러 나도 계속"이라
--   GRANT 가 실패해도 exit code 0 을 준다 (fire_schema.sql 머리말 §10 참조).
--
-- ── 왜 fire_threshold 에 행을 더하지 않고 별도 테이블인가 ──
--   fire_threshold  = 이력 (INSERT only, is_active 로 현재 행 지정, UPDATE 안 함)
--   season_threshold = 카탈로그 (고정 5행, UPDATE 대상, 활성 개념 없음)
--   섞으면 ① 감사 조회에 "적용된 적 없는 행"이 끼어들고 ② append-only 규칙이
--   깨지며 ③ 모든 이력 쿼리에 프리셋 제외 필터를 달아야 한다.
--
-- ── 이 테이블은 앱에서 읽기 전용이다 ──
--   값 수정은 guardx_admin 이 psql 로만. VMS 는 SELECT 만 하므로 운영 중
--   실수로 프리셋이 덮일 경로가 없다.
-- ============================================================

BEGIN;

-- 소유자를 guardx_admin 으로 고정한다. fire_schema.sql 머리말이 정한 규칙이고
-- (실행 계정 : guardx_admin — 테이블 소유자), 나머지 테이블이 전부 그 소유다.
--
-- postgres 로 실행하면 이 테이블만 소유자가 postgres 가 되어, 나중에
-- guardx_admin 이 ALTER/DROP 하려 할 때 막히고 DEFAULT PRIVILEGES 도 안 걸린다.
-- SET ROLE 은 postgres(슈퍼유저)에게 비밀번호를 묻지 않으므로, 이 한 줄로
-- "peer 인증으로 실행하되 소유자는 올바르게"가 동시에 된다.
-- 이미 guardx_admin 으로 접속했다면 무해한 no-op 다.
SET ROLE guardx_admin;

CREATE TABLE IF NOT EXISTS season_threshold (
    season_key   TEXT PRIMARY KEY
                 CHECK (season_key IN ('default','spring','summer','autumn','winter')),
    season_name  TEXT     NOT NULL,       -- VMS 버튼에 찍히는 글자
    sort_order   SMALLINT NOT NULL,       -- 버튼 표시 순서

    -- ── fire_threshold 와 동일한 값 컬럼 22개 ──
    gas_raw_min           REAL NOT NULL,
    gas_raw_max           REAL NOT NULL,
    spark_raw_safe        REAL NOT NULL,
    spark_raw_danger      REAL NOT NULL,
    temp_min_c            REAL NOT NULL,
    temp_max_c            REAL NOT NULL,
    humi_safe_percent     REAL NOT NULL,
    humi_danger_percent   REAL NOT NULL,
    irtemp_min_c          REAL NOT NULL,
    irtemp_max_c          REAL NOT NULL,

    weight_gas            REAL NOT NULL,
    weight_spark          REAL NOT NULL,
    weight_temp           REAL NOT NULL,
    weight_humi           REAL NOT NULL,
    weight_irtemp         REAL NOT NULL,

    fire_score_threshold  REAL NOT NULL,
    n_confirm             INT  NOT NULL,
    n_recover             INT  NOT NULL,
    freeze_relax_cycles   INT  NOT NULL,
    min_valid_weight      REAL NOT NULL,
    override_spark_score  REAL NOT NULL,
    override_irtemp_score REAL NOT NULL,

    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_by   TEXT,

    -- ── CHECK 은 fire_threshold 와 반드시 동일하게 ──
    -- 없으면 잘못된 프리셋을 불러와 [화재 임계 적용]했을 때 DB 가 거부하는데,
    -- 운영자는 자기가 만진 것도 없이 왜 거부됐는지 알 수 없다.
    CONSTRAINT chk_season_gas_range    CHECK (gas_raw_min < gas_raw_max),
    CONSTRAINT chk_season_spark_range  CHECK (spark_raw_safe > spark_raw_danger),
    CONSTRAINT chk_season_temp_range   CHECK (temp_min_c < temp_max_c),
    CONSTRAINT chk_season_irtemp_range CHECK (irtemp_min_c < irtemp_max_c),
    CONSTRAINT chk_season_humi_range   CHECK (humi_safe_percent > humi_danger_percent),
    CONSTRAINT chk_season_weight_sum   CHECK (
        ABS((weight_gas + weight_spark + weight_temp
             + weight_humi + weight_irtemp) - 1.0) < 0.001
    ),
    CONSTRAINT chk_season_score_range  CHECK (fire_score_threshold BETWEEN 0 AND 100),
    CONSTRAINT chk_season_override_range CHECK (
        override_spark_score BETWEEN 0 AND 100
        AND override_irtemp_score BETWEEN 0 AND 100
    ),
    CONSTRAINT chk_season_min_valid_weight
        CHECK (min_valid_weight > 0 AND min_valid_weight <= 1),
    CONSTRAINT chk_season_cycles
        CHECK (n_confirm > 0 AND n_recover > 0 AND freeze_relax_cycles > 0)
);

COMMENT ON TABLE season_threshold IS
    '화재 임계 계절 프리셋 카탈로그 (읽기 전용). VMS SETTINGS 가 폼에 채우는 원본. '
    '실제 판정에 쓰이는 값은 fire_threshold 의 is_active 행이다.';

-- ----------------------------------------------------------------------------
-- 시드 5행 (멱등: 이미 있으면 건드리지 않음)
--
-- ⚠ 2026-08-04 현재 5행 전부 fire_threshold 시드(threshold_id 1)와 같은 값이다.
--   계절별 실측이 아직 없다 — 프로젝트가 7월 시작이라 여름 데이터뿐이다.
--   버튼과 배선을 먼저 검증하려고 값은 동일하게 두고 넣는다.
--
-- 값이 정해지면 UPDATE 로 채운다 (온습도·표면온도 6개 컬럼만 계절차를 둔다.
-- 가중치·사이클·가스/불꽃 ADC 는 계절이 아니라 센서 신뢰도·대응 정책의 문제):
--
--   UPDATE season_threshold
--      SET humi_safe_percent = 32, humi_danger_percent = 8,
--          temp_min_c = 32, temp_max_c = 58,
--          irtemp_min_c = 38, irtemp_max_c = 78,
--          updated_at = now(), updated_by = '실측 2026-12-xx'
--    WHERE season_key = 'winter';
--
-- 'default' 행은 VMS 버튼에 안 쓴다([기본값 불러오기]가 fire_threshold 시드를
-- 직접 쓰기 때문). 계절값을 정할 때 "계절 무관 기준선"으로 비교하려고 둔다.
-- ----------------------------------------------------------------------------
INSERT INTO season_threshold (
    season_key, season_name, sort_order,
    gas_raw_min, gas_raw_max, spark_raw_safe, spark_raw_danger,
    temp_min_c, temp_max_c, humi_safe_percent, humi_danger_percent,
    irtemp_min_c, irtemp_max_c,
    weight_gas, weight_spark, weight_temp, weight_humi, weight_irtemp,
    fire_score_threshold, n_confirm, n_recover, freeze_relax_cycles,
    min_valid_weight, override_spark_score, override_irtemp_score,
    updated_by
) VALUES
 ('default','기본',0, 650,800, 850,30, 35,60, 50,15, 40,80,
   0.20,0.35,0.15,0.05,0.25, 65,3,10,60, 0.50,70,70,
   'seed 2026-08-04 — fire_threshold id 1 과 동일 (기준선)'),

 ('spring','봄',1,   650,800, 850,30, 35,60, 50,15, 40,80,
   0.20,0.35,0.15,0.05,0.25, 65,3,10,60, 0.50,70,70,
   'seed 2026-08-04 — 계절값 미정, 현재 기본값과 동일'),

 ('summer','여름',2, 650,800, 850,30, 35,60, 50,15, 40,80,
   0.20,0.35,0.15,0.05,0.25, 65,3,10,60, 0.50,70,70,
   'seed 2026-08-04 — 계절값 미정, 현재 기본값과 동일'),

 ('autumn','가을',3, 650,800, 850,30, 35,60, 50,15, 40,80,
   0.20,0.35,0.15,0.05,0.25, 65,3,10,60, 0.50,70,70,
   'seed 2026-08-04 — 계절값 미정, 현재 기본값과 동일'),

 ('winter','겨울',4, 650,800, 850,30, 35,60, 50,15, 40,80,
   0.20,0.35,0.15,0.05,0.25, 65,3,10,60, 0.50,70,70,
   'seed 2026-08-04 — 계절값 미정, 현재 기본값과 동일')
ON CONFLICT (season_key) DO NOTHING;

-- ============================================================
-- 권한 — §6 권한 함정(실사고 2026-08-03) 재발 방지
--   ALL TABLES 는 실행 시점 테이블에만 적용되므로 신규 테이블은 명시해야 한다.
--   season_key 가 TEXT PK 라 SERIAL 이 없다 → 시퀀스 권한은 불필요하다
--   (그 사고의 후반부가 시퀀스 누락이었다).
--
--   앱은 SELECT 만. 프리셋 수정은 guardx_admin 이 psql 로만 한다.
-- ============================================================
GRANT SELECT ON season_threshold TO guardx_reader, guardx_writer;

-- juan 계정이 있으면 같이 (migration_v15_feeds.sql 과 동일 패턴).
-- 없는 장비에서 이 파일이 죽지 않게 존재 확인 후 부여한다.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'juan') THEN
        GRANT SELECT ON season_threshold TO juan;
    END IF;
END $$;

RESET ROLE;

COMMIT;

-- ── 확인 ──
--   \d season_threshold
--   -- 소유자가 guardx_admin 이어야 한다
--   SELECT tableowner FROM pg_tables WHERE tablename = 'season_threshold';
--   SELECT season_key, season_name, sort_order, temp_min_c,
--          humi_safe_percent, humi_danger_percent, updated_by
--     FROM season_threshold ORDER BY sort_order;
--   -- 권한 확인 (guardx_writer 가 읽을 수 있어야 한다)
--   SELECT has_table_privilege('guardx_writer', 'season_threshold', 'SELECT');
