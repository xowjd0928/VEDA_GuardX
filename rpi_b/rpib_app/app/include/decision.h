#ifndef DECISION_H
#define DECISION_H

#include <stdbool.h>
#include "sensor_parser.h"

/*
 * 화재 판단 상태머신 (퍼지 가중치 다중 센서 융합 + 연속 카운트 기반).
 *
 * 설계 원칙:
 *  - 센서 1사이클(1Hz) 단위로 DECISION_FEED()에 먹인다. 판단은 전부
 *    여기 안에서만 일어나고, MQTT/DB를 전혀 모른다 (단위 테스트 가능).
 *  - PHASE 2: 센서 5종(gas/spark/temperature/humidity/irtemp_object)을
 *    각각 0~100 "위험도 점수"로 정규화(퍼지화)한 뒤 가중합산해 "종합
 *    위험도" 1개를 낸다. 단일 지표 하나가 튀어도 다른 지표가 낮으면
 *    화재로 확정되지 않는다 - 단일 센서 오작동에 덜 취약함.
 *    irtemp_ambient(주변온도)는 판단 입력에서 제외한다(기록만) - 화재
 *    발화부의 "표면온도"는 irtemp_object가 대표하므로 중복 지표를
 *    가중치에 넣지 않는다.
 *  - "순간 스파이크 1번"으로 소화 시나리오를 발동하지 않도록, 종합
 *    위험도가 N_CONFIRM 사이클 연속 임계값을 넘어야 화재 확정
 *    (히스테리시스는 PHASE 1과 동일 철학 유지, 대상만 개별 지표에서
 *    종합 점수로 바뀜).
 *  - 가중합산만으로는 "확실한 증거"를 놓친다. 단독 최대 기여도가
 *    불꽃 35 / 표면온도 25뿐이라, 눈앞에 불꽃이 타고 표면이 80도를
 *    넘어도 35+25=60점으로 임계(65)에 못 미친다. 그래서 가중합산을
 *    우회하는 교차 확증(OVERRIDE) 경로를 따로 둔다 - 아래 참조.
 *  - valid=false 정책이 방향별로 다르다(의도적 비대칭):
 *      NORMAL->FIRE 판정: 무효 채널을 분모(가중치 합)에서도 빼서
 *        재정규화한다. 즉 살아있는 채널만으로도 0~100 전 구간을 낼
 *        수 있다. 무효 채널을 "위험도 0"으로만 처리하면 그 가중치만큼
 *        감지 능력이 통째로 사라져서(불꽃이 죽으면 -35점) 오히려
 *        화재를 놓치므로, 분모까지 줄여야 진짜 안전 우선이 된다.
 *        단, 분모에 MIN_VALID_WEIGHT 하한을 둔다 - 그렇지 않으면
 *        가중치 0.05짜리 습도 하나만 살아남았을 때 습도 100점이
 *        곧바로 종합 100점이 되어 단일 센서가 화재를 결정해 버린다.
 *      FIRE->NORMAL 판정: 5채널 중 하나라도 무효면 그 사이클은 해제
 *        카운트에 넣지 않는다(동결). 센서가 죽은 상태에서 소화 대응을
 *        멈추는 쪽이 더 위험하므로 보수적으로 간다. 단 무한 동결은
 *        막는다 - 아래 3단 구조 참조.
 *
 * 동결 3단 구조 (영구 잠금 방지):
 *   1단 일시 동결 : 무효 발생. 해제 카운트를 리셋이 아니라 "보존"하므로
 *      센서가 깜빡거려도 유효 사이클에서 계속 누적된다.
 *   2단 지속 동결 : n_recover 사이클 넘게 동결이 이어지면 경고(로그).
 *      판정은 그대로 멈춰 있고, 상태만 밖에서 보이게 한다.
 *   3단 완화      : freeze_relax_cycles 초과 AND 살아있는 채널의 가중치
 *      합 >= min_valid_weight 일 때만, 살아있는 채널만으로 해제 판정을
 *      재개한다.
 *
 *   3단의 AND가 핵심이다. 시간만으로 완화하면 가중치 0.05짜리 습도
 *   센서 하나가 화재를 종료시킬 수 있다. 생존 가중치 게이트가 그걸
 *   막고, 동시에 의미 있는 구분을 만든다:
 *     가스만 사망(생존 0.80)          -> 완화 허용 (나머지 4채널이 판단)
 *     불꽃+표면온도 사망(생존 0.40)   -> 완화 거부, 계속 동결
 *   후자가 바로 "화재가 직접 증거 센서를 태워먹은" 패턴이라, 그 경우엔
 *   여전히 사람이 개입해야 한다. 이 구분이 이 설계의 전부다.
 *
 *   완화에 진입하는 순간 해제 카운터를 리셋한다 - 동결 이전의 오래된
 *   관측으로 해제가 성립하면 안 되므로 새로 n_recover를 채우게 한다.
 *
 *   PHASE 7 이후 이 3단 구조는 "해제 권고를 언제 띄울지"만 정한다 - 실제
 *   해제 권한이 사람에게 있으므로, 완화가 거부돼 권고가 영영 안 떠도
 *   갇히지 않는다(수동 해제는 권고와 무관하게 언제든 가능). 예전 여기
 *   있던 "탈출 경로 없음" 미확정 항목은 그래서 해소됐다.
 *
 *   !!! 별건: 엔진 재시작 시 state가 NORMAL로 리셋되는데 C의 액추에이터는
 *   켜진 채로 남는다(B는 FIRE->NORMAL 전이를 안 하므로 OFF를 안 보냄).
 *   상태 영속화가 필요하며 별도 항목으로 분리 !!!
 *
 * 상태 전이 (아래 "위험 조건" = 가중합산 초과 OR 교차 확증 성립):
 *
 *   NORMAL ──(위험 조건이 N_CONFIRM 사이클 연속 성립)──→ FIRE
 *   FIRE   ──(사람이 수동 해제)────────────────────────→ NORMAL (RECOVER 이벤트)
 *
 * !!! PHASE 7: 자동 해제를 제거했다 !!!
 * 전에는 "위험 조건이 N_RECOVER 사이클 연속 불성립"이면 스스로 NORMAL로
 * 돌아갔지만, 이제 그 조건은 "해제 권고"(DECISION_RECOVER_READY)일 뿐이고
 * 실제 전이는 DECISION_FORCE_RECOVER()를 통해서만 일어난다. 불씨가 남은
 * 상태에서 표면온도만 잠깐 떨어져도 소화를 멈추면 재발화하기 때문이다.
 *
 * 두 방향 모두 같은 "위험 조건" 함수를 쓴다(정확히 여집합). 오버라이드로
 * 진입한 화재를 가중합산 기준으로만 판단하면, 불꽃이 계속 타는 중에
 * 종합 점수 60점이라는 이유로 "해제 권고"가 뜨는 사고가 난다.
 *
 * 비상 버튼은 여기 없다 - 규약 4-2대로 버튼의 실제 제어는 하드웨어
 * 인터락(RPi A→C 직결)이 이미 처리했고 MQTT 경로는 로깅 전용이므로,
 * 판단 로직의 입력이 아니다.
 */

