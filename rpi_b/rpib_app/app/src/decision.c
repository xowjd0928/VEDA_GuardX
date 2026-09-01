/*
 * decision.c - 화재 판단 상태머신 구현
 *
 * 순수 로직만 있다 - MQTT도 DB도 시계도 모른다. 입력은 sensor_msg_t
 * 하나, 출력은 이벤트 하나. 그래서 test/의 가짜 센서 주입만으로
 * 전이 전체를 검증할 수 있다.
 */

#include <string.h>

#include "decision.h"

/* PHASE 6: 판정 상태(score_hits/recover_hits/freeze_cycles/relaxed/
 * last_score/state/last_cause/last_trigger_seq)는 더 이상 여기 파일
 * 전역이 아니다 - decision_zone_t(decision.h) 구조체로 옮겨 호출측
 * (main.c)이 zone마다 하나씩 들고 있는다. cfg만 여기 남는다 - 사이트
 * 전체가 같은 임계값 하나를 쓰기 때문(2절 참조). */

void DECISION_ZONE_INIT(decision_zone_t *z)
{
    memset(z, 0, sizeof(*z));
    z->state = DECISION_STATE_NORMAL;
    z->last_cause = DECISION_CAUSE_NONE;
    z->last_score = -1.0f;
}

/* PHASE 4: 런타임 설정. DEFAULT_* 값으로 초기화해둬서 threshold_loader의
 * 최초 로드가 실패해도(DB 연결 불가 등) 화재 감지가 멈추지 않는다 -
 * 로드가 성공하면 DECISION_SET_CONFIG()가 통째로 덮어쓴다. */
static fire_config_t cfg = {
    .gas_raw_min    = DEFAULT_GAS_RAW_MIN,    .gas_raw_max    = DEFAULT_GAS_RAW_MAX,
    .spark_raw_safe = DEFAULT_SPARK_RAW_SAFE, .spark_raw_danger = DEFAULT_SPARK_RAW_DANGER,
    .temp_min_c     = DEFAULT_TEMP_MIN_C,     .temp_max_c     = DEFAULT_TEMP_MAX_C,
    .humi_safe_percent   = DEFAULT_HUMI_SAFE_PERCENT,
    .humi_danger_percent = DEFAULT_HUMI_DANGER_PERCENT,
    .irtemp_min_c   = DEFAULT_IRTEMP_MIN_C,   .irtemp_max_c   = DEFAULT_IRTEMP_MAX_C,
    .weight_gas    = DEFAULT_WEIGHT_GAS,   .weight_spark  = DEFAULT_WEIGHT_SPARK,
    .weight_temp   = DEFAULT_WEIGHT_TEMP,  .weight_humi   = DEFAULT_WEIGHT_HUMI,
    .weight_irtemp = DEFAULT_WEIGHT_IRTEMP,
    .fire_score_threshold = DEFAULT_FIRE_SCORE_THRESHOLD,
    .n_confirm = DEFAULT_N_CONFIRM, .n_recover = DEFAULT_N_RECOVER,
    .min_valid_weight = DEFAULT_MIN_VALID_WEIGHT,
    .override_spark_score  = DEFAULT_OVERRIDE_SPARK_SCORE,
    .override_irtemp_score = DEFAULT_OVERRIDE_IRTEMP_SCORE,
};

/* 퓨전 채널 정의. 아래 함수(ch_weights)와 CH_CAUSE 테이블은 이 인덱스를
 * 공유하므로, 채널을 추가할 때 enum과 두 곳만 같이 고치면 된다 - 이전에는
 * composite_score()와 dominant_cause()가 각자 순서를 가정하고 있어서
 * 한쪽만 고치면 원인 판정이 조용히 어긋났다. */
#define FUSION_CHANNELS 5

enum {
    CH_GAS = 0,
    CH_SPARK,
    CH_TEMP,
    CH_HUMI,
    CH_IRTEMP,
};

/* cfg가 리로드로 바뀔 수 있어 더 이상 static const 배열로 못 박아둘 수
 * 없다 - 호출될 때마다 현재 cfg에서 채운다. */
static void ch_weights(float w[FUSION_CHANNELS])
{
    w[CH_GAS]    = cfg.weight_gas;
    w[CH_SPARK]  = cfg.weight_spark;
    w[CH_TEMP]   = cfg.weight_temp;
    w[CH_HUMI]   = cfg.weight_humi;
    w[CH_IRTEMP] = cfg.weight_irtemp;
}

