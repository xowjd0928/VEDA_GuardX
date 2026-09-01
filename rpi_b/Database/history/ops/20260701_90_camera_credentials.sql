-- ============================================================
-- GuardX — camera_credentials (카메라 접속 계정 DB 저장)
-- 실행:  sudo -u postgres psql -d guardx -f migration_camera_credentials.sql
--
-- 설계 판단 (명시):
--   비밀번호는 평문 저장. 근거: DB는 RPiB localhost 전용 + 폴러가 digest 인증에
--   원문 비번이 필요(해시 저장 불가). 접근 통제는 DB 권한으로 (juan: SELECT only).
--   외부 노출 DB로 바뀌면 이 판단 재검토 필요.
-- 멱등: 재실행 안전.
-- ============================================================

BEGIN;

CREATE TABLE IF NOT EXISTS camera_credentials (
    camera_id  integer PRIMARY KEY REFERENCES cameras(camera_id),
    cam_user   text NOT NULL,
    cam_pass   text NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT now()
);

-- 폴러(juan)는 읽기만
GRANT SELECT ON camera_credentials TO juan;

-- 계정 시드 (현재 카메라 admin — 운영 시 폴링 전용 계정으로 교체 권장)
INSERT INTO camera_credentials (camera_id, cam_user, cam_pass)
VALUES (1, 'admin', 'qkdwnsgks123!')
ON CONFLICT (camera_id) DO UPDATE
  SET cam_user = EXCLUDED.cam_user,
      cam_pass = EXCLUDED.cam_pass,
      updated_at = now();

COMMIT;

-- 검증:
--   psql -U juan -d guardx -h localhost -c "SELECT camera_id, cam_user FROM camera_credentials;"
--   (juan 으로 조회돼야 폴러가 읽을 수 있음. cam_pass 는 화면에 안 찍는 습관 권장.)
--
-- 이후 config.env 에서 CAM_PASS 제거 가능 (CAM_HOST 는 유지 — DB 접속 전에 필요하진
-- 않지만 카메라 주소는 여전히 config 소관).
--
-- 롤백:
--   DROP TABLE IF EXISTS camera_credentials;