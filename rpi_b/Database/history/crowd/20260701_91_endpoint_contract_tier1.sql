-- ============================================================
-- GuardX 엔드포인트 계약 정합 — Tier 1 (additive, 비파괴)
-- 대상 DB: guardx  /  실행 권한: guardx_admin 또는 postgres (juan 불가 — 소유자 아님)
-- 실행:  sudo -u postgres psql -d guardx -f migration_endpoint_contract_tier1.sql
--
-- 근거: /detections·/prediction·/config 카메라 출력(v9) ↔ 라이브 스키마 대조.
--   - detections : 이미 정합 (channel/category/likelihood/rect_* 존재) → 스키마 변경 없음.
--   - congestion_prediction : 예측을 형상 epoch에 태그할 config_version 부재 → 추가.
--   - zones.capacity_limit : 전 행 NULL → congestion 비율 분모 채우기 (값은 아래 참조).
--
-- 이 스크립트가 하는 것 (전부 additive, 롤백 쉬움):
--   1. congestion_prediction.config_version 컬럼 추가
--   그 외(capacity 값, zones 버저닝)는 값/결정이 필요 → 하단 주석 참조.
-- ============================================================

BEGIN;

-- ── 1. 예측 epoch 태그 ────────────────────────────────────
-- /prediction 응답의 config_version을 저장. NULL 허용(기존 행은 epoch 이전).
-- model_version(text)과 의미 분리: model_version=알고리즘 판, config_version=형상 epoch.
ALTER TABLE congestion_prediction
    ADD COLUMN IF NOT EXISTS config_version integer;

COMMENT ON COLUMN congestion_prediction.config_version IS
    '예측 계산 시점의 zones 형상 epoch (/prediction config_version). 형상 변경 경계 넘는 MAE 오염 방지.';

COMMIT;

-- ── 검증 ─────────────────────────────────────────────────
-- \d congestion_prediction   → config_version 컬럼 확인
-- 폴러가 아직 이 컬럼을 안 채우면 NULL로 들어감(무해). task_prediction INSERT에
-- config_version 매핑 추가는 폴러 트랙에서.

-- ============================================================
-- 아래는 이 스크립트에 넣지 않음 — 이유와 함께 명시 (blind 실행 금지)
-- ============================================================
--
-- [A] zones.capacity_limit 채우기 — 값이 필요함(내가 지어낼 수 없음)
--     congestion = predicted_count / capacity_limit 의 분모. 4존 전부 NULL.
--     실제 수용인원을 알면 아래처럼 UPDATE (예시 숫자는 반드시 교체):
--
--     UPDATE zones SET capacity_limit = 20 WHERE camera_id=1 AND channel=0;  -- zone_A
--     UPDATE zones SET capacity_limit = 20 WHERE camera_id=1 AND channel=1;  -- zone_B
--     UPDATE zones SET capacity_limit = 20 WHERE camera_id=1 AND channel=2;  -- zone_C
--     UPDATE zones SET capacity_limit = 20 WHERE camera_id=1 AND channel=3;  -- zone_D
--
--     또는: 카메라가 /prediction에 capacity를 실어 보내므로, config sync 때
--     폴러가 zones.capacity_limit로 동기화(카메라를 capacity 마스터로). 어느 쪽인지 결정 필요.
--
-- [B] zones 형상 버저닝 (valid_from/valid_to/zone_version) — 설계 결정 필요, 보류
--     §9.3 close-old+insert-new는 "버전마다 새 zone_id 행"을 함의 →
--     congestion_prediction·devices·incidents 의 zone_id FK가 깨짐(어느 버전을 가리키나).
--     안전히 하려면 존 정체성을 (camera_id,channel) 안정키로 분리하고 FK 전략 재설계.
--     이건 additive ALTER로 끝나는 게 아니라 식별자 모델 결정이라 별도 처리.
--     → Tier 2. 정체성 모델(안정 zone_id 유지 vs 버전별 새 행) 확정 후 생성.
--
-- ── 롤백 (Tier 1) ────────────────────────────────────────
-- BEGIN;
--   ALTER TABLE congestion_prediction DROP COLUMN IF EXISTS config_version;
-- COMMIT;