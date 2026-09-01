/*
 * mqtt_sub.c - libmosquitto 기반 MQTT 구독 (액추에이터 명령 수신)
 *
 * QoS 정책 (프로토콜 규약 2-1):
 *  - 액추에이터 명령: QoS 1. ON/OFF/SET은 멱등이라 중복 수신은
 *    허용하고(같은 명령 두 번 = 같은 결과), 유실만 막는다.
 *    따라서 seq 기반 중복 제거는 하지 않는다.
 *
 * 구독은 on_connect 콜백 안에서 수행한다 - libmosquitto가 재접속하면
 * on_connect가 다시 불려 자동 재구독되므로, 브로커(RPi B)가 재시작해도
 * 별도 복구 로직이 필요 없다.
 *
 * mosquitto_loop_start()로 내부 네트워크 스레드를 돌리므로 메시지
 * 콜백(=액추에이터 구동)은 그 스레드에서 실행된다. 콜백 스레드는
 * 하나뿐이라 명령 처리 자체는 직렬화되어 있다.
 */

#include <stdio.h>
#include <stdlib.h>   /* strtoul - 방송 명령의 session_id 파싱 */
#include <string.h>
#include <unistd.h>

/* Mosquitto 2.1 split the client API from broker/common headers. */
#if defined(__has_include)
#  if __has_include(<mosquitto/libmosquitto.h>)
#    include <mosquitto/libmosquitto.h>
#  else
#    include <mosquitto.h>
#  endif
#else
#  include <mosquitto.h>
#endif

#include "audio_arbiter.h"
#include "audio_event.h"
#include "fan_control.h"
#include "fan_protocol.h"
#include "broadcast_protocol.h"
#include "guardx_modbus_regs.h"   /* GX_ZONE_COUNT - zone 토픽 개수 */
#include "matrix_link.h"
#include "mqtt_sub.h"

static struct mosquitto *mosq;
static mqtt_cmd_handler_t cmd_handler;
static int last_congestion_severity[GX_ZONE_COUNT];

/* guardx_protocol.h의 포맷 매크로 + 노드 ID로 조립한 구독 토픽.
 * 끝의 /#로 기존 기본 토픽과 fan 같은 액추에이터별 하위 토픽을
 * 모두 수신한다. */
static char topic_actuator[128];
static char topic_ack[128];
static char topic_status[128];

/* zone별 온습도 토픽(guardx/display/rpic/zones/1..4). 와일드카드 대신
 * 네 개를 그대로 조립해 두고 구독/라우팅 모두 이 배열로 처리한다 -
 * 이유는 matrix_link.h의 GUARDX_TOPIC_MATRIX_ZONE_FMT 주석 참조. */
static char topic_zone[GX_ZONE_COUNT][128];

static const char *mqtt_error_name(int rc)
{
    switch (rc) {
    case MOSQ_ERR_SUCCESS:      return "success";
    case MOSQ_ERR_NOMEM:        return "out of memory";
    case MOSQ_ERR_PROTOCOL:     return "protocol error";
    case MOSQ_ERR_INVAL:        return "invalid argument";
    case MOSQ_ERR_NO_CONN:      return "not connected";
    case MOSQ_ERR_CONN_REFUSED: return "connection refused";
    case MOSQ_ERR_CONN_LOST:    return "connection lost";
    case MOSQ_ERR_TLS:          return "TLS error";
    case MOSQ_ERR_PAYLOAD_SIZE: return "payload too large";
    case MOSQ_ERR_AUTH:         return "authentication failed";
    default:                    return "unknown MQTT error";
    }
}