/* 채널별 유효성. 온도와 습도는 같은 SHT30에서 오므로 플래그를 공유한다.
 * composite_score()와 feed_fire()가 같이 쓴다 - 예전엔 두 곳이 각자
 * 매핑을 들고 있어서 센서를 추가할 때 한쪽만 고치면 조용히 어긋났다. */
static void ch_valid(const sensor_msg_t *msg, bool v[FUSION_CHANNELS])
{
    v[CH_GAS]    = msg->gas_valid;
    v[CH_SPARK]  = msg->spark_valid;
    v[CH_TEMP]   = msg->temphum_valid;
    v[CH_HUMI]   = msg->temphum_valid;
    v[CH_IRTEMP] = msg->irtemp_valid;
}

static const decision_cause_t CH_CAUSE[FUSION_CHANNELS] = {
    [CH_GAS]    = DECISION_CAUSE_GAS,
    [CH_SPARK]  = DECISION_CAUSE_SPARK,
    [CH_TEMP]   = DECISION_CAUSE_TEMP,
    [CH_HUMI]   = DECISION_CAUSE_HUMIDITY,
    [CH_IRTEMP] = DECISION_CAUSE_IRTEMP,
};

/* 오름차순 선형 퍼지화: value<=min -> 0, value>=max -> 100,
 * 구간 내부는 비례 상승 (가스/온도/표면온도 공통) */
static float fuzzify_asc(double value, double min, double max)
{
    if (value <= min)
        return 0.0f;
    if (value >= max)
        return 100.0f;
    return (float)((value - min) / (max - min) * 100.0);
}

/* 내림차순 선형 퍼지화: 값이 작을수록 위험도가 오른다.
 * value>=safe -> 0, value<=danger -> 100, 구간 내부는 비례
 * (습도 - 건조할수록 위험 / 스파크 - 실측상 raw가 낮을수록 불꽃 근접) */
static float fuzzify_desc(double value, double safe, double danger)
{
    if (value >= safe)
        return 0.0f;
    if (value <= danger)
        return 100.0f;
    return (float)((safe - value) / (safe - danger) * 100.0);
}

/* 센서 5종 위험도 산출(퍼지화) + 가중합산해 종합 위험도(0~100) 반환.
 * out_scores에 채널별 원점수(퍼지화 후, 가중치 곱하기 전)를 남겨
 * dominant_cause()/cross_confirm()이 재사용한다.
 *
 * valid=false 채널은 분자(가중합)뿐 아니라 분모(가중치 합)에서도 빠진다
 * - 재정규화. 분자에서만 빼면 그 채널의 가중치만큼 도달 가능한 최대
 * 점수가 깎여서, 센서가 죽을수록 화재를 놓치게 된다(불꽃이 죽으면
 * 나머지 전부 100점이어도 정확히 65점). 분모까지 줄여야 살아있는
 * 채널만으로 100점을 낼 수 있다.
 *
 * 분모에는 MIN_VALID_WEIGHT 하한이 있다 - decision.h 주석 참조. */
static float composite_score(const sensor_msg_t *msg,
                             float out_scores[FUSION_CHANNELS])
{
    bool valid[FUSION_CHANNELS];
    float w[FUSION_CHANNELS];
    float weighted = 0.0f;
    float wsum = 0.0f;
    int i;

    ch_weights(w);
    ch_valid(msg, valid);

    out_scores[CH_GAS]    = fuzzify_asc(msg->gas_raw,
                                        cfg.gas_raw_min, cfg.gas_raw_max);
    out_scores[CH_SPARK]  = fuzzify_desc(msg->spark_raw,
                                         cfg.spark_raw_safe, cfg.spark_raw_danger);
    out_scores[CH_TEMP]   = fuzzify_asc(msg->temperature,
                                        cfg.temp_min_c, cfg.temp_max_c);
    out_scores[CH_HUMI]   = fuzzify_desc(msg->humidity,
                                         cfg.humi_safe_percent, cfg.humi_danger_percent);
    out_scores[CH_IRTEMP] = fuzzify_asc(msg->irtemp_object,
                                        cfg.irtemp_min_c, cfg.irtemp_max_c);

    for (i = 0; i < FUSION_CHANNELS; i++) {
        if (!valid[i]) {
            /* 무효 채널의 원값은 신뢰할 수 없으므로 0으로 덮는다.
             * 원인 판정(dominant_cause)과 교차 확증에서도 함께 빠진다. */
            out_scores[i] = 0.0f;
            continue;
        }
        weighted += out_scores[i] * w[i];
        wsum     += w[i];
    }

    if (wsum < cfg.min_valid_weight)
        wsum = cfg.min_valid_weight;   /* 전 채널 무효(wsum=0)일 때의 0 나눗셈도 여기서 막힌다 */

    return weighted / wsum;
}