/* --- PHASE 4: 런타임 설정 구조체 ---------------------------------------
 * 아래 값들은 더 이상 컴파일타임 상수가 아니다. fire_threshold 테이블의
 * is_active=true 행을 threshold_loader.c가 읽어 DECISION_SET_CONFIG()로
 * 주입하고, decision.c는 static 변수 하나(cfg)로 들고 있다가 매 판정마다
 * 참조한다. 필드명은 DB 컬럼명과 동일 - sensor_msg_t/DB 매핑과 같은
 * 이유(변환 없는 1:1 대응)다.
 *
 * 콜백 스레드 하나가 판정과 리로드를 모두 처리하므로(main.c 참조) 이
 * 구조체 교체에 락이 필요 없다 - 둘이 동시에 도는 경우가 구조적으로
 * 없다.
 *
 * PHASE 3에서 DB 기록 워커 스레드가 추가되며 이 전제를 재검토했고,
 * 결론은 "유지"다:
 *   - 워커는 decision.c를 전혀 호출하지 않는다. 큐에서 꺼낸 레코드를
 *     포맷해 쓸 뿐이다.
 *   - DECISION_LAST_SCORE()는 콜백 스레드가 호출해 레코드에 값으로
 *     복사해 넣는다. 워커가 나중에 읽는 구조가 아니다.
 *   => cfg를 만지는 스레드는 여전히 콜백 하나.
 * !!! 앞으로 스레드를 추가할 때마다 이 검토를 다시 할 것. 컴파일러가
 *     잡아주지 않는 종류의 전제다 !!! */
