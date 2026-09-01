/*
 * db_writer.c - JSON Lines 파일 기반 DB 스텁 + 기록 워커 스레드
 *
 * >>> PostgreSQL 교체 지점 <<<
 * PHASE 5에서 이 파일의 write_*_line() 3개만 libpq INSERT로 바꾼다.
 * 헤더 인터페이스, main.c 호출부, db_queue는 그대로 재사용한다 -
 * 큐에 원본 구조체가 들어 있어 재파싱이 필요 없기 때문.
 *
 * PHASE 3: 기록이 판단을 막지 않도록 워커 스레드로 분리했다.
 * db_write_*()는 이제 "기록한다"가 아니라 "기록을 맡긴다"이며 즉시
 * 반환한다. 실제 파일 쓰기는 전부 worker()에서 일어난다.
 * (분리 이유와 큐 정책은 db_queue.h 참조)
 *
 * fopen을 append 모드로 유지하고 매 기록 후 fflush - 프로세스가
 * 죽어도 마지막 기록까지 남도록. (fsync까지는 안 한다 - 1Hz 기록에
 * SD카드 수명을 갈아넣을 이유가 없음. 전원단 유실 허용은 프로토타입
 * 수준에서 감수). 이제 워커 스레드에서 도므로 fflush 비용이 판단
 * 경로에 실리지 않는다.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "db_writer.h"
#include "db_queue.h"
#include "db_writer_pg.h"

static FILE     *fp;
static pthread_t worker_tid;
static int       worker_running;

/* PHASE 5: DB_BACKEND 환경변수로 고른다 (기본 jsonl).
 * jsonl을 남겨두는 이유는 두 가지다 - PostgreSQL 없이 브로커만으로
 * 테스트할 수 있어야 하고, pg 모드에서도 폴백 기록처가 필요하다.
 * 그래서 pg 모드에서도 JSONL 파일은 계속 열어둔다. */
static int use_pg;

static guardx_err_t write_line_end(int n)
{
    if (n < 0) {
        fprintf(stderr, "db: write failed\n");
        return GUARDX_ERR_WRITE;
    }
    fputc('\n', fp);
    fflush(fp);
    return GUARDX_OK;
}

/* --- 실제 기록 (워커 스레드에서만 호출) ------------------------------- */

static void write_sensor_line(const db_record_t *rec)
{
    const sensor_msg_t *msg = &rec->u.sensor;
    int n;

    /* 필드 순서·이름을 fire_schema.sql에 맞춤:
     *   sensor_seq/ts        -> sensor_reading (sensor_seq, received_at)
     *   score                -> sensor_reading.composite_score
     *   gas_raw..irtemp_object -> sensor_value 6행 (channel_id 1~6 순서)
     *   valid.*              -> sensor_value.is_valid
     * PostgreSQL 전환 시 이 한 줄이 INSERT 1건(reading) + 6건(value)로
     * 풀리며, 값·이름 재매핑이 없다.
     *
     * score가 음수면 판단이 점수를 내지 않은 사이클(FIRE 상태에서 센서
     * 무효로 동결된 경우)이다. 직전 사이클 점수가 남아 오해를 부르지
     * 않도록 null로 적는다 - DB에서도 NULL이 된다. */
    n = fprintf(fp,
        "{\"type\":\"sensor\",\"ts\":%llu,\"zone_id\":%d,\"sensor_seq\":%u,",
        (unsigned long long)rec->ts_ms, rec->zone_id, msg->seq);
    if (n >= 0) {
        if (rec->score < 0.0f)
            n = fprintf(fp, "\"score\":null,");
        else
            n = fprintf(fp, "\"score\":%.1f,", rec->score);
    }
    if (n >= 0)
        n = fprintf(fp,
            "\"gas_raw\":%d,\"spark_raw\":%d,"
            "\"temperature\":%.1f,\"humidity\":%.1f,"
            "\"irtemp_ambient\":%.1f,\"irtemp_object\":%.1f,"
            "\"valid\":{\"gas\":%s,\"temphum\":%s,\"spark\":%s,\"irtemp\":%s}}",
            msg->gas_raw, msg->spark_raw,
            msg->temperature, msg->humidity,
            msg->irtemp_ambient, msg->irtemp_object,
            msg->gas_valid ? "true" : "false",
            msg->temphum_valid ? "true" : "false",
            msg->spark_valid ? "true" : "false",
            msg->irtemp_valid ? "true" : "false");
    write_line_end(n);
}

