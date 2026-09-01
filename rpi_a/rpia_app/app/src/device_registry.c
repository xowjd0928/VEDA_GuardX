/*
 * device_registry.c - 주기 폴링 센서 registry
 *
 * GuardX_App_Convention.md:
 *  - OPEN_ALL: 하나라도 실패하면 즉시 실패
 *  - READ_ALL: 개별 실패는 valid=false 처리 후 계속 진행
 */

#include <stdio.h>
#include "device_registry.h"

/* open/close는 시그니처가 동일해서 함수 포인터 배열로 처리 가능.
 * read는 디바이스마다 out 타입이 달라 READ_ALL에서 개별 호출한다. */
typedef struct {
    const char   *name;
    guardx_err_t (*open_fn)(void);
    guardx_err_t (*close_fn)(void);
} device_entry_t;

static const device_entry_t devices[] = {
    { "gas",     rpia_gas_open,     rpia_gas_close     },
    { "temphum", rpia_temphum_open, rpia_temphum_close },
    { "spark",   rpia_spark_open,   rpia_spark_close   },
    { "irtemp",  rpia_irtemp_open,  rpia_irtemp_close  },
    /* NOTE: button은 poll 이벤트 기반이라 registry 미포함.
     * 이벤트 루프(main.c)에서 rpia_button_open()을 직접 호출한다. */
};

#define NUM_DEVICES (sizeof(devices) / sizeof(devices[0]))

guardx_err_t OPEN_ALL(void)
{
    size_t i;
    guardx_err_t ret;

    for (i = 0; i < NUM_DEVICES; i++) {
        ret = devices[i].open_fn();
        if (ret != GUARDX_OK) {
            fprintf(stderr, "OPEN_ALL: %s open failed (%d)\n",
                    devices[i].name, ret);
            /* 이미 열린 것들 되감기 */
            while (i > 0) {
                i--;
                devices[i].close_fn();
            }
            return ret;
        }
    }
    return GUARDX_OK;
}

void READ_ALL(sensor_snapshot_t *snap)
{
    snap->gas_valid     = (rpia_gas_read(&snap->gas)         == GUARDX_OK);
    snap->temphum_valid = (rpia_temphum_read(&snap->temphum) == GUARDX_OK);
    snap->spark_valid   = (rpia_spark_read(&snap->spark)     == GUARDX_OK);
    snap->irtemp_valid  = (rpia_irtemp_read(&snap->irtemp)   == GUARDX_OK);

    if (!snap->gas_valid)
        fprintf(stderr, "READ_ALL: gas read failed\n");
    if (!snap->temphum_valid)
        fprintf(stderr, "READ_ALL: temphum read failed\n");
    if (!snap->spark_valid)
        fprintf(stderr, "READ_ALL: spark read failed\n");
    if (!snap->irtemp_valid)
        fprintf(stderr, "READ_ALL: irtemp read failed\n");
}

void CLOSE_ALL(void)
{
    size_t i;

    for (i = 0; i < NUM_DEVICES; i++)
        devices[i].close_fn();
}