typedef struct {
    float gas_raw_min,    gas_raw_max;      /* MQ-2 raw (0~1023), 오름차순 */
    float spark_raw_safe, spark_raw_danger; /* TS0226 raw (0~1023), 내림차순 -
                                              * 실측 결과 불꽃 근접 시 raw가
                                              * 낮아지는 포토트랜지스터형이라
                                              * 습도와 같은 방향(safe>danger) */
    float temp_min_c,     temp_max_c;       /* SHT30 대기 온도 */
    float humi_safe_percent, humi_danger_percent; /* 내림차순 퍼지화 */
    float irtemp_min_c,   irtemp_max_c;     /* MLX90614 표면온도 */

    float weight_gas, weight_spark, weight_temp, weight_humi, weight_irtemp;

    float fire_score_threshold;  /* 종합 위험도 임계 (0~100) */
    int   n_confirm;             /* 화재 확정 연속 초과 사이클 */
    int   n_recover;             /* 해제 연속 정상 사이클 */
    int   freeze_relax_cycles;   /* 이만큼 동결되면 완화 검토 */

    float min_valid_weight;      /* 재정규화 분모 하한 + 완화 허용 하한 */
    float override_spark_score;  /* 교차 확증 임계 (불꽃) */
    float override_irtemp_score; /* 교차 확증 임계 (표면온도) */
} fire_config_t;

/* --- 컴파일타임 폴백 기본값 --------------------------------------------
 * DB를 아예 못 읽는 상태(최초 기동 시 연결 실패 등)로 시작해도 화재
 * 감지 자체가 멈추지 않도록, decision.c는 이 값들로 cfg를 초기화해둔다.
 * threshold_loader.c가 성공적으로 로드하면 이 값들은 즉시 덮어써진다.
 * !!! 전부 잠정치 - 실측 벤치마킹(로드맵 마지막 단계) 후 DB 값을 조정 !!! */
/* 실측(2026-07-31): 평시 500~600(700 안 넘음), 장시간 노출 시 780~790
 * 포화(한 번 800). MIN을 평시 상한보다 여유 있게 잡아 정상 변동이 0점에
 * 묶이게 하고, MAX는 실측 포화점과 거의 일치해 그대로 둔다. */
#define DEFAULT_GAS_RAW_MIN          650.0f
#define DEFAULT_GAS_RAW_MAX          800.0f
/* 실측(2026-07-31): 불꽃 없을 때 900~1000, 라이터 근접 시 2~3까지 하강.
 * 극단치를 그대로 쓰면 위험하다 - SAFE는 평상시 하한보다 낮춰 ADC
 * 노이즈 여유를 두고, DANGER는 실측 최저(2~3)보다 높여서 약한/먼
 * 불꽃도 100점 근처를 받을 수 있게 여유를 둔다. */
