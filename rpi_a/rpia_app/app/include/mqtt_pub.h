#ifndef MQTT_PUB_H
#define MQTT_PUB_H

#include "guardx_err.h"
#include "guardx_protocol.h"   /* 공통 토픽/QoS 정의 (common/include/) */

/*
 * MQTT 발행 wrapper (libmosquitto 기반).
 * MQTT 접속 설정
 *
 * 토픽 문자열은 guardx_protocol.h의 GUARDX_TOPIC_*_FMT + GUARDX_NODE_RPIA
 * 조합으로 mqtt_pub.c에서 런타임에 snprintf 조립한다. 다른 노드(B/C)
 * 코드도 같은 패턴을 쓰게 될 것이므로 여기서 미리 통일해둔다.
 */

/* 브로커(RPi B) 접속 정보.
 * >>> 실제 IP로 교체해서 쓸 것 (RPi B의 hostname -I 결과) <<<
 *
 * mTLS 활성화됨 (2025-XX). RPi A/B/C 각각 common/certs/gen_certs.sh로
 * 발급한 CA/노드 인증서를 /etc/guardx/certs/에 배치해야 함.
 * 인증서 미배치 상태에서 그냥 실행하면 mosquitto_tls_set 단계에서
 * 파일을 못 찾아 mqtt_pub_init()이 GUARDX_ERR_OPEN을 반환하고 종료됨. */
/* 아래 두 개는 로컬 테스트 시 빌드 플래그로 덮어쓸 수 있게 #ifndef 가드.
 * 예) make EXTRA_CFLAGS='-DMQTT_USE_TLS=0 -DMQTT_BROKER_HOST=\"localhost\"'
 * 프로덕션(RPi B) 기본값은 그대로 유지된다. */
#ifndef MQTT_USE_TLS
#define MQTT_USE_TLS        1
#endif
#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST    "172.20.33.251" //RPIB의 IP
#endif
#define MQTT_BROKER_PORT    (MQTT_USE_TLS ? 8883 : 1883)
#define MQTT_TLS_CA_PATH    "/etc/guardx/certs/ca.crt"
#define MQTT_TLS_CERT_PATH  "/etc/guardx/certs/rpia.crt"
#define MQTT_TLS_KEY_PATH   "/etc/guardx/certs/rpia.key"

guardx_err_t mqtt_pub_init(void);
guardx_err_t mqtt_pub_sensor(const char *json, int len);   /* GUARDX_QOS_SENSOR */
guardx_err_t mqtt_pub_button(const char *json, int len);   /* GUARDX_QOS_BUTTON */
void         mqtt_pub_cleanup(void);

#endif /* MQTT_PUB_H */
