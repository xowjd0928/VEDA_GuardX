-- ============================================================================
-- backfill_jsonl.sql — rpib_events.jsonl → PostgreSQL 백필 (PHASE 5)
--
-- 언제 쓰나 : DB가 죽어 있는 동안 db_writer가 JSONL로 폴백 기록한 구간을
--             복구할 때. pg 모드에서도 JSONL 파일은 항상 열려 있고,
--             INSERT가 실패한 레코드만 그리로 흘러간다.
--
-- 실행 방법 :
--   psql -v ON_ERROR_STOP=1 -h localhost -U guardx_writer -d guardx \
--        -v path='/home/dev/7th_VEDA_GROUP2/rpi_b/rpib_app/app/rpib_events.jsonl' \
--        -f backfill_jsonl.sql
--
--   경로는 psql이 도는 "클라이언트" 쪽 경로다(\copy이므로). 서버 파일
--   읽기 권한이 필요한 COPY와 달리 슈퍼유저가 아니어도 된다.
--
-- 왜 C 프로그램이 아닌가 : PostgreSQL이 jsonb를 다룰 수 있는데 C로 JSON
--   파서를 또 만들 이유가 없다. 파싱 규칙이 두 곳에 생기면 언젠가 어긋난다.
--
-- 중복 방지 : 이 스크립트는 "이미 들어간 것"을 알지 못한다. sensor_reading
--   중복은 (sensor_seq, received_at) 조합으로 걸러내지만, 완벽하지 않다
--   (A 재시작 시 seq가 리셋되므로 - 규약 3절). 백필은 장애 구간을 한 번만
--   돌리는 용도이며, 끝나면 JSONL을 비우거나(> rpib_events.jsonl) 옮겨두는
--   운용을 전제한다.
-- ============================================================================

\set ON_ERROR_STOP on

BEGIN;

