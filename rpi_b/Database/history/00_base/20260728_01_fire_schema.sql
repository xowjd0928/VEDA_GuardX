-- ============================================================================
-- fire_schema.sql — GuardX RPi B 화재 파이프라인 전용 스키마 (정규화형, 3NF)
--
-- 실행 위치 : guardx DB 내부
-- 실행 계정 : guardx_admin (테이블 소유자)
-- 실행 횟수 : 반복 실행 가능 (테스트 사이클용, 매 실행 시 아래 테이블만 리셋)
-- 실행 방법 : psql -v ON_ERROR_STOP=1 -h localhost -U guardx_admin -d guardx \
--                  -f fire_schema.sql
--             ON_ERROR_STOP=1 을 빼지 말 것. psql은 기본적으로 에러가 나도
--             다음 문장을 계속 실행하고 exit code도 0을 반환해서, GRANT 절
--             하나가 죽어도 "성공한 것처럼" 보인다. 실제로 그렇게 놓친
--             시퀀스 권한 누락이 나중에 INSERT 시점에야 터진 적이 있다.
--             -h localhost 는 유닉스 소켓 peer 인증(OS 계정명 = 롤명 강제)을
--             피해 TCP 비밀번호 인증으로 붙기 위함.
--
-- 위치 관계
--   - 기존 schema.sql(카메라/군중/추적 + incidents/device_logs)과 별개의
--     독립 파일이다. 기존 12테이블을 건드리지 않고, RPi B 화재 감지
--     파이프라인(센서 수신 → 판단 → 제어 발행)만 자체 완결적으로 담는다.
--   - 기존 zones/devices 테이블에 FK를 걸지 않는다(완전 독립). 나중에
--     구역/디바이스와 묶을 필요가 생기면 sensor_reading에 zone_id 컬럼을
--     추가해 "논리 연결"로 붙이면 된다(물리 FK 없이).
--
-- 정규화 원칙 (전부 3NF, ⑤/⑦은 BCNF)
--   1) 반복그룹 제거(1NF): 센서 6값을 가로 6컬럼이 아니라 "채널마다 1행"
--      (sensor_value)으로 세로 저장.
--   2) 부분종속 제거(2NF): 복합키(sensor_value, fire_event_command)의
--      비키 컬럼은 키 전체에 종속.
--   3) 이행종속 제거(3NF): 반복되는 문자열(cause, command)을 마스터
--      테이블로 빼고 숫자 ID(FK)로 참조.
--
-- 사전 요건 : guardx_writer / guardx_reader 역할이 이미 존재해야 함
--             (기존 init 스크립트가 생성. 없으면 맨 아래 GRANT 절에서 실패).
--             PostGIS 불필요(공간 컬럼 없음).
-- ============================================================================

-- 재실행용 리셋: FK 역순으로 삭제 (자식 → 부모)
DROP FUNCTION IF EXISTS guardx_fire_maintain(int);
DROP TABLE IF EXISTS manual_command     CASCADE;
DROP TABLE IF EXISTS fire_event_command CASCADE;
DROP TABLE IF EXISTS fire_event         CASCADE;
DROP TABLE IF EXISTS button_event       CASCADE;
DROP TABLE IF EXISTS sensor_value       CASCADE;
DROP TABLE IF EXISTS sensor_reading     CASCADE;
DROP TABLE IF EXISTS fire_threshold     CASCADE;
DROP TABLE IF EXISTS actuator_command   CASCADE;
DROP TABLE IF EXISTS sensor_channel     CASCADE;
DROP TABLE IF EXISTS fire_zone          CASCADE;


-- ============================================================================
-- 1. 마스터(참조) 테이블 — 거의 안 변하는 정의. 코드가 ID로 참조한다.
-- ============================================================================

