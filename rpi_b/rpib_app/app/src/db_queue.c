/*
 * db_queue.c - 링버퍼 두 개(센서/이벤트) + 공유 뮤텍스·조건변수
 *
 * 순수 자료구조다. MQTT도 DB도 파일도 모른다 - 무엇을 기록하는지는
 * db_writer.c의 관심사이고, 여기는 "안전하게 건네주는 것"만 한다.
 */

#include <stdbool.h>
#include <pthread.h>

#include "db_queue.h"

typedef struct {
    db_record_t  *buf;
    int           cap;
    int           head;        /* 꺼낼 위치 */
    int           tail;        /* 넣을 위치 */
    unsigned long dropped;
} ring_t;

static db_record_t sensor_buf[DB_QUEUE_SENSOR_CAP];
static db_record_t event_buf[DB_QUEUE_EVENT_CAP];

static ring_t sensor_ring = { sensor_buf, DB_QUEUE_SENSOR_CAP, 0, 0, 0 };
static ring_t event_ring  = { event_buf,  DB_QUEUE_EVENT_CAP,  0, 0, 0 };

/* 두 링이 락과 조건변수를 공유한다 - 워커 하나가 양쪽을 동시에
 * 기다리려면 대기 대상이 하나여야 하기 때문 (db_queue.h 참조) */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv   = PTHREAD_COND_INITIALIZER;
static bool            shutting_down;

static bool ring_empty(const ring_t *r)
{
    return r->head == r->tail;
}

static bool ring_full(const ring_t *r)
{
    return (r->tail + 1) % r->cap == r->head;
}

/* 가득 차면 가장 오래된 것을 버리고 넣는다. 절대 기다리지 않는다 */
static void ring_push(ring_t *r, const db_record_t *rec)
{
    if (ring_full(r)) {
        r->head = (r->head + 1) % r->cap;
        r->dropped++;
    }
    r->buf[r->tail] = *rec;
    r->tail = (r->tail + 1) % r->cap;
}

static bool ring_pop(ring_t *r, db_record_t *out)
{
    if (ring_empty(r))
        return false;
    *out = r->buf[r->head];
    r->head = (r->head + 1) % r->cap;
    return true;
}

guardx_err_t db_queue_init(void)
{
    pthread_mutex_lock(&lock);
    sensor_ring.head = sensor_ring.tail = 0;
    event_ring.head  = event_ring.tail  = 0;
    sensor_ring.dropped = event_ring.dropped = 0;
    shutting_down = false;
    pthread_mutex_unlock(&lock);
    return GUARDX_OK;
}

void db_queue_push(const db_record_t *rec)
{
    ring_t *r;

    if (!rec)
        return;

    /* 버튼도 이벤트 링으로 보낸다 - QoS2로 "정확히 1회"를 보장받아 온
     * 로그라, 센서 폭주에 밀려 사라지면 그 보장이 무의미해진다 */
    r = (rec->type == REC_SENSOR) ? &sensor_ring : &event_ring;

    pthread_mutex_lock(&lock);
    ring_push(r, rec);
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lock);
}

int db_queue_pop(db_record_t *out)
{
    int got = 0;

    if (!out)
        return 0;

    pthread_mutex_lock(&lock);

    /* while(조건)로 감싸는 것이 중요하다 - 조건변수는 신호 없이도
     * 깨어날 수 있고(spurious wakeup), 그때 빈 큐를 꺼내면 안 된다 */
    while (ring_empty(&event_ring) && ring_empty(&sensor_ring) &&
           !shutting_down)
        pthread_cond_wait(&cv, &lock);

    /* 이벤트 우선. 화재 전이는 유일무이하고 건수가 적어서, 먼저 봐도
     * 센서가 굶지 않는다 */
    if (ring_pop(&event_ring, out) || ring_pop(&sensor_ring, out))
        got = 1;

    pthread_mutex_unlock(&lock);
    return got;   /* 0 = 큐가 비었고 종료 중 -> 워커 루프 탈출 */
}

void db_queue_shutdown(void)
{
    pthread_mutex_lock(&lock);
    shutting_down = true;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&lock);
}

unsigned long db_queue_dropped_sensor(void)
{
    unsigned long n;

    pthread_mutex_lock(&lock);
    n = sensor_ring.dropped;
    pthread_mutex_unlock(&lock);
    return n;
}

unsigned long db_queue_dropped_event(void)
{
    unsigned long n;

    pthread_mutex_lock(&lock);
    n = event_ring.dropped;
    pthread_mutex_unlock(&lock);
    return n;
}
