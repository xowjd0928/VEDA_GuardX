/*
 * test_db_queue.c - db_queue 단위 테스트 (PHASE 3)
 *
 * 빌드·실행:  cd ../app && make test
 *
 * 동시성 코드는 눈으로 읽어서 맞는지 알기 어렵고, 실기 시나리오
 * (02_fake_rpia.sh)로는 큐 포화나 드롭 회계를 재현할 수 없다 - 1Hz로는
 * 256칸이 안 찬다. 그래서 자료구조만 따로 떼어 검증한다.
 *
 * 특히 [5]는 Pi에서 꼭 다시 돌릴 것. 개발 PC와 Pi는 pthread 구현과
 * 코어 수가 달라서, 한쪽에서만 드러나는 경합이 있을 수 있다.
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "db_queue.h"

static int fails;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } \
    else printf("  ok  : %s\n", msg); } while (0)

static db_record_t mk_sensor(uint32_t seq)
{
    db_record_t r;
    memset(&r, 0, sizeof(r));
    r.type = REC_SENSOR;
    r.u.sensor.seq = seq;
    r.score = (float)seq;
    return r;
}

static db_record_t mk_event(uint32_t seq)
{
    db_record_t r;
    memset(&r, 0, sizeof(r));
    r.type = REC_TRANSITION;
    r.u.trans.trigger_seq = seq;
    return r;
}

static void test_fifo(void)
{
    db_record_t r;
    int i, ok = 1;

    printf("[1] FIFO 순서\n");
    db_queue_init();
    for (i = 0; i < 10; i++) { r = mk_sensor(i); db_queue_push(&r); }
    db_queue_shutdown();
    for (i = 0; i < 10; i++) {
        if (!db_queue_pop(&r) || r.u.sensor.seq != (uint32_t)i) ok = 0;
    }
    CHECK(ok, "10건 넣은 순서대로 나옴");
    CHECK(db_queue_pop(&r) == 0, "비면 0 반환(종료 신호 후)");
}

static void test_drop_oldest(void)
{
    db_record_t r;
    int i;
    int usable = DB_QUEUE_SENSOR_CAP - 1;

    printf("[2] 포화 시 가장 오래된 것 드롭\n");
    db_queue_init();
    for (i = 0; i < DB_QUEUE_SENSOR_CAP + 4; i++) { r = mk_sensor(i); db_queue_push(&r); }
    CHECK(db_queue_dropped_sensor() == (unsigned long)(DB_QUEUE_SENSOR_CAP + 4 - usable),
          "드롭 개수 = 투입 - 적재가능(CAP-1)");
    db_queue_shutdown();
    db_queue_pop(&r);
    CHECK(r.u.sensor.seq == (uint32_t)(DB_QUEUE_SENSOR_CAP + 4 - usable),
          "가장 오래된 것이 버려지고 그 다음 것이 선두");
}

static void test_event_priority(void)
{
    db_record_t r;

    printf("[3] 이벤트 우선 + 링 분리\n");
    db_queue_init();
    r = mk_sensor(1); db_queue_push(&r);
    r = mk_sensor(2); db_queue_push(&r);
    r = mk_event(99); db_queue_push(&r);
    db_queue_shutdown();
    db_queue_pop(&r);
    CHECK(r.type == REC_TRANSITION, "나중에 넣은 이벤트가 먼저 나옴");
    db_queue_pop(&r);
    CHECK(r.type == REC_SENSOR && r.u.sensor.seq == 1, "그 뒤 센서가 FIFO대로");
}

static void test_event_ring_isolated(void)
{
    db_record_t r;
    int i;

    printf("[4] 센서 폭주가 이벤트를 밀어내지 않음\n");
    db_queue_init();
    r = mk_event(7); db_queue_push(&r);
    for (i = 0; i < DB_QUEUE_SENSOR_CAP * 3; i++) { r = mk_sensor(i); db_queue_push(&r); }
    CHECK(db_queue_dropped_sensor() > 0, "센서는 드롭됨");
    CHECK(db_queue_dropped_event() == 0, "이벤트는 한 건도 안 버려짐");
    db_queue_shutdown();
    db_queue_pop(&r);
    CHECK(r.type == REC_TRANSITION && r.u.trans.trigger_seq == 7,
          "묻혀 있던 이벤트가 그대로 살아남음");
}

/* 실제 두 스레드로 돌려서 유실/중복이 없는지 */
#define N_PROD 5000
static void *producer(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < N_PROD; i++) {
        db_record_t r = mk_event(i);   /* 이벤트 링(32)이라 반드시 드롭 발생 */
        db_queue_push(&r);
    }
    db_queue_shutdown();
    return NULL;
}

static void test_threaded(void)
{
    pthread_t tid;
    db_record_t r;
    unsigned long popped = 0, dropped;
    uint32_t prev = 0;
    int monotonic = 1, first = 1;

    printf("[5] 2스레드 - 유실 회계가 맞는지\n");
    db_queue_init();
    pthread_create(&tid, NULL, producer, NULL);
    while (db_queue_pop(&r)) {
        if (!first && r.u.trans.trigger_seq <= prev) monotonic = 0;
        prev = r.u.trans.trigger_seq;
        first = 0;
        popped++;
    }
    pthread_join(tid, NULL);
    dropped = db_queue_dropped_event();
    printf("       꺼냄 %lu + 드롭 %lu = %lu (투입 %d)\n",
           popped, dropped, popped + dropped, N_PROD);
    CHECK(popped + dropped == N_PROD, "꺼낸 것 + 버린 것 = 넣은 것 (유실/중복 없음)");
    CHECK(monotonic, "순서 역전 없음");
}

int main(void)
{
    test_fifo();
    test_drop_oldest();
    test_event_priority();
    test_event_ring_isolated();
    test_threaded();
    printf("\n%s (실패 %d건)\n", fails ? "== 실패 ==" : "== 전부 통과 ==", fails);
    return fails ? 1 : 0;
}
