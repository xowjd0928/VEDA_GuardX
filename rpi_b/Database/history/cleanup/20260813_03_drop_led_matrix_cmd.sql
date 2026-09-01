-- ============================================================
-- GuardX — led_matrix 액추에이터 명령 완전 제거 (이력 포함)
-- 실행:  sudo -u postgres psql -d guardx -f \
--            Database/history/cleanup/20260813_03_drop_led_matrix_cmd.sql
--
-- 멱등 · 무중단.
--
-- 왜:
--   led_matrix 는 RPi C -> STM32 UART 브릿지를 전제로 카탈로그에만 있었고
--   그 브릿지는 구현되지 않았다. RPi C 는 명령을 받아도 경고만 찍고 버렸다
--   (actuator_registry 의 handle_unwired). VMS 화면에는 눌리는데 아무 일도
--   안 나는 ON/OFF 버튼이 남아 있었다.
--
--   **실제로 동작한 적이 없는 기능이므로 과거 명령 로그도 남길 이유가 없다.**
--   그 행들은 "이 명령을 눌렀다"는 기록일 뿐 무엇도 일어나지 않았고,
--   남겨두면 나중에 감사 로그를 읽는 사람이 실제 동작한 명령과 구분하지
--   못한다. 그래서 이력까지 함께 지운다(운영 결정).
--
-- ⚠ LED 매트릭스 **표시** 기능과는 무관하다.
--   온습도·화재·트래킹 표시는 guardx/display/rpic/{zones/N,fire,track} 으로
--   RPi B 가 직접 쏘고 RPi C 의 matrix_link 가 Modbus 로 STM32 에 넘긴다.
--   DB 를 지나지 않는 경로라 이 마이그레이션의 영향을 받지 않는다.
--
-- ⚠ 다른 명령(servo_1·shutter·fan·water_pump·sound·fan_auto)의 기록은
--   건드리지 않는다. 모든 DELETE 가 led_matrix 의 command_id 하나로만
--   좁혀져 있다.
--
-- 삭제 순서 (FK 때문에 자식 -> 부모):
--   1. manual_command      (운영자 수동 명령 로그)
--   2. fire_event_command  (화재 자동 대응 이력)
--   3. actuator_command    (카탈로그 본체)
-- ============================================================

BEGIN;

DO $$
DECLARE
    cid        SMALLINT;
    n_man      BIGINT := 0;
    n_fev      BIGINT := 0;
    unexpected TEXT;
BEGIN
    SELECT command_id INTO cid
      FROM actuator_command WHERE command_key = 'led_matrix';

    IF cid IS NULL THEN
        RAISE NOTICE 'led_matrix 없음 - 이미 정리됨 (멱등)';
        RETURN;
    END IF;

    -- 이 리포가 아는 참조 테이블은 manual_command·fire_event_command 둘뿐이다.
    -- 운영 DB 에 그 사이 늘어난 표가 있으면 여기서 멈춘다 - 모르는 표를
    -- 그냥 두면 아래 DELETE 가 FK 위반으로 실패하고, 트랜잭션이 통째로
    -- 되감기며 "왜 안 되는지"만 남는다. 이름을 찍어주는 편이 낫다.
    SELECT string_agg(DISTINCT conrelid::regclass::text, ', ')
      INTO unexpected
      FROM pg_constraint
     WHERE contype = 'f'
       AND confrelid = 'actuator_command'::regclass
       AND conrelid NOT IN ('manual_command'::regclass,
                            'fire_event_command'::regclass);

    IF unexpected IS NOT NULL THEN
        RAISE EXCEPTION
            'actuator_command 를 참조하는 예상 밖 테이블: %. '
            '이 마이그레이션을 갱신한 뒤 다시 실행할 것.', unexpected;
    END IF;

    -- 전부 command_id = cid 로만 좁힌다. 다른 명령의 기록은 손대지 않는다.
    DELETE FROM manual_command WHERE command_id = cid;
    GET DIAGNOSTICS n_man = ROW_COUNT;

    DELETE FROM fire_event_command WHERE command_id = cid;
    GET DIAGNOSTICS n_fev = ROW_COUNT;

    DELETE FROM actuator_command WHERE command_id = cid;

    RAISE NOTICE
        'led_matrix(command_id=%) 제거 완료 - manual_command %행, '
        'fire_event_command %행, 카탈로그 1행 삭제', cid, n_man, n_fev;
END $$;

-- main 의 마이그레이션 이력 규칙. 위 정리와 같은 트랜잭션에 기록한다.
-- led_matrix 가 이미 없어 DO 블록이 no-op 이어도 "확인 후 적용 완료"로 남긴다.
INSERT INTO schema_migrations (version, note) VALUES
    ('20260813_03_drop_led_matrix_cmd',
     '미구현 led_matrix 액추에이터 명령과 해당 명령 참조 이력 정리')
ON CONFLICT (version) DO NOTHING;

COMMIT;

-- ── 확인 ──
-- 1) led_matrix 가 어디에도 없어야 한다
--    SELECT * FROM actuator_command ORDER BY command_id;
--
-- 2) 다른 명령의 기록은 그대로여야 한다 (숫자가 실행 전과 같아야 함)
--
--    ⚠ 자식 표 둘을 한 번에 LEFT JOIN 하면 안 된다. 같은 command_id 에
--      manual 3행 · fire 2행이 있으면 조인 결과가 6행으로 불어나 두 count 가
--      전부 6 으로 나온다(팬아웃). 표마다 따로 세야 실제 건수가 나온다.
--
--    SELECT ac.command_key,
--           (SELECT count(*) FROM manual_command mc
--             WHERE mc.command_id = ac.command_id)  AS manual,
--           (SELECT count(*) FROM fire_event_command fec
--             WHERE fec.command_id = ac.command_id) AS fire
--      FROM actuator_command ac
--     ORDER BY ac.command_key;
--
-- 3) 표시 기능은 DB 가 아니라 브로커에서 본다 - 계속 흘러야 한다
--    mosquitto_sub -h <broker> -t 'guardx/display/rpic/#' -v
