/*
 * guardx_modbus_regs.h - GuardX STM32 Modbus Holding Register 맵 (RPi C 측 사본)
 *
 * "GuardX STM32 <-> Raspberry Pi Modbus RTU 설계 기준서" v0.2 "레지스터 맵"
 * 시트를 옮긴 것. STM32 펌웨어의 rpi_c/stm32/Led_Matrix_Test/Core/Inc/
 * modbus_slave.h 와 반드시 값이 일치해야 한다(양쪽이 같은 표를 본다).
 *
 * 표시 주소(4xxxx) = PDU + 40001. 코드/도구는 PDU 주소만 쓴다.
 */
#ifndef GUARDX_MODBUS_REGS_H
#define GUARDX_MODBUS_REGS_H

/* Slave ID / Broadcast */
#define GX_MODBUS_SLAVE_ID       1
#define GX_MODBUS_BROADCAST_ID   0

/* 기능 코드 */
#define GX_FC_READ_HOLDING       0x03
#define GX_FC_WRITE_SINGLE       0x06
#define GX_FC_WRITE_MULTIPLE     0x10
#define GX_FC_EXCEPTION_FLAG     0x80

/* 예외 코드 */
#define GX_EX_ILLEGAL_FUNCTION   0x01
#define GX_EX_ILLEGAL_ADDRESS    0x02
#define GX_EX_ILLEGAL_VALUE      0x03

/* 수량 제한 */
#define GX_MAX_READ_QTY          125
#define GX_MAX_WRITE_QTY         123

/* Holding Register PDU 주소 (STM32 modbus_slave.h 와 동일) */
enum {
  REG_LED_COMMAND           = 0,    /* 40001 RW 0=OFF 1=ON  (PA4 LED)          */
  REG_PANEL_BRIGHTNESS      = 1,    /* 40002 RW 0~255                          */
  REG_PANEL_REFRESH_LEVEL   = 2,    /* 40003 RW 1(최고)~4(최저)                */
  REG_DEVICE_STATUS         = 3,    /* 40004 RO bit0=UART bit1=HUB75           */
  REG_UPTIME_SECONDS        = 4,    /* 40005 RO s (순환)                       */
  REG_RX_FRAME_COUNT        = 5,    /* 40006 RO frames                         */
  REG_CRC_ERROR_COUNT       = 6,    /* 40007 RO frames                         */
  REG_EXCEPTION_COUNT       = 7,    /* 40008 RO frames                         */
  REG_ZONE1_TEMP_X10        = 100,  /* 40101 RW 0.1℃  (0xFFFF=없음)            */
  REG_ZONE1_HUMIDITY        = 101,  /* 40102 RW %RH   (0x00FF=없음)            */
  REG_ZONE2_TEMP_X10        = 102,  /* 40103                                   */
  REG_ZONE2_HUMIDITY        = 103,  /* 40104                                   */
  REG_ZONE3_TEMP_X10        = 104,  /* 40105                                   */
  REG_ZONE3_HUMIDITY        = 105,  /* 40106                                   */
  REG_ZONE4_TEMP_X10        = 106,  /* 40107                                   */
  REG_ZONE4_HUMIDITY        = 107,  /* 40108                                   */
  REG_FIRE_ZONE_BITMAP       = 120, /* 40121 RW bit0~3 = Zone1~4               */
  REG_INTRUDER_TRACK_STATUS  = 121, /* 40122 RW bit0=A, bit1=B 유효            */
  REG_INTRUDER_CURRENT_X     = 122, /* 40123 RW 현재점 A X, 0~1000             */
  REG_INTRUDER_CURRENT_Y     = 123, /* 40124 RW 현재점 A Y, 0~1000             */
  REG_INTRUDER_DIRECTION_X   = 124, /* 40125 RW 방향점 B X, 0~1000             */
  REG_INTRUDER_DIRECTION_Y   = 125, /* 40126 RW 방향점 B Y, 0~1000             */
  REG_SCREEN_SELECT         = 130,  /* 40131 RW 0=자동 1~3=고정                */
  REG_PROTOCOL_VERSION      = 200   /* 40201 RO 0x0002 = v0.2                  */
};

/* 침입자 좌표 규약: 평면도 좌상단=(0,0), 우하단=(1000,1000). */
#define GX_COORD_MAX                    1000
#define GX_INTRUDER_CURRENT_VALID       (1u << 0)
#define GX_INTRUDER_DIRECTION_VALID     (1u << 1)
#define GX_INTRUDER_BOTH_VALID          (GX_INTRUDER_CURRENT_VALID | \
                                         GX_INTRUDER_DIRECTION_VALID)
#define GX_INTRUDER_TIMEOUT_MS          10000

/* Zone 규약. 개수 4는 임의값이 아니라 레지스터 맵이 못 박은 값이다
 * (100~107 = zone 4개 x 온/습 2칸, 화재 bitmap도 bit0~3). 늘리려면
 * STM32 펌웨어의 레지스터 표부터 늘어나야 한다. */
#define GX_ZONE_COUNT                   4

/* 화재 bitmap: bit0~3 = Zone1~4. STM32 s_regs의 max(15)와 같은 값. */
#define GX_FIRE_ZONE_BITMAP_MAX         ((1u << GX_ZONE_COUNT) - 1u)

/*
 * Zone 온습도 쓰기 허용 범위 - STM32 s_regs의 min/max를 그대로 옮긴 것.
 * 범위를 벗어난 값이 하나라도 끼면 FC10 프레임 전체가 예외 0x03으로
 * 거절되므로(온도만 틀려도 습도까지 안 써진다) RPi 쪽에서 먼저 거른다.
 *
 * !!! "값 없음"은 쓸 수 없다 !!!
 * 리셋 초기값 0xFFFF(온도)/0x00FF(습도)가 곧 "없음" 표시지만, 둘 다
 * 쓰기 허용 범위 밖이라 Modbus로는 그 상태로 되돌릴 방법이 없다. 즉
 * 한 번 실측값을 쓰면 그 zone은 부팅 전까지 계속 숫자를 보여준다.
 * 센서가 죽어도 마지막 값이 남는다는 뜻이라, 발신측(RPi B)은 값이
 * 유효할 때만 발행한다 - 자세한 것은 matrix_link.h 참조.
 */
#define GX_ZONE_TEMP_X10_MAX            65534
#define GX_ZONE_HUMIDITY_MAX            100

#endif /* GUARDX_MODBUS_REGS_H */
