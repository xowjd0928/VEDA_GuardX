#ifndef RPIA_BUTTON_H
#define RPIA_BUTTON_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ---------------------------------------------------------------------
 * 비상 버튼 이벤트 데이터.
 * read()는 "마지막 read 이후 발생한 눌림 횟수"를 반환하고 카운터를
 * 리셋한다. App은 poll()로 대기하다가 POLLIN이 오면 read()로 가져간다.
 *
 * 주의: 비상 버튼의 "제어 경로"(RPi A -> RPi C 하드웨어 GPIO/릴레이
 * 인터락)는 이 드라이버/App과 무관하게 유선으로 즉시 동작한다.
 * 이 드라이버는 "로깅 경로"(RPi A -> RPi B, MQTT QoS2) 전용이다.
 * --------------------------------------------------------------------- */
typedef uint32_t button_data_t;   /* 누적 눌림 횟수 (read 시 리셋) */

/* GuardX_Driver_Convention.md 6번: RPi A 매직넘버 0xA?, 버튼은 0xA4 */
#define RPIA_BUTTON_MAGIC   0xA4
#define BUTTON_IOC_READ     _IOR(RPIA_BUTTON_MAGIC, 1, button_data_t)

/* BCM GPIO23 (물리 16번핀).
 * !!! 이 커널은 pinctrl-bcm2711 gpiochip의 base가 512라(실기 확인:
 * gpiochip512 base=512), legacy gpio_request/gpio_to_irq에 넘길 전역
 * GPIO 번호는 BCM 번호 + 512 이다. (예: BCM23 -> 535) !!!
 * 회로: 내부 풀업 + 눌림 시 GND(=falling edge). 배선이 반대(풀다운+
 * 눌림 시 VCC)면 드라이버 IRQF_TRIGGER_RISING으로 변경. */
#define RPIA_BUTTON_GPIO_BASE   512
#define RPIA_BUTTON_GPIO        (23 + RPIA_BUTTON_GPIO_BASE)

#endif /* RPIA_BUTTON_H */