static void on_connect(struct mosquitto *m, void *obj, int rc)
{
    int i;

    (void)obj;

    if (rc != 0) {
        fprintf(stderr, "mqtt: connect refused (rc=%d)\n", rc);
        return;
    }

    /* LWT로 등록한 offline과 짝이 되는 online — 접속될 때마다(첫 연결이든
     * 재접속이든) 다시 알린다. retain=true라 VMS는 구독 시점과 무관하게
     * 항상 최신 상태를 받는다. */
    rc = mosquitto_publish(m, NULL, topic_status,
                           (int)strlen(GUARDX_STATUS_ONLINE), GUARDX_STATUS_ONLINE,
                           GUARDX_QOS_STATUS, true);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: status online publish failed: %s\n",
                mqtt_error_name(rc));

    /* 재접속 때마다 여기로 돌아오므로 구독도 매번 다시 건다 */
    rc = mosquitto_subscribe(m, NULL, topic_actuator, GUARDX_QOS_ACTUATOR);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: subscribe failed: %s\n", mqtt_error_name(rc));
    else
        printf("mqtt: subscribed to %s (qos%d)\n",
               topic_actuator, GUARDX_QOS_ACTUATOR);

    /* 방송 명령만 구독한다. 오디오 스트림 토픽(MQTT/PCM)은 더 이상 쓰지
     * 않는다 - VMS 가 RTP 전용으로 바뀌면서 발행하는 쪽이 사라졌고, RPi C 가
     * 그걸 계속 듣고 자기도 ALSA 를 열면 조율기가 띄운 RTP 수신기와 스피커를
     * 두고 싸운다(Device or resource busy). */
    rc = mosquitto_subscribe(m, NULL, GUARDX_BROADCAST_COMMAND_TOPIC,
                             GUARDX_BROADCAST_QOS_COMMAND);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: broadcast command subscribe failed: %s\n",
                mqtt_error_name(rc));
    else
        printf("mqtt: subscribed to %s (qos%d)\n",
               GUARDX_BROADCAST_COMMAND_TOPIC, GUARDX_BROADCAST_QOS_COMMAND);

    /* 방송 점유 상태를 다시 실어 둔다. 끊겨 있는 동안 방송이 끝났다면
     * 브로커의 retained 값이 낡아 있고, 그걸 그대로 두면 VMS 들이 이미
     * 끝난 방송의 소유자를 계속 믿어 아무도 방송을 못 건다. */
    audio_arbiter_republish_state();

    /* 혼잡 단계(RPi B -> 팬). retained 라 구독 즉시 현재 단계가 온다. */
    rc = mosquitto_subscribe(m, NULL, GUARDX_FAN_LEVEL_TOPIC,
                             GUARDX_FAN_LEVEL_QOS);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: fan level subscribe failed: %s\n",
                mqtt_error_name(rc));
    else
        printf("mqtt: subscribed to %s (qos%d)\n",
               GUARDX_FAN_LEVEL_TOPIC, GUARDX_FAN_LEVEL_QOS);

    /* VMS와 같은 혼잡 상태 전이 이벤트를 받아 critical 경고음을 맞춰 낸다. */
    rc = mosquitto_subscribe(m, NULL, GUARDX_CONGESTION_ALERT_TOPIC,
                             GUARDX_CONGESTION_ALERT_QOS);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: congestion alert subscribe failed: %s\n",
                mqtt_error_name(rc));
    else
        printf("mqtt: subscribed to %s (qos%d)\n",
               GUARDX_CONGESTION_ALERT_TOPIC,
               GUARDX_CONGESTION_ALERT_QOS);

    /* 팬 상태도 다시 실어 둔다 - 방송 점유와 같은 이유다. */
    fan_control_republish();

    /* 거수자 추적 좌표(RPi B -> LED 매트릭스). matrix_link가 비활성이어도
     * 구독은 건다 - 포트를 못 잡은 것과 규약을 모르는 것은 다른 문제이고,
     * 구독해 두면 값이 실제로 오고 있는지를 로그로 구분할 수 있다. */
    rc = mosquitto_subscribe(m, NULL, GUARDX_TOPIC_MATRIX_TRACK,
                             GUARDX_QOS_MATRIX_TRACK);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: matrix track subscribe failed: %s\n",
                mqtt_error_name(rc));
    else
        printf("mqtt: subscribed to %s (qos%d)\n",
               GUARDX_TOPIC_MATRIX_TRACK, GUARDX_QOS_MATRIX_TRACK);

    /* 화재 Zone bitmap. retained라 구독하는 순간 브로커가 현재 상태를
     * 곧바로 준다 - 화재 중에 RPi C가 재시작해도 LED가 비어 있지 않다. */
    rc = mosquitto_subscribe(m, NULL, GUARDX_TOPIC_MATRIX_FIRE,
                             GUARDX_QOS_MATRIX_FIRE);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: matrix fire subscribe failed: %s\n",
                mqtt_error_name(rc));
    else
        printf("mqtt: subscribed to %s (qos%d)\n",
               GUARDX_TOPIC_MATRIX_FIRE, GUARDX_QOS_MATRIX_FIRE);

    /* zone별 온습도. 토픽이 나뉜 것도 retain 때문이다(한 토픽을 공유하면
     * 브로커가 마지막 zone 한 건만 보관한다). */
    for (i = 0; i < GX_ZONE_COUNT; i++) {
        rc = mosquitto_subscribe(m, NULL, topic_zone[i], GUARDX_QOS_MATRIX_ZONE);
        if (rc != MOSQ_ERR_SUCCESS)
            fprintf(stderr, "mqtt: matrix zone subscribe(%s) failed: %s\n",
                    topic_zone[i], mqtt_error_name(rc));
    }
    printf("mqtt: subscribed to %s x%d (qos%d)\n",
           GUARDX_TOPIC_MATRIX_ZONE_FMT, GX_ZONE_COUNT, GUARDX_QOS_MATRIX_ZONE);
}

