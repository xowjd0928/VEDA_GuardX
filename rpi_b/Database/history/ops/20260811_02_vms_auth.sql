-- ============================================================
-- GuardX — VMS 계정·세션 (vms_user / vms_session)   2026-08-11
--
-- 실행:  sudo -u postgres psql -v ON_ERROR_STOP=1 -d guardx \
--            < ~/7th_VEDA_GROUP2/rpi_b/Database/migration_vms_auth.sql
--
-- ⚠ `-f 파일경로` 가 아니라 `< 파일경로` 다 (sudo -u postgres 는 개발자 홈을
--   못 읽는다 — migration_season_threshold.sql 머리말 참조).
-- ⚠ ON_ERROR_STOP=1 필수. psql 기본값은 "에러 나도 계속"이라 GRANT/REVOKE 가
--   실패해도 exit code 0 을 준다 (fire_schema.sql 머리말 §10).
--
-- 추가 전용 · 멱등 · 무중단. 기존 테이블에 ALTER 없음.
--
-- ── ⚠ 이 파일에서 가장 중요한 줄은 CREATE TABLE 이 아니라 REVOKE 다 ──
--   운영 DB 의 `pg_default_acl` 은 두 줄이다 (2026-08-11 실측):
--     guardx_admin | TABLE | {guardx_writer=arw, guardx_reader=r}
--     postgres     | TABLE | {guardx_writer=arw, guardx_reader=r, juan=arwdDxtm}
--   즉 **테이블을 만드는 순간 guardx_reader 가 SELECT 를 자동으로 얻는다.**
--   postgres 로 만들면 `juan` 이 전권까지 함께 얻는다. 비밀번호 해시와 세션
--   토큰 해시가 읽기 롤·전권 롤에 그대로 열린다는 뜻이다.
--   `camera_credentials` 선례(schema.sql §6)와 같은 방식으로 명시 회수한다.
-- ============================================================

BEGIN;

-- 소유자를 guardx_admin 으로 고정 (migration_season_threshold.sql §34-41).
-- postgres 로 만들면 이 테이블만 소유자가 postgres 가 되어 나중에
-- guardx_admin 이 ALTER/DROP 하려 할 때 막힌다.
SET ROLE guardx_admin;

-- ── 계정 ──────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS vms_user (
    user_id       SERIAL PRIMARY KEY,
    username      TEXT UNIQUE NOT NULL,
    display_name  TEXT NOT NULL,

    -- 알고리즘·반복횟수를 행에 함께 적는다. 나중에 반복횟수를 올리거나
    -- 알고리즘을 바꿀 때 기존 행을 그대로 두고 새 행부터 적용할 수 있다 —
    -- 전부 한 번에 재해시할 방법이 없기 때문에(원문 비밀번호가 없다) 이
    -- 컬럼이 없으면 알고리즘을 영영 못 바꾼다.
    pw_algo       TEXT NOT NULL DEFAULT 'pbkdf2-sha256',
    pw_iters      INT  NOT NULL DEFAULT 200000 CHECK (pw_iters >= 1000),
    pw_salt       BYTEA NOT NULL CHECK (length(pw_salt) >= 16),
    pw_hash       BYTEA NOT NULL CHECK (length(pw_hash)  =  32),

    role          TEXT NOT NULL CHECK (role IN ('admin','operator')),
    enabled       BOOL NOT NULL DEFAULT TRUE,

    -- 실패 잠금 (§5): 5회 → 60초, 10회 → 10분
    failed_count  INT  NOT NULL DEFAULT 0,
    locked_until  TIMESTAMPTZ,

    last_login_at TIMESTAMPTZ,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON TABLE vms_user IS
    'VMS 로그인 계정. 비밀번호는 PBKDF2-HMAC-SHA256 해시로만 보관(원문 없음).';
COMMENT ON COLUMN vms_user.pw_iters IS
    '해시 생성 당시의 반복 횟수. 행마다 달라도 되므로 나중에 상향 가능.';

-- ── 세션 ──────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS vms_session (
    -- 토큰 **원문은 저장하지 않는다.** DB 가 통째로 새도 남의 세션을 탈취할 수
    -- 없어야 하기 때문이다. 조회는 SHA-256(token) 으로 한다.
    token_hash  BYTEA PRIMARY KEY CHECK (length(token_hash) = 32),
    user_id     INT NOT NULL REFERENCES vms_user(user_id) ON DELETE CASCADE,
    issued_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at  TIMESTAMPTZ NOT NULL,       -- 발급 + 30일
    device      TEXT                        -- "vms vms-3-11" (updated_by 형식)
);

-- 만료 세션 청소용. PK 는 token_hash 라 expires_at 범위 삭제가 전 행 스캔이 된다.
CREATE INDEX IF NOT EXISTS idx_vms_session_expires ON vms_session (expires_at);
-- "이 사용자의 세션 전부" (비밀번호 변경·계정 비활성 시 일괄 무효화)
CREATE INDEX IF NOT EXISTS idx_vms_session_user    ON vms_session (user_id);

-- ============================================================
-- 권한 — 이 파일의 핵심
-- ============================================================

-- ── 쓰기 롤: 필요한 것만. INSERT/DELETE 를 주지 않는다 ──
--
-- guardx_mqttd 가 하는 일은 ①계정 조회 ②실패 카운트·잠금·마지막 로그인 갱신
-- ③세션 발급·조회·삭제뿐이다. **계정 생성은 하지 않는다** — 계정은
-- guardx_admin 이 psql 로 만든다(season_threshold 와 같은 규약).
--
-- 그래서 vms_user 에는 INSERT 를 주지 않는다. 이 한 줄이 "mqttd 가 뚫려도
-- 공격자가 admin 계정을 새로 만들 수는 없다"를 만든다. 기본권한이 준 arw 중
-- INSERT 를 도로 걷어내는 형태가 된다.
REVOKE ALL ON vms_user    FROM guardx_writer;
GRANT  SELECT, UPDATE ON vms_user TO guardx_writer;

-- 세션은 DELETE 가 필요하다 — logout 과 만료 청소. 기본권한(arw)에는 DELETE 가
-- 없으므로 명시해야 한다.
REVOKE ALL ON vms_session FROM guardx_writer;
GRANT  SELECT, INSERT, UPDATE, DELETE ON vms_session TO guardx_writer;

-- ⚠⚠ ── 읽기 롤·사람 계정에서 회수 (이 파일에서 가장 중요한 두 줄) ──
-- 기본권한 때문에 자동으로 붙은 SELECT 를 걷어낸다. 이게 없으면 조회 전용
-- 커넥션(guardx_mqttd 의 roDb)이 비밀번호 해시를 읽을 수 있다.
REVOKE ALL ON vms_user, vms_session FROM guardx_reader;

-- juan 은 postgres 기본권한 줄에 arwdDxtm 로 들어 있다. 이 파일을
-- guardx_admin 으로 실행하면 애초에 안 붙지만, postgres 로 실행됐을 때를
-- 대비해 명시 회수한다(멱등·무해).
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'juan') THEN
        REVOKE ALL ON vms_user, vms_session FROM juan;
    END IF;
