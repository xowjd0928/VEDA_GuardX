/*
 * threshold_loader.c - fire_threshold 테이블 읽기 + decision.c 런타임
 * 설정 갱신 (PHASE 4)
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <libpq-fe.h>

#include "threshold_loader.h"
#include "decision.h"

#define SELECT_ACTIVE_SQL \
    "SELECT gas_raw_min, gas_raw_max, spark_raw_safe, spark_raw_danger, " \
    "temp_min_c, temp_max_c, humi_safe_percent, humi_danger_percent, " \
    "irtemp_min_c, irtemp_max_c, weight_gas, weight_spark, weight_temp, " \
    "weight_humi, weight_irtemp, fire_score_threshold, n_confirm, " \
    "n_recover, min_valid_weight, override_spark_score, override_irtemp_score, " \
    "freeze_relax_cycles " \
    "FROM fire_threshold WHERE is_active LIMIT 1"

/* fire_schema.sql의 CHECK 제약과 동일한 조건을 여기서 다시 검사한다.
 * DB가 이미 강제하므로 원칙적으로는 중복이지만, 화재 판단 임계값이라
 * "DB와 코드가 어떤 이유로든 어긋난 상태"를 그대로 믿지 않는 쪽을
 * 택했다 - 검증 비용보다 오판 비용이 훨씬 크다. */
static int validate(const fire_config_t *c)
{
    float wsum = c->weight_gas + c->weight_spark + c->weight_temp +
                 c->weight_humi + c->weight_irtemp;

    if (c->gas_raw_min   >= c->gas_raw_max)   return 0;
    if (c->spark_raw_safe <= c->spark_raw_danger) return 0;
    if (c->temp_min_c    >= c->temp_max_c)    return 0;
    if (c->irtemp_min_c  >= c->irtemp_max_c)  return 0;
    if (c->humi_safe_percent <= c->humi_danger_percent) return 0;
    if (fabsf(wsum - 1.0f) > 0.01f) return 0;
    if (c->n_confirm <= 0 || c->n_recover <= 0) return 0;
    if (c->freeze_relax_cycles <= 0) return 0;
    if (c->fire_score_threshold < 0.0f || c->fire_score_threshold > 100.0f)
        return 0;
    if (c->override_spark_score  < 0.0f || c->override_spark_score  > 100.0f)
        return 0;
    if (c->override_irtemp_score < 0.0f || c->override_irtemp_score > 100.0f)
        return 0;
    if (c->min_valid_weight <= 0.0f || c->min_valid_weight > 1.0f)
        return 0;
    return 1;
}

static void fill_config(PGresult *res, fire_config_t *c)
{
    c->gas_raw_min           = (float)atof(PQgetvalue(res, 0, 0));
    c->gas_raw_max           = (float)atof(PQgetvalue(res, 0, 1));
    c->spark_raw_safe        = (float)atof(PQgetvalue(res, 0, 2));
    c->spark_raw_danger      = (float)atof(PQgetvalue(res, 0, 3));
    c->temp_min_c            = (float)atof(PQgetvalue(res, 0, 4));
    c->temp_max_c            = (float)atof(PQgetvalue(res, 0, 5));
    c->humi_safe_percent     = (float)atof(PQgetvalue(res, 0, 6));
    c->humi_danger_percent   = (float)atof(PQgetvalue(res, 0, 7));
    c->irtemp_min_c          = (float)atof(PQgetvalue(res, 0, 8));
    c->irtemp_max_c          = (float)atof(PQgetvalue(res, 0, 9));
    c->weight_gas            = (float)atof(PQgetvalue(res, 0, 10));
    c->weight_spark          = (float)atof(PQgetvalue(res, 0, 11));
    c->weight_temp           = (float)atof(PQgetvalue(res, 0, 12));
    c->weight_humi           = (float)atof(PQgetvalue(res, 0, 13));
    c->weight_irtemp         = (float)atof(PQgetvalue(res, 0, 14));
    c->fire_score_threshold  = (float)atof(PQgetvalue(res, 0, 15));
    c->n_confirm              = atoi(PQgetvalue(res, 0, 16));
    c->n_recover               = atoi(PQgetvalue(res, 0, 17));
    c->min_valid_weight       = (float)atof(PQgetvalue(res, 0, 18));
    c->override_spark_score   = (float)atof(PQgetvalue(res, 0, 19));
    c->override_irtemp_score  = (float)atof(PQgetvalue(res, 0, 20));
    c->freeze_relax_cycles    = atoi(PQgetvalue(res, 0, 21));
}

guardx_err_t threshold_load_and_apply(void)
{
    PGconn   *conn;
    PGresult *res;
    fire_config_t c;

    /* 빈 문자열 -> libpq가 PG* 환경변수로 접속 정보를 채운다.
     * (systemd 배포 시 EnvironmentFile로 그대로 주입 가능) */
    conn = PQconnectdb("");
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "threshold: connect failed: %s",
                PQerrorMessage(conn));
        PQfinish(conn);
        return GUARDX_ERR_OPEN;
    }

    res = PQexec(conn, SELECT_ACTIVE_SQL);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1) {
        fprintf(stderr, "threshold: query failed or no active row: %s",
                PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
        return GUARDX_ERR_READ;
    }

    fill_config(res, &c);
    PQclear(res);
    PQfinish(conn);

    if (!validate(&c)) {
        fprintf(stderr, "threshold: loaded row failed validation, "
                "keeping previous config\n");
        return GUARDX_ERR_INVALID;
    }

    DECISION_SET_CONFIG(&c);
    printf("threshold: applied (fire_score>=%.1f OR spark>=%.1f&irtemp>=%.1f, "
           "confirm=%d recover=%d relax=%d cycles)\n",
           c.fire_score_threshold, c.override_spark_score,
           c.override_irtemp_score, c.n_confirm, c.n_recover,
           c.freeze_relax_cycles);
    return GUARDX_OK;
}
