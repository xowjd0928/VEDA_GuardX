/*
 * fan_control.c - 팬 듀티 결정. 규칙과 배경은 fan_control.h 참조.
 */

#define _POSIX_C_SOURCE 200809L

#include "fan_control.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "fan_protocol.h"
#include "guardx_hal.h"
#include "mqtt_sub.h"

/* MQTT 콜백 스레드(단계·화재·명령)와 main 이 함께 만진다. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static bool auto_on;          /* 기본 꺼짐 - 켜는 것은 운영자의 조작이다 */
static bool fire_active;
static int  level;            /* 0 정상 / 1 주의 / 2 위험 */
static int  applied_duty = -1;   /* 실제로 팬에 넣은 값. -1 = 아직 없음 */

static int duty_for_level(int lv)
{
    switch (lv) {
    case 2:  return GUARDX_FAN_DUTY_CRIT;
    case 1:  return GUARDX_FAN_DUTY_WARN;
    default: return GUARDX_FAN_DUTY_NORMAL;
    }
}

/* 상태 한 건 발행. lock 을 쥐지 않는다 - 호출부가 값을 복사해 넘긴다. */
static void publish_state(bool a, int lv, int duty, bool fire)
{
    char json[192];
    int n = snprintf(json, sizeof(json),
                     "{\"node_id\":\"rpic\",\"timestamp\":%lld,"
                     "\"auto\":%s,\"level\":%d,\"duty\":%d,\"fire\":%s}",
                     (long long)time(NULL) * 1000LL,
                     a ? "true" : "false", lv, duty,
                     fire ? "true" : "false");

    if (n <= 0)
        return;
    if (n > (int)sizeof(json))
        n = (int)sizeof(json);
    /* retained - VMS 가 언제 켜지든 구독 즉시 AUTO 표시를 맞출 수 있어야 한다. */
    (void)mqtt_sub_publish_retained(GUARDX_FAN_STATE_TOPIC, json, n);
}

/*
 * 지금 상태에서 나와야 할 듀티를 계산해 적용한다. lock 을 쥔 채 부르고,
 * 발행은 lock 밖에서 하도록 인자로 값을 돌려준다.
 *
 * @return 값이 바뀌어 적용했으면 true
 */
static bool apply_locked(int *out_duty)
{
    int want;

    if (fire_active)
        want = 0;
    else if (auto_on)
        want = duty_for_level(level);
    else
        return false;   /* 수동 구간 - 여기서 건드리지 않는다 */

    *out_duty = want;
    if (want == applied_duty)
        return false;

    if (rpic_fan_set((fan_data_t)want) != GUARDX_OK) {
        fprintf(stderr, "fan: 듀티 %d%% 적용 실패\n", want);
        return false;
    }
    applied_duty = want;
    return true;
}

guardx_err_t fan_control_init(void)
{
    bool a;
    int lv, duty;

    pthread_mutex_lock(&lock);
    auto_on = false;
    fire_active = false;
    level = 0;
    /* 직접 끈다. actuator_registry 의 apply_safe_state 가 이미 껐지만, 그걸
     * 믿고 장부만 맞추면 이 모듈이 "팬의 현재 값을 아는 유일한 곳"이라는
     * 전제가 다른 파일의 호출 순서에 매달린다. 0 을 두 번 쓰는 비용보다
     * 그 결합이 비싸다. */
    if (rpic_fan_set(0) != GUARDX_OK)
        fprintf(stderr, "fan: 기동 시 정지 실패\n");
    applied_duty = 0;
    a = auto_on;
    lv = level;
    duty = applied_duty;
    pthread_mutex_unlock(&lock);

    printf("fan: 자동 제어 준비 (AUTO 꺼짐, 단계 %d/%d/%d%%)\n",
           GUARDX_FAN_DUTY_NORMAL, GUARDX_FAN_DUTY_WARN, GUARDX_FAN_DUTY_CRIT);
    publish_state(a, lv, duty, false);
    return GUARDX_OK;
}