static void write_button_line(const db_record_t *rec)
{
    int n = fprintf(fp,
        "{\"type\":\"button\",\"ts\":%llu,\"zone_id\":%d,\"sensor_seq\":%u,"
        "\"press_count\":%u}",
        (unsigned long long)rec->ts_ms, rec->zone_id,
        rec->u.button.seq, rec->u.button.press_count);

    write_line_end(n);
}

static void write_transition_line(const db_record_t *rec)
{
    int n = fprintf(fp,
        "{\"type\":\"%s\",\"ts\":%llu,\"zone_id\":%d,\"cause\":\"%s\","
        "\"trigger_seq\":%u}",
        (rec->u.trans.ev == DECISION_EVENT_FIRE) ? "fire_confirmed" : "recovered",
        (unsigned long long)rec->ts_ms, rec->zone_id,
        DECISION_CAUSE_STR(rec->u.trans.cause), rec->u.trans.trigger_seq);

    write_line_end(n);
}

/* PHASE 5: fire_event_command에 대응. 백필 로더가 직전 전이 줄과
 * 묶어야 하므로, 줄 순서가 곧 소속 관계라는 점이 JSONL에서도 유지된다
 * (워커가 이벤트 링을 FIFO로 처리하므로 순서가 보장된다). */
static void write_command_line(const db_record_t *rec)
{
    int n;

    if (rec->u.cmd.has_value)
        n = fprintf(fp,
            "{\"type\":\"command\",\"ts\":%llu,\"command\":\"%s\","
            "\"action\":\"%s\",\"value\":%d,\"published_seq\":%u}",
            (unsigned long long)rec->ts_ms, rec->u.cmd.command_key,
            rec->u.cmd.action, rec->u.cmd.value, rec->u.cmd.published_seq);
    else
        n = fprintf(fp,
            "{\"type\":\"command\",\"ts\":%llu,\"command\":\"%s\","
            "\"action\":\"%s\",\"value\":null,\"published_seq\":%u}",
            (unsigned long long)rec->ts_ms, rec->u.cmd.command_key,
            rec->u.cmd.action, rec->u.cmd.published_seq);

    write_line_end(n);
}

static void write_jsonl(const db_record_t *rec)
{
    switch (rec->type) {
    case REC_SENSOR:     write_sensor_line(rec);     break;
    case REC_BUTTON:     write_button_line(rec);     break;
    case REC_TRANSITION: write_transition_line(rec); break;
    case REC_COMMAND:    write_command_line(rec);    break;
    }
}

/* 드롭은 조용히 넘어가면 안 된다. 카운터가 변할 때만 찍으므로, 큐가
 * 계속 넘치는 상황에서도 워커가 느린 만큼 로그도 드물게 나온다. */
static void report_drops(void)
{
    static unsigned long last_sensor, last_event;
    unsigned long s = db_queue_dropped_sensor();
    unsigned long e = db_queue_dropped_event();

    if (s != last_sensor || e != last_event) {
        fprintf(stderr, "db: dropped %lu sensor / %lu event records "
                "(queue full - writer too slow?)\n", s, e);
        last_sensor = s;
        last_event = e;
    }
}

static void *worker(void *arg)
{
    db_record_t rec;

    (void)arg;

    /* pop이 0을 반환하는 시점 = 종료 신호를 받았고 큐도 다 비었을 때.
     * 따라서 이 루프 자체가 "잔여분 flush 후 종료"를 처리한다. */
    while (db_queue_pop(&rec)) {
        /* pg 모드에서 기록이 실패하면(연결 장애든 데이터 오류든) 그대로
         * 버리지 않고 JSONL에 남긴다. 나중에 backfill_jsonl.sql로 넣을
         * 수 있으므로, DB가 몇 시간 죽어 있어도 데이터가 보존된다. */
        if (!use_pg || pg_write_record(&rec) != GUARDX_OK)
            write_jsonl(&rec);
        report_drops();
    }
    return NULL;
}