#define DEFAULT_SPARK_RAW_SAFE       850.0f
#define DEFAULT_SPARK_RAW_DANGER     30.0f
#define DEFAULT_TEMP_MIN_C           35.0f
#define DEFAULT_TEMP_MAX_C           60.0f
#define DEFAULT_HUMI_SAFE_PERCENT    50.0f
#define DEFAULT_HUMI_DANGER_PERCENT  15.0f
#define DEFAULT_IRTEMP_MIN_C         40.0f
#define DEFAULT_IRTEMP_MAX_C         80.0f

/* 신뢰도·반응속도 순: 불꽃(직접 증거) > 표면온도 > 가스 > 대기온도 > 습도 */
#define DEFAULT_WEIGHT_SPARK   0.35f
#define DEFAULT_WEIGHT_IRTEMP  0.25f
#define DEFAULT_WEIGHT_GAS     0.20f
#define DEFAULT_WEIGHT_TEMP    0.15f
#define DEFAULT_WEIGHT_HUMI    0.05f

#define DEFAULT_FIRE_SCORE_THRESHOLD  65.0f
#define DEFAULT_N_CONFIRM             3
#define DEFAULT_N_RECOVER             10

/* 60사이클 = 60초. 일시적 통신 유실이나 센서 리트라이는 이보다 훨씬
 * 짧으므로 완화가 발동하지 않고, 그렇다고 몇 시간씩 갇혀 있지도 않은
 * 절충점. 벤치마킹에서 실제 센서 결측 지속시간 분포를 보고 조정할 것.
 * (핫리로드 대상이라 검증 시엔 5 정도로 낮춰 쓰면 테스트가 빠르다) */
#define DEFAULT_FREEZE_RELAX_CYCLES   60

/* 유효 채널 가중치 합이 이 값보다 작아도 분모는 이 값으로 고정한다.
 * 목적은 "센서 대부분이 죽었을 때 남은 소수 채널이 종합 점수를 독점하는
 * 것"을 막는 것 (예: 습도만 살아남으면 5/0.05 = 100점).
 * 부수 효과로 전 채널 무효(가중치 합 0)일 때의 0 나눗셈도 막는다.
 *   하한 0.50 적용 시 단독 생존 채널의 최대 종합 점수:
 *     불꽃 0.35/0.50 -> 70 (임계 초과, 의도적 - 직접 증거이므로)
 *     표면온도 0.25/0.50 -> 50, 가스 0.20/0.50 -> 40,
 *     온도 0.15/0.50 -> 30, 습도 0.05/0.50 -> 10  (전부 임계 미만) */
#define DEFAULT_MIN_VALID_WEIGHT      0.50f

/* 불꽃과 표면온도가 "동시에" 이 점수 이상이면 종합 점수와 무관하게
 * 화재로 본다. 두 지표를 고른 이유:
 *   - 화재의 직접 증거 2종이고 물리적으로 독립이다 (광학 IR 시그니처 +
 *     복사 표면온도). 하나가 오작동해도 둘이 동시에 틀릴 확률은 낮다.
 *   - 가스/대기온도는 조리·난방으로 흔히 오르므로 오버라이드에서 뺐다.
 * 단독 불꽃 오버라이드를 쓰지 않는 이유: TS0226은 적외선 감지라
 * 직사광선에 포화된다. 창가 햇빛만으로 화재가 확정되면 안 된다.
 * (실측 벤치마킹에서 주변광 내성이 확인되면 단독 규칙 추가 검토)
 * !!! 오버라이드도 n_confirm 연속 성립을 요구한다 - 우회하는 것은
 *     가중합산이지 시간 히스테리시스가 아니다 !!! */
#define DEFAULT_OVERRIDE_SPARK_SCORE   70.0f
#define DEFAULT_OVERRIDE_IRTEMP_SCORE  70.0f

typedef enum {
    DECISION_STATE_NORMAL = 0,
    DECISION_STATE_FIRE   = 1,
} decision_state_t;