void fan_control_set_level(int lv)
{
    bool a;
    bool fire;
    int duty;
    int ignored = 0;

    if (lv < 0 || lv > 2) {
        fprintf(stderr, "fan: 단계 %d 범위 밖 - 무시\n", lv);
        return;
    }

    pthread_mutex_lock(&lock);
    if (lv == level) {
        pthread_mutex_unlock(&lock);
        return;   /* 같은 단계 재발행(retained 재수신 포함) */
    }
    level = lv;
    (void)apply_locked(&ignored);
    /* 발행할 값은 전부 잠금 안에서 복사한다 - 푼 뒤에 전역을 읽으면 그 사이
     * 바뀐 값을 내보내게 된다. */
    a = auto_on;
    fire = fire_active;
    duty = applied_duty;
    pthread_mutex_unlock(&lock);

    if (a)
        printf("fan: 혼잡 단계 %d -> %d%%%s\n", lv, duty,
               fire ? " (화재 중이라 0% 유지)" : "");
    publish_state(a, lv, duty, fire);
}

void fan_control_set_fire(bool active)
{
    bool a;
    int lv, duty;

    pthread_mutex_lock(&lock);
    if (active == fire_active) {
        pthread_mutex_unlock(&lock);
        return;   /* 재발행/retained 반복 - 전이만 처리한다 */
    }
    fire_active = active;

    if (active) {
        /* AUTO 든 수동이든 무조건 0%. 여기서는 apply_locked 를 쓰지 않는다 -
         * 수동 구간에서도 반드시 내려야 하기 때문이다. */
        if (rpic_fan_set(0) != GUARDX_OK)
            fprintf(stderr, "fan: 화재 - 정지 실패\n");
        applied_duty = 0;
    } else if (auto_on) {
        /* 화재 해제 + AUTO ON -> 자동 재개 */
        int d = 0;
        (void)apply_locked(&d);
    }
    /* 화재 해제 + AUTO OFF -> 0% 인 채로 둔다. 아무도 안 눌렀는데 팬이
     * 스스로 되살아나면 안 된다 - 수동 위젯만 풀린다(VMS 쪽). */

    a = auto_on;
    lv = level;
    duty = applied_duty;
    pthread_mutex_unlock(&lock);

    printf("fan: 화재 %s - 듀티 %d%%\n", active ? "발생" : "해제", duty);
    publish_state(a, lv, duty, active);
}

void fan_control_set_auto(bool on)
{
    bool a, fire;
    int lv, duty;

    pthread_mutex_lock(&lock);
    if (on == auto_on) {
        pthread_mutex_unlock(&lock);
        return;
    }
    auto_on = on;

    if (on) {
        int d = 0;
        (void)apply_locked(&d);
    }
    /* 끌 때는 현재 출력을 그대로 둔다. 0 으로 떨어뜨리면 운영자가 수동으로
     * 넘긴 것뿐인데 환기가 끊긴다. */

    a = auto_on;
    lv = level;
    duty = applied_duty;
    fire = fire_active;
    pthread_mutex_unlock(&lock);

    printf("fan: AUTO %s - 듀티 %d%%\n", on ? "켬" : "끔", duty);
    publish_state(a, lv, duty, fire);
}

bool fan_control_auto_enabled(void)
{
    bool a;

    pthread_mutex_lock(&lock);
    a = auto_on;
    pthread_mutex_unlock(&lock);
    return a;
}

guardx_err_t fan_control_manual(int duty)
{
    guardx_err_t ret;
    bool a;
    int lv;

    if (duty < 0 || duty > 100)
        return GUARDX_ERR_INVALID;

    pthread_mutex_lock(&lock);
    if (fire_active) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "fan: 화재 중 - 수동 명령 %d%% 거절\n", duty);
        return GUARDX_ERR_INVALID;
    }
    if (auto_on) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "fan: AUTO 중 - 수동 명령 %d%% 거절\n", duty);
        return GUARDX_ERR_INVALID;
    }

    ret = rpic_fan_set((fan_data_t)duty);
    if (ret == GUARDX_OK)
        applied_duty = duty;
    a = auto_on;
    lv = level;
    pthread_mutex_unlock(&lock);

    if (ret == GUARDX_OK)
        publish_state(a, lv, duty, false);
    return ret;
}

void fan_control_republish(void)
{
    bool a, fire;
    int lv, duty;

    pthread_mutex_lock(&lock);
    a = auto_on;
    fire = fire_active;
    lv = level;
    duty = applied_duty;
    pthread_mutex_unlock(&lock);

    publish_state(a, lv, duty < 0 ? 0 : duty, fire);
}
