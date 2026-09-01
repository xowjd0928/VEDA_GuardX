/*
 * main.c - GuardX RPi B 판단 엔진(rpib_engine) 메인
 *
 * B는 "중계기"가 아니다. 두 독립 MQTT 구간의 접점이다:
 *
 *   [구간 1: A->B]  guardx/sensor/+(/button)  ─→ 여기서 소비되고 끝난다
 *   [구간 2: B->C]  guardx/actuator/{node}    ←─ 여기서 새로 만들어진다
 *
 * A의 메시지는 C로 전달되지 않는다. B가 센서 스냅샷을 먹고(decision),
 * 화재가 확정되면 자기 명의(node_id=rpib, 자기 seq)로 명령을 새로
 * 발행한다. 두 구간의 인과를 잇는 유일한 흔적이 명령 payload의
 * reason/sensor_seq 필드다 (cmd_builder.h의 확장 제안 참조).
 *
 * PHASE 6: zone(RPi A/C 물리 쌍)이 여러 개일 수 있다. 어느 zone의
 * 메시지인지는 도착한 토픽의 node_id로 정해지며(mqtt_bus.c 와일드카드
 * 구독), g_zones[]가 zone마다 독립된 판정 상태·워치독을 들고 있다.
 *
 * 스레드 구조: 판단·발행·DB기록 전부 mosquitto 콜백 스레드에서
 * 일어난다 (콜백 스레드는 하나라 자연 직렬화). main 스레드는
 * 센서 침묵 감시(워치독, zone마다)와 종료 대기만 한다. 콜백과 main이
 * 공유하는 것은 g_zones[].last_sensor_ms 뿐이라 락 대신 단조시계
 * 스냅샷으로 처리(zone 개수·배열 자체는 부팅 후 불변이라 배열 구조
 * 변경에 대한 동시성 걱정은 없다).
 */

#include <stdio.h>
#include <stdlib.h>   /* atoi - clear_fire payload의 zone_id 파싱 */
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>

#include "sensor_parser.h"
#include "decision.h"
#include "cmd_builder.h"
#include "db_writer.h"
#include "mqtt_bus.h"
#include "threshold_loader.h"
#include "zone_loader.h"

/* 센서 침묵 경고 임계 (A는 1Hz 발행이므로 5초면 5사이클 유실) */
#define SENSOR_SILENCE_WARN_MS 5000

/* --- 화재 대응 시나리오 각도/듀티 (전부 잠정치) -----------------------
 * !!! 미확정: 서보 각도는 기구(문 힌지/밸브 레버) 확정 후 실측.
 * RPi C rpic_pca9685.h의 SAFE_ANGLE과 짝으로 정해야 한다 !!! */
#define SCENARIO_VALVE_CLOSE_ANGLE 90   /* servo_1: 가스밸브 잠금 */
#define SCENARIO_SOUND_FIRE         1   /* 화재 경보음 (sound value: 1=화재) */

static volatile sig_atomic_t running = 1;

/* 발행 seq (프로세스 시작 시 0, 재시작 리셋 허용 - 규약 3절). zone과
 * 무관하게 이 프로세스(node_id=rpib) 하나 기준으로 단조 증가한다 -
 * 프로토콜 규약의 "seq = 발행 프로세스 기준"이 zone별이 아니라
 * 프로세스별이기 때문. zone이 여러 개여도 나눌 이유가 없다. */
static uint32_t pub_seq;

/*
 * PHASE 6: zone(RPi A/C 물리 쌍) 런타임 상태 - 고정 상한 배열.
 *
 * 이전엔 "센서 노드가 하나"라는 하드웨어 제약이 last_sensor_ms 변수
 * 하나, decision.c의 전역 static들로 코드에 그대로 배어 있었다. 이제
 * zone마다 독립된 판정 상태·워치독·동결 경고 플래그를 들고 있어야
 * 하므로 이 구조체 하나로 묶는다.
 *
 * g_zones/g_zone_count는 부팅 시 zone_loader_load()로 채워진 뒤
 * 프로세스 수명 내내 고정이다 - zone 목록 자체의 핫리로드(운영 중
 * zone 추가/제거)는 지금 하지 않는다(아래 reload_zones_at_boot 주석
 * 참조). fire_threshold 핫리로드(on_config)와는 별개다.
 */