/* 화재 확정의 원인 지표 (DB 기록 + 명령 payload 역추적 필드용).
 * PHASE 2: 종합 점수에 가장 크게 기여한(점수*가중치 최대) 채널로 정한다
 * - 개별 지표가 임계를 "넘었는가"가 아니라 "융합 판단에 가장 크게
 * 기여했는가" 기준이라는 점에 주의 (가중치가 작으면 원값이 커도 원인이
 * 되기 어려움 - 예: 습도는 가중치 0.05라 거의 원인으로 뽑히지 않음).
 * decision_zone_t가 이 타입을 필드로 쓰므로 그보다 먼저 선언해야 한다. */
typedef enum {
    DECISION_CAUSE_NONE     = 0,
    DECISION_CAUSE_GAS      = 1,
    DECISION_CAUSE_TEMP     = 2,
    DECISION_CAUSE_SPARK    = 3,
    DECISION_CAUSE_HUMIDITY = 4,
    DECISION_CAUSE_IRTEMP   = 5,
} decision_cause_t;

/*
 * PHASE 6: zone별 판정 상태.
 *
 * 이전엔 이 필드들이 decision.c의 파일 전역 static이었다 - "센서 노드가
 * 하나뿐"이라는 당시 하드웨어 제약을 그대로 코드 구조에 반영한 것.
 * zone(=RPi A/C 물리 쌍)이 여러 개가 될 수 있게 되면서, 판정 상태를
 * zone마다 독립으로 들고 있어야 한다 - 한 zone의 화재가 다른 zone의
 * 연속 카운터를 밟으면 안 되기 때문이다.
 *
 * cfg(fire_threshold)는 여기 포함하지 않는다 - 지금은 zone과 무관하게
 * 사이트 전체가 같은 임계값 하나를 쓴다(SETTINGS 화면도 zone 개념이
 * 없다). zone별 임계값이 필요해지면 그때 cfg도 zone 배열로 옮기면 된다.
 *
 * main.c가 이 구조체를 zone 개수만큼 배열로 들고 있다가(MAX_FIRE_ZONES
 * 상한, zone_loader.c 참조) DECISION_FEED() 등에 해당 zone의 포인터를
 * 넘긴다. decision.c는 여전히 "MQTT/DB를 모르는 순수 로직"이라 이 구조체
 * 안의 값 외에는 아무것도 참조하지 않는다(단위 테스트 가능성 유지).
 */
typedef struct {
    decision_state_t state;
    decision_cause_t last_cause;
    uint32_t         last_trigger_seq;

    int   score_hits;      /* NORMAL 상태에서 위험 조건 연속 성립 횟수 */
    int   recover_hits;    /* FIRE 상태에서 위험 조건 연속 불성립 횟수 */
    int   freeze_cycles;   /* FIRE 상태에서 센서 무효로 동결된 연속 사이클 */
    bool  relaxed;         /* 완화 모드 진입 여부 (freeze_relax_cycles 초과) */
    float last_score;      /* 직전 FEED의 종합 위험도. 음수=계산 안 함 */
} decision_zone_t;

/* zone 하나를 갓 로드했을 때(부팅 시, 또는 신규 zone 핫추가 시) 초기
 * 상태로 되돌린다 - NORMAL, 카운터 전부 0. */
void DECISION_ZONE_INIT(decision_zone_t *z);

/* FEED 1회의 결과. NONE이 대부분이고, 상태가 "바뀌는 순간"에만
 * FIRE/RECOVER가 한 번 나온다 (엣지 트리거 - 명령 중복 발행 방지). */
typedef enum {
    DECISION_EVENT_NONE    = 0,
    DECISION_EVENT_FIRE    = 1,   /* 화재 확정 - 대응 시나리오 발동 */
    DECISION_EVENT_RECOVER = 2,   /* 상황 해제 - 사후 조치 */
} decision_event_t;

