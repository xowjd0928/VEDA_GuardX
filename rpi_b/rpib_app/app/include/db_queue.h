#ifndef DB_QUEUE_H
#define DB_QUEUE_H

#include <stdint.h>

#include "guardx_err.h"
#include "sensor_parser.h"
#include "decision.h"

/*
 * db_queue - 판단 스레드와 기록 스레드를 잇는 생산자-소비자 큐 (PHASE 3)
 *
 * 목적: DB 기록이 판단을 막지 않게 하는 것. 지금은 JSONL이라 기록이
 * 빠르지만, PHASE 5에서 PostgreSQL로 바뀌면 한 건이 네트워크 왕복 +
 * 디스크 커밋이 되고, DB가 체크포인트 중이거나 연결이 끊기면 수십 초
 * 멈출 수 있다. 그 지연이 콜백 스레드에 그대로 실리면 화재 중 소화
 * 명령까지 늦어진다.
 *
 *   [콜백 스레드]  수신→판단→발행     [워커 스레드]  느린 I/O 전담
 *          │  push() 즉시 반환             ▲  pop() 없으면 잠들어 대기
 *          └────────── 링버퍼 ────────────┘
 *
 * 링이 두 개인 이유:
 *   센서는 1Hz로 계속 오므로 몇 건 잃어도 추세가 남지만, 화재 전이와
 *   버튼은 유일무이하다(버튼은 QoS2로 "정확히 1회"를 보장받아 온 것이라
 *   더더욱). 한 링에 섞으면 센서 폭주가 이벤트를 밀어내므로 분리한다.
 *   대신 뮤텍스와 조건변수는 공유한다 - 워커 하나가 "둘 중 아무거나
 *   들어오면 깨어난다"를 하려면 기다릴 대상이 하나여야 한다.
 *
 * 가득 찼을 때: 가장 오래된 것을 버린다(드롭). 자리가 날 때까지
 * 기다리는 블로킹 방식은 콜백을 다시 막아버려서 이 모듈의 존재 이유가
 * 사라진다. 기록보다 감지가 우선이다. 대신 조용히 잃지 않도록 종류별
 * 드롭 카운터를 세고, 변할 때마다 stderr에 남긴다.
 *
 * 락은 인덱스 몇 개를 갱신하는 동안만 잡는다. 실제 파일/DB I/O는
 * 반드시 락 밖에서 - 안에서 하면 워커가 락을 쥔 채 수십 초를 버텨
 * 콜백이 push에서 막힌다.
 */

/* 실제 적재 가능 개수는 CAP-1이다 (head==tail을 "비었음"으로 쓰므로
 * 가득참과 구별하려면 한 칸을 비워둬야 한다).
 * 센서 256 = 1Hz 기준 약 4분치. DB가 4분간 죽어 있으면 기록 유실보다
 * 큰 문제이므로 더 키울 실익이 없다. */
#define DB_QUEUE_SENSOR_CAP 256
#define DB_QUEUE_EVENT_CAP   32

typedef enum {
    REC_SENSOR = 0,
    REC_BUTTON,
    REC_TRANSITION,
    REC_COMMAND,      /* PHASE 5: 화재/해제 시 실제로 발행한 제어 명령 */
} rec_type_t;

/* 큐에는 "포맷된 문자열"이 아니라 원본 데이터를 넣는다. 문자열로 넣으면
 * PHASE 5에서 SQL INSERT를 만들 때 그 문자열을 다시 파싱해야 한다.
 * 구조체 그대로 옮기면 워커가 JSONL이든 SQL이든 원하는 형태로 뽑는다. */
typedef struct {
    rec_type_t type;
    uint64_t   ts_ms;
    int        zone_id;      /* PHASE 6: REC_SENSOR/BUTTON/TRANSITION 전용.
                              * fire_zone.zone_id - main.c가 도착한 토픽의
                              * node_id로 찾아서 채운다. REC_COMMAND는 필요
                              * 없다(직전 fire_event에 붙어 그쪽 zone_id를
                              * 상속받으므로 - db_writer_pg.c ins_command 참조) */
    float      score;        /* REC_SENSOR 전용. 판단이 점수를 내지 않은
                              * 사이클(FIRE 동결 등)은 음수 - 기록에서
                              * NULL로 구분된다 */
    union {
        sensor_msg_t sensor;
        button_msg_t button;
        struct {
            decision_event_t ev;
            decision_cause_t cause;
            uint32_t         trigger_seq;
        } trans;
        /* REC_COMMAND: fire_event_command 행 하나가 된다.
         * command_key는 guardx_protocol.h의 GUARDX_CMD_* 문자열이자
         * actuator_command.command_key와 동일하므로, ID 매핑을 C에 두지
         * 않고 SQL 서브쿼리로 찾는다 - 매핑표를 양쪽에 두면 어긋난다.
         * 소속 fire_event는 별도로 싣지 않는다: 이벤트 링이 FIFO이고
         * main.c가 전이 기록을 먼저 남긴 뒤 명령을 발행하므로, 워커가
         * 직전에 INSERT한 event_id에 붙이면 정확히 맞는다. */
        struct {
            char     command_key[24];
            char     action[8];        /* "ON" / "OFF" / "SET" */
            int      value;            /* SET일 때 각도·듀티 */
            bool     has_value;        /* false면 DB에 NULL */
            uint32_t published_seq;    /* B가 발행한 seq (역추적용) */
        } cmd;
    } u;
} db_record_t;

/* 인덱스·플래그 초기화. 스레드 생성은 db_writer.c의 몫이다 */
guardx_err_t  db_queue_init(void);

/* 생산자(콜백 스레드)용. 절대 블로킹하지 않는다 - 가득 차면 드롭 */
void          db_queue_push(const db_record_t *rec);

/* 소비자(워커)용. 데이터가 있으면 1을 반환하며 out을 채우고, 비어 있으면
 * 잠들어 기다린다. shutdown 이후 남은 것을 모두 비우고 나서야 0을
 * 반환한다 - 워커의 while 조건으로 그대로 쓰면 "잔여분 flush 후 종료"가
 * 된다. 이벤트 링을 항상 먼저 본다(건수가 적어 센서를 굶기지 않는다). */
int           db_queue_pop(db_record_t *out);

/* 워커를 깨워 종료 절차로 보낸다. 이 시점 이후의 push도 받아들이지만,
 * 남은 것을 다 비운 뒤 pop이 0을 반환하므로 join이 걸리지 않는다 */
void          db_queue_shutdown(void);

unsigned long db_queue_dropped_sensor(void);
unsigned long db_queue_dropped_event(void);

#endif /* DB_QUEUE_H */