typedef struct {
    fire_zone_t     info;
    decision_zone_t decision;
    uint64_t        last_sensor_ms;   /* mono_ms() 기준, 이 zone의 워치독 */
    bool            warned_stuck;     /* check_freeze()의 zone별 경고 상태 */
    bool            warned_relax;
    bool            silence_warned;
} zone_runtime_t;

static zone_runtime_t g_zones[MAX_FIRE_ZONES];
static int            g_zone_count;

/* 도착한 메시지의 node_id(mqtt_bus.c가 토픽에서 뽑아준 것)로 zone을
 * 찾는다. 버튼도 센서와 같은 RPi A에서 오므로 rpia_node_id 하나로
 * 둘 다 해결된다 - rpic_node_id는 발행(명령) 쪽에서만 쓴다. */
static zone_runtime_t *find_zone_by_rpia(const char *node_id)
{
    int i;

    for (i = 0; i < g_zone_count; i++)
        if (strcmp(g_zones[i].info.rpia_node_id, node_id) == 0)
            return &g_zones[i];
    return NULL;
}

/* 부팅 시 1회 - fire_zone 테이블을 읽어 g_zones를 채우고, zone마다
 * 판정 상태를 초기화한다. 실패하면(DB 미가동 등) 프로세스를 못 띄운다 -
 * zone 매핑 없이는 도착한 메시지를 어느 판정에 먹일지 알 수 없어서,
 * "일단 떠서 기다리는" 폴백이 threshold_loader처럼 성립하지 않는다.
 *
 * !!! 미확정: 운영 중 zone 핫추가(재시작 없이)는 지금 안 한다. 화재
 * 진행 중에 zone 목록을 다시 읽다가 상태를 잘못 리셋하면 오히려
 * 위험해질 수 있어, 검증된 병합 전략이 나오기 전까진 "zone 구성이
 * 바뀌면 재시작"이 더 안전한 선택이다. 지금 하드웨어가 1개뿐이라
 * 당장 급하지 않기도 하다. !!! */
static guardx_err_t load_zones_at_boot(void)
{
    fire_zone_t loaded[MAX_FIRE_ZONES];
    int count, i;
    guardx_err_t err = zone_loader_load(loaded, &count);

    if (err != GUARDX_OK)
        return err;

    for (i = 0; i < count; i++) {
        memset(&g_zones[i], 0, sizeof(g_zones[i]));
        g_zones[i].info = loaded[i];
        DECISION_ZONE_INIT(&g_zones[i].decision);
    }
    g_zone_count = count;
    return GUARDX_OK;
}

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* epoch 기준 밀리초 (JSON timestamp 필드용, RPi A와 동일) */
static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 단조시계 밀리초 (워치독용 - 벽시계 조정에 흔들리면 안 됨) */
static uint64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------------
 * 명령 발행 헬퍼 - 시나리오는 명령 여러 건의 묶음이므로 한 건 실패가
 * 나머지를 막지 않게 각각 발행하고 로그만 남긴다
 * --------------------------------------------------------------------- */

/* 발행에 성공한 것만 기록한다 - fire_event_command는 "B가 C로 실제로
 * 내보낸 명령"의 감사 기록이므로, 발행 실패한 것을 남기면 사실과 다르다.
 * 기록은 전이(db_write_transition)가 큐에 들어간 뒤에 이뤄져야 하며,
 * 화재 시나리오가 항상 그 순서로 호출되므로 자연히 지켜진다.
 *
 * rpic_node: 이 명령을 받을 RPi C (zone_runtime_t.info.rpic_node_id) -
 * PHASE 6부터 고정 "rpic"가 아니라 호출측(fire_scenario 등)이 zone마다
 * 알려준다. fire_event_command 자체엔 zone_id 컬럼이 없다 - 직전
 * db_write_transition()이 남긴 fire_event(이미 zone_id를 가짐)에 붙기
 * 때문에 중복해서 실을 필요가 없다. */