-- ----------------------------------------------------------------------------
-- sensor_channel : 측정 "채널" 정의. 물리 센서 1개가 값을 여러 개 줄 수 있어
--                  (SHT30=온+습, MLX90614=주변+표면) 센서가 아니라 채널 단위.
--                  channel_key는 RPi A json_builder.c의 values 키와 1:1.
-- raw_min/raw_max : 그 채널 ADC의 고유 범위(채널 속성). raw가 아닌 물리값
--                   채널은 NULL. (value_kind에 종속시키지 않으므로 3NF 유지)
-- ----------------------------------------------------------------------------
CREATE TABLE sensor_channel (
    channel_id   SMALLINT PRIMARY KEY,
    channel_key  TEXT NOT NULL UNIQUE,        -- gas_raw / temperature / ...
    device       TEXT NOT NULL,               -- MQ-2 / SHT30 / MLX90614 / TS0226
    unit         TEXT NOT NULL,               -- adc / °C / %
    value_kind   TEXT NOT NULL CHECK (value_kind IN ('raw', 'physical')),
    raw_min      INT,                         -- raw 채널만, 아니면 NULL
    raw_max      INT
);

-- ----------------------------------------------------------------------------
-- actuator_command : RPi B→C 제어 명령 카탈로그. command_key는
--                    guardx_protocol.h의 GUARDX_CMD_* 문자열과 1:1.
-- kind : onoff(ON/OFF만) / set(value 동반) / both(둘 다 가능)
--        shutter는 예외 - kind='both'지만 실제 action은 OPEN/CLOSE/STOP
--        (일반 ON/OFF/SET이 아님, fire_event_command CHECK 참조)
-- ----------------------------------------------------------------------------
CREATE TABLE actuator_command (
    command_id   SMALLINT PRIMARY KEY,
    command_key  TEXT NOT NULL UNIQUE,        -- servo_1 / water_pump / ...
    actuator     TEXT NOT NULL,               -- 사람이 읽는 설명
    kind         TEXT NOT NULL CHECK (kind IN ('onoff', 'set', 'both'))
);

-- ----------------------------------------------------------------------------
-- fire_zone : 화재 감지 "구역" ↔ 물리 노드(RPi A/C 쌍) 매핑.
--   지금은 하드웨어 제약으로 1행뿐이지만(zone_id=1, rpia/rpic), 구역이
--   늘어나도 코드를 고치지 않고 이 표에 행만 추가하면 되도록 rpib_engine이
--   부팅 시(+ guardx/config/rpib 신호로 핫리로드) 이 표 전체를 읽어들인다
--   (rpi_b/rpib_app/app/src/zone_loader.c).
--   기존 zones(카메라 구역) 테이블과 의도적으로 분리한다 - 그쪽은
--   camera_id가 NOT NULL이라 카메라 없는 화재 구역엔 억지로 맞지 않는다.
--   나중에 "카메라 구역 == 화재 구역"으로 통합하고 싶어지면 그때 다리를
--   놓으면 된다 (지금은 각자 독립된 zone_id 체계).
-- rpia_node_id/rpic_node_id : guardx_protocol.h 규약의 실제 노드 ID 문자열
--   ("rpia", "rpia-2" 등 - 노드별 config.env의 NODE_ID로 배포 시 지정).
--   MQTT 토픽(guardx/sensor/%s 등)의 %s 자리에 그대로 들어간다.
-- ----------------------------------------------------------------------------
CREATE TABLE fire_zone (
    zone_id       SMALLINT PRIMARY KEY,
    zone_name     TEXT NOT NULL,
    rpia_node_id  TEXT NOT NULL UNIQUE,
    rpic_node_id  TEXT NOT NULL UNIQUE
);


-- ============================================================================
-- 2. 설정 테이블 — 런타임 임계값(핫 리로드 소스). 이력 보존 방식.
-- ============================================================================