/* 평평한 JSON 에서 문자열 값 하나를 꺼낸다. cmd_parser.c 와 같은 철학이다 -
 * 발신자가 아군 VMS 뿐이고 스키마가 정수 몇 개와 짧은 문자열로 고정이라
 * 라이브러리를 들일 이유가 없다. 못 찾으면 out 을 빈 문자열로 둔다. */
static void json_str(const char *json, const char *key, char *out, size_t cap)
{
    const char *p;
    const char *q;
    const char *e;
    size_t n;

    out[0] = '\0';
    p = strstr(json, key);
    if (!p)
        return;
    q = strchr(p + strlen(key), ':');
    if (!q)
        return;
    q = strchr(q, '"');
    if (!q)
        return;
    e = strchr(q + 1, '"');
    if (!e)
        return;
    n = (size_t)(e - q - 1);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, q + 1, n);
    out[n] = '\0';
}

/* 평평한 JSON 의 불리언 하나. 없으면 def. */
static bool json_bool(const char *json, const char *key, bool def)
{
    const char *p = strstr(json, key);
    const char *q;

    if (!p)
        return def;
    q = strchr(p + strlen(key), ':');
    if (!q)
        return def;
    ++q;
    while (*q == ' ' || *q == '\t')
        ++q;
    if (strncmp(q, "true", 4) == 0)
        return true;
    if (strncmp(q, "false", 5) == 0)
        return false;
    return def;
}

/* VMS 혼잡 팝업과 같은 edge 이벤트를 소비한다. QoS 1 중복 전달은 채널별
 * 마지막 severity로 제거하고, critical로 실제 전이할 때만 한 번 재생한다. */
static void notify_congestion_alert(const char *payload, int len)
{
    char json[512];
    char event[32];
    char severity[16];
    const char *p;
    char *end;
    long channel;
    int next;
    int n = len;

    if (!payload || n <= 0)
        return;
    if (n > (int)sizeof(json) - 1)
        n = (int)sizeof(json) - 1;
    memcpy(json, payload, (size_t)n);
    json[n] = '\0';

    json_str(json, "\"event\"", event, sizeof(event));
    json_str(json, "\"severity\"", severity, sizeof(severity));
    if (strcmp(event, "congestion") != 0)
        return;

    p = strstr(json, "\"channel\"");
    if (!p || !(p = strchr(p, ':')))
        return;
    channel = strtol(p + 1, &end, 10);
    if (end == p + 1 || channel < 0 || channel >= GX_ZONE_COUNT)
        return;

    if (strcmp(severity, "critical") == 0)
        next = 2;
    else if (strcmp(severity, "warn") == 0)
        next = 1;
    else if (strcmp(severity, "clear") == 0)
        next = 0;
    else
        return;

    if (last_congestion_severity[channel] == next)
        return;
    last_congestion_severity[channel] = next;

    if (next == 2)
        audio_event_play(AUDIO_SCENE_CROWD);
}

