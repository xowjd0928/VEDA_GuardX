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
 *   led          : ON/OFF만
 *   water_pump   : ON/OFF만
 *   amp          : ON/OFF만 (오디오 신호 자체가 아니라 전원/뮤트 핀 제어.
 *                  실제 재생 콘텐츠는 이 프로토콜 범위 밖, I2S/ALSA 별도 경로)
 *   fan          : ON/OFF 또는 SET(value=0~100, PWM 듀티 %)
 *   servo_1      : SET(value=0~180, 각도)
 *   servo_2      : SET(value=0~180, 각도)
 *   stepper      : SET(value=회전 각도(도), 부호=방향. +=CW, -=CCW,
 *                  범위 -360~360. 상대 회전 후 코일 해제.
 *                  !!! 잠정 규격 - RPi B와 최종 합의 필요 !!!)
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
#define GUARDX_ACTION_SET       "SET"   /* value 필드 동반 */

/* 확정된 액추에이터 command 이름 (2025-XX 기준, 실물 배선 전 확정)
 * !!! 배선 확정 후 GPIO/PWM 채널 번호는 rpic 드라이버 헤더에서 정의 !!! */
#define GUARDX_CMD_LED         "led"
#define GUARDX_CMD_WATER_PUMP  "water_pump"
#define GUARDX_CMD_SOUND       "sound"
#define GUARDX_CMD_FAN         "fan"
#define GUARDX_CMD_SERVO_1     "servo_1"
#define GUARDX_CMD_STEPPER     "stepper"      /* 28BYJ-48, 연속 회전(부호=방향, 0=정지) */

#endif /* GUARDX_PROTOCOL_H */
