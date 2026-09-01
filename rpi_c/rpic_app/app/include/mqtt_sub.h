#ifndef MQTT_SUB_H
#define MQTT_SUB_H

#include "guardx_err.h"
#include "guardx_protocol.h"   /* 공통 토픽/QoS 정의 (common/include/) */

/*
 * MQTT 구독 wrapper (libmosquitto 기반).
 *
 * RPi A mqtt_pub와 대칭 구조: 토픽 문자열은 guardx_protocol.h의
 * GUARDX_TOPIC_*_FMT + GUARDX_NODE_RPIC 조합으로 mqtt_sub.c에서
 * 런타임에 snprintf 조립한다.
 */

/* 브로커(RPi B) 접속 정보.
 * 첫 브링업이라 평문/localhost 기본값으로 시작한다 (test/02 브로커와
 * 맞물림). RPi B 실기 연동 시:
 *   1) MQTT_USE_TLS 1로 변경
 *   2) MQTT_BROKER_HOST를 RPi B 실제 IP로 교체 (hostname -I 결과)
 *   3) common/certs/gen_certs.sh로 발급한 인증서를 /etc/guardx/certs/에
 *      배치 (CN=rpic). 미배치 상태로 실행하면 mosquitto_tls_set에서
 *      파일을 못 찾아 mqtt_sub_init()이 GUARDX_ERR_OPEN 반환하고 종료됨.
 * (RPi A mqtt_pub.h는 이미 TLS=1 상태 - 배포 시 여기도 맞출 것) */
#define MQTT_USE_TLS        1
#define MQTT_BROKER_HOST    "172.20.33.251" //RPIB의 IP
#define MQTT_BROKER_PORT    (MQTT_USE_TLS ? 8883 : 1883)
#define MQTT_TLS_CA_PATH    "/etc/guardx/certs/ca.crt"
#define MQTT_TLS_CERT_PATH  "/etc/guardx/certs/rpic.crt"
#define MQTT_TLS_KEY_PATH   "/etc/guardx/certs/rpic.key"

/* ACK 발행 스위치. VMS DEVICE CONTROL의 액추에이터 상태 라벨이 ACK 없이는
 * "—"(상태 미상)로만 뜬다(device_control_page.cpp state_label 주석) — 켠다. */
#define RPIC_ENABLE_ACK     1

/* 명령 수신 콜백. payload는 NUL 종료 보장 안 됨 - len 기준으로 다룰 것.
 * mosquitto 내부 네트워크 스레드에서 호출된다 (main 스레드 아님). */
typedef void (*mqtt_cmd_handler_t)(const char *payload, int len);

guardx_err_t mqtt_sub_init(mqtt_cmd_handler_t handler);
void         mqtt_sub_cleanup(void);

#if RPIC_ENABLE_ACK
guardx_err_t mqtt_sub_publish_ack(const char *json, int len);  /* GUARDX_QOS_ACTUATOR_ACK */
#endif

/* 임의 토픽 발행 (QoS1, retain 없음).
 * 방송 READY ACK 처럼 액추에이터 ACK 토픽에 실을 수 없는 응답에 쓴다.
 * 브로커 미연결이면 GUARDX_ERR_WRITE. */
guardx_err_t mqtt_sub_publish(const char *topic, const char *json, int len);

/* 임의 토픽 발행 (QoS1, **retain**).
 * 방송 점유 상태처럼 "지금 값이 얼마인가"에 답해야 하는 토픽에 쓴다 -
 * 나중에 켜진 VMS 도 구독 즉시 현재 소유자를 받아야 한다. */
guardx_err_t mqtt_sub_publish_retained(const char *topic, const char *json,
                                       int len);

#endif /* MQTT_SUB_H */
