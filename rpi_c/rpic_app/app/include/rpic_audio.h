#ifndef RPIC_AUDIO_H
#define RPIC_AUDIO_H

/*
 * rpic_audio.h - GuardX RPi C I2S 오디오 재생 모듈 (MAX98357A / ALSA)
 *
 * MAX98357A는 I2S 입력을 받는 디지털 앰프다(부팅 오버레이
 * dtoverlay=max98357a 로 ALSA 카드로 잡힘 - RUN.md 참조). 소리 자체는
 * 커널 드라이버가 아니라 ALSA(libasound) 유저스페이스 경로로 낸다.
 * 검증된 max98357a_i2s_test.c 와 동일한 설정(S16_LE / 48kHz / 스테레오)을
 * 쓰되, "톤 생성 / raw PCM / WAV 파일"을 분리해 추후 파일 재생과
 * 연동하기 쉽게 만들었다.
 *
 * 계층상 위치: 팬이 sysfs를, 오디오가 ALSA를 쓰듯 "커널 모듈이 아닌
 * 하드웨어 접근"이라 App 레벨(app/)에 둔다.
 *
 * 전형적 사용:
 *   rpic_audio_open(NULL);                 // 기본 MAX98357A 카드
 *   rpic_audio_play_tone(880, 0.4, 20);    // 알림음 (지금 내장 톤)
 *   rpic_audio_play_wav("/absolute/path/alarm.wav");  // PCM 16-bit WAV 재생
 *   rpic_audio_close();
 *
 * 스레드 안전성: 한 번에 한 재생만 가정한다. 재생 중 다른 스레드/시그널
 * 핸들러에서 rpic_audio_stop()을 불러 진행 중 재생을 중단시킬 수 있다.
 *
 * 빌드: -lasound -lm 필요 (sudo apt install libasound2-dev)
 */

#include <stddef.h>
#include <stdint.h>

#include "guardx_err.h"

/* ALSA 재생 파라미터 (검증 코드와 동일, MAX98357A 네이티브) */
#define RPIC_AUDIO_SAMPLE_RATE  48000
#define RPIC_AUDIO_CHANNELS     2
#define RPIC_AUDIO_DEFAULT_DEV  "plughw:CARD=MAX98357A,DEV=1"

/* 지속 오픈. device=NULL 이면 기본 카드(RPIC_AUDIO_DEFAULT_DEV).
 * 48kHz/스테레오로 열어 톤·raw PCM 재생에 재사용한다.
 * API 로 넘기는 샘플은 S16 이지만 하드웨어로는 S32_LE 로 나간다 - TOIMIC
 * 캡처(S32_LE)와 I2S 프레임 길이를 맞추기 위해서다(rpic_audio.c 참조). */
guardx_err_t rpic_audio_open(const char *device);

/* 사인파 톤 재생. freq 20~20000Hz, seconds 0.1~3600, volume 0.1~100(%).
 * 앞뒤 50ms 페이드로 팝 노이즈 방지. 재생이 끝날 때까지(또는 stop 호출까지)
 * 블로킹한다. rpic_audio_open() 선행 필요. */
guardx_err_t rpic_audio_play_tone(double freq_hz,
                                  double seconds,
                                  double volume_pct);

/* 이미 디코딩된 인터리브 S16 스테레오 PCM(48kHz)을 그대로 재생.
 * 추후 파일 디코더(WAV/MP3 등)가 이 함수로 PCM을 밀어 넣으면 된다.
 * frames = (L,R) 샘플쌍의 개수. rpic_audio_open() 선행 필요. */
guardx_err_t rpic_audio_play_pcm(const int16_t *interleaved_lr,
                                 size_t frames);

/* Live stream API: begin/prepare once, feed PCM continuously, then close. */
guardx_err_t rpic_audio_stream_begin(const char *device);
guardx_err_t rpic_audio_stream_write(const int16_t *interleaved_lr,
                                     size_t frames);
guardx_err_t rpic_audio_stream_end(void);

/* WAV 파일 재생 (PCM 16-bit). 파일의 샘플레이트/채널로 별도 핸들을
 * 잠깐 열어 재생하므로 rpic_audio_open()의 지속 핸들과 독립적이다
 * (파일 레이트가 48kHz가 아니어도 정상 재생). 모노도 그대로 재생.
 * device는 open()에서 지정한 카드(없으면 기본)를 쓴다. */
guardx_err_t rpic_audio_play_wav(const char *path);

/* 진행 중인 재생을 중단시키는 비동기 신호(다른 스레드/시그널 핸들러에서
 * 호출 가능). 다음 재생 시작 시 자동으로 해제된다. */
void rpic_audio_stop(void);

/* 지속 핸들 닫기 */
guardx_err_t rpic_audio_close(void);

#endif /* RPIC_AUDIO_H */
