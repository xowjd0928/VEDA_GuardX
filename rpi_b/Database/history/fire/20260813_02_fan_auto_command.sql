-- ============================================================
-- GuardX — 팬 자동 제어(fan_auto) 명령 카탈로그 등록
-- 실행:  sudo -u postgres psql -d guardx -f \
--            Database/history/fire/20260813_02_fan_auto_command.sql
--
-- 추가 전용 · 멱등 · 무중단.
--
-- 왜 필요한가:
--   guardx_mqttd 의 set_actuator 는 command_key 를 actuator_command 에서
--   찾지 못하면 "카탈로그 미등록" 으로 **거부**한다(task_vms.cpp).
--   VMS 의 AUTO 버튼은 fan_auto 명령을 그 경로로 보내므로, 이 행이 없으면
--   버튼이 눌려도 RPi C 까지 가지 못하고 조용히 막힌다.
--
--   fire_schema.sql 의 시드에도 같은 행을 넣었지만, 그건 **새로 만드는 DB**
--   에만 적용된다. 이미 도는 운영 DB 는 이 마이그레이션으로 따라온다.
--
-- kind = 'onoff':
--   AUTO 는 켜고 끄는 것뿐이다. SET 을 허용하면 set_actuator 의 kind 검증이
--   value 를 통과시키는데, RPi C 의 handle_fan_auto 는 그걸 거절하므로
--   "명령은 받아들여졌는데 아무 일도 안 일어나는" 구간이 생긴다.
--
-- command_id = 6:
--   기존 시드가 1,2,3,4,5,7 을 쓰고 6 이 비어 있다.
-- ============================================================

BEGIN;

INSERT INTO actuator_command (command_id, command_key, actuator, kind) VALUES
    (6, 'fan_auto', '배연팬 자동 제어(혼잡 단계 연동)', 'onoff')
ON CONFLICT (command_key) DO UPDATE
    SET actuator = EXCLUDED.actuator,
        kind     = EXCLUDED.kind;

-- main 의 마이그레이션 이력 규칙. 기능 변경과 같은 트랜잭션에 기록해야
-- 적용은 됐는데 이력만 빠지거나, 반대로 이력만 남는 상태가 생기지 않는다.
INSERT INTO schema_migrations (version, note) VALUES
    ('20260813_02_fan_auto_command',
     'actuator_command 카탈로그에 fan_auto(onoff) 등록')
ON CONFLICT (version) DO NOTHING;

COMMIT;

-- 확인:
--   SELECT * FROM actuator_command ORDER BY command_id;
--   -- fan_auto 행이 kind='onoff' 로 보여야 한다.