/* 방송 명령에서 START/STOP 만 뽑아 조율기에 넘긴다.
 *
 * 옛 MQTT/PCM 방송 경로(broadcast_audio.c)는 이제 빌드에서 빠졌다 - VMS 가 RTP
 * 전용으로 바뀌어 발행하는 쪽이 없어졌고, 남겨두면 같은 스피커를 두고 조율기가
 * 띄운 RTP 수신기와 싸운다. 이제 이 파싱이 방송 상태를 아는 유일한 경로다.
 */
static void notify_arbiter_broadcast(const char *payload, int len)
{
    char json[320];
    char owner[64];
    const char *action;
    const char *sid;
    unsigned long session = 0;
    bool takeover;
    int n = len;

    if (!payload || n <= 0)
        return;
    if (n > (int)sizeof(json) - 1)
        n = (int)sizeof(json) - 1;
    memcpy(json, payload, (size_t)n);
    json[n] = '\0';

    action = strstr(json, "\"action\"");
    sid = strstr(json, "\"session_id\"");
    if (!action || !sid)
        return;

    sid = strchr(sid, ':');
    if (!sid)
        return;
    session = strtoul(sid + 1, NULL, 10);

    /* owner 는 화면 표시용이다 - 세션 판별은 여전히 session_id 로만 한다. */
    json_str(json, "\"" GUARDX_BROADCAST_FIELD_OWNER "\"", owner, sizeof(owner));

    /* takeover 는 **없으면 false** 다. 기본이 거절이라야 서로를 모르는 VMS 두
     * 대가 상대의 방송을 예고 없이 끊지 않는다. */
    takeover = json_bool(json, "\"" GUARDX_BROADCAST_FIELD_TAKEOVER "\"", false);

    /* START 와 KEEPALIVE 를 갈라 넘긴다. KEEPALIVE 로 방송을 새로 시작할 수
     * 있게 두면, 화재가 방송을 선점한 직후 VMS 가 계속 보내는 KEEPALIVE 가
     * 2초 만에 방송을 되살려 사이렌을 죽인다(audio_arbiter.h 참조). */
    if (strstr(action, "\"" GUARDX_BROADCAST_ACTION_KEEPALIVE "\""))
        audio_arbiter_broadcast_keepalive(session);
    else if (strstr(action, "\"" GUARDX_BROADCAST_ACTION_START "\""))
        audio_arbiter_set_broadcast(true, session, owner, takeover);
    else if (strstr(action, "\"" GUARDX_BROADCAST_ACTION_STOP "\""))
        audio_arbiter_set_broadcast(false, session, owner, false);
}