static void pub_action(const char *rpic_node, const char *command,
                       const char *action, const char *reason,
                       uint32_t sensor_seq)
{
    char json[GUARDX_JSON_MAX];
    uint32_t seq = pub_seq++;
    int len = CREATE_CMD_ACTION_JSON(json, sizeof(json), command, action,
                                    now_ms(), seq, reason, sensor_seq);

    if (mqtt_bus_publish_actuator(rpic_node, json, len) == GUARDX_OK) {
        printf("main: -> [%s] %s %s\n", rpic_node, command, action);
        db_write_command(command, action, 0, false, seq, now_ms());
    }
}

static void pub_set(const char *rpic_node, const char *command, int value,
                    const char *reason, uint32_t sensor_seq)
{
    char json[GUARDX_JSON_MAX];
    uint32_t seq = pub_seq++;
    int len = CREATE_CMD_SET_JSON(json, sizeof(json), command, value,
                                  now_ms(), seq, reason, sensor_seq);

    if (mqtt_bus_publish_actuator(rpic_node, json, len) == GUARDX_OK) {
        printf("main: -> [%s] %s SET %d\n", rpic_node, command, value);
        db_write_command(command, GUARDX_ACTION_SET, value, true, seq, now_ms());
    }
}

/* VMS 화재 경보 발행 - db_write_transition() 직후에만 부른다(순서 보장은
 * 호출부 책임). "recovered"는 cause_channel_id가 NULL 허용인 것과 같은
 * 이유로 cause 필드를 아예 뺀다 - 해제엔 원인이랄 게 없다.
 * guardx_mqttd(C++)가 재접속 시 복원용 retained 스냅샷을 별도로 관리하므로
 * 여기선 전이 순간만 알리면 된다(edge, retain=false).
 *
 * zone_id를 payload에 싣는다(PHASE 6) - "zone마다 토픽을 따로 만들지
 * 않고 필드로 구분"하는 쪽을 택했다. congestion 경보(guardx/alert/rpib)가
 * channel 필드로 같은 방식을 이미 쓰고 있고, 이러면 zone이 몇 개든
 * VMS의 구독 토픽 개수가 고정이라 더 확장에 유리하다. */
static void publish_fire_alert(const zone_runtime_t *z, decision_event_t ev)
{
    char json[GUARDX_JSON_MAX];
    int len;

    if (ev == DECISION_EVENT_FIRE) {
        len = snprintf(json, sizeof(json),
            "{\"node_id\":\"rpib\",\"timestamp\":%llu,\"zone_id\":%d,"
            "\"event_type\":\"fire_confirmed\",\"cause\":\"%s\",\"trigger_seq\":%u}",
            (unsigned long long)now_ms(), z->info.zone_id,
            DECISION_CAUSE_STR(DECISION_LAST_CAUSE(&z->decision)),
            DECISION_LAST_TRIGGER_SEQ(&z->decision));
    } else {
        len = snprintf(json, sizeof(json),
            "{\"node_id\":\"rpib\",\"timestamp\":%llu,\"zone_id\":%d,"
            "\"event_type\":\"recovered\"}",
            (unsigned long long)now_ms(), z->info.zone_id);
    }

    mqtt_bus_publish_alert("guardx/alert/fire", json, len);
}