-- ----------------------------------------------------------------------------
-- fire_threshold : 화재 판단 임계값. 엔진은 is_active=true 한 행만 읽어
--                  런타임 변수로 적재한다. 값을 바꿀 땐 새 행 INSERT +
--                  이전 행 is_active=false → guardx/config/rpib 로 신호.
--                  (재시작 없이 반영, 이력은 사후 감사용으로 남는다)
-- ----------------------------------------------------------------------------
-- PHASE 4: PHASE 2 퍼지 가중치 융합(decision.c)에 맞춰 재설계. 기존
-- gas_ppm_limit는 raw→ppm 환산을 전제한 컬럼인데, "raw 정책"(RPi A는
-- raw만 발행, 환산은 하지 않음) 결정 이후 실제로는 gas_raw를 그대로
-- 퍼지화하므로 애초에 안 맞는 컬럼이었다 - 이번에 같이 정리한다.
--
-- 가로형(1행 = 설정 스냅샷 전체) 유지 - 세로형(키-값)으로 쪼개면
-- "가중치 5개 합=1.0"류 제약을 여러 행에 걸쳐 검증해야 해서 원자성이
-- 깨진다. 지금처럼 한 행이 통째로 유효 단위여야 INSERT 1건 + is_active
-- 플립만으로 새 버전 전환이 원자적으로 끝난다.
CREATE TABLE fire_threshold (
    threshold_id           SERIAL PRIMARY KEY,

    -- 퍼지화 구간 [MIN, MAX] (decision.h 계승, MIN 이하 위험도 0 / MAX 이상 100)
    gas_raw_min             REAL NOT NULL,   -- MQ-2 raw (0~1023), 오름차순
    gas_raw_max             REAL NOT NULL,
    -- TS0226 raw (0~1023), 내림차순: 실측상 불꽃 근접 시 raw가 낮아지는
    -- 포토트랜지스터형이라 습도와 같은 방향(safe 이상 0 / danger 이하 100)
    spark_raw_safe          REAL NOT NULL,
    spark_raw_danger        REAL NOT NULL,
    temp_min_c              REAL NOT NULL,   -- SHT30 대기 온도
    temp_max_c              REAL NOT NULL,
    humi_safe_percent       REAL NOT NULL,   -- 이 이상이면 위험도 0 (내림차순 퍼지화)
    humi_danger_percent     REAL NOT NULL,   -- 이 이하면 위험도 100
    irtemp_min_c            REAL NOT NULL,   -- MLX90614 표면온도
    irtemp_max_c            REAL NOT NULL,

    -- 센서별 가중치 (합계 = 1.0, CHECK로 강제)
    weight_gas              REAL NOT NULL,
    weight_spark            REAL NOT NULL,
    weight_temp             REAL NOT NULL,
    weight_humi             REAL NOT NULL,
    weight_irtemp           REAL NOT NULL,

    fire_score_threshold    REAL NOT NULL,   -- 종합 위험도 임계 (0~100)
    n_confirm               INT  NOT NULL,   -- 화재 확정에 필요한 연속 초과 사이클
    n_recover                INT  NOT NULL,  -- 해제에 필요한 연속 정상 사이클

    -- FIRE 중 센서 무효로 해제 판정이 이만큼 동결되면 완화를 검토한다.
    -- (완화 = 살아있는 채널만으로 해제 판정 재개. 단 생존 가중치가
    -- min_valid_weight 이상일 때만. decision.h 3단 구조 주석 참조)
    -- 이게 없으면 센서 영구 고장 시 FIRE에서 영원히 못 빠져나온다.
    freeze_relax_cycles      INT  NOT NULL,

    -- 무효 채널 재정규화 분모 하한 (decision.h MIN_VALID_WEIGHT 참조)
    min_valid_weight         REAL NOT NULL,

    -- 교차 확증 오버라이드 임계 (불꽃+표면온도 동시 초과 시 가중합산 우회)
    override_spark_score     REAL NOT NULL,
    override_irtemp_score    REAL NOT NULL,

    is_active                BOOLEAN NOT NULL DEFAULT FALSE,
    updated_at                TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_by                TEXT,

    CONSTRAINT chk_gas_range    CHECK (gas_raw_min < gas_raw_max),
    CONSTRAINT chk_spark_range  CHECK (spark_raw_safe > spark_raw_danger),
    CONSTRAINT chk_temp_range   CHECK (temp_min_c < temp_max_c),
    CONSTRAINT chk_irtemp_range CHECK (irtemp_min_c < irtemp_max_c),
    CONSTRAINT chk_humi_range   CHECK (humi_safe_percent > humi_danger_percent),
    CONSTRAINT chk_weight_sum   CHECK (
        ABS((weight_gas + weight_spark + weight_temp + weight_humi + weight_irtemp) - 1.0) < 0.001
    ),
    CONSTRAINT chk_score_range  CHECK (fire_score_threshold BETWEEN 0 AND 100),
    CONSTRAINT chk_override_range CHECK (
        override_spark_score BETWEEN 0 AND 100 AND override_irtemp_score BETWEEN 0 AND 100
    ),
    CONSTRAINT chk_min_valid_weight CHECK (min_valid_weight > 0 AND min_valid_weight <= 1),
    CONSTRAINT chk_cycles CHECK (n_confirm > 0 AND n_recover > 0
                                 AND freeze_relax_cycles > 0)
);

