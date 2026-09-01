#ifndef AUDIO_REF_H
#define AUDIO_REF_H

#include <stddef.h>
#include <stdint.h>

#include "guardx_err.h"

/*
 * audio_ref.h - 스피커로 내보내는 소리의 사본을 TOIMIC 에 넘긴다.
 *
 * 규격과 배경은 shared/audio_ref_protocol.h 참조. 요약하면: 감지기가 방송·
 * 사이렌 중에도 비명과 총성을 잡으려면 마이크 신호에서 스피커 소리를 빼야
 * 하고, 빼려면 "무엇이 나갔는지"를 알아야 한다.
 *
 * ── 실패해도 소리는 계속 난다 ──
 * 여기서 일어나는 모든 실패(소켓 생성 실패, 전송 실패)는 조용히 무시된다.
 * 이 모듈은 감지 품질을 위한 부가 경로이고, 재생은 안전 기능이다. 감지기를
 * 위해 사이렌이 끊기는 일은 없어야 한다.
 *
 * 스레드: rpic_audio 의 재생 스레드 하나에서만 부른다는 전제다. 여러
 * 스레드에서 부르면 리샘플 상태가 섞인다.
 */

/* UDP 소켓을 연다. 실패해도 GUARDX_OK 가 아닌 값을 돌려줄 뿐, 이후
 * audio_ref_feed() 는 계속 불러도 안전하다(조용히 버린다). */
guardx_err_t audio_ref_init(void);

/* 재생 직전의 PCM 을 그대로 넘긴다.
 *
 * @param interleaved  S16 인터리브 샘플 (rpic_audio 가 ALSA 에 쓰는 그것)
 * @param frames       프레임 수 (채널 묶음 개수)
 * @param channels     채널 수
 * @param rate_hz      샘플레이트. 48000 만 처리하고 그 외는 버린다 -
 *                     리샘플러를 일반화하면 이 부가 기능이 재생 경로에서
 *                     쓰는 CPU 가 늘어난다.
 *
 * 내부에서 모노 16 kHz 로 낮춰 20 ms 씩 모아 보낸다. */
void audio_ref_feed(const int16_t *interleaved, size_t frames,
                    unsigned int channels, unsigned int rate_hz);

/* 재생이 끝났을 때 부른다. 모아둔 자투리를 버리고 리샘플 상태를 되돌린다 -
 * 다음 재생이 이전 재생의 꼬리에 이어 붙지 않게 한다. */
void audio_ref_flush(void);

void audio_ref_cleanup(void);

#endif /* AUDIO_REF_H */
