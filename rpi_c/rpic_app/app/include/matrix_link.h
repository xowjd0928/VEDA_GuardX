/*
 * matrix_link.h - LED 매트릭스 표시 데이터 MQTT 수신 -> STM32(Modbus)
 *
 * 경로:  RPi B -> 여기(RPi C) -> Modbus RTU -> STM32 -> HUB75 평면도 화면
 *
 * 지금 세 종류를 나른다. 셋 다 레지스터 구간이 겹치지 않는다:
 *
 *   guardx/display/rpic/track     거수자 추적 좌표   -> 121~125
 *   guardx/display/rpic/fire      화재 Zone bitmap   -> 120
 *   guardx/display/rpic/zones/N   Zone N 온습도      -> 100~107 중 2칸
 *
 * RPi C는 아무것도 계산하지 않는다. RPi B가 이미 LED가 쓰는 단위(평면도
 * 0~1000 좌표, bitmap, 0.1℃ 정수)로 변환해 보내므로 이 모듈은 "검증하고
 * 레지스터에 쓴다"만 한다. 판단을 여기 두면 규약이 두 곳으로 갈라진다.
 *
 * ── 스레드 ──
 * mqtt_sub의 콜백 스레드가 matrix_link_handle_*()로 값을 넣고, 전용 워커
 * 스레드 하나가 Modbus 트랜잭션을 수행한다. 콜백 스레드에서 직접 쓰면
 * 최악 600ms(타임아웃 200ms x 재시도 2회) 동안 방송 오디오 패킷 처리가
 * 함께 멈춘다 - 그래서 스레드를 나눈다.
 *
 * 워커가 하나인 것은 시리얼 포트를 한 번에 한 트랜잭션만 쓸 수 있기
 * 때문이다. 표시 종류마다 스레드를 따로 두면 프레임이 섞인다.
 *
 * ── 우편함 ──
 * 표시 종류마다 한 칸짜리 우편함을 따로 둔다(추적 1 + 화재 1 + zone 4).
 * 칸을 공유하면 서로 다른 레지스터 구간인데도 나중 값이 앞 값을 덮어
 * 유실시킨다 - 추적 좌표가 막 도착했는데 화재 상태가 뒤이어 오면 좌표
 * 쓰기가 통째로 사라지는 식이다.
 *
 * 같은 칸 안에서는 최신 값이 이긴다(latest-wins). 워커가 밀리는 동안
 * 쌓인 옛 좌표를 나중에 순서대로 그려봐야 의미가 없고, 큐가 길어지면
 * 화면이 실제보다 뒤처진 위치를 보여주기 때문이다.
 *
 * 워커는 칸들을 라운드로빈으로 훑는다. 한 칸이 계속 갱신될 때 다른 칸이
 * 굶지 않게 하려는 것이다(1Hz 온습도가 2초 주기 좌표를 밀어내면 안 된다).
 *
 * 시리얼 포트는 한 프로세스만 열 수 있다. modbus_test CLI를 쓰려면 이
 * 모듈을 꺼야 한다 - GUARDX_MATRIX_DEV=off 로 기동하면 구독만 하고
 * 포트를 잡지 않는다.
 *
 * ── 수신 페이로드 ──
 *
 * (1) guardx/display/rpic/track
 *   {"node_id":"rpib","timestamp":1234567890,"seq":42,
 *    "status":3,"ax":420,"ay":180,"bx":510,"by":205}
 *
 *   status : bit0 = 현재점 A 유효, bit1 = 예측점 B 유효 (0 = 두 점 제거)
 *   ax,ay  : 현재점 A 좌표 0~1000
 *   bx,by  : 예측점 B 좌표 0~1000
 *
 *   status가 0이 아니면 STM32가 10초 안에 다음 묶음을 못 받을 때 스스로
 *   점을 지운다(GX_INTRUDER_TIMEOUT_MS). 발행이 끊겨도 화면에 옛 좌표가
 *   영원히 남지 않는다는 뜻이라, 여기서 따로 만료를 걸지 않는다.
 *
 * (2) guardx/display/rpic/fire
 *   {"node_id":"rpib","timestamp":1234567890,"seq":7,"zone_bitmap":5}
 *
 *   zone_bitmap : bit0~3 = Zone1~4 화재중 (0 = 전 구역 정상)
 *
 *   추적과 달리 STM32에 만료 타이머가 없다. 즉 RPi B가 죽으면 마지막
 *   화재 표시가 화면에 남는다 - 의도한 방향이다. 경보를 "발신자가 조용해
 *   졌다"는 이유로 저절로 내리는 것이 더 위험하기 때문이다. 대신 발행이
 *   retained라, RPi C가 재접속하면 브로커가 최신 상태를 바로 물려준다.
 *
 * (3) guardx/display/rpic/zones/N   (N = 1~4, 토픽마다 따로 retained)
 *   {"node_id":"rpib","timestamp":1234567890,"seq":8,
 *    "zone_id":1,"temp_x10":235,"humidity":45}
 *
 *   temp_x10 : 섭씨 x10 (235 = 23.5℃), 0~65534
 *   humidity : %RH 정수, 0~100
 *
 *   zone마다 토픽을 나눈 이유는 retain 때문이다. 한 토픽을 네 zone이
 *   공유하면 브로커가 마지막 한 건만 보관해, 재접속한 RPi C가 나머지
 *   세 zone의 값을 영영 못 받는다.
 *
 *   대상 zone은 토픽이 아니라 payload의 zone_id로 정한다(토픽 문자열을
 *   파싱하지 않는다 - 라우팅과 의미를 한 곳에 두는 편이 읽기 쉽다).
 *
 *   !!! 유효할 때만 온다 !!! 센서가 죽어도 "없음"을 보내오지 않는다.
 *   그 상태를 나타내는 0xFFFF/0x00FF가 쓰기 허용 범위 밖이라 Modbus로
 *   되돌릴 방법이 없기 때문이다(guardx_modbus_regs.h 참조). 따라서
 *   센서가 죽으면 LED에는 마지막 정상값이 그대로 남는다. 고치려면 STM32
 *   펌웨어에 추적 좌표와 같은 만료 타이머가 필요하다 - 지금 범위 밖.
 */
