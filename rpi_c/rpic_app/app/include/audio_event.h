#ifndef AUDIO_EVENT_H
#define AUDIO_EVENT_H

#include <stdbool.h>

/*
 * audio_event.h - I2S 알림음/상황음 재생 (MAX98357A, 비동기 워커)
 *
 * RPi B(또는 테스트 도구)가 `sound` 명령을 보내면 registry가
 * audio_event_play(scenario)로 상황음을 재생한다.
 *
 * ALSA 재생은 블로킹이라 전용 워커 스레드가 처리하고, trigger/play는 즉시
 * 반환한다. "무슨 소리를 낼지"는 audio_event.c의 play_scenario() 한 곳에만
 * 있다 - 지금은 상황별 톤이고, 추후 rpic_audio_play_wav("경로")로 음원 파일
 * 경보 음원은 실행 파일 위치 기준 ../assets/audio/에서 찾는다.
 *
 * ALSA 장치는 sound 요청을 처리할 때만 열고 재생 직후 닫는다. 소리는 "부가
 * 기능"이라 카드가 없거나 재생에 실패해도 노드는 계속 산다.
 */

#include "guardx_err.h"

/* 상황(scenario) ID - sound 명령의 value로도 그대로 쓴다. */
#define AUDIO_SCENE_BEEP       0   /* 기본 알림음 (sound SET 0 또는 ON) */
#define AUDIO_SCENE_FIRE       1   /* 화재 */
#define AUDIO_SCENE_INTRUDER   2   /* 강도(침입) */
#define AUDIO_SCENE_EMERGENCY  3   /* 비상(버튼) */
#define AUDIO_SCENE_CROWD      4   /* 혼잡 위험 */

/* ALSA 장치는 열지 않고 워커 스레드만 시작한다. device=NULL이면 재생 시
 * 기본 카드를 연다. 실패해도 노드 기동은 막지 않는다. */
guardx_err_t audio_event_init(const char *device);

/* 지정 상황음(scenario) 1회 재생. 즉시 반환(비블로킹).
 * 알 수 없는 scenario는 기본 알림음으로 대체. */
void audio_event_play(int scenario);

/* Pause situation sounds while live broadcast owns the shared ALSA device. */
void audio_event_set_paused(bool paused);

/* 진행 중인 재생만 즉시 끊는다(일시정지 상태는 건드리지 않음).
 * 화재가 해제됐을 때 울리던 사이렌을 바로 멈추는 용도. 재생이 실제로 끝나
 * 장치가 닫힐 때까지 기다린 뒤 반환한다. */
void audio_event_stop(void);

/* 워커 정지 (진행 중 재생은 즉시 중단하고 장치를 닫음) */
void audio_event_cleanup(void);

#endif /* AUDIO_EVENT_H */
