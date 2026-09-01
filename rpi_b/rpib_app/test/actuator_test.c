/*
 * actuator_test.c - GuardX RPi B -> RPi C 액추에이터 발행 테스트 도구 (C 버전)
 *
 * RPi B(이 노드)에서 guardx/actuator/rpic 토픽으로 명령을 직접 발행해
 * RPi C에 붙은 액추에이터(가스밸브 서보/팬/화재셔터/펌프/스피커)를
 * 테스트한다. 실제 판단 엔진(rpib_engine)과는 별개인 수동 발행 도구다
 * (payload node_id는 실제 발신자와 동일하게 "rpib"). RPi C에서
 * rpic_subscriber가 켜져 있어야 명령이 실제로 동작한다.
 * (셔터의 물리 구동기는 스텝모터, 가스밸브=servo_1. 경보음은 sound 명령)
 *
 * ── 빌드 ─────────────────────────────────────────────────────────────
 *   sudo apt install -y libmosquitto-dev
 *   gcc -O2 -Wall actuator_test.c -o actuator_test -lmosquitto
 *
 * ── 다른 사람에게 줄 때 ──────────────────────────────────────────────
 *   1) actuator_test.c 를 그 사람 PC/RPi B에 복사
 *   2) 위 빌드 명령으로 컴파일
 *   3) 아래 DEFAULT_BROKER 가 맞는지 확인(또는 MQTT_HOST 환경변수로 덮어쓰기)
 *   4) 그냥 실행하면 번호 메뉴가 뜬다:  ./actuator_test
 * ─────────────────────────────────────────────────────────────────────
 *
 * 명령어로 바로 쓰기(메뉴 없이):
 *   ./actuator_test fan 60        # 팬 60%
 *   ./actuator_test fan off       # 팬 끄기
 *   ./actuator_test servo1 90     # 가스밸브 서보 90도
 *   ./actuator_test pump on|off   # 워터펌프
 *   ./actuator_test shutter close # 화재셔터 닫기
 *   ./actuator_test shutter open  # 화재셔터 열기
 *   ./actuator_test shutter stop  # 화재셔터 정지
 *   ./actuator_test sound 1       # 스피커: 0=기본 1=화재 2=강도 3=비상
 *   ./actuator_test alloff        # 전부 끄기(안전 상태)
 *   ./actuator_test demo          # 전체 자동 시퀀스
 *
 * 브로커 IP 바꾸기: 아래 DEFAULT_BROKER 수정, 또는
 *   MQTT_HOST=192.168.0.10 ./actuator_test
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mosquitto.h>

/* ===== 여기만 환경에 맞게 바꾸세요 ================================== */
#define DEFAULT_BROKER  "localhost"   /* RPi B에서 실행 - 브로커가 이 노드면 localhost
                                        (다른 노드면 MQTT_HOST=IP ./actuator_test) */
#define BROKER_PORT     1883
#define TOPIC           "guardx/actuator/rpic"

/* mTLS (환경변수 MQTT_TLS=1 이면 켜짐, 포트 8883). 발행자는 rpib 신원으로
 * 붙는다 - 실엔진(rpib_engine)과 같은 CN이라 브로커 입장에선 동일 클라이언트.
 * 브로커 인증서(rpib.crt)의 SAN이 "DNS:rpib, IP:172.20.33.251" 뿐이라
 * localhost로 접속하면 호스트네임 검증에 걸린다 -> mTLS 땐 반드시
 *   MQTT_TLS=1 MQTT_HOST=172.20.33.251 ./actuator_test
 * 처럼 SAN에 든 주소(IP)로 실행할 것. */
#define BROKER_PORT_TLS 8883
#define TLS_CA_PATH     "/etc/guardx/certs/ca.crt"
#define TLS_CERT_PATH   "/etc/guardx/certs/rpib.crt"
#define TLS_KEY_PATH    "/etc/guardx/certs/rpib.key"
/* =================================================================== */

static struct mosquitto *mosq;
static int  last_mid = -1;
static volatile int acked = 0;
static volatile int connected = 0;
static int  seq = 1;

