-- ============================================================
-- GuardX — 스키마 피드백 4호: congestion_prediction 예측 구간 컬럼 (2026-07-28)
-- 실행:  sudo -u postgres psql -d guardx < Database/migration_prediction_feedback4.sql
--
-- 카메라 /prediction v2는 horizon별 p10/p90/p_over_capacity, model.warmup을
-- 이미 서빙 중 — DB 컬럼 4개(nullable) 신설 + task_prediction INSERT 확장.
-- 효용: 위험 기반 경보(p_over_capacity 확률 임계), 예측 구간 적중률 평가,
--       MAE 평가에서 warmup 구간 구분.
--
-- ⚠ 적재 규칙: 카메라의 p_over_capacity = -1 은 "불명" → NULL 로 저장 (확률 0 아님).
-- ⚠ warmup 행도 이제 적재한다 (기존 skip 대신 플래그 구분) — 소비자는
--    warmup IS NOT TRUE 로 걸러라 (기존 행 NULL = 비-warmup으로 해석).
-- 멱등. 폴러 무중단 (구 바이너리는 새 컬럼 NULL 적재 — 무해).
-- ============================================================

ALTER TABLE congestion_prediction ADD COLUMN IF NOT EXISTS p10             REAL;
ALTER TABLE congestion_prediction ADD COLUMN IF NOT EXISTS p90             REAL;
ALTER TABLE congestion_prediction ADD COLUMN IF NOT EXISTS p_over_capacity REAL;
ALTER TABLE congestion_prediction ADD COLUMN IF NOT EXISTS warmup          BOOLEAN;

COMMENT ON COLUMN congestion_prediction.p10 IS
    '예측 하한 (10분위, 명 단위 소수). NULL = 구 폴러 적재분';
COMMENT ON COLUMN congestion_prediction.p90 IS
    '예측 상한 (90분위, 명 단위 소수). NULL = 구 폴러 적재분';
COMMENT ON COLUMN congestion_prediction.p_over_capacity IS
    '용량 초과 확률 0..1. 카메라 -1(불명)은 NULL — 확률 0과 다름';
COMMENT ON COLUMN congestion_prediction.warmup IS
    'true = 모델 워밍업 중(관측 30개 미만) 예측. 평가·경보에서 제외 대상. NULL = 구 적재분(비-warmup)';

-- 검증: \d congestion_prediction → 컬럼 4개 확인
