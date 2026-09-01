-- ============================================================================
-- 01_init.sql — GuardX 서버 레벨 초기 설정
--
-- 실행 위치 : PostgreSQL 서버 (guardx DB 생성 이전, 서버 전역 레벨)
-- 실행 계정 : postgres (관리자)
-- 실행 상황 : 최초 1회만. DB를 밀고 재구축할 때도 이 파일은 재실행 불필요
--             (계정은 서버 레벨 객체라 DROP DATABASE에 영향받지 않음)
-- 실행 방법 : sudo -u postgres psql -f init.sql
--
-- 주의     : 비밀번호 'CHANGE_ME_*'는 실행 전 반드시 실제 값으로 교체.
--            이 파일은 교체 후 git에 올리지 말 것 (플레이스홀더 상태만 커밋)
-- ============================================================================


-- ============================================================================
-- 1. 계정 생성 (역할 분리 — D9)
--
-- 계정을 3개로 나누는 이유: 각 접속 주체가 탈취되더라도 피해 범위를
-- 자기 권한 안으로 한정하기 위함 (최소 권한 원칙)
--   guardx_admin  : 스키마 생성/변경 전용 (02_schema.sql 실행 주체)
--   guardx_writer : 라파 B Data Handler 전용 (INSERT/UPDATE만)
--   guardx_reader : VMS 또는 read API 전용 (SELECT만)
--
-- DO 블록을 쓰는 이유: CREATE ROLE은 IF NOT EXISTS를 지원하지 않아
-- 재실행 시 에러가 나므로, 존재 여부를 확인해 멱등하게 만듦
-- ============================================================================

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'guardx_admin') THEN
        CREATE ROLE guardx_admin LOGIN PASSWORD 'qkdwnsgks123!';
    END IF;

    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'guardx_writer') THEN
        CREATE ROLE guardx_writer LOGIN PASSWORD 'qkdwnsgks123!';
    END IF;

    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'guardx_reader') THEN
        CREATE ROLE guardx_reader LOGIN PASSWORD 'qkdwnsgks123!';
    END IF;
END
$$;


-- ============================================================================
-- 2. 데이터베이스 생성
--
-- OWNER를 guardx_admin으로 지정해, 이후 스키마 작업(02_schema.sql)을
-- postgres 없이 admin 계정만으로 수행 가능하게 함
-- ============================================================================

-- CREATE DATABASE는 IF NOT EXISTS 미지원 → psql 변수로 존재 확인 후 생성
SELECT 'CREATE DATABASE guardx OWNER guardx_admin'
WHERE NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = 'guardx') \gexec


-- ============================================================================
-- 3. PostGIS 확장 설치
--
-- CREATE EXTENSION은 슈퍼유저 권한이 필요하므로 02(admin 실행)가 아닌
-- 여기(postgres 실행)에서 처리. guardx DB에 접속해 설치
-- ============================================================================

\connect guardx

CREATE EXTENSION IF NOT EXISTS postgis;


-- ============================================================================
-- 4. 설치 확인
-- ============================================================================

-- 계정 3종 생성 확인
SELECT rolname FROM pg_roles WHERE rolname LIKE 'guardx_%';

-- PostGIS 버전 확인 (버전 문자열이 나오면 정상)
SELECT postgis_version();
