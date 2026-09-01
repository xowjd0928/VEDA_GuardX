#ifndef GUARDX_PROTOCOL_H
#define GUARDX_PROTOCOL_H

/*
 * guardx_protocol.h - GuardX 3노드(RPi A/B/C) 공통 MQTT 프로토콜 정의
 *
 * 목적: 토픽 문자열/QoS를 각 노드 코드에 따로 하드코딩하지 않고
 * 이 헤더 하나로 통일한다. 노드별 코드는 이 파일을 include해서
 * 매크로만 쓰고, 토픽 구조가 바뀌면 여기 한 곳만 고치면 된다.
 *
 * !!! 미확정 상태 !!!
 * 아래 값들은 현재 RPi A 구현(guardx/sensor/rpia)을 기준으로 확장한
 * 초안이다. 팀 합의 전까지는 잠정치로 취급할 것. 특히:
 *   - 카메라 토픽/QoS: 카메라 담당자 확인 대기
 *   - 액추에이터 ACK 토픽 필요 여부: 미정 (일단 정의만 해두고 미사용)
 *
 * 원칙 (기존 설계 문서 계승):
 *   - 디바이스 인스턴스가 항상 1개인 노드는 토픽에 인스턴스 번호 없음
 *   - QoS는 "정확히 1회 전달이 필요한가(로그/이벤트)" vs
 *     "유실 허용/중복 허용되는 멱등 동작인가"로 결정
 */

/* ---------------------------------------------------------------------
 * 노드 ID (client id 겸용, 인증서 CN과도 일치시킬 것)
 * --------------------------------------------------------------------- */
#define GUARDX_NODE_RPIA   "rpia"
#define GUARDX_NODE_RPIB   "rpib"
#define GUARDX_NODE_RPIC   "rpic"

/* ---------------------------------------------------------------------
 * 토픽 (printf 스타일 포맷, %s 자리에 GUARDX_NODE_* 삽입)
 * --------------------------------------------------------------------- */

/* 센서 노드(RPi A) -> RPi B, 1Hz 주기 발행 */
#define GUARDX_TOPIC_SENSOR_FMT      "guardx/sensor/%s"
#define GUARDX_QOS_SENSOR            0   /* 유실 허용, 다음 주기에 새 값 옴 */

/* 비상 버튼 로깅(RPi A) -> RPi B. 제어 자체는 하드웨어 인터락으로
 * 별도 처리되며, 이 토픽은 "로그 정확히 1회 기록"용이다. */
#define GUARDX_TOPIC_BUTTON_FMT      "guardx/sensor/%s/button"
#define GUARDX_QOS_BUTTON            2   /* 로그 중복 방지 */

/* 카메라 -> RPi B, DETECTIONS 테이블 연계.
 * !!! 카메라 담당자 확인 전까지 잠정치 !!! */
#define GUARDX_TOPIC_CAMERA_FMT      "guardx/camera/%s"
#define GUARDX_QOS_CAMERA            0   /* TODO: 프레임 단위면 0, 이벤트 단위면 1로 재검토 */

/* RPi B -> 액추에이터 노드(RPi C), 제어 명령.
 * 멱등 동작(ON/OFF 등) 기준 QoS1: 유실 안 됨, 중복은 허용. */
#define GUARDX_TOPIC_ACTUATOR_FMT    "guardx/actuator/%s"
#define GUARDX_QOS_ACTUATOR          1

/* 액추에이터 -> RPi B, 명령 수신/실행 확인 (선택 사항, 아직 미사용).
 * 필요해지면 QoS는 ACTUATOR와 동일하게 1로 시작 권장. */
#define GUARDX_TOPIC_ACTUATOR_ACK_FMT "guardx/actuator/%s/ack"
#define GUARDX_QOS_ACTUATOR_ACK      1

/* 노드 생존 신호(LWT). 접속 시 온라인 payload를 retain 발행하고,
 * mosquitto_will_set으로 등록해 둔 오프라인 payload는 접속이 뚝 끊기면
 * (정상 종료든 전원 차단이든) 브로커가 대신 발행한다 - 노드 쪽 코드가
 * "나 죽는다"를 스스로 보낼 필요가 없다. retain=true라 VMS가 나중에
 * 구독해도 마지막 상태를 바로 받는다. */
#define GUARDX_TOPIC_STATUS_FMT      "guardx/status/%s"
#define GUARDX_QOS_STATUS            1
#define GUARDX_STATUS_ONLINE         "online"
#define GUARDX_STATUS_OFFLINE        "offline"

/* 설정(fire_threshold) 변경 신호. DB에 새 활성 행을 넣은 뒤 이 토픽에
 * 아무 payload나 발행하면 대상 노드가 즉시 재조회한다(재시작 불필요).
 * payload 내용 자체는 쓰지 않는다 - "값을 실어 보낸다"가 아니라
 * "다시 읽어라"는 신호이므로, 신호와 실제 값이 따로 노는 상황(신호는
 * 갔는데 옛 값을 실어 보내는 등)이 구조적으로 생기지 않는다. */
#define GUARDX_TOPIC_CONFIG_FMT      "guardx/config/%s"
#define GUARDX_QOS_CONFIG            1   /* 신호 유실 시 다음 재기동까지 갱신 안 되므로 신뢰성 필요 */

