#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

/*
 * GuardX_App_Convention.md: 주기 폴링 대상 센서(gas/temphum/spark)의
 * registry. 비상 버튼은 poll() 이벤트 기반이라 registry에 포함하지
 * 않고 이벤트 루프에서 별도 취급한다.
 * (문서에 명시적 서술은 없으나 devices[] 예시에 button이 빠져 있어
 *  이렇게 해석함 - 팀 확인 필요 시 조정)
 */

#include <stdbool.h>
#include "guardx_hal.h"

/* READ_ALL 결과를 담는 스냅샷. valid[]가 false인 항목의 값은
 * 신뢰할 수 없다(JSON 직렬화 시 "valid" 필드로 그대로 노출). */
typedef struct {
    gas_data_t     gas;
    temphum_data_t temphum;
    spark_data_t   spark;
    irtemp_data_t  irtemp;
    bool           gas_valid;
    bool           temphum_valid;
    bool           spark_valid;
    bool           irtemp_valid;
} sensor_snapshot_t;

/* 전체 초기화. 하나라도 open 실패하면 즉시 실패 반환(App 컨벤션). */
guardx_err_t OPEN_ALL(void);

/* 전체 read. 개별 실패는 전체를 막지 않고 valid=false 처리 후 계속. */
void READ_ALL(sensor_snapshot_t *snap);

/* 전체 해제 */
void CLOSE_ALL(void);

#endif /* DEVICE_REGISTRY_H */