/* 교차 확증: 화재의 직접 증거 2종(불꽃 + 표면온도)이 동시에 높으면
 * 가중합산 결과와 무관하게 위험으로 본다. 둘 다 유효해야 성립한다 -
 * 무효 채널은 위에서 0점으로 덮였으므로 점수 비교만으로도 걸러지지만,
 * 의도를 명시적으로 남긴다. */
static bool cross_confirm(const float scores[FUSION_CHANNELS])
{
    return scores[CH_SPARK]  >= cfg.override_spark_score &&
           scores[CH_IRTEMP] >= cfg.override_irtemp_score;
}

/* 이번 사이클이 "위험"인가. NORMAL->FIRE와 FIRE->NORMAL이 모두 이 함수
 * 하나를 쓰므로 두 방향의 조건이 정확히 여집합이 된다(오버라이드로 들어간
 * 화재가 가중합산 기준으로만 해제되는 사고를 구조적으로 차단). */
static bool fire_condition(decision_zone_t *z, const sensor_msg_t *msg,
                           float out_scores[FUSION_CHANNELS])
{
    float total = composite_score(msg, out_scores);

    z->last_score = total;   /* 기록용 부수효과 - 판단에는 영향 없음 */
    return cross_confirm(out_scores) || total >= cfg.fire_score_threshold;
}

/* 종합 점수에 가장 크게 기여한(점수*가중치 최대) 채널을 원인으로 기록.
 * 원값이 커도 가중치가 작으면(예: 습도 0.05) 원인으로 뽑히기 어렵다 -
 * "융합 판단에 실제로 얼마나 기여했는가" 기준이지 "원값이 임계를
 * 넘었는가" 기준이 아니다. */
static decision_cause_t dominant_cause(const float scores[FUSION_CHANNELS])
{
    float w[FUSION_CHANNELS];
    int best = 0;
    int i;

    ch_weights(w);
    for (i = 1; i < FUSION_CHANNELS; i++) {
        if (scores[i] * w[i] > scores[best] * w[best])
            best = i;
    }
    return CH_CAUSE[best];
}

static decision_event_t feed_normal(decision_zone_t *z, const sensor_msg_t *msg)
{
    float scores[FUSION_CHANNELS];

    z->score_hits = fire_condition(z, msg, scores) ? (z->score_hits + 1) : 0;
    if (z->score_hits < cfg.n_confirm)
        return DECISION_EVENT_NONE;

    z->last_cause = dominant_cause(scores);
    z->state = DECISION_STATE_FIRE;
    z->last_trigger_seq = msg->seq;
    z->score_hits = 0;
    z->recover_hits = 0;
    z->freeze_cycles = 0;   /* 새 화재는 동결 이력 없이 시작한다 */
    z->relaxed = false;
    return DECISION_EVENT_FIRE;
}