-- is_active=true 행은 동시에 최대 1개만 존재하도록 강제 (부분 유니크 인덱스)
CREATE UNIQUE INDEX uq_fire_threshold_active
    ON fire_threshold (is_active) WHERE is_active;


-- ============================================================================
-- 3. 거래(시계열) 테이블 — 계속 쌓이는 데이터
-- ============================================================================

-- ----------------------------------------------------------------------------
-- sensor_reading : "한 사이클 수신" 봉투. 값은 여기 없고 sensor_value로 분리.
-- sensor_seq     : RPi A 발행 순번. A 재시작 시 리셋되므로(규약 3절) 전역
--                  유일이 아니다 → PK/FK로 쓰지 않고 역추적 단서로만 사용.
-- received_at    : 기록 시각. 기본 now()이나, A의 timestamp를 쓰고 싶으면
--                  writer가 명시적으로 넣어도 된다.
-- ----------------------------------------------------------------------------
-- composite_score : 그 사이클의 퍼지 융합 종합 위험도(0~100). 판단이
--   점수를 내지 않은 사이클(FIRE 상태에서 센서 무효로 해제 판정이
--   동결된 경우)은 NULL - "0점"과 "계산 안 함"은 전혀 다른 의미다.
--   실측 임계값 튜닝(벤치마킹)의 전제 조건. 이 값이 없으면 사후에
--   남는 단서가 cause 하나뿐이라 "왜 그 점수가 나왔는지"를 되짚을 수
--   없다. 파생값이지만 원본 센서값이 sensor_value에 함께 남으므로
--   재계산 가능하며, 여기 저장하는 것은 "그때 그 설정으로 판단한
--   결과"를 보존하기 위함이다(임계값이 핫리로드로 바뀌므로 나중에
--   재계산하면 당시 판단과 달라진다).
CREATE TABLE sensor_reading (
    reading_id      BIGSERIAL PRIMARY KEY,
    zone_id         SMALLINT NOT NULL REFERENCES fire_zone (zone_id),
    sensor_seq      BIGINT NOT NULL,
    composite_score REAL,
    received_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_sensor_reading_ts  ON sensor_reading (received_at);
CREATE INDEX idx_sensor_reading_seq ON sensor_reading (sensor_seq);
-- (zone_id, sensor_seq) UNIQUE는 시도했다가 뺐다 - sensor_seq는 RPi A가
-- 재시작마다 0으로 리셋하는 카운터라(rpia_app/app/src/main.c) 전역 유일성이
-- 없다. rpib_ingest(원시값 INSERT)와 rpib_decision(composite_score UPDATE)의
-- 병합은 대신 "그 zone의 최신 reading_id"를 기준으로 한다
-- (rpib_decision/app/src/db_writer.c 참조).
-- zone별 "가장 최근 사이클" 조회 전용 (task_vms.cpp querySensors의 LATERAL).
-- 이 표는 1Hz로 쌓여 하루 8.6만 행이라, 이 인덱스가 없으면 1초마다 도는
-- 상태 발행이 전체 스캔을 하게 된다. zone_id 등가 + reading_id 역순이라
-- LATERAL 안쪽이 인덱스 첫 행만 읽고 끝난다.
CREATE INDEX idx_sensor_reading_zone_latest
    ON sensor_reading (zone_id, reading_id DESC);

-- ----------------------------------------------------------------------------
-- sensor_value : 채널별 측정값. 한 사이클(reading_id) × 6채널(channel_id).
--                복합 PK가 "한 사이클에 같은 채널 중복 없음"을 구조로 보장.
-- value        : raw든 물리값이든 단일 컬럼. REAL은 0~1023 정수를 손실 없이
--                담는다. is_valid=false면 그 값은 신뢰 불가(센서 장애).
-- ON DELETE CASCADE : 오래된 reading을 지우면 딸린 값도 함께 정리.
-- ----------------------------------------------------------------------------
CREATE TABLE sensor_value (
    reading_id  BIGINT   NOT NULL REFERENCES sensor_reading (reading_id) ON DELETE CASCADE,
    channel_id  SMALLINT NOT NULL REFERENCES sensor_channel (channel_id),
    value       REAL,
    is_valid    BOOLEAN  NOT NULL,
    PRIMARY KEY (reading_id, channel_id)
);

-- 채널별 시계열 분석(예: gas_raw 추이)용 보조 인덱스
CREATE INDEX idx_sensor_value_channel ON sensor_value (channel_id, reading_id);

-- ----------------------------------------------------------------------------
-- fire_event : 화재 상태 전이 사건(전이 순간만 1행).
-- cause_channel_id : 확정을 촉발한 채널(gas/spark/temp...). recovered 사건은
--                    원인이 없으므로 NULL 허용. 문자열 대신 FK로 정규화.
-- trigger_seq      : 유발한 sensor_seq (sensor_reading와 논리 연결, FK 아님).
-- ----------------------------------------------------------------------------
CREATE TABLE fire_event (
    event_id          BIGSERIAL PRIMARY KEY,
    zone_id           SMALLINT NOT NULL REFERENCES fire_zone (zone_id),
    event_type        TEXT NOT NULL CHECK (event_type IN ('fire_confirmed', 'recovered')),
    cause_channel_id  SMALLINT REFERENCES sensor_channel (channel_id),
    trigger_seq       BIGINT,
    occurred_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_fire_event_ts   ON fire_event (occurred_at);
CREATE INDEX idx_fire_event_type ON fire_event (event_type);

-- ----------------------------------------------------------------------------
-- fire_event_command : 사건 1건에서 발행한 제어 명령들(사건 ↔ 명령 다대다).
--                      JSONB 요약 대신 이 연결 테이블로 풀어 명령별 SQL 분석 가능.
-- action : guardx_protocol.h와 동일하게 대문자. 'ON'/'OFF'/'SET'은 일반
--          onoff/set 액추에이터, 'OPEN'/'CLOSE'/'STOP'은 화재셔터(stepper)
--          전용 - 리밋센서 IRQ가 정지를 알아서 처리하므로 방향값(SET) 대신
--          동사형 명령만 노출한다.
-- value  : SET일 때 각도/듀티(90 등), 나머지는 NULL.
-- ----------------------------------------------------------------------------
CREATE TABLE fire_event_command (
    event_id       BIGINT   NOT NULL REFERENCES fire_event (event_id) ON DELETE CASCADE,
    command_id     SMALLINT NOT NULL REFERENCES actuator_command (command_id),
    action         TEXT NOT NULL CHECK (action IN ('ON', 'OFF', 'SET', 'OPEN', 'CLOSE', 'STOP')),
    value          INT,
    published_seq  BIGINT,
    PRIMARY KEY (event_id, command_id)
);

-- ----------------------------------------------------------------------------
-- button_event : 비상 버튼 눌림 로그.
--   실제 액추에이터 제어는 RPi A→C 유선 릴레이 인터락(하드웨어)이 즉시
--   처리하며, 이 테이블은 규약 4-2의 "로그 정확히 1회" 기록 경로다
--   (A→B guardx/sensor/rpia/button QoS2 → main.c on_button → 여기).
--   판단 로직 입력이 아니라 감사/표시(VMS)용이다.
-- press_count : RPi A가 붙인 버튼 누적 누름 횟수(중복 수신 판별에도 활용).
-- ----------------------------------------------------------------------------
CREATE TABLE button_event (
    button_event_id  BIGSERIAL PRIMARY KEY,
    zone_id          SMALLINT NOT NULL REFERENCES fire_zone (zone_id),
    sensor_seq       BIGINT NOT NULL,        -- RPi A 발행 seq
    press_count      INT NOT NULL,           -- 버튼 누적 누름 횟수
    occurred_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_button_event_ts ON button_event (occurred_at);

-- ----------------------------------------------------------------------------
-- manual_command : VMS → RPi B → RPi C 수동 제어 명령 로그.
--   fire_event_command와 컬럼 모양은 비슷하지만, 그건 fire_event(화재
--   사건)에 종속된 FK라 화재가 아닌 수동 명령엔 쓸 수 없어 별도 테이블로
--   분리한다. VMS가 직접 RPi C에 발행하지 않고 반드시 B를 거치므로
--   (설계 확정: VMS→B→C), C로 나가는 명령은 항상 이 표 아니면
--   fire_event_command 둘 중 하나에 남는다 - 명령 발행 감사 경로가 하나로
--   통일됨.
-- source        : 명령 발신 주체. 지금은 'vms' 하나지만 나중에 다른
--                 운영 콘솔이 추가될 수 있어 고정 CHECK를 걸지 않는다.
-- published_seq : B가 실제 발행한 seq (guardx/actuator/rpic 페이로드의
--                 seq와 대조해 역추적 가능).
-- !!! 미확정: 화재 진행 중(FIRE 상태) 수동 명령을 그대로 통과시킬지,
--     안전 방향이 아니면 거부할지는 팀 논의 필요 (main.c 쪽 정책 미구현) !!!
-- ----------------------------------------------------------------------------
CREATE TABLE manual_command (
    manual_command_id  BIGSERIAL PRIMARY KEY,
    zone_id              SMALLINT NOT NULL REFERENCES fire_zone (zone_id),
    command_id          SMALLINT NOT NULL REFERENCES actuator_command (command_id),
    action               TEXT NOT NULL CHECK (action IN ('ON', 'OFF', 'SET', 'OPEN', 'CLOSE', 'STOP')),
    value                INT,               -- SET일 때 각도/듀티, ON/OFF면 NULL
    source               TEXT NOT NULL,     -- 'vms' 등 명령 발신 주체
    published_seq        BIGINT,            -- B가 발행한 seq (역추적용)
    issued_at            TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_manual_command_ts ON manual_command (issued_at);


-- ============================================================================
-- 3.5. 유지보수 — 보존 정책 (2026-08-10 결정)
--
-- sensor_reading(+cascade로 sensor_value) 만 대상 — 1Hz로 계속 쌓이는
-- "blackbox"성 원시 시계열이라 30일 지나면 지운다. fire_event/
-- fire_event_command/button_event/manual_command/fire_threshold 는 하루
-- 몇 건 수준의 감사·이력 기록이라 의도적으로 삭제 대상에서 제외한다
-- (fire_threshold는 특히 "그 시점에 어떤 임계값이었는지" 역추적용이라
-- 지우면 존재 목적이 깨진다). 카메라 스키마(schema.sql)의 guardx_maintain()과
-- 같은 cron(매일 00:05)에 같이 태우되, 함수 자체는 분리한다 — 이 파일이
-- "기존 12테이블을 건드리지 않는 독립 파일" 원칙을 갖고 있어서다.
-- ============================================================================
CREATE FUNCTION guardx_fire_maintain(p_sensor_reading_days INT DEFAULT 30)
RETURNS TEXT LANGUAGE plpgsql AS $$
DECLARE
    n_reading bigint;
BEGIN
    DELETE FROM sensor_reading
        WHERE received_at < now() - make_interval(days => p_sensor_reading_days);
    GET DIAGNOSTICS n_reading = ROW_COUNT;

    RETURN format('sensor_reading -%s (sensor_value는 CASCADE로 함께 정리됨)',
                  n_reading);
END $$;


-- ============================================================================
-- 4. 시드 데이터 — 마스터 + 임계값 초기치
-- ============================================================================

-- 측정 채널 6종 (channel_key = RPi A json_builder.c values 키와 일치)
INSERT INTO sensor_channel (channel_id, channel_key, device, unit, value_kind, raw_min, raw_max) VALUES
    (1, 'gas_raw',        'MQ-2',     'adc', 'raw',      0, 1023),
    (2, 'spark_raw',      'TS0226',   'adc', 'raw',      0, 1023),
    (3, 'temperature',    'SHT30',    '°C',  'physical', NULL, NULL),
    (4, 'humidity',       'SHT30',    '%',   'physical', NULL, NULL),
    (5, 'irtemp_ambient', 'MLX90614', '°C',  'physical', NULL, NULL),
    (6, 'irtemp_object',  'MLX90614', '°C',  'physical', NULL, NULL);

-- 제어 명령 카탈로그 (command_key = guardx_protocol.h GUARDX_CMD_* 와 일치)
INSERT INTO actuator_command (command_id, command_key, actuator, kind) VALUES
    (1, 'servo_1',    '가스밸브',                  'set'),
    (2, 'shutter',    '화재셔터(28BYJ-48)',        'both'),  -- action은 OPEN/CLOSE/STOP (SET 아님)
    (3, 'fan',        '배연팬',                    'both'),
    (4, 'water_pump', '소화펌프',                  'onoff'),
    (5, 'sound',      'I2S 경보음(MAX98357A)',     'both'),
    -- fan_auto 는 값이 없다(켜고 끄기뿐). SET 을 열어두면 set_actuator 는
    -- 통과시키는데 RPi C 가 거절해 "받아들여졌는데 아무 일도 안 나는" 구간이 생긴다.
    (6, 'fan_auto',   '배연팬 자동 제어(혼잡 단계 연동)', 'onoff');
-- led_matrix(구 command_id 7)는 뺐다 - STM32 브릿지가 구현되지 않아 명령을
-- 받아도 무시만 했다. LED 매트릭스 표시 기능(guardx/display/rpic/...)은
-- 액추에이터 명령과 무관한 별도 경로이므로 그대로 돈다.

-- 화재 구역 시드 - 지금 하드웨어는 이 1행뿐. 나중에 zone 2가 실제로 생기면
-- 코드 변경 없이 이 표에 INSERT 한 행 + 그 RPi A/C의 config.env에
-- NODE_ID=rpia-2/rpic-2 설정만으로 확장된다(rpib_engine이 재시작 또는
-- guardx/config/rpib 신호로 다시 읽음 - zone_loader.c 참조).
INSERT INTO fire_zone (zone_id, zone_name, rpia_node_id, rpic_node_id) VALUES
    (1, '1구역', 'rpia', 'rpic');

-- 임계값 초기 행 : PHASE 2 decision.h 잠정치를 그대로 계승. is_active=true 한 행.
INSERT INTO fire_threshold (
    gas_raw_min, gas_raw_max, spark_raw_safe, spark_raw_danger,
    temp_min_c, temp_max_c, humi_safe_percent, humi_danger_percent,
    irtemp_min_c, irtemp_max_c,
    weight_gas, weight_spark, weight_temp, weight_humi, weight_irtemp,
    fire_score_threshold, n_confirm, n_recover, freeze_relax_cycles,
    min_valid_weight, override_spark_score, override_irtemp_score,
    is_active, updated_by
) VALUES (
    650.0, 800.0, 850.0, 30.0,
    35.0, 60.0, 50.0, 15.0,
    40.0, 80.0,
    0.20, 0.35, 0.15, 0.05, 0.25,
    65.0, 3, 10, 60,
    0.50, 70.0, 70.0,
    TRUE, 'default (decision.h PHASE 2 계승, 가스/스파크 실측 반영 2026-07-31)'
);


-- ============================================================================
-- 5. 권한 (기존 schema.sql과 동일한 guardx_writer / guardx_reader 모델)
--    이 파일 실행 이후 새로 추가되는 테이블에도 적용되도록 DEFAULT PRIVILEGES 포함
-- ============================================================================

GRANT USAGE ON SCHEMA public TO guardx_writer, guardx_reader;

GRANT INSERT, UPDATE, SELECT ON
    sensor_channel, actuator_command, fire_threshold, fire_zone,
    sensor_reading, sensor_value, fire_event, fire_event_command,
    button_event, manual_command
    TO guardx_writer;
-- 시퀀스 권한: SERIAL 컬럼에 INSERT하려면 테이블 INSERT 권한과 별개로
-- 시퀀스 nextval() = USAGE 권한이 필요하다. 이게 빠지면 SELECT/UPDATE는
-- 되는데 INSERT만 "permission denied for sequence"로 죽어서 원인을 찾기 어렵다.
--
-- "ON ALL SEQUENCES IN SCHEMA public"을 쓰지 않는다 - 같은 public 스키마에
-- 기존 카메라 스키마(schema.sql)의 시퀀스들이 함께 있고 그건 guardx_admin
-- 소유가 아니라서, 남의 시퀀스에서 permission denied로 통째로 실패한다
-- (이 파일의 "기존 12테이블을 건드리지 않는다" 원칙과도 어긋남).
-- 우리 9테이블 중 SERIAL/BIGSERIAL을 쓰는 5개만 명시한다.
GRANT USAGE, SELECT ON SEQUENCE
    fire_threshold_threshold_id_seq,
    sensor_reading_reading_id_seq,
    fire_event_event_id_seq,
    button_event_button_event_id_seq,
    manual_command_manual_command_id_seq
    TO guardx_writer;

GRANT SELECT ON
    sensor_channel, actuator_command, fire_threshold, fire_zone,
    sensor_reading, sensor_value, fire_event, fire_event_command,
    button_event, manual_command
    TO guardx_reader;

ALTER DEFAULT PRIVILEGES IN SCHEMA public
    GRANT INSERT, UPDATE, SELECT ON TABLES TO guardx_writer;
ALTER DEFAULT PRIVILEGES IN SCHEMA public
    GRANT SELECT ON TABLES TO guardx_reader;

-- 시퀀스도 반드시 같이 걸어야 한다. SERIAL 컬럼에 INSERT하려면 테이블
-- INSERT 권한과 별개로 시퀀스 nextval() = USAGE 권한이 필요해서, 이게
-- 빠지면 SELECT/UPDATE는 되는데 INSERT만 "permission denied for sequence"로
-- 죽는다 (테이블 권한만 보고 있으면 원인을 찾기 어려운 유형).
ALTER DEFAULT PRIVILEGES IN SCHEMA public
    GRANT USAGE, SELECT ON SEQUENCES TO guardx_writer;


-- ============================================================================
-- 6. 설치 확인
-- ============================================================================

-- 테이블 10개가 나와야 정상
SELECT table_name FROM information_schema.tables
WHERE table_schema = 'public'
  AND table_name IN ('sensor_channel', 'actuator_command', 'fire_threshold', 'fire_zone',
                     'sensor_reading', 'sensor_value', 'fire_event',
                     'fire_event_command', 'button_event', 'manual_command')
ORDER BY table_name;

-- 시드 확인 (6 / 6 / 1 / 1 이 나와야 정상)
SELECT 'sensor_channel'   AS tbl, count(*) FROM sensor_channel
UNION ALL
SELECT 'actuator_command',       count(*) FROM actuator_command
UNION ALL
SELECT 'fire_zone',              count(*) FROM fire_zone
UNION ALL
SELECT 'fire_threshold (active)', count(*) FROM fire_threshold WHERE is_active;