static void on_message(struct mosquitto *m, void *obj,
                       const struct mosquitto_message *msg)
{
    (void)m;
    (void)obj;

    if (msg->payloadlen <= 0)
        return;

    if (strcmp(msg->topic, GUARDX_BROADCAST_COMMAND_TOPIC) == 0) {
        /* 이 명령은 이제 "스피커를 방송이 쓴다"는 신호일 뿐이다. 실제 오디오는
         * RTP/UDP 로만 흐르고, 그 수신기는 조율기가 켜고 끈다. */
        notify_arbiter_broadcast((const char *)msg->payload, msg->payloadlen);
        return;
    }
    if (strcmp(msg->topic, GUARDX_CONGESTION_ALERT_TOPIC) == 0) {
        notify_congestion_alert((const char *)msg->payload, msg->payloadlen);
        return;
    }
    if (strcmp(msg->topic, GUARDX_TOPIC_MATRIX_TRACK) == 0) {
        matrix_link_handle_track((const char *)msg->payload, msg->payloadlen);
        return;
    }
    if (strcmp(msg->topic, GUARDX_TOPIC_MATRIX_FIRE) == 0) {
        matrix_link_handle_fire((const char *)msg->payload, msg->payloadlen);
        return;
    }
    if (strcmp(msg->topic, GUARDX_FAN_LEVEL_TOPIC) == 0) {
        char buf[128];
        const char *p;
        int n = msg->payloadlen;

        if (n > (int)sizeof(buf) - 1)
            n = (int)sizeof(buf) - 1;
        memcpy(buf, msg->payload, (size_t)n);
        buf[n] = '\0';
        p = strstr(buf, "\"level\"");
        if (p && (p = strchr(p, ':')))
            fan_control_set_level((int)strtol(p + 1, NULL, 10));
        return;
    }
    {
        /* zone 토픽 4개 중 하나인지. 어느 zone인지는 핸들러가 payload의
         * zone_id로 정하므로 여기서는 "표시 토픽이 맞나"만 본다. */
        int i;
        for (i = 0; i < GX_ZONE_COUNT; i++) {
            if (strcmp(msg->topic, topic_zone[i]) == 0) {
                matrix_link_handle_zones((const char *)msg->payload,
                                         msg->payloadlen);
                return;
            }
        }
    }

    /* topic_actuator가 "guardx/actuator/rpic/#" 와일드카드라 RPi C 자신이
     * 발행한 ACK(guardx/actuator/rpic/ack)도 그 하위 토픽이라 도로 받는다.
     * ACK payload에도 command/action 키가 그대로 있어 PARSE_CMD_JSON이
     * 진짜 명령으로 오인 -> 재실행 -> 재ACK -> 재수신으로 무한 자기
     * 재생 루프가 생긴다(RPIC_ENABLE_ACK를 켜기 전엔 ACK을 안 보냈으니
     * 이 경로 자체가 없어 드러나지 않았다). 걸러낸다. */
    if (strcmp(msg->topic, topic_ack) == 0)
        return;

    if (cmd_handler)
        cmd_handler((const char *)msg->payload, msg->payloadlen);
}

guardx_err_t mqtt_sub_init(mqtt_cmd_handler_t handler)
{
    int rc;
    int i;

    if (handler == NULL)
        return GUARDX_ERR_INVALID;
    cmd_handler = handler;
    memset(last_congestion_severity, 0, sizeof(last_congestion_severity));

    /* 기본 토픽과 액추에이터별 하위 토픽을 모두 받는 MQTT wildcard.
     * guardx/actuator/rpic/#는 guardx/actuator/rpic 자체도 포함한다. */
    snprintf(topic_actuator, sizeof(topic_actuator),
             GUARDX_TOPIC_ACTUATOR_FMT "/#", GUARDX_NODE_RPIC);
    snprintf(topic_ack, sizeof(topic_ack),
             GUARDX_TOPIC_ACTUATOR_ACK_FMT, GUARDX_NODE_RPIC);
    snprintf(topic_status, sizeof(topic_status),
             GUARDX_TOPIC_STATUS_FMT, GUARDX_NODE_RPIC);

    /* zone_id는 1-based(레지스터 맵/화면 표기와 같음)라 +1 한다. */
    for (i = 0; i < GX_ZONE_COUNT; i++)
        snprintf(topic_zone[i], sizeof(topic_zone[i]),
                 GUARDX_TOPIC_MATRIX_ZONE_FMT, i + 1);

    mosquitto_lib_init();

    /* client id = 노드 ID (TLS 사용 시 인증서 CN과도 일치시킬 것).
     * clean_session=true: QoS1 명령이라도 오프라인 큐잉에 기대지
     * 않는다 - 액추에이터는 "재접속 후 밀린 옛 명령 폭주"가 오히려
     * 위험하므로, 끊긴 동안의 명령은 RPi B 판단 로직이 현재 상태
     * 기준으로 다시 내리는 것을 전제로 한다. */
    mosq = mosquitto_new(GUARDX_NODE_RPIC, true, NULL);
    if (!mosq) {
        fprintf(stderr, "mqtt: mosquitto_new failed\n");
        return GUARDX_ERR_OPEN;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);

    /* mosquitto_connect보다 먼저 등록해야 한다 - 연결 자체가 이 will을
     * 브로커에 넘기는 핸드셰이크의 일부라서, 연결 뒤에 걸면 이미 늦다.
     * retain=true: VMS가 늦게 구독해도 "죽어있었다"를 그대로 받는다. */
    rc = mosquitto_will_set(mosq, topic_status,
                            (int)strlen(GUARDX_STATUS_OFFLINE), GUARDX_STATUS_OFFLINE,
                            GUARDX_QOS_STATUS, true);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: will_set failed: %s\n", mqtt_error_name(rc));

#if MQTT_USE_TLS
    /* [REAL DEPLOYMENT] mTLS 설정 (포트 8883, CN=rpic) */
    rc = mosquitto_tls_set(mosq, MQTT_TLS_CA_PATH, NULL,
                           MQTT_TLS_CERT_PATH, MQTT_TLS_KEY_PATH, NULL);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: tls_set failed: %s\n", mqtt_error_name(rc));
        mosquitto_destroy(mosq);
        return GUARDX_ERR_OPEN;
    }