static decision_event_t feed_fire(decision_zone_t *z, const sensor_msg_t *msg)
{
    float scores[FUSION_CHANNELS];
    bool  valid[FUSION_CHANNELS];
    float w[FUSION_CHANNELS];
    float surviving = 0.0f;   /* 살아있는 채널의 가중치 합 (하한 보정 없는 실값) */
    bool  all_valid = true;
    int   i;

    ch_valid(msg, valid);
    ch_weights(w);
    for (i = 0; i < FUSION_CHANNELS; i++) {
        if (valid[i])
            surviving += w[i];
        else
            all_valid = false;
    }

    /* 해제 조건: 원칙적으로 5채널 전부 유효해야 계산한다. 하나라도
     * invalid면 그 사이클은 해제 카운트에 넣지 않는다(동결) - 센서가
     * 죽은 상태에서 소화 대응을 멈추는 쪽이 더 위험하므로 보수적으로
     * 간다. (NORMAL->FIRE 방향과 의도적으로 비대칭)
     * 다만 영구 고장이면 영원히 못 빠져나오므로 완화 경로를 둔다 -
     * 3단 구조 전체 설명은 decision.h 헤더 주석 참조. */
    if (all_valid) {
        z->freeze_cycles = 0;
        z->relaxed = false;
    } else {
        z->freeze_cycles++;

        /* 완화 조건 두 가지를 모두 만족해야 한다. 시간만으로 풀면
         * 가중치 0.05짜리 습도 하나가 화재를 종료시킬 수 있다. */
        if (z->freeze_cycles < cfg.freeze_relax_cycles ||
            surviving < cfg.min_valid_weight)
            return DECISION_EVENT_NONE;

        if (!z->relaxed) {
            /* 동결 이전의 오래된 관측으로 해제가 성립하면 안 되므로,
             * 완화 후 새로 n_recover 사이클을 채우게 한다. */
            z->relaxed = true;
            z->recover_hits = 0;
        }
    }

    /* NORMAL 방향과 같은 조건 함수의 부정 - 오버라이드로 확정된 화재는
     * 오버라이드가 풀려야 해제된다. 완화 모드에서는 composite_score()의
     * 재정규화가 살아있는 채널만으로 점수를 내준다(무효 채널은 분자·분모
     * 양쪽에서 빠짐). 별도 처리가 필요 없는 이유다. */
    z->recover_hits = fire_condition(z, msg, scores) ? 0 : (z->recover_hits + 1);
    if (z->recover_hits > cfg.n_recover)
        z->recover_hits = cfg.n_recover;   /* 자동 해제가 없으니 무한 증가 방지 */

    /* PHASE 7: 자동 해제를 없앴다. 해제 조건이 충족돼도 상태를 바꾸지 않고,
     * DECISION_RECOVER_READY()로 "이제 꺼도 될 것 같다"는 권고만 낸다.
     * 실제 해제는 사람이 DECISION_FORCE_RECOVER()로 직접 내려야 한다.
     *
     * 이유: 센서가 "안전"이라고 말하는 것과 현장이 실제로 안전한 것은 다르다.
     * 불씨가 남았는데 표면온도만 잠깐 내려가도 여기서 소화를 멈추면 재발화한다.
     * 사람이 현장을 확인하고 내리는 판단을 자동화로 대체하지 않는다.
     *
     * 부수 효과: 이 전까지 헤더에 "!!! 미확정 !!!"으로 남아 있던 "완화가
     * 거부되면 영원히 못 빠져나온다" 문제가 사라진다 - 애초에 자동 경로에
     * 기대지 않으므로 탈출구가 항상 있다(수동 해제). */
    return DECISION_EVENT_NONE;
}

decision_event_t DECISION_FORCE_RECOVER(decision_zone_t *z)
{
    if (z == NULL || z->state != DECISION_STATE_FIRE)
        return DECISION_EVENT_NONE;   /* 이미 NORMAL - 중복 해제는 무시(멱등) */

    z->state = DECISION_STATE_NORMAL;
    z->recover_hits = 0;
    z->freeze_cycles = 0;
    z->relaxed = false;
    return DECISION_EVENT_RECOVER;
}

bool DECISION_RECOVER_READY(const decision_zone_t *z)
{
    return z != NULL && z->state == DECISION_STATE_FIRE &&
           z->recover_hits >= cfg.n_recover;
}

decision_event_t DECISION_FEED(decision_zone_t *z, const sensor_msg_t *msg)
{
    if (z == NULL || msg == NULL)
        return DECISION_EVENT_NONE;

    z->last_score = -1.0f;   /* 이번 사이클에 점수가 안 나올 수도 있다 */
    return (z->state == DECISION_STATE_NORMAL) ? feed_normal(z, msg)
                                               : feed_fire(z, msg);
}

decision_cause_t DECISION_LAST_CAUSE(const decision_zone_t *z)
{
    return z->last_cause;
}

uint32_t DECISION_LAST_TRIGGER_SEQ(const decision_zone_t *z)
{
    return z->last_trigger_seq;
}

const char *DECISION_CAUSE_STR(decision_cause_t c)
{
    switch (c) {
    case DECISION_CAUSE_GAS:      return "gas";
    case DECISION_CAUSE_TEMP:     return "temp";
    case DECISION_CAUSE_SPARK:    return "spark";
    case DECISION_CAUSE_HUMIDITY: return "humidity";
    case DECISION_CAUSE_IRTEMP:   return "irtemp";
    default:                      return "none";
    }
}

decision_state_t DECISION_STATE(const decision_zone_t *z)
{
    return z->state;
}

int DECISION_FREEZE_CYCLES(const decision_zone_t *z)
{
    return z->freeze_cycles;
}

float DECISION_LAST_SCORE(const decision_zone_t *z)
{
    return z->last_score;
}

void DECISION_SET_CONFIG(const fire_config_t *new_cfg)
{
    if (new_cfg)
        cfg = *new_cfg;   /* 통째로 교체 - 콜백 스레드 하나뿐이라 락 불필요 */
}

const fire_config_t *DECISION_GET_CONFIG(void)
{
    return &cfg;
}
