/*
 * audio_test.c - rpic_audio(MAX98357A I2S) 재생 검증 CLI
 *
 * 원본 max98357a_i2s_test.c 를 rpic_audio 모듈 위에서 재현 + 파일 재생.
 * 구독자에 알림음이 이미 통합돼 있지만, "소리만 따로" 확인하고 싶을 때 쓴다.
 *
 * 빌드 (app/ 에서):
 *   make tools               # libasound2-dev 필요
 *
 * 사용:
 *   ./audio_test                                  # 기본 카드, 440Hz 5s 10%
 *   ./audio_test <DEVICE> tone <SEC> <FREQ> <VOL> # 톤 재생
 *   ./audio_test <DEVICE> wav <PATH>              # WAV 파일 재생
 *
 * 예:
 *   ./audio_test default tone 3 880 20
 *   ./audio_test plughw:CARD=MAX98357A,DEV=0 wav ../assets/audio/fire_alert.wav
 *
 * DEVICE 를 "-" 로 주면 기본 카드(rpic_audio 기본값)를 쓴다.
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpic_audio.h"

static void on_sigint(int sig)
{
    (void)sig;
    rpic_audio_stop();   /* 진행 중 재생 중단 */
}

int main(int argc, char **argv)
{
    const char *device = (argc > 1 && strcmp(argv[1], "-") != 0)
                             ? argv[1] : NULL;
    const char *mode = (argc > 2) ? argv[2] : "tone";

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (rpic_audio_open(device) != GUARDX_OK) {
        fprintf(stderr, "audio_test: open 실패\n");
        return EXIT_FAILURE;
    }

    guardx_err_t ret;

    if (strcmp(mode, "wav") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s <DEVICE|-> wav <PATH>\n", argv[0]);
            rpic_audio_close();
            return EXIT_FAILURE;
        }
        printf("audio_test: WAV 재생 '%s' (Ctrl+C 중단)\n", argv[3]);
        ret = rpic_audio_play_wav(argv[3]);
    } else if (strcmp(mode, "tone") == 0) {
        double sec  = (argc > 3) ? atof(argv[3]) : 5.0;
        double freq = (argc > 4) ? atof(argv[4]) : 440.0;
        double vol  = (argc > 5) ? atof(argv[5]) : 10.0;
        printf("audio_test: 톤 %.1fHz, %.1f%%, %.2fs (Ctrl+C 중단)\n",
               freq, vol, sec);
        ret = rpic_audio_play_tone(freq, sec, vol);
    } else {
        fprintf(stderr,
                "Usage: %s [DEVICE|-] [tone SEC FREQ VOL | wav PATH]\n",
                argv[0]);
        rpic_audio_close();
        return EXIT_FAILURE;
    }

    rpic_audio_close();

    if (ret != GUARDX_OK) {
        fprintf(stderr, "audio_test: 재생 실패 (%d)\n", ret);
        return EXIT_FAILURE;
    }
    printf("audio_test: 완료\n");
    return EXIT_SUCCESS;
}