END $$;

-- 시퀀스(vms_user_user_id_seq)는 일부러 아무에게도 주지 않는다 —
-- INSERT 를 못 하니 필요가 없고, 주면 위 REVOKE 의 의미가 흐려진다.
REVOKE ALL ON SEQUENCE vms_user_user_id_seq FROM guardx_writer, guardx_reader;

-- ============================================================
-- 시드 관리자 1명
--
-- ⚠⚠ **이 비밀번호는 git 에 공개돼 있다.** 아래 해시는
--     PBKDF2-HMAC-SHA256('guardx-admin-CHANGEME', salt, 200000, 32) 이고
--     salt 도 이 파일에 그대로 있다. 즉 **누구나 계산해서 맞출 수 있다.**
--     시연·운영 전에 반드시 교체할 것:
--
--       ./build/guardx_passwd admin            # 새 비밀번호를 물어보고 SQL 출력
--       # 출력된 UPDATE 문을 psql 로 실행
--
--     교체 전에는 이 계정으로 admin 권한 쓰기 명령이 전부 통과한다.
-- ============================================================
INSERT INTO vms_user (username, display_name, pw_algo, pw_iters,
                      pw_salt, pw_hash, role)
VALUES ('admin', '관리자', 'pbkdf2-sha256', 200000,
        '\xa3f1c05e7b294d8613fae0927c5b41d8'::bytea,
        '\xef4a2dfb1950a4ddf2f3a236cddf3365ce4dec31b8e624cc6514c8ad0376bf35'::bytea,
        'admin')
ON CONFLICT (username) DO NOTHING;

RESET ROLE;

COMMIT;

-- ── 확인 ──
--   -- 소유자가 guardx_admin 이어야 한다
--   SELECT tablename, tableowner FROM pg_tables
--    WHERE tablename IN ('vms_user','vms_session');
--
--   -- ⭐ 넷 다 f 여야 한다 (여기가 f 가 아니면 이 마이그레이션은 실패한 것이다)
--   SELECT has_table_privilege('guardx_reader','vms_user','SELECT')    AS r_user,
--          has_table_privilege('guardx_reader','vms_session','SELECT') AS r_sess,
--          has_table_privilege('juan','vms_user','SELECT')             AS j_user,
--          has_table_privilege('guardx_writer','vms_user','INSERT')    AS w_ins;
--
--   -- 넷 다 t 여야 한다
--   SELECT has_table_privilege('guardx_writer','vms_user','SELECT')     AS w_user_sel,
--          has_table_privilege('guardx_writer','vms_user','UPDATE')     AS w_user_upd,
--          has_table_privilege('guardx_writer','vms_session','INSERT')  AS w_sess_ins,
--          has_table_privilege('guardx_writer','vms_session','DELETE')  AS w_sess_del;
--
--   -- 실제로 막히는지 (권한표보다 이쪽이 진짜 증거다)
--   SET ROLE guardx_reader;
--   SELECT * FROM vms_user;        -- ERROR: permission denied 가 나와야 정상
--   RESET ROLE;

-- ── 롤백 ──
--   BEGIN;
--     DROP TABLE IF EXISTS vms_session;   -- FK 때문에 이쪽이 먼저
--     DROP TABLE IF EXISTS vms_user;
--   COMMIT;