/* 화재 확정 시나리오: 격리(밸브) -> 피난(문) -> 팬 강제정지 -> 소화(펌프)
 * -> 경보(앰프) 순. 팬은 "배연"이 아니라 "송풍"으로 쓰이는 하드웨어라 화재
 * 중에 돌면 연소를 부채질한다 - 수동으로 켜져 있던 것도 화재가 뜨는
 * 순간 강제로 끈다(recover_scenario와 달리 이쪽은 무조건 OFF, 상태 조회
 * 없이 그냥 끈다 - 어차피 꺼져 있으면 멱등).
 * QoS1이라 발행 순서가 곧 도착 순서 보장은 아니지만
 * (재전송 시 역전 가능), 각 명령이 독립 멱등이라 순서 의존이 없도록
 * 시나리오를 짰다 - "밸브가 잠기기 전에 펌프가 켜져도" 안전상 문제
 * 없는 조합만 사용.
 *
 * RPi C 매핑: 가스밸브=servo_1(각도), 화재셔터=shutter(OPEN/CLOSE/STOP 동사형,
 * 리밋센서 자동 정지는 RPi C 드라이버가 IRQ로 직접 처리), 경보=sound(상황음,
 * 1=화재). (구 servo_2 밸브 / amp 경보는 폐기, 구 stepper/SET(방향값)도 대체됨)
 *
 * z: 어느 zone의 화재인지 - 명령을 z->info.rpic_node_id로 보낸다. 다른
 * zone의 RPi C는 절대 안 건드린다(zone 격리 - 3구역 화재로 1구역 밸브가
 * 잠기면 안 된다). */
static void fire_scenario(const zone_runtime_t *z)
{
    const char *cause = DECISION_CAUSE_STR(DECISION_LAST_CAUSE(&z->decision));
    uint32_t sseq = DECISION_LAST_TRIGGER_SEQ(&z->decision);
    const char *c = z->info.rpic_node_id;

    printf("main: !!! FIRE CONFIRMED zone %d '%s' (cause=%s, sensor_seq=%u) !!!\n",
           z->info.zone_id, z->info.zone_name, cause, sseq);

    pub_set(c, GUARDX_CMD_SERVO_1, SCENARIO_VALVE_CLOSE_ANGLE, cause, sseq); /* 가스밸브 잠금 */
    pub_action(c, GUARDX_CMD_SHUTTER, GUARDX_ACTION_CLOSE, cause, sseq);     /* 화재셔터 닫기 */
    pub_action(c, GUARDX_CMD_FAN,     GUARDX_ACTION_OFF,       cause, sseq); /* 팬 강제정지(연소 조장 방지) */
    pub_action(c, GUARDX_CMD_WATER_PUMP, GUARDX_ACTION_ON,  cause, sseq);    /* 소화 */
    pub_set(c, GUARDX_CMD_SOUND,   SCENARIO_SOUND_FIRE,       cause, sseq);  /* 화재 경보음 */
}

/* 상황 해제: 능동 대응(펌프/팬/경보)만 끈다. 밸브·셔터는 되돌리지
 * 않는다 - 가스 누출 후 밸브 재개방은 사람이 현장 확인 후 해야 할
 * 일이지 센서 수치가 내려갔다고 자동으로 할 일이 아니다.
 * !!! 미확정: 수동 복구 절차(별도 토픽? 물리 스위치?)는 팀 논의 필요 !!! */
static void recover_scenario(const zone_runtime_t *z)
{
    const char *c = z->info.rpic_node_id;

    printf("main: zone %d situation recovered - stopping active response\n",
           z->info.zone_id);

    pub_action(c, GUARDX_CMD_WATER_PUMP, GUARDX_ACTION_OFF, "recover", 0);
    pub_set(c, GUARDX_CMD_SOUND, 0, "recover", 0);
    pub_set(c, GUARDX_CMD_FAN, 0, "recover", 0);
    printf("main: (servo_1 valve / shutter left as-is - manual reset required)\n");
}

/* ---------------------------------------------------------------------
 * 수신 핸들러 (mosquitto 콜백 스레드)
 * --------------------------------------------------------------------- */

/* FIRE 중 센서 무효로 해제 판정이 멈춰 있는 상태를 밖에서 보이게 한다.
 * decision.c는 시계도 로깅도 모르는 순수 로직이라 경고를 판정 안에서
 * 찍지 않고 여기서 꺼내 쓴다 (워치독의 silence_warned와 같은 관용구).
 * 각 경고는 동결 구간당 1회. 센서가 살아나면(freeze=0) 리셋된다.
 * 경고 상태(warned_stuck/warned_relax)는 zone_runtime_t에 있다 - zone마다
 * 독립이어야 하기 때문(한 zone의 동결이 다른 zone 경고를 억누르면 안 됨). */