/* CONNACK 수신 확인용 콜백. mosquitto_connect()는 TCP만 맺고 리턴하므로
 * 실제 세션 연결(CONNACK)은 여기서 확정된다. rc!=0이면 브로커가 거부. */
static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud;
    if (rc == 0) {
        connected = 1;
        fprintf(stderr, "[진단] on_connect: 연결 확정(CONNACK rc=0)\n");
    } else {
        fprintf(stderr, "[진단] 브로커 연결 거부(rc=%d): %s\n",
                rc, mosquitto_connack_string(rc));
    }
}

/* 연결이 끊기면 이유를 찍는다. rc=0이면 우리가 disconnect 호출한 정상 종료,
 * rc!=0이면 브로커/네트워크가 끊은 것(비정상). */
static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud;
    connected = 0;
    fprintf(stderr, "[진단] on_disconnect: 연결 끊김 rc=%d (%s)\n",
            rc, rc == 0 ? "정상 종료" : "브로커/네트워크가 끊음");
}

/* QoS1 PUBACK 수신 확인용 콜백 */
static void on_publish(struct mosquitto *m, void *ud, int mid)
{
    (void)m; (void)ud;
    if (mid == last_mid)
        acked = 1;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* command/action(+선택적 value)로 JSON 조립 후 발행하고, 전송(PUBACK)까지
 * 최대 ~2초 대기한다. has_value=0이면 value 필드 없이 보낸다. */
static void pub(const char *command, const char *action, int has_value, int value)
{
    char payload[256];
    int rc, i;

    if (has_value)
        snprintf(payload, sizeof(payload),
            "{\"node_id\":\"rpib\",\"timestamp\":%lld,\"seq\":%d,"
            "\"command\":\"%s\",\"action\":\"%s\",\"value\":%d}",
            now_ms(), seq++, command, action, value);
    else
        snprintf(payload, sizeof(payload),
            "{\"node_id\":\"rpib\",\"timestamp\":%lld,\"seq\":%d,"
            "\"command\":\"%s\",\"action\":\"%s\"}",
            now_ms(), seq++, command, action);

    if (has_value)
        printf("  -> 보냄: %s %s %d\n", command, action, value);
    else
        printf("  -> 보냄: %s %s\n", command, action);

    acked = 0;
    rc = mosquitto_publish(mosq, &last_mid, TOPIC,
                           (int)strlen(payload), payload, 1, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        printf("  [실패] publish 오류: %s\n", mosquitto_strerror(rc));
        return;
    }

    {
        struct timespec ten_ms = { 0, 10 * 1000 * 1000 };   /* 10ms */
        for (i = 0; i < 200 && !acked; i++)   /* 최대 2초 대기 */
            nanosleep(&ten_ms, NULL);
    }
}

/* 전부 끄기(안전 상태) */
static void all_off(void)
{
    pub("fan",        "OFF", 0, 0);
    pub("water_pump", "OFF", 0, 0);
    pub("shutter",    "STOP", 0, 0);
    pub("servo_1",    "SET", 1, 0);
    printf("전부 끔(안전 상태).\n");
}

/* 전체 자동 시퀀스 */
static void run_demo(void)
{
    pub("servo_1",    "SET", 1, 90); sleep(1);   /* 가스밸브 */
    pub("fan",        "SET", 1, 70); sleep(1);
    pub("shutter",    "CLOSE", 0, 0); sleep(2);
    pub("water_pump", "ON",  0, 0);  sleep(1);
    pub("sound",      "SET", 1, 1);  sleep(2);   /* 화재음 */
    all_off();
    printf("데모 완료.\n");
}

/* 한 줄 입력받아 개행 제거. EOF면 0. */
static int read_line(char *buf, size_t n)
{
    if (!fgets(buf, (int)n, stdin))
        return 0;
    buf[strcspn(buf, "\n")] = '\0';
    return 1;
}

/* 값 프롬프트 -> int (빈 입력이면 0) */
static int ask_int(const char *prompt)
{
    char buf[32];
    printf("%s", prompt);
    fflush(stdout);
    if (!read_line(buf, sizeof(buf)))
        return 0;
    return atoi(buf);
}

/* ── 명령어 모드 (인자 하나 처리하고 종료) ────────────────────────── */
static int run_command(int argc, char **argv)
{
    const char *cmd = argv[1];
    const char *arg = (argc > 2) ? argv[2] : "";

    if (strcmp(cmd, "servo1") == 0) {
        pub("servo_1", "SET", 1, atoi(arg));
    } else if (strcmp(cmd, "fan") == 0) {
        if (strcmp(arg, "on") == 0)       pub("fan", "ON",  0, 0);
        else if (strcmp(arg, "off") == 0) pub("fan", "OFF", 0, 0);
        else                              pub("fan", "SET", 1, atoi(arg));
    } else if (strcmp(cmd, "shutter") == 0) {
        if (strcmp(arg, "close") == 0)      pub("shutter", "CLOSE", 0, 0);
        else if (strcmp(arg, "open") == 0)  pub("shutter", "OPEN",  0, 0);
        else if (strcmp(arg, "stop") == 0)  pub("shutter", "STOP",  0, 0);
        else {
            printf("셔터 동작은 close / open / stop 중 하나여야 합니다.\n");
            return 1;
        }
    } else if (strcmp(cmd, "pump") == 0) {
        pub("water_pump", (strcmp(arg, "on") == 0) ? "ON" : "OFF", 0, 0);
    } else if (strcmp(cmd, "sound") == 0) {
        pub("sound", "SET", 1, atoi(arg));   /* 0 기본/1 화재/2 강도/3 비상 */
    } else if (strcmp(cmd, "alloff") == 0) {
        all_off();
    } else if (strcmp(cmd, "demo") == 0) {
        run_demo();
    } else {
        printf("모르는 명령: %s\n", cmd);
        printf("쓸 수 있는 것: fan / servo1 / pump / shutter / sound / alloff / demo\n");
        return 1;
    }
    return 0;
}

/* ── 메뉴 모드 (인자 없이 실행) ───────────────────────────────────── */
static void run_menu(const char *broker)
{
    char buf[32];

    printf("======================================\n");
    printf(" GuardX 액추에이터 테스트  (브로커: %s)\n", broker);
    printf(" * RPi C에서 rpic_subscriber가 켜져 있어야 합니다.\n");
    printf("======================================\n");

    for (;;) {
        printf("\n");
        printf(" 1) 팬 켜기 (속도 입력)      2) 팬 끄기\n");
        printf(" 3) 가스밸브 서보 각도\n");
        printf(" 4) 워터펌프 ON             5) 워터펌프 OFF\n");
        printf(" 6) 화재셔터 (close / open / stop, 리밋센서 자동 정지)\n");
        printf(" 7) 스피커 (0=기본 1=화재 2=강도 3=비상)\n");
        printf(" a) 전부 끄기(안전)         d) 데모 시퀀스        q) 종료\n");
        printf("번호 선택: ");
        fflush(stdout);

        if (!read_line(buf, sizeof(buf)))
            break;   /* EOF (Ctrl+D) */

        if      (strcmp(buf, "1") == 0) pub("fan", "SET", 1, ask_int("  팬 속도(0~100): "));
        else if (strcmp(buf, "2") == 0) pub("fan", "OFF", 0, 0);
        else if (strcmp(buf, "3") == 0) pub("servo_1", "SET", 1, ask_int("  가스밸브 각도(0~180): "));
        else if (strcmp(buf, "4") == 0) pub("water_pump", "ON",  0, 0);
        else if (strcmp(buf, "5") == 0) pub("water_pump", "OFF", 0, 0);
        else if (strcmp(buf, "6") == 0) {
            char action[16];
            printf("  셔터 동작(close/open/stop): ");
            fflush(stdout);
            if (!read_line(action, sizeof(action)))
                break;
            if (strcmp(action, "close") == 0)      pub("shutter", "CLOSE", 0, 0);
            else if (strcmp(action, "open") == 0)  pub("shutter", "OPEN",  0, 0);
            else if (strcmp(action, "stop") == 0)  pub("shutter", "STOP",  0, 0);
            else printf("  close / open / stop 중에서 골라주세요.\n");
        }
        else if (strcmp(buf, "7") == 0) pub("sound", "SET", 1, ask_int("  스피커 상황(0=기본 1=화재 2=강도 3=비상): "));
        else if (strcmp(buf, "a") == 0 || strcmp(buf, "A") == 0) all_off();
        else if (strcmp(buf, "d") == 0 || strcmp(buf, "D") == 0) run_demo();
        else if (strcmp(buf, "q") == 0 || strcmp(buf, "Q") == 0) { printf("종료합니다.\n"); break; }
        else printf("  1~7, a, d, q 중에서 골라주세요.\n");
    }
}

int main(int argc, char **argv)
{
    const char *broker = getenv("MQTT_HOST");
    const char *tls_env = getenv("MQTT_TLS");
    int use_tls = (tls_env && atoi(tls_env) != 0);
    int port = use_tls ? BROKER_PORT_TLS : BROKER_PORT;
    int rc;

    if (!broker || !broker[0])
        broker = DEFAULT_BROKER;

    mosquitto_lib_init();

    /* client ID를 매 실행마다 고유하게(PID 부착). 고정 ID면 테스터를
     * 두 번 이상 동시에 띄웠을 때 브로커가 같은 ID의 기존 연결을 끊어
     * "not currently connected"로 서로 튕겨나간다. */
    {
        char client_id[64];
        snprintf(client_id, sizeof(client_id), "rpic_tester_%d", (int)getpid());
        mosq = mosquitto_new(client_id, true, NULL);
    }
    if (!mosq) {
        fprintf(stderr, "mosquitto_new 실패\n");
        mosquitto_lib_cleanup();
        return 1;
    }
    fprintf(stderr, "[build: connect-wait-diag v3]\n");
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_publish_callback_set(mosq, on_publish);

    /* mTLS: 클라이언트 인증서를 붙인다. tls_set은 반드시 connect 전에 호출해야
     * 적용된다. 인증서 파일이 없거나(미배치) 읽기 권한이 없으면 여기서 실패 -
     * 브로커에 붙기도 전에 원인을 알 수 있게 경로를 같이 찍는다. */
    if (use_tls) {
        rc = mosquitto_tls_set(mosq, TLS_CA_PATH, NULL,
                               TLS_CERT_PATH, TLS_KEY_PATH, NULL);
        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "TLS 설정 실패: %s\n", mosquitto_strerror(rc));
            fprintf(stderr, "인증서 경로/권한 확인: %s , %s , %s\n",
                    TLS_CA_PATH, TLS_CERT_PATH, TLS_KEY_PATH);
            mosquitto_destroy(mosq);
            mosquitto_lib_cleanup();
            return 1;
        }
    }

    fprintf(stderr, "[접속] %s:%d (tls=%d)\n", broker, port, use_tls);
    rc = mosquitto_connect(mosq, broker, port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "브로커(%s:%d) 접속 실패: %s\n",
                broker, port, mosquitto_strerror(rc));
        fprintf(stderr, "IP/네트워크와 브로커 실행 여부를 확인하세요.\n");
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }
    mosquitto_loop_start(mosq);   /* 백그라운드로 keepalive/PUBACK 처리 */

    /* CONNACK 도착까지 대기. 이걸 안 하면 loop_start 직후 publish가
     * "not currently connected"(MOSQ_ERR_NO_CONN)로 실패한다. */
    {
        struct timespec ten_ms = { 0, 10 * 1000 * 1000 };   /* 10ms */
        int i;
        for (i = 0; i < 500 && !connected; i++)   /* 최대 5초 */
            nanosleep(&ten_ms, NULL);
        if (!connected) {
            fprintf(stderr, "브로커(%s:%d) 연결 확인 시간 초과.\n",
                    broker, port);
            fprintf(stderr, "브로커 실행 여부/방화벽/IP를 확인하세요.\n");
            mosquitto_loop_stop(mosq, true);
            mosquitto_destroy(mosq);
            mosquitto_lib_cleanup();
            return 1;
        }
    }

    if (argc > 1)
        rc = run_command(argc, argv);
    else {
        run_menu(broker);
        rc = 0;
    }

    mosquitto_disconnect(mosq);
    mosquitto_loop_stop(mosq, false);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return rc;
}