-- ----------------------------------------------------------------------------
-- 1. 원본 적재
--    JSON 한 줄을 통째로 한 필드로 읽어야 하는데, 기본 text 포맷은 백슬래시를
--    이스케이프로 해석하고 CSV 기본 인용부호(")는 JSON에 잔뜩 들어 있다.
--    그래서 JSON에 절대 나올 수 없는 제어문자를 인용/구분자로 지정한다
--    (라인 원문 그대로 읽기 위한 관용구).
-- ----------------------------------------------------------------------------
CREATE TEMP TABLE ev ON COMMIT DROP AS
SELECT
    (line::jsonb) AS j,
    (line::jsonb)->>'type'                        AS type,
    to_timestamp(((line::jsonb)->>'ts')::bigint / 1000.0) AS ts,
    row_number() OVER ()                          AS lineno
FROM backfill_raw
WHERE line <> '' AND line LIKE '{%';

-- ----------------------------------------------------------------------------
-- 2. 센서 사이클 : sensor_reading + sensor_value 6건
--    channel_id는 fire_schema.sql의 sensor_channel 시드와 같은 순서.
--    is_valid는 센서 단위로 공유된다(SHT30=온·습, MLX90614=주변·표면).
-- ----------------------------------------------------------------------------
WITH ins AS (
    INSERT INTO sensor_reading (sensor_seq, composite_score, received_at)
    SELECT (j->>'sensor_seq')::bigint,
           (j->>'score')::real,          -- JSON null -> SQL NULL 자동
           ts
    FROM ev
    WHERE type = 'sensor'
      AND NOT EXISTS (                   -- 같은 seq+시각이 이미 있으면 건너뜀
          SELECT 1 FROM sensor_reading sr
          WHERE sr.sensor_seq = (ev.j->>'sensor_seq')::bigint
            AND sr.received_at = ev.ts)
    RETURNING reading_id, sensor_seq, received_at
)
INSERT INTO sensor_value (reading_id, channel_id, value, is_valid)
SELECT i.reading_id, v.ch, v.val, v.ok
FROM ins i
JOIN ev e ON e.type = 'sensor'
         AND (e.j->>'sensor_seq')::bigint = i.sensor_seq
         AND e.ts = i.received_at
CROSS JOIN LATERAL (VALUES
    (1::smallint, (e.j->>'gas_raw')::real,        (e.j#>>'{valid,gas}')::boolean),
    (2,           (e.j->>'spark_raw')::real,      (e.j#>>'{valid,spark}')::boolean),
    (3,           (e.j->>'temperature')::real,    (e.j#>>'{valid,temphum}')::boolean),
    (4,           (e.j->>'humidity')::real,       (e.j#>>'{valid,temphum}')::boolean),
    (5,           (e.j->>'irtemp_ambient')::real, (e.j#>>'{valid,irtemp}')::boolean),
    (6,           (e.j->>'irtemp_object')::real,  (e.j#>>'{valid,irtemp}')::boolean)
) AS v(ch, val, ok)
ON CONFLICT (reading_id, channel_id) DO NOTHING;

-- ----------------------------------------------------------------------------
-- 3. 버튼
-- ----------------------------------------------------------------------------
INSERT INTO button_event (sensor_seq, press_count, occurred_at)
SELECT (j->>'sensor_seq')::bigint, (j->>'press_count')::int, ts
FROM ev
WHERE type = 'button'
  AND NOT EXISTS (
      SELECT 1 FROM button_event be
      WHERE be.sensor_seq = (ev.j->>'sensor_seq')::bigint
        AND be.occurred_at = ev.ts);

-- ----------------------------------------------------------------------------
-- 4. 화재/해제 전이
--    cause 문자열 -> channel_id 매핑에 주의. decision.c의 cause 이름과
--    sensor_channel.channel_key가 다르다(db_writer_pg.c의 매핑표와 동일해야
--    함). 특히 'irtemp' -> irtemp_object(6)이며 irtemp_ambient(5)가 아니다.
--    해제(recovered)는 원인이 없으므로 NULL - 엔진의 pg 경로와 동일 규칙.
-- ----------------------------------------------------------------------------
INSERT INTO fire_event (event_type, cause_channel_id, trigger_seq, occurred_at)
SELECT type,
       CASE WHEN type = 'recovered' THEN NULL
            ELSE (CASE j->>'cause'
                    WHEN 'gas'      THEN 1
                    WHEN 'spark'    THEN 2
                    WHEN 'temp'     THEN 3
                    WHEN 'humidity' THEN 4
                    WHEN 'irtemp'   THEN 6
                  END)::smallint
       END,
       (j->>'trigger_seq')::bigint,
       ts
FROM ev
WHERE type IN ('fire_confirmed', 'recovered')
  AND NOT EXISTS (
      SELECT 1 FROM fire_event fe WHERE fe.occurred_at = ev.ts
                                    AND fe.event_type = ev.type);

-- ----------------------------------------------------------------------------
-- 5. 발행 명령 -> 소속 fire_event 연결
--    JSONL에는 event_id가 없다(엔진이 INSERT 후에야 알기 때문). 대신 워커가
--    이벤트 링을 FIFO로 처리하므로 "줄 순서"가 곧 소속 관계다: 각 command
--    줄의 주인은 그보다 앞에 있는 가장 가까운 전이 줄이다.
-- ----------------------------------------------------------------------------
WITH owner AS (
    SELECT lineno, j, ts,
           max(lineno) FILTER (WHERE type IN ('fire_confirmed', 'recovered'))
               OVER (ORDER BY lineno) AS owner_lineno
    FROM ev
),
cmd AS (
    SELECT o.j, e.ts AS owner_ts, e.type AS owner_type
    FROM owner o
    JOIN ev e ON e.lineno = o.owner_lineno
    WHERE o.j->>'type' = 'command'
)
INSERT INTO fire_event_command (event_id, command_id, action, value, published_seq)
SELECT fe.event_id, ac.command_id,
       cmd.j->>'action',
       (cmd.j->>'value')::int,
       (cmd.j->>'published_seq')::bigint
FROM cmd
JOIN fire_event fe ON fe.occurred_at = cmd.owner_ts
                  AND fe.event_type = cmd.owner_type
JOIN actuator_command ac ON ac.command_key = cmd.j->>'command'
ON CONFLICT (event_id, command_id) DO NOTHING;

COMMIT;

-- ============================================================================
-- 확인
-- ============================================================================
SELECT 'sensor_reading' AS tbl, count(*) FROM sensor_reading
UNION ALL SELECT 'sensor_value',       count(*) FROM sensor_value
UNION ALL SELECT 'button_event',       count(*) FROM button_event
UNION ALL SELECT 'fire_event',         count(*) FROM fire_event
UNION ALL SELECT 'fire_event_command', count(*) FROM fire_event_command;