static void check_freeze(zone_runtime_t *z)
{
    const fire_config_t *c = DECISION_GET_CONFIG();
    int fz = DECISION_FREEZE_CYCLES(&z->decision);

    if (fz == 0) {
        z->warned_stuck = false;
        z->warned_relax = false;
        return;
    }

    /* 완화 임계를 이미 넘었으면 "동결됐다"고 찍지 않는다 - 그 시점엔
     * 해제 판정이 재개된 상태라 사실과 다르다. relax를 작게 잡고
     * 테스트할 때(fastfreeze) 이 조건이 실제로 갈린다. */
    if (fz >= c->n_recover && fz < c->freeze_relax_cycles && !z->warned_stuck) {
        fprintf(stderr, "main: WARNING - zone %d recovery frozen for %d cycles "
                "(sensor invalid during FIRE), relax threshold at %d\n",
                z->info.zone_id, fz, c->freeze_relax_cycles);
        z->warned_stuck = true;
    }

    if (fz >= c->freeze_relax_cycles && !z->warned_relax) {
        fprintf(stderr, "main: zone %d freeze relax threshold reached (%d cycles) - "
                "recovery resumes on surviving channels if their weight "
                ">= %.2f\n", z->info.zone_id, fz, c->min_valid_weight);
        z->warned_relax = true;
    }
}

