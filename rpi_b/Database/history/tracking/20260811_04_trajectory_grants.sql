-- ============================================================
-- GuardX — trajectory_segments 소유자·권한 정정   2026-08-11
--
-- 실행:  sudo -u postgres psql -v ON_ERROR_STOP=1 -d guardx \
--            < ~/7th_VEDA_GROUP2/rpi_b/Database/migration_trajectory_grants.sql
--
-- ⚠ **반드시 postgres(슈퍼유저)로 실행한다.** 다른 마이그레이션과 달리
--   `SET ROLE guardx_admin` 을 쓰지 않는다 — 소유자를 **바꾸는** 작업이라
--   현재 소유자(juan)도 새 소유자(guardx_admin)도 이 문장을 실행할 수 없다.
--
-- ⚠ `-f 파일경로` 가 아니라 `< 파일경로` 다 (sudo -u postgres 는 개발자 홈을
--   못 읽는다 — migration_season_threshold.sql 머리말 참조).
--
-- 멱등: 재실행 안전 (OWNER TO 와 GRANT 는 같은 결과로 수렴한다).
--
-- ── 무엇이 잘못돼 있었나 (2026-08-11 운영 DB 실측) ──
--   trajectory_segments | 소유자 juan | relacl 비어 있음
--
--   `migration_trajectory_segments.sql` 에 GRANT 절이 없고, juan 은
--   `ALTER DEFAULT PRIVILEGES` 를 건 적이 없다(기본권한은 guardx_admin·
--   postgres 두 줄뿐). 그래서 **소유자 외에는 아무도 접근할 수 없다.**
--
--     has_table_privilege('guardx_writer', 'trajectory_segments', 'INSERT') = f
--     has_table_privilege('guardx_reader', 'trajectory_segments', 'SELECT') = f
--
--   지금 동선 파이프라인이 도는 유일한 이유는 폴러가 `user=juan` 으로 붙어
--   소유자 권한을 쓰기 때문이다. 이 상태로 `PGCONN` 을 guardx_writer 로
--   바꾸면 그 순간 동선 적재(task_analytics_trajectories.cpp)와 조회
--   (query/trajectory)가 함께 멎는다. **이 파일이 그 선행 조건이다.**
--
-- ── ⚠ 뷰 3개를 함께 옮겨야 하는 이유 ──
--   뷰는 **뷰 소유자의 권한으로** 본 테이블을 읽는다. 테이블만
--   guardx_admin 으로 옮기고 뷰를 juan 소유로 두면, 이관 직후 juan 이
--   테이블 권한을 잃으면서 **세 뷰가 전부 permission denied 로 죽는다**
--   (뷰에 SELECT 를 아무리 GRANT 해도 소용없다 — 막히는 곳이 본 테이블이다).
--   테이블과 뷰의 소유자는 함께 움직여야 한다.
-- ============================================================

BEGIN;

-- ── 1. 소유자 이관 ──
-- 나머지 테이블 규약과 통일한다: fire_schema.sql 계열이 전부 guardx_admin
-- 소유이고, migration_season_threshold.sql 이 그 규칙을 명문화했다.
-- 소유자가 juan 이면 guardx_admin 이 앞으로 ALTER/DROP 을 못 한다.
ALTER TABLE trajectory_segments OWNER TO guardx_admin;

-- 뷰도 함께 (위 ⚠ 참조 — 안 하면 이관 직후 세 뷰가 죽는다)
ALTER VIEW reliable_trajectory_segments       OWNER TO guardx_admin;
ALTER VIEW trajectory_zone_dwell_summary      OWNER TO guardx_admin;
ALTER VIEW trajectory_zone_transition_summary OWNER TO guardx_admin;

-- ── 2. 권한 ──
-- writer = 폴러의 적재(INSERT). UPDATE·SELECT 는 다른 테이블과 같은 조합으로
-- 맞춘다 (schema.sql §6 이 writer 에 준 것이 arw 다).
GRANT INSERT, UPDATE, SELECT ON trajectory_segments TO guardx_writer;

-- reader = guardx_mqttd 의 query/trajectory 응답 (task_vms.cpp
-- handleTrajectoryAnalytics — 이 테이블을 세 번 읽는다).
GRANT SELECT ON trajectory_segments TO guardx_reader;

-- 뷰에도 SELECT. 지금 코드는 본 테이블을 직접 읽지만(뷰 미사용), 뷰만
-- 권한이 빠져 있으면 psql·DBeaver 로 들여다볼 때 이유 없이 막힌다.
GRANT SELECT ON reliable_trajectory_segments,
                trajectory_zone_dwell_summary,
                trajectory_zone_transition_summary
      TO guardx_reader, guardx_writer;

-- juan 계정 — 사람이 psql 로 쓰는 계정으로 남긴다. 이 테이블을 만든 계정이라
-- 이관 후 접근을 통째로 잃으면 기존 조회 습관이 이유 없이 깨진다.
-- 다른 테이블에서 juan 이 이미 갖고 있는 것보다 좁게 준다(SELECT 만) —
-- 적재는 이제 guardx_writer 의 몫이다.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'juan') THEN
        GRANT SELECT ON trajectory_segments TO juan;
        GRANT SELECT ON reliable_trajectory_segments,
                        trajectory_zone_dwell_summary,
                        trajectory_zone_transition_summary TO juan;
    END IF;
END $$;

COMMIT;

-- ── 확인 ──
--   -- 소유자 4개가 전부 guardx_admin 이어야 한다
--   SELECT c.relname, pg_get_userbyid(c.relowner) AS owner
--     FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
--    WHERE n.nspname = 'public' AND c.relname LIKE '%trajectory%'
--    ORDER BY 1;
--
--   -- 넷 다 t 여야 한다
--   SELECT has_table_privilege('guardx_writer','trajectory_segments','INSERT') AS w_ins,
--          has_table_privilege('guardx_writer','trajectory_segments','SELECT') AS w_sel,
--          has_table_privilege('guardx_reader','trajectory_segments','SELECT') AS r_sel,
--          has_table_privilege('guardx_reader','reliable_trajectory_segments','SELECT') AS r_view;
--
--   -- 뷰가 실제로 읽히는지 (권한만 봐서는 뷰 소유자 문제를 못 잡는다)
--   SET ROLE guardx_reader;
--   SELECT count(*) FROM reliable_trajectory_segments;
--   RESET ROLE;

-- ── 롤백 ──
--   BEGIN;
--     ALTER TABLE trajectory_segments OWNER TO juan;
--     ALTER VIEW reliable_trajectory_segments       OWNER TO juan;
--     ALTER VIEW trajectory_zone_dwell_summary      OWNER TO juan;
--     ALTER VIEW trajectory_zone_transition_summary OWNER TO juan;
--     REVOKE ALL ON trajectory_segments FROM guardx_writer, guardx_reader;
--   COMMIT;
--   -- 되돌리면 PGCONN 도 user=juan 으로 함께 되돌려야 한다.
