-- ============================================================
-- GuardX — 비밀번호 강제 변경 + 계정 생성 (작업 G, §5b)   2026-08-11
--
-- 실행:  sudo -u postgres psql -v ON_ERROR_STOP=1 -d guardx \
--            < ~/7th_VEDA_GROUP2/rpi_b/Database/migration_vms_pw_change.sql
--
-- ⚠ `-f 파일경로` 가 아니라 `< 파일경로` 다 (sudo -u postgres 는 개발자 홈을
--   못 읽는다 — migration_season_threshold.sql 머리말 참조).
-- ⚠ ON_ERROR_STOP=1 필수. psql 기본값은 "에러 나도 계속"이라 GRANT 가 실패해도
--   exit code 0 을 준다 (fire_schema.sql 머리말 §10).
--
-- 선행: migration_vms_auth.sql
-- 추가 전용 · 멱등 · 무중단. 컬럼 1개 + 권한 2줄.
--
-- ── 왜 필요한가 ──
--   시드 admin 의 해시와 salt 가 `migration_vms_auth.sql` 에 그대로 있다.
--   누구나 계산해서 맞출 수 있으므로 시연 전 교체가 필수인데, 지금은 그 방법이
--   Pi 에 SSH 로 붙어 guardx_passwd 를 돌리는 것뿐이다. 운영자가 자기 비밀번호를
--   못 바꾸는 시스템은 그 자체로 결함이다.
-- ============================================================

BEGIN;

SET ROLE guardx_admin;   -- 소유자 고정 (migration_season_threshold.sql §34-41)

-- ── 1. 강제 변경 플래그 ──
-- 기본 FALSE — 기존 계정의 동작은 안 바뀐다.
ALTER TABLE vms_user
    ADD COLUMN IF NOT EXISTS must_change_pw BOOL NOT NULL DEFAULT FALSE;

COMMENT ON COLUMN vms_user.must_change_pw IS
    'TRUE 면 로그인은 되지만 비밀번호를 바꾸기 전에는 쓰기 명령이 거부된다. '
    '시드 계정(해시가 저장소에 공개)과 관리자가 만든 새 계정이 대상.';

-- 시드 관리자는 강제 대상이다 — 해시가 저장소에 공개돼 있다.
-- ⚠ 이미 비밀번호를 바꾼 뒤에 이 파일을 처음 돌리면 그 계정이 다시 강제 대상이
--   된다. 그건 무해하다(한 번 더 바꾸면 된다). 반대로 여기서 빼면 공개
--   비밀번호가 그대로 남을 위험이 있어, 안전한 쪽으로 기운다.
UPDATE vms_user SET must_change_pw = TRUE WHERE username = 'admin';

-- ============================================================
-- 2. 권한 — ⚠ 여기서 D 의 통제 하나를 내려놓는다
--
-- migration_vms_auth.sql 은 `vms_user` 에서 writer 의 INSERT 를 일부러 뺐고,
-- 근거를 이렇게 적었다:
--     "mqttd 가 뚫려도 공격자가 admin 계정을 새로 만들 수는 없다"
--
-- `create_user`(§5b) 는 그 INSERT 를 필요로 한다. 기능이 결정된 이상 열어야
-- 하고, 그 통제는 사라진다. **우회로 때문이 아니라 기능 자체가 요구하는 것**
-- 이므로 SECURITY DEFINER 같은 포장으로 되살릴 수도 없다 — 프로세스가 뚫리면
-- 그 함수도 똑같이 호출된다.
--
-- 남는 통제는 이것들이다:
--   · 애플리케이션에서 admin 역할 토큰만 create_user 를 호출할 수 있다
--   · role 은 'admin'|'operator' 만 (테이블 CHECK 이 DB 단에서도 막는다)
--   · 새 계정은 must_change_pw = TRUE 로 만들어진다
--   · vms_user 는 여전히 reader·juan 에서 회수돼 있다 (해시 노출 없음)
--   · DELETE 는 여전히 안 준다 — 계정 삭제 경로는 만들지 않았다
-- ============================================================
GRANT INSERT ON vms_user TO guardx_writer;

-- SERIAL 이라 시퀀스 USAGE 가 함께 필요하다. migration_vms_auth.sql 에서
-- "INSERT 를 못 하니 필요 없다"며 회수했던 것을 되돌린다.
GRANT USAGE, SELECT ON SEQUENCE vms_user_user_id_seq TO guardx_writer;

RESET ROLE;

COMMIT;

-- ── 확인 ──
--   -- 컬럼과 시드 상태
--   SELECT username, role, must_change_pw FROM vms_user ORDER BY user_id;
--   --> admin 은 must_change_pw = t 여야 한다
--
--   -- ⭐ 권한 (앞 셋 t / 뒤 둘 f 여야 한다)
--   SELECT has_table_privilege('guardx_writer','vms_user','INSERT') AS w_ins,
--          has_table_privilege('guardx_writer','vms_user','UPDATE') AS w_upd,
--          has_sequence_privilege('guardx_writer','vms_user_user_id_seq','USAGE') AS w_seq,
--          has_table_privilege('guardx_writer','vms_user','DELETE') AS w_del,
--          has_table_privilege('guardx_reader','vms_user','SELECT') AS r_sel;
--
--   -- 실제로 막히는지 (권한표보다 이쪽이 진짜 증거다)
--   SET ROLE guardx_reader;
--   SELECT * FROM vms_user;        -- ERROR: permission denied 가 나와야 정상
--   RESET ROLE;

-- ── 롤백 ──
--   BEGIN;
--     REVOKE INSERT ON vms_user FROM guardx_writer;
--     REVOKE ALL ON SEQUENCE vms_user_user_id_seq FROM guardx_writer;
--     ALTER TABLE vms_user DROP COLUMN IF EXISTS must_change_pw;
--   COMMIT;
--   -- ⚠ 롤백하면 create_user 가 죽는다. VMS 의 [계정 만들기] 카드도 함께 꺼야 한다.