#ifndef MATRIX_LINK_H
#define MATRIX_LINK_H

#include "guardx_err.h"

/* RPi B -> RPi C 표시 토픽.
 *
 * 발신측 상수와 같은 문자열이어야 한다. 양쪽이 자기 상수로 들고 있는
 * 것은 액추에이터 토픽과 같은 관례다 - 이 파일은 rpi_b 헤더를 include
 * 하지 않는다(빌드 의존 최소화). 바뀌면 양쪽을 함께 고칠 것.
 *   track       : rpi_b/src/MqttDb/task_track_display.cpp   (guardx_mqttd)
 *   fire, zones : rpi_b/rpib_decision/app/src/main.c        (rpib_decision)
 */
#define GUARDX_TOPIC_MATRIX_TRACK  "guardx/display/rpic/track"
#define GUARDX_TOPIC_MATRIX_FIRE   "guardx/display/rpic/fire"

/* zone별 토픽. %d 자리에 zone_id(1~GX_ZONE_COUNT). 와일드카드로 구독하지
 * 않고 네 개를 그대로 건다 - zone 개수가 레지스터 맵에 고정돼 있어 늘 4개고,
 * 와일드카드는 예상 밖 하위 토픽까지 받아 사고가 난 전례가 있다
 * (guardx/actuator/rpic/# 가 자기 ACK를 되받아 무한 루프). */
#define GUARDX_TOPIC_MATRIX_ZONE_FMT "guardx/display/rpic/zones/%d"

/* 표시 데이터는 상태다 - 유실되면 다음 갱신까지 화면이 틀린 값을 보여준다.
 * 좌표만은 흐름에 가깝지만, 마지막 "지우기"(status=0)를 흘리면 점이 10초간
 * 남으므로 여기서도 QoS 0은 쓰지 않는다. */
#define GUARDX_QOS_MATRIX_TRACK    1
#define GUARDX_QOS_MATRIX_FIRE     1
#define GUARDX_QOS_MATRIX_ZONE     1

/*
 * 시리얼 포트를 열고 워커 스레드를 띄운다.
 *
 * 실패해도 앱 기동을 막지 않는 부가기능이다(방송 오디오와 같은 정책).
 * 실패 시 GUARDX_ERR_OPEN을 반환하지만 matrix_link_handle_*()은 계속
 * 호출해도 안전하다 - 조용히 버린다.
 *
 * 장치는 환경변수 GUARDX_MATRIX_DEV로 바꾼다. 값이 "off"면 포트를 열지
 * 않고 비활성으로 시작한다(modbus_test CLI와 포트를 다투지 않기 위함).
 */
guardx_err_t matrix_link_init(void);

/* 수신한 페이로드 한 건 처리 (mosquitto 콜백 스레드에서 호출).
 * NUL 종료 보장이 없는 원본 payload를 그대로 받는다. 파싱/범위 실패는
 * 로그만 남기고 버린다 - 잘못된 한 건이 프로세스를 세우면 안 된다. */
void matrix_link_handle_track(const char *payload, int len);
void matrix_link_handle_fire(const char *payload, int len);
void matrix_link_handle_zones(const char *payload, int len);

/* 워커 정지 + 시리얼 반납. mqtt_sub_cleanup() 뒤에 부를 것 - 콜백 스레드가
 * 살아 있는 동안 부르면 닫는 중인 우편함에 값을 넣는 경합이 생긴다. */
void matrix_link_cleanup(void);

#endif /* MATRIX_LINK_H */
