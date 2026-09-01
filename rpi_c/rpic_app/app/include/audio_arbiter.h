#ifndef AUDIO_ARBITER_H
#define AUDIO_ARBITER_H

#include <stdbool.h>

#include "guardx_err.h"

/*
 * audio_arbiter.h - 스피커 한 대를 방송과 화재 사이렌이 나눠 쓰는 규칙
 *
 * MAX98357A 는 hw 장치라 한 프로세스만 연다. 믹싱은 하지 않는다 - 작은
 * 스피커 하나에서 사이렌과 안내방송이 겹치면 둘 다 못 알아듣기 때문에,
 * 항상 둘 중 하나만 점유한다.
 *
 * 규칙(운영 합의):
 *   1. 방송 중 화재 발생  -> 방송 즉시 종료, 사이렌 시작
 *   2. 사이렌 중 방송 시작 -> 사이렌 정지, 방송 시작
 *      (화재 중에 운영자가 방송 버튼을 누른 행위 자체를 "사이렌 대신
 *       안내방송을 하겠다"는 선점 의사로 본다. 별도 비상방송 버튼은 두지
 *       않는다.)
 *   3. 방송 종료          -> 화재가 계속이면 사이렌 재개, 아니면 정지
 *
 * 상태 세 가지만 기억한다:
 *   fire_active       화재 중인가        (display/rpic/fire 의 zone_bitmap!=0)
 *   broadcast_active  방송 중인가        (broadcast command START/STOP)
 *   alarm_playing     사이렌 재생 중인가 (내부 워커가 관리)
 *
 * 화재 토픽은 재발행/retained 로 같은 내용이 반복 도착한다. 그래서 매 수신마다
 * 방송을 끊으면 안 되고, false->true 로 "바뀌는 순간"에만 선점한다. 화재 중
 * 운영자가 다시 시작한 방송이 반복 메시지에 끊기지 않는 이유가 이것이다.
 *
 * 선점은 브로커를 거치지 않는다 - RTP 수신기 정지는 로컬 systemctl 로 즉시
 * 처리한다. 안전 기능이 네트워크 상태에 묶이면 안 된다.
 */

/* 워커 스레드 시작. audio_event_init() 뒤에 부른다. */
guardx_err_t audio_arbiter_init(void);

/* 화재 표시 메시지를 받을 때마다 부른다(값이 같아도 무방).
 * 내부에서 false->true 전이만 골라 방송 선점 + 사이렌 시작을 한다. */
void audio_arbiter_set_fire(bool active);

/* 방송 START/STOP 을 받을 때 부른다.
 *
 * 다중 VMS: 진행 중인 세션이 있는데 **다른** 세션의 START 가 오면
 *   takeover=false -> 거절하고 READY 로 "busy" + 현재 owner 를 돌려준다.
 *                    진행 중인 방송은 건드리지 않는다.
 *   takeover=true  -> 이전 세션을 끊고(state reason="taken_over") 넘긴다.
 *
 * 예전에는 나중 START 가 무조건 이겼다. VMS 두 대가 서로 모른 채 버튼을
 * 누르면 앞사람의 방송이 예고 없이 끊겼다 - 인수는 운영자가 확인창에서
 * 고른 경우에만 일어나야 한다.
 *
 * owner 는 발신 VMS 의 client id 다(화면 표시용, 판별은 session_id).
 * NULL 이면 빈 문자열로 다룬다.
 *   true  : 사이렌 정지 후 방송에 장치를 넘긴다
 *   false : 화재가 계속이면 사이렌을 재개한다
 *
 * session 은 명령의 session_id 다. 낡은 STOP(이전 방송의 것이 늦게 도착)이
 * 지금 진행 중인 방송을 끊지 않도록, STOP 은 세션이 일치할 때만 받는다.
 * broadcast_audio.c 가 MQTT/PCM 경로에서 하던 판별과 같은 이유다. */
void audio_arbiter_set_broadcast(bool active, unsigned long session,
                                 const char *owner, bool takeover);

/* 방송 중 재발행되는 KEEPALIVE. 만료 시계만 되감고 상태는 바꾸지 않는다.
 *
 * START 와 갈라둔 이유: 화재가 방송을 선점한 직후에도 VMS 는 자기가 방송
 * 중인 줄 알고 KEEPALIVE 를 계속 보낸다. 그걸 START 로 받으면 2초 만에
 * 방송이 되살아나 사이렌을 다시 죽인다 - 운영자가 버튼을 누른 것도 아닌데.
 * 이어가기는 이어가기일 뿐이라 진행 중인 세션에만 먹는다. */
void audio_arbiter_broadcast_keepalive(unsigned long session);

/* 현재 점유 상태를 retained 로 다시 발행한다.
 *
 * 브로커에 (재)접속한 직후에 부른다. retained 메시지는 브로커가 들고
 * 있으므로 평소에는 필요 없지만, 접속이 끊겨 있던 동안 상태가 바뀌었다면
 * 브로커의 값이 낡아 있다 - 그 상태로 두면 VMS 들이 이미 끝난 방송의
 * 소유자를 계속 믿는다. */
void audio_arbiter_republish_state(void);

/* 워커 정지. mqtt_sub_cleanup() 뒤, audio_event_cleanup() 앞에 부른다. */
void audio_arbiter_cleanup(void);

#endif /* AUDIO_ARBITER_H */