static void on_sensor(const char *node_id, const char *payload, int len)
{
    char buf[GUARDX_JSON_MAX];
    sensor_msg_t msg;
    decision_event_t ev;
    zone_runtime_t *z = find_zone_by_rpia(node_id);

    if (!z) {
        /* fire_zone에 없는 노드 - 오배선/오타 설정이거나 아직 등록 안 된
         * 신규 zone(부팅 후 핫추가는 지금 미지원, 위 load_zones_at_boot
         * 주석 참조). 조용히 버리면 원인 파악이 안 되니 매번 찍는다. */
        fprintf(stderr, "main: 미등록 노드 '%s'의 센서 메시지 무시 "
                "(fire_zone에 없음)\n", node_id);
        return;
    }

    if (len >= (int)sizeof(buf)) {
        fprintf(stderr, "main: sensor payload too large (%d), dropped\n", len);
        return;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';

    if (PARSE_SENSOR_JSON(buf, &msg) != GUARDX_OK) {
        fprintf(stderr, "main: malformed sensor payload dropped: %s\n", buf);
        return;
    }

    z->last_sensor_ms = mono_ms();

    /* 판단을 먼저 하고 기록한다 - 종합 점수는 판단 후에야 존재하기
     * 때문. 기록이 큐에 넣기만 하는 비동기라(PHASE 3) 이 순서 변경으로
     * 판단이 늦어지지는 않는다. */
    ev = DECISION_FEED(&z->decision, &msg);
    db_write_sensor(&msg, now_ms(), DECISION_LAST_SCORE(&z->decision), z->info.zone_id);
    check_freeze(z);   /* NONE일 때가 곧 동결 구간이므로 early return보다 먼저 */
    if (ev == DECISION_EVENT_NONE)
        return;

    db_write_transition(ev, DECISION_LAST_CAUSE(&z->decision),
                        DECISION_LAST_TRIGGER_SEQ(&z->decision), now_ms(),
                        z->info.zone_id);
    publish_fire_alert(z, ev);

    if (ev == DECISION_EVENT_FIRE)
        fire_scenario(z);
    else
        recover_scenario(z);
}

static void on_button(const char *node_id, const char *payload, int len)
{
    char buf[GUARDX_JSON_MAX];
    button_msg_t msg;
    zone_runtime_t *z = find_zone_by_rpia(node_id);

    if (!z) {
        fprintf(stderr, "main: 미등록 노드 '%s'의 버튼 메시지 무시 "
                "(fire_zone에 없음)\n", node_id);
        return;
    }

    if (len >= (int)sizeof(buf))
        return;
    memcpy(buf, payload, len);
    buf[len] = '\0';

    if (PARSE_BUTTON_JSON(buf, &msg) != GUARDX_OK) {
        fprintf(stderr, "main: malformed button payload dropped: %s\n", buf);
        return;
    }

    /* 규약 4-2: 이 토픽은 로깅 전용. 실제 제어는 A->C 하드웨어
     * 인터락이 이미 처리한 뒤다. 판단 로직에도 넣지 않는다. */
    db_write_button(&msg, now_ms(), z->info.zone_id);
    printf("main: zone %d emergency button logged (count=%u, sensor_seq=%u)\n",
           z->info.zone_id, msg.press_count, msg.seq);

    /* VMS 경보 - 버튼은 상태가 아니라 사건이라 retained 스냅샷이 없다
     * (fire_incident와 달리). 눌린 순간만 의미 있고, 사람이 이미 현장에
     * 있다는 뜻이라 화재보다도 즉각 확인이 필요하다. */
    {
        char json[GUARDX_JSON_MAX];
        int len = snprintf(json, sizeof(json),
            "{\"node_id\":\"rpib\",\"timestamp\":%llu,\"zone_id\":%d,"
            "\"press_count\":%u}",
            (unsigned long long)now_ms(), z->info.zone_id, msg.press_count);

        mqtt_bus_publish_alert("guardx/alert/button", json, len);
    }
}

/* PHASE 7: 수동 화재 해제 (VMS -> B, guardx/cmd/rpib/clear_fire).
 *
 * 자동 해제를 없앴으므로(decision.c feed_fire 주석) FIRE에서 빠져나오는
 * 유일한 경로다. 해제 이후 처리는 예전 자동 해제와 완전히 동일하다 -
 * db_write_transition + VMS 알림 + recover_scenario. 즉 "누가 결정했는가"만
 * 바뀌고 "무엇을 하는가"는 그대로라, 기존 검증된 해제 경로를 재사용한다.
 *
 * DECISION_FORCE_RECOVER()가 멱등이라 중복 요청(QoS1 재전송, 버튼 연타)이
 * 와도 시나리오가 두 번 돌지 않는다 - 이미 NORMAL이면 NONE을 반환한다. */
static void on_clear_fire(const char *payload, int len)
{
    char buf[GUARDX_JSON_MAX];
    const char *p;
    int zone_id;
    zone_runtime_t *z = NULL;
    decision_event_t ev;
    int i;

    if (len <= 0 || len >= (int)sizeof(buf)) {
        fprintf(stderr, "main: clear_fire payload 크기 이상 (%d) 무시\n", len);
        return;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';

    /* zone_id만 뽑으면 되므로 파서를 새로 두지 않는다. 값이 없거나 못 읽으면
     * 해제하지 않는다 - "어느 zone인지 모르겠으니 다 끄자"는 절대 안 된다. */
    p = strstr(buf, "\"zone_id\"");
    if (!p || !(p = strchr(p, ':'))) {
        fprintf(stderr, "main: clear_fire에 zone_id 없음 - 무시: %s\n", buf);
        return;
    }
    zone_id = atoi(p + 1);

    for (i = 0; i < g_zone_count; i++)
        if (g_zones[i].info.zone_id == zone_id) {
            z = &g_zones[i];
            break;
        }
    if (!z) {
        fprintf(stderr, "main: clear_fire 대상 zone %d 없음 - 무시\n", zone_id);
        return;
    }

    ev = DECISION_FORCE_RECOVER(&z->decision);
    if (ev != DECISION_EVENT_RECOVER) {
        printf("main: zone %d은 이미 NORMAL - 수동 해제 무시(멱등)\n", zone_id);
        return;
    }

    printf("main: zone %d '%s' 수동 화재 해제 (운영자 요청)\n",
           z->info.zone_id, z->info.zone_name);

    db_write_transition(ev, DECISION_LAST_CAUSE(&z->decision),
                        DECISION_LAST_TRIGGER_SEQ(&z->decision), now_ms(),
                        z->info.zone_id);
    publish_fire_alert(z, ev);
    recover_scenario(z);
}

/* PHASE 4: fire_threshold 갱신 신호. payload 내용은 안 본다 - 신호가
 * 오면 무조건 DB에서 다시 읽는다(threshold_load_and_apply 참조).
 * 실패해도 기존 설정을 유지하며 계속 동작한다. */
static void on_config(const char *payload, int len)
{
    (void)payload;
    (void)len;

    printf("main: config reload signal received\n");
    if (threshold_load_and_apply() != GUARDX_OK)
        fprintf(stderr, "main: threshold reload failed, keeping current config\n");
}

int main(void)
{
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* 1) DB(스텁) 열기 - 기록 못 하는 채로 돌지 않도록 기동 실패 처리 */
    if (db_writer_open() != GUARDX_OK) {
        fprintf(stderr, "main: db open failed, aborting\n");
        return 1;
    }

    /* 2) zone 매핑 최초 적재 (PHASE 6). threshold와 달리 폴백이 없다 -
     * 어느 zone의 메시지인지 모르면 판정 자체를 어디에 먹일지 알 수
     * 없으므로, DB에서 이걸 못 읽으면 기동을 포기한다. */
    if (load_zones_at_boot() != GUARDX_OK) {
        fprintf(stderr, "main: zone mapping load failed, aborting "
                "(fire_zone 테이블 확인 - 최소 1행 필요)\n");
        db_writer_close();
        return 1;
    }

    /* 3) 화재 임계값 최초 적재. 실패해도 기동은 계속한다 - decision.c가
     * 컴파일타임 폴백값(DEFAULT_*)으로 이미 초기화돼 있으므로, DB를
     * 못 읽는다고 화재 감지 자체가 멈추면 안 된다는 판단(임계 잠정치로
     * 라도 도는 게 안 도는 것보다 안전). MQTT 구독보다 먼저 해서, 첫
     * 센서 메시지가 도착하기 전에 이미 최신 설정이 적용돼 있게 한다. */
    if (threshold_load_and_apply() != GUARDX_OK)
        fprintf(stderr, "main: threshold load failed, using compiled-in defaults\n");

    /* 4) MQTT 연결 + 구독 (와일드카드 - zone이 몇 개든 이 한 줄로 다 받는다) */
    if (mqtt_bus_init(on_sensor, on_button, on_config, on_clear_fire) != GUARDX_OK) {
        fprintf(stderr, "main: mqtt init failed, aborting\n");
        db_writer_close();
        return 1;
    }

    for (i = 0; i < g_zone_count; i++)
        g_zones[i].last_sensor_ms = mono_ms();
    {
        const fire_config_t *c = DECISION_GET_CONFIG();

        printf("main: rpib engine started - %d zone(s) (fuzzy fusion: score>=%.0f "
               "OR spark>=%.0f&irtemp>=%.0f, confirm=%d cycles, "
               "recover=%d cycles)\n",
               g_zone_count, c->fire_score_threshold, c->override_spark_score,
               c->override_irtemp_score, c->n_confirm, c->n_recover);
    }

    /* main 스레드 = 워치독. zone마다 담당 RPi A가 따로 있으므로 침묵도
     * zone별로 판단한다 - 한 zone이 죽어도 다른 zone은 정상 감지 중일
     * 수 있어 전체를 하나로 묶으면 그 사실이 가려진다.
     * (1회만 경고, 복구 시 리셋. 침묵을 화재로 격상할지는 미확정이라
     * 지금은 로그만 남긴다) */
    while (running) {
        poll(NULL, 0, 1000);

        for (i = 0; i < g_zone_count; i++) {
            zone_runtime_t *z = &g_zones[i];

            if (mono_ms() - z->last_sensor_ms > SENSOR_SILENCE_WARN_MS) {
                if (!z->silence_warned) {
                    fprintf(stderr, "main: WARNING - zone %d '%s' no sensor "
                            "data for %d ms (rpia '%s' down? network?)\n",
                            z->info.zone_id, z->info.zone_name,
                            SENSOR_SILENCE_WARN_MS, z->info.rpia_node_id);
                    z->silence_warned = true;
                }
            } else {
                z->silence_warned = false;
            }
        }
    }

    printf("main: shutting down\n");
    mqtt_bus_cleanup();   /* 콜백 스레드 먼저 정지 후 DB를 닫는다 */
    db_writer_close();
    return 0;
}