#endif

    rc = mosquitto_connect(mosq, MQTT_BROKER_HOST, MQTT_BROKER_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: connect failed: %s\n", mqtt_error_name(rc));
        mosquitto_destroy(mosq);
        return GUARDX_ERR_OPEN;
    }

    /* 재접속·keepalive·수신을 처리하는 내부 스레드 시작 */
    rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: loop_start failed: %s\n", mqtt_error_name(rc));
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        return GUARDX_ERR_OPEN;
    }

    printf("mqtt: connected to %s:%d (tls=%d)\n",
           MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_USE_TLS);
    return GUARDX_OK;
}

#if RPIC_ENABLE_ACK
guardx_err_t mqtt_sub_publish_ack(const char *json, int len)
{
    int rc = mosquitto_publish(mosq, NULL, topic_ack, len, json,
                               GUARDX_QOS_ACTUATOR_ACK, false);

    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s) failed: %s\n",
                topic_ack, mqtt_error_name(rc));
        return GUARDX_ERR_WRITE;
    }
    return GUARDX_OK;
}
#endif

guardx_err_t mqtt_sub_publish_retained(const char *topic, const char *json,
                                       int len)
{
    int rc;

    if (!mosq || !topic || !json || len <= 0)
        return GUARDX_ERR_INVALID;

    rc = mosquitto_publish(mosq, NULL, topic, len, json, 1, true);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s, retain) failed: %s\n",
                topic, mqtt_error_name(rc));
        return GUARDX_ERR_WRITE;
    }
    return GUARDX_OK;
}

guardx_err_t mqtt_sub_publish(const char *topic, const char *json, int len)
{
    int rc;

    if (!mosq || !topic || !json || len <= 0)
        return GUARDX_ERR_INVALID;

    rc = mosquitto_publish(mosq, NULL, topic, len, json, 1, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s) failed: %s\n",
                topic, mqtt_error_name(rc));
        return GUARDX_ERR_WRITE;
    }
    return GUARDX_OK;
}

void mqtt_sub_cleanup(void)
{
    if (mosq) {
        /* LWT는 "비정상" 끊김에서만 발동한다 - MQTT 스펙상 클라이언트가
         * DISCONNECT를 보내고 끊으면 브로커는 등록된 will을 취소한다.
         * 여기가 정확히 그 "정상 종료" 경로라, offline을 스스로 먼저
         * 발행하지 않으면 systemctl stop/Ctrl+C로 내려도 VMS엔 계속
         * online으로 남는다(guardx_protocol.h LWT 주석의 반쪽만 자동임). */
        mosquitto_publish(mosq, NULL, topic_status,
                          (int)strlen(GUARDX_STATUS_OFFLINE), GUARDX_STATUS_OFFLINE,
                          GUARDX_QOS_STATUS, true);
        /* publish는 네트워크 스레드가 비동기로 내보낸다 - 바로 이어서
         * disconnect하면 소켓이 먼저 닫혀 못 나갈 수 있다. 스레드가
         * 한 틱 돌 시간을 준다(길게 잡을 필요 없음 - 로컬 네트워크). */
        usleep(200000);

        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, false);
        mosquitto_destroy(mosq);
        mosq = NULL;
    }
    cmd_handler = NULL;
    mosquitto_lib_cleanup();
}
