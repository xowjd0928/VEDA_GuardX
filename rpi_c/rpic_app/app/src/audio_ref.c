/*
 * audio_ref.c - 스피커 출력 사본을 TOIMIC 에 UDP 로 넘긴다.
 *
 * 설계 배경은 audio_ref.h 와 shared/audio_ref_protocol.h 참조.
 */

#define _POSIX_C_SOURCE 200809L

#include "audio_ref.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio_ref_protocol.h"

/* 48 kHz -> 16 kHz. 정수배(3)라 리샘플러가 필요 없다. */
#define SRC_RATE   48000
#define DECIM      (SRC_RATE / GUARDX_AUDIO_REF_RATE)

static int sock_fd = -1;
static struct sockaddr_in dest;

/* 20 ms 를 모으는 버퍼와, 데시메이션 경계에 걸친 자투리 상태. */
static int16_t pending[GUARDX_AUDIO_REF_SAMPLES];
static int pending_n;
static int32_t acc;        /* 데시메이션 구간 합 */
static int acc_n;          /* 그 구간에 들어간 입력 프레임 수 */

guardx_err_t audio_ref_init(void)
{
    if (sock_fd >= 0)
        return GUARDX_OK;

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        fprintf(stderr, "audio_ref: 소켓 생성 실패 - 기준 신호 없이 진행\n");
        return GUARDX_ERR_OPEN;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(GUARDX_AUDIO_REF_PORT);
    if (inet_pton(AF_INET, GUARDX_AUDIO_REF_HOST, &dest.sin_addr) != 1) {
        fprintf(stderr, "audio_ref: 주소 변환 실패 - 기준 신호 없이 진행\n");
        close(sock_fd);
        sock_fd = -1;
        return GUARDX_ERR_INVALID;
    }

    pending_n = 0;
    acc = 0;
    acc_n = 0;
    printf("audio_ref: 스피커 기준 신호 -> udp://%s:%d (%d Hz mono)\n",
           GUARDX_AUDIO_REF_HOST, GUARDX_AUDIO_REF_PORT,
           GUARDX_AUDIO_REF_RATE);
    return GUARDX_OK;
}

static void flush_packet(void)
{
    if (sock_fd < 0 || pending_n <= 0)
        return;

    /* 실패를 보고하지 않는다. 받는 쪽(감지기)이 안 떠 있으면 ICMP 로
     * 거절당하는데, 그건 정상 상태다 - 감지기는 있어도 되고 없어도 된다. */
    (void)sendto(sock_fd, pending, (size_t)pending_n * sizeof(int16_t), 0,
                 (const struct sockaddr *)&dest, sizeof(dest));
    pending_n = 0;
}

void audio_ref_feed(const int16_t *interleaved, size_t frames,
                    unsigned int channels, unsigned int rate_hz)
{
    if (sock_fd < 0 || !interleaved || frames == 0 || channels == 0)
        return;
    /* 48 kHz 만 다룬다. 재생 경로는 전부 48 kHz 고정이고(RPIC_AUDIO_SAMPLE_RATE),
     * 일반 리샘플러를 넣으면 이 부가 기능이 재생 스레드의 CPU 를 먹는다. */
    if (rate_hz != SRC_RATE)
        return;

    for (size_t i = 0; i < frames; i++) {
        /* 모노로 내린다. 스피커가 모노 앰프라 L/R 이 같은 소리지만, 평균을
         * 쓰면 한쪽만 실린 음원에서도 진폭이 유지된다. */
        int32_t sum = 0;
        for (unsigned int c = 0; c < channels; c++)
            sum += interleaved[(i * channels) + c];
        acc += sum / (int32_t)channels;

        if (++acc_n < DECIM)
            continue;

        /* 3개 평균 = 아주 거친 안티앨리어싱이지만 기준 신호에는 충분하다.
         * AEC 가 맞추는 것은 파형의 모양이고, 8 kHz 위쪽은 어차피 감지기의
         * 16 kHz 대역 밖이다. */
        pending[pending_n++] = (int16_t)(acc / DECIM);
        acc = 0;
        acc_n = 0;

        if (pending_n >= GUARDX_AUDIO_REF_SAMPLES)
            flush_packet();
    }
}

void audio_ref_flush(void)
{
    flush_packet();
    acc = 0;
    acc_n = 0;
}

void audio_ref_cleanup(void)
{
    if (sock_fd < 0)
        return;
    flush_packet();
    close(sock_fd);
    sock_fd = -1;
}