/* 수동 화재 해제 명령 (VMS -> RPi B 판단 엔진). PHASE 7에서 자동 해제를
 * 없애면서 신설 - 화재 상태를 NORMAL로 되돌리는 유일한 경로다.
 *   payload: {"node_id":"vms","timestamp":<ms>,"zone_id":<int>}
 * zone_id로 어느 zone을 해제할지 지정한다(그 zone이 FIRE가 아니면 무시).
 *
 * config 신호와 같은 자리에 두는 이유: 둘 다 "운영자가 판단 엔진에 직접
 * 내리는 지시"라 성격이 같다. guardx/db/rpib/cmd/… 계열(set_zone 등)은
 * guardx_mqttd가 DB를 검증·기록해야 하는 명령이라 그쪽에 두지만, 화재
 * 해제는 판정 상태(rpib_engine 메모리)를 바꾸는 것이라 DB 검증이 낄
 * 자리가 없다 - 중계 프로세스를 하나 더 거칠 이유가 없다.
 * 감사 기록은 해제 결과로 남는 fire_event('recovered') 행이 담당한다. */
#define GUARDX_TOPIC_CLEAR_FIRE_FMT  "guardx/cmd/%s/clear_fire"
#define GUARDX_QOS_CLEAR_FIRE        1   /* 유실되면 화재가 안 꺼진다 - 신뢰성 필요 */

/* ---------------------------------------------------------------------
 * 공통 payload envelope 필드명 (JSON 키 이름 통일)
 * 모든 발행 메시지는 최소한 이 3개 필드를 갖는다:
 *   node_id   : 위 GUARDX_NODE_* 중 하나
 *   timestamp : epoch 밀리초 (RPi 시각 동기화 필요 - NTP 전제)
 *   seq       : 발행 프로세스 기준 단조 증가 (재시작 시 리셋 허용)
 * --------------------------------------------------------------------- */
#define GUARDX_JSON_KEY_NODE_ID   "node_id"
#define GUARDX_JSON_KEY_TIMESTAMP "timestamp"
#define GUARDX_JSON_KEY_SEQ       "seq"

/* ---------------------------------------------------------------------
 * 액추에이터 제어 명령 payload 규격 (RPi B -> RPi C)
 *
 * 이진 제어 (LED, 워터펌프, AMP mute 등):
 * {
 *   "node_id": "rpic",
 *   "timestamp": 1752300000123,
 *   "seq": 42,
 *   "command": "water_pump",
 *   "action": "ON"          // "ON" | "OFF"
 * }
 *
 * 숫자값 제어 (서보 각도, 팬 속도 등):
 * {
 *   "node_id": "rpic",
 *   "timestamp": 1752300000123,
 *   "seq": 43,
 *   "command": "servo_1",
 *   "action": "SET",
 *   "value": 90              // 의미는 command마다 다름 (아래 표 참조)
 * }
 *
 * command 이름은 실제 액추에이터 확정 목록(아래) 기준. 값의 의미:
 *   led          : ON/OFF만 (미배선)
 *   water_pump   : ON/OFF만
 *   sound        : SET(value=0 기본/1 화재/2 강도/3 비상) 또는 ON(기본음).
 *                  MAX98357A(I2S)로 rpic_audio가 상황음 재생 (구 amp 명령 대체 -
 *                  MAX98357A는 상시 ON이라 전원 제어 자체가 불필요해짐)
 *   fan          : ON/OFF 또는 SET(value=0~100, PWM 듀티 %)
 *   servo_1      : SET(value=0~180, 각도) - 가스밸브 (구 servo_2는 삭제됨)
 *   shutter      : OPEN/CLOSE/STOP 동사형 명령만 받는다(SET 아님). 실제
 *                  모터는 28BYJ-48 연속회전 스텝모터지만, 방향별 리밋
 *                  리드센서 자동 정지를 RPi C 스텝모터 드라이버가 IRQ로
 *                  직접 처리하므로(App 개입 없음) B는 방향값을 몰라도 됨.
 *                  화재셔터 구동 (구 stepper/SET(부호=방향) 명령 대체).
 *
 * led_matrix 액추에이터 명령은 삭제했다(브릿지가 끝내 구현되지 않았고
 * 받아도 무시만 했다). LED 매트릭스 자체는 **표시 경로**로 계속 쓴다 -
 * guardx/display/rpic/{track,fire,zones/N} -> RPi C matrix_link ->
 * Modbus -> STM32. 그쪽은 이 액추에이터 규약과 무관하다.
 * --------------------------------------------------------------------- */
#define GUARDX_JSON_KEY_COMMAND "command"
#define GUARDX_JSON_KEY_ACTION  "action"
#define GUARDX_JSON_KEY_VALUE   "value"
#define GUARDX_ACTION_ON        "ON"
#define GUARDX_ACTION_OFF       "OFF"
#define GUARDX_ACTION_SET       "SET"    /* value 필드 동반 */
#define GUARDX_ACTION_OPEN      "OPEN"   /* shutter 전용 */
#define GUARDX_ACTION_CLOSE     "CLOSE"  /* shutter 전용 */
#define GUARDX_ACTION_STOP      "STOP"   /* shutter 전용 */

/* 확정된 액추에이터 command 이름 (2025-XX 기준, 실물 배선 전 확정)
 * !!! 배선 확정 후 GPIO/PWM 채널 번호는 rpic 드라이버 헤더에서 정의 !!! */
#define GUARDX_CMD_LED         "led"
#define GUARDX_CMD_WATER_PUMP  "water_pump"
#define GUARDX_CMD_SOUND       "sound"
#define GUARDX_CMD_FAN         "fan"
#define GUARDX_CMD_SERVO_1     "servo_1"      /* 가스밸브 */
#define GUARDX_CMD_SHUTTER     "shutter"      /* 화재셔터, action=OPEN/CLOSE/STOP (구 GUARDX_CMD_STEPPER 대체) */

#endif /* GUARDX_PROTOCOL_H */