/* --- 공개 인터페이스 (콜백 스레드에서 호출) --------------------------- */

guardx_err_t db_writer_open(void)
{
    const char *backend = getenv("DB_BACKEND");

    if (fp)
        return GUARDX_OK;

    /* pg 모드에서도 JSONL을 연다 - 폴백 기록처로 쓴다 */
    fp = fopen(DB_WRITER_PATH, "a");
    if (!fp) {
        perror(DB_WRITER_PATH " open");
        return GUARDX_ERR_OPEN;
    }

    use_pg = (backend && strcmp(backend, "pg") == 0);
    printf("db: backend=%s (fallback=%s)\n",
           use_pg ? "pg" : "jsonl", DB_WRITER_PATH);
    if (use_pg)
        pg_writer_open();

    db_queue_init();

    if (pthread_create(&worker_tid, NULL, worker, NULL) != 0) {
        fprintf(stderr, "db: worker thread create failed\n");
        fclose(fp);
        fp = NULL;
        return GUARDX_ERR_OPEN;
    }
    worker_running = 1;
    return GUARDX_OK;
}

void db_writer_close(void)
{
    if (worker_running) {
        /* 순서 중요: 종료 신호 -> join(잔여분이 다 기록될 때까지 대기)
         * -> 그 다음에야 파일을 닫는다. join 전에 fclose하면 워커가
         * 닫힌 FILE*에 쓴다. */
        db_queue_shutdown();
        pthread_join(worker_tid, NULL);
        worker_running = 0;
        report_drops();
    }
    if (use_pg) {
        pg_writer_close();   /* 워커 join 이후 - 워커가 쓰던 연결이다 */
        use_pg = 0;
    }
    if (fp) {
        fclose(fp);
        fp = NULL;
    }
}

guardx_err_t db_write_sensor(const sensor_msg_t *msg, uint64_t timestamp_ms,
                             float score, int zone_id)
{
    db_record_t rec;

    if (!worker_running || !msg)
        return GUARDX_ERR_NOT_OPEN;

    rec.type = REC_SENSOR;
    rec.ts_ms = timestamp_ms;
    rec.zone_id = zone_id;
    rec.score = score;
    rec.u.sensor = *msg;
    db_queue_push(&rec);
    return GUARDX_OK;
}

guardx_err_t db_write_button(const button_msg_t *msg, uint64_t timestamp_ms,
                             int zone_id)
{
    db_record_t rec;

    if (!worker_running || !msg)
        return GUARDX_ERR_NOT_OPEN;

    rec.type = REC_BUTTON;
    rec.ts_ms = timestamp_ms;
    rec.zone_id = zone_id;
    rec.score = -1.0f;
    rec.u.button = *msg;
    db_queue_push(&rec);
    return GUARDX_OK;
}

guardx_err_t db_write_transition(decision_event_t ev, decision_cause_t cause,
                                 uint32_t trigger_seq, uint64_t timestamp_ms,
                                 int zone_id)
{
    db_record_t rec;

    if (!worker_running)
        return GUARDX_ERR_NOT_OPEN;

    rec.type = REC_TRANSITION;
    rec.ts_ms = timestamp_ms;
    rec.zone_id = zone_id;
    rec.score = -1.0f;
    rec.u.trans.ev = ev;
    rec.u.trans.cause = cause;
    rec.u.trans.trigger_seq = trigger_seq;
    db_queue_push(&rec);
    return GUARDX_OK;
}

guardx_err_t db_write_command(const char *command_key, const char *action,
                              int value, bool has_value,
                              uint32_t published_seq, uint64_t timestamp_ms)
{
    db_record_t rec;

    if (!worker_running || !command_key || !action)
        return GUARDX_ERR_NOT_OPEN;

    rec.type = REC_COMMAND;
    rec.ts_ms = timestamp_ms;
    rec.score = -1.0f;
    snprintf(rec.u.cmd.command_key, sizeof(rec.u.cmd.command_key),
             "%s", command_key);
    snprintf(rec.u.cmd.action, sizeof(rec.u.cmd.action), "%s", action);
    rec.u.cmd.value = value;
    rec.u.cmd.has_value = has_value;
    rec.u.cmd.published_seq = published_seq;
    db_queue_push(&rec);
    return GUARDX_OK;
}
