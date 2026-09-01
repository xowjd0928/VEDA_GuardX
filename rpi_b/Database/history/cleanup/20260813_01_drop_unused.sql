BEGIN;

-- (1) guardx_maintain 재정의 — device_logs 블록만 제거. 실행이 아니라 정의 변경.
DROP FUNCTION IF EXISTS guardx_maintain(INT, INT, INT, INT, INT, INT);

CREATE FUNCTION guardx_maintain(
    p_detections_days  INT DEFAULT 14,
    p_prediction_days  INT DEFAULT 180,
    p_faces_days       INT DEFAULT 30,
    p_flow_days        INT DEFAULT 180,
    p_occupancy_days   INT DEFAULT 365)
RETURNS TEXT LANGUAGE plpgsql AS $$
DECLARE
    n_created int; n_dropped int; n_pred bigint;
    n_face bigint; n_flow bigint; n_occ bigint;
BEGIN
    n_created := detections_ensure_partitions(7);
    n_dropped := detections_drop_old(p_detections_days);
    DELETE FROM congestion_prediction
        WHERE predicted_at < now() - make_interval(days => p_prediction_days);
    GET DIAGNOSTICS n_pred = ROW_COUNT;
    DELETE FROM faces
        WHERE ts < now() - make_interval(days => p_faces_days);
    GET DIAGNOSTICS n_face = ROW_COUNT;
    DELETE FROM line_flow
        WHERE bucket_ts < now() - make_interval(days => p_flow_days);
    GET DIAGNOSTICS n_flow = ROW_COUNT;
    DELETE FROM zone_occupancy
        WHERE bucket_ts < now() - make_interval(days => p_occupancy_days);
    GET DIAGNOSTICS n_occ = ROW_COUNT;
    RETURN format('partitions +%s/-%s, predictions -%s, '
                  'faces -%s, line_flow -%s, zone_occupancy -%s',
                  n_created, n_dropped, n_pred, n_face, n_flow, n_occ);
END $$;

-- (2) 미사용 테이블 삭제. CASCADE 안 씀 — 예상 못 한 의존이 있으면 에러로 멈춰야 한다.
DROP TABLE IF EXISTS device_logs;
DROP TABLE IF EXISTS devices;
DROP TABLE IF EXISTS zone_flow;

-- (3) 이력 기록
INSERT INTO schema_migrations (version, note) VALUES
  ('20260813_01_drop_unused',
   'zone_flow/device_logs/devices 삭제 + guardx_maintain에서 device_logs 제거');

COMMIT;