/* 센서 1사이클 판정. z는 그 사이클이 속한 zone의 상태 - 호출측이 어느
 * zone의 메시지인지 이미 알고 있어야 한다(node_id -> zone_id 매핑,
 * zone_loader.c). 반환이 FIRE면 호출측이 대응할 것.
 *
 * PHASE 7부터 이 함수는 RECOVER를 절대 반환하지 않는다 - 해제는 오직
 * DECISION_FORCE_RECOVER()로만 일어난다(아래 참조). */
decision_event_t DECISION_FEED(decision_zone_t *z, const sensor_msg_t *msg);

/* PHASE 7: 수동 화재 해제. FIRE -> NORMAL 전이를 사람이 직접 내린다.
 *
 * 자동 해제를 없앤 이유는 decision.c feed_fire()의 주석 참조 - 요약하면
 * "센서가 안전이라고 말하는 것"과 "현장이 실제로 안전한 것"이 다르기
 * 때문이다. 재발화 위험을 자동 판정에 맡기지 않는다.
 *
 * 반환값은 DECISION_FEED와 같은 규약이다 - RECOVER면 호출측이 해제
 * 시나리오(펌프/팬/경보 정지)와 DB 기록·VMS 알림을 평소대로 수행하면 된다.
 * 이미 NORMAL인 zone에 대한 호출은 NONE을 반환한다(멱등 - 중복 요청이
 * 와도 해제 시나리오가 두 번 돌지 않는다). */
decision_event_t DECISION_FORCE_RECOVER(decision_zone_t *z);

/* "지금 해제해도 될 것 같다"는 권고 (FIRE 상태에서 해제 조건이 n_recover
 * 사이클 연속 충족됨). 해제를 자동으로 하지는 않되, 운영자가 판단할 근거는
 * 줘야 하므로 상태만 노출한다 - VMS의 수동 해제 버튼이 이 값으로 "해제
 * 권장" 표시를 띄운다. NORMAL 상태에서는 항상 false. */
bool DECISION_RECOVER_READY(const decision_zone_t *z);

/* 해당 zone의 마지막 FIRE 이벤트 원인/트리거 시점 seq (RPi A의 seq) */
decision_cause_t DECISION_LAST_CAUSE(const decision_zone_t *z);
uint32_t         DECISION_LAST_TRIGGER_SEQ(const decision_zone_t *z);
const char      *DECISION_CAUSE_STR(decision_cause_t c);   /* zone 무관 - 순수 변환 */

decision_state_t DECISION_STATE(const decision_zone_t *z);

/* FIRE 상태에서 센서 무효로 해제 판정이 멈춰 있는 연속 사이클 수
 * (정상이면 0). decision.c는 시계도 로깅도 모르는 순수 로직이라 여기서
 * 직접 경고를 찍지 않고, main.c가 이 값을 보고 로그를 남긴다. */
int              DECISION_FREEZE_CYCLES(const decision_zone_t *z);

/* 직전 DECISION_FEED()의 종합 위험도(0~100). 판단이 점수를 내지 않은
 * 사이클(FIRE 상태 동결)은 음수를 반환한다 - "0점"과 "계산 안 함"은
 * 전혀 다른 의미이므로 구분한다.
 * 기록·벤치마킹 전용이다. 실측 임계값 튜닝을 하려면 "이 센서값 조합이
 * 몇 점이었나"를 사후에 SQL로 조회할 수 있어야 하는데, 지금까지는
 * 점수가 계산되고 그대로 버려졌다. */
float            DECISION_LAST_SCORE(const decision_zone_t *z);

/* PHASE 4: 런타임 설정 교체/조회. threshold_loader.c가 SET을,
 * main.c(배너 로그)와 threshold_loader.c(리로드 로그)가 GET을 쓴다.
 * SET은 유효성 검증을 하지 않는다 - 호출측(threshold_loader.c)이 DB의
 * CHECK 제약과 동일한 검증을 먼저 통과시킨 값만 넘기는 걸 전제로 한다. */
void                    DECISION_SET_CONFIG(const fire_config_t *cfg);
const fire_config_t   *DECISION_GET_CONFIG(void);

#endif /* DECISION_H */
