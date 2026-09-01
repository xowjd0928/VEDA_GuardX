#ifndef RPIC_AMP_H
#define RPIC_AMP_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ---------------------------------------------------------------------
 * 실제 하드웨어: LM386 오디오 앰프 모듈 + 스피커
 *
 * 오디오 신호 자체는 RPi 3.5mm 잭 -> LM386 입력으로 가는 아날로그
 * 경로이며 이 드라이버 범위 밖이다 (재생은 ALSA/aplay 등 별도 경로,
 * 프로토콜 규약 4-3의 amp 항목 비고와 동일).
 *
 * 이 드라이버는 프로토콜의 `amp` ON/OFF 명령에 대응하는 전원/뮤트
 * 제어만 담당한다. LM386 모듈에는 enable 핀이 없으므로 모듈 전원
 * 라인을 트랜지스터(로우사이드 스위치)로 끊고 잇는 구성을 전제로
 * 하며, GPIO는 그 트랜지스터 게이트에 연결된다.
 *  - GPIO=H : 앰프 전원 ON (소리 남)
 *  - GPIO=L : 앰프 전원 OFF (뮤트)
 * --------------------------------------------------------------------- */
typedef __s32 amp_data_t;   /* 0=OFF(뮤트), 1=ON */

/* ---------------------------------------------------------------------
 * GuardX_Driver_Convention.md 6번 참조 (RPi C는 0xC1부터)
 * --------------------------------------------------------------------- */
#define RPIC_AMP_MAGIC  0xC3
#define AMP_IOC_SET     _IOW(RPIC_AMP_MAGIC, 1, amp_data_t)
#define AMP_IOC_GET     _IOR(RPIC_AMP_MAGIC, 2, amp_data_t)

/* GPIO 핀 (하드코딩, 전원 스위칭 트랜지스터 게이트)
 * !!! 임의값 - 실물 배선 후 확정 (RPi A와 동일한 지위) !!! */
#define RPIC_AMP_GPIO  25

#endif /* RPIC_AMP_H */
