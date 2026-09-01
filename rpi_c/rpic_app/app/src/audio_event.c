#define _POSIX_C_SOURCE 200809L

/*
 * audio_event.c - I2S 알림음/상황음 재생 (워커 스레드)
 *
 * 상세/의도는 audio_event.h 주석 참조.
 *
 * 동작:
 *   - init: ALSA 장치는 열지 않고 워커 스레드만 생성.
 *   - trigger/play: 재생할 scenario를 pending에 넣고 워커를 깨운 뒤 즉시 반환.
 *   - worker: pending이 있으면 ALSA open -> 상황음 1회 재생 -> 즉시 close.
 *   - cleanup: 진행 중 재생 중단 + 워커 join.
 *
 * 오디오 핸들(rpic_audio의 g_pcm)을 열고 재생하고 닫는 주체는 이 워커
 * 하나뿐이라 ALSA 동시 접근이 없다(콜백 스레드는 신호만 준다).
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "audio_event.h"
#include "rpic_audio.h"

#define AUDIO_DEVICE_MAX 128
#define AUDIO_PATH_MAX   4096
#define AUDIO_REL_DIR    "../assets/audio"

static bool enabled;
static bool worker_running;
static bool paused;
static bool playing;
static int  pending_scenario = -1;   /* -1 = 없음, 그 외 = 재생할 scenario */
static char audio_device[AUDIO_DEVICE_MAX]; /* 빈 문자열 = rpic_audio 기본 장치 */
static char fire_wav_path[AUDIO_PATH_MAX];
static char intruder_wav_path[AUDIO_PATH_MAX];
static char crowd_wav_path[AUDIO_PATH_MAX];
static pthread_t worker;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;

/*
 * 현재 작업 디렉터리가 아니라 실행 중인 rpic_subscriber 자체의 위치를 기준으로
 * 음원을 찾는다.
 *
 *   <rpic_app>/app/rpic_subscriber
 *   <rpic_app>/assets/audio/(WAV files)
 */
static guardx_err_t configure_audio_paths(void)
{
    char exe_path[AUDIO_PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);

    if (len < 0 || (size_t)len >= sizeof(exe_path) - 1) {
        perror("audio_event: /proc/self/exe readlink");
        return GUARDX_ERR_OPEN;
    }
    exe_path[len] = '\0';

    char *slash = strrchr(exe_path, '/');
    if (!slash) {
        fprintf(stderr, "audio_event: executable directory not found\n");
        return GUARDX_ERR_INVALID;
    }
    *slash = '\0';

    int fire_len = snprintf(fire_wav_path, sizeof(fire_wav_path),
                            "%s/%s/fire_alert.wav", exe_path, AUDIO_REL_DIR);
    int intruder_len = snprintf(intruder_wav_path, sizeof(intruder_wav_path),
                                "%s/%s/intruder_alert.wav",
                                exe_path, AUDIO_REL_DIR);
    int crowd_len = snprintf(crowd_wav_path, sizeof(crowd_wav_path),
                             "%s/%s/crowd_alert.wav",
                             exe_path, AUDIO_REL_DIR);
    if (fire_len < 0 || (size_t)fire_len >= sizeof(fire_wav_path) ||
        intruder_len < 0 ||
        (size_t)intruder_len >= sizeof(intruder_wav_path) ||
        crowd_len < 0 || (size_t)crowd_len >= sizeof(crowd_wav_path)) {
        fprintf(stderr, "audio_event: audio path is too long\n");
        return GUARDX_ERR_INVALID;
    }

    return GUARDX_OK;
}

/* 화재/거수자는 배포된 WAV를 재생하고, 나머지 상황은 내장 톤을 사용한다. */
static void play_wav_or_fallback(const char *path,
                                 double fallback_hz,
                                 double fallback_seconds)
{
    /*
     * rpic_audio_play_wav() opens a temporary ALSA handle matching the WAV
     * format. Close the tone handle first so the I2S device is not opened
     * twice. The selected device name remains configured after close.
     */
    rpic_audio_close();
    if (rpic_audio_play_wav(path) == GUARDX_OK)
        return;

    fprintf(stderr,
            "audio_event: '%s' playback failed - using built-in tone\n",
            path);
    if (rpic_audio_open(audio_device[0] ? audio_device : NULL) == GUARDX_OK)
        rpic_audio_play_tone(fallback_hz, fallback_seconds, 35.0);
}

static void play_scenario(int scenario)
{
    switch (scenario) {
    case AUDIO_SCENE_FIRE:       /* 화재 - 낮고 길게 (사이렌 느낌) */
        play_wav_or_fallback(fire_wav_path, 440.0, 2.0);
        break;
    case AUDIO_SCENE_INTRUDER:   /* 강도 - 높고 날카롭게 */
        play_wav_or_fallback(intruder_wav_path, 1500.0, 1.5);
        break;
    case AUDIO_SCENE_CROWD:      /* 혼잡 위험 - 경고음 */
        play_wav_or_fallback(crowd_wav_path, 1000.0, 2.0);
        break;
    case AUDIO_SCENE_EMERGENCY:  /* 비상 - 중간 톤 길게 */
        rpic_audio_play_tone(880.0, 2.0, 35.0);
        break;
    case AUDIO_SCENE_BEEP:       /* 기본 알림음 (sound SET 0 또는 ON) */
    default:
        rpic_audio_play_tone(880.0, 0.4, 20.0);
        break;
    }
}

static void *worker_fn(void *arg)
{
    (void)arg;

    for (;;) {
        int scenario;

        pthread_mutex_lock(&lock);
        while (worker_running && (pending_scenario < 0 || paused))
            pthread_cond_wait(&cond, &lock);

        if (!worker_running) {
            pthread_mutex_unlock(&lock);
            break;
        }
        scenario = pending_scenario;
        pending_scenario = -1;
        playing = true;
        pthread_mutex_unlock(&lock);

        /*
         * 대기 중에는 PCM 핸들을 보유하지 않는다. sound 요청을 실제 처리하는
         * 이 구간에서만 장치를 열고, 성공/실패와 무관하게 재생 직후 닫는다.
         * SD_MODE를 ASoC가 관리할 수 있도록 PCM 수명도 재생 수명에 맞춘다.
         */
        if (rpic_audio_open(audio_device[0] ? audio_device : NULL) != GUARDX_OK) {
            fprintf(stderr,
                    "audio_event: 재생 시 오디오 open 실패 - 요청을 건너뜀\n");
        } else {
            play_scenario(scenario);
            if (rpic_audio_close() != GUARDX_OK)
                fprintf(stderr, "audio_event: 재생 후 오디오 close 실패\n");
        }

        pthread_mutex_lock(&lock);
        playing = false;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

/* pending에 scenario를 넣고 워커를 깨운다(비블로킹). */
static void request_play(int scenario)
{
    if (!enabled)
        return;

    pthread_mutex_lock(&lock);
    if (paused) {
        /* 조용히 버리면 "사이렌이 왜 안 울리지"를 로그로 못 쫓는다.
         * 방송이 스피커를 쥔 동안이라 정상이지만, 사실은 남겨야 한다. */
        pthread_mutex_unlock(&lock);
        fprintf(stderr,
                "audio_event: 일시정지 상태 - scenario %d 요청을 버림"
                " (방송이 스피커 점유 중)\n", scenario);
        return;
    }
    pending_scenario = scenario;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
}

guardx_err_t audio_event_init(const char *device)
{
    if (enabled)
        return GUARDX_OK;

    if (configure_audio_paths() != GUARDX_OK)
        return GUARDX_ERR_OPEN;

    if (device && device[0]) {
        strncpy(audio_device, device, sizeof(audio_device) - 1);
        audio_device[sizeof(audio_device) - 1] = '\0';
    } else {
        audio_device[0] = '\0';
    }

    worker_running = true;
    paused = false;
    playing = false;
    pending_scenario = -1;
    if (pthread_create(&worker, NULL, worker_fn, NULL) != 0) {
        fprintf(stderr, "audio_event: 워커 스레드 생성 실패 - 소리 비활성화\n");
        worker_running = false;
        return GUARDX_ERR_OPEN;
    }

    enabled = true;
    return GUARDX_OK;
}

void audio_event_play(int scenario)
{
    request_play(scenario);
}

void audio_event_set_paused(bool value)
{
    if (!enabled)
        return;

    pthread_mutex_lock(&lock);
    paused = value;
    if (paused)
        pending_scenario = -1;
    const bool must_stop = paused && playing;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);

    if (!must_stop)
        return;

    rpic_audio_stop();
    pthread_mutex_lock(&lock);
    while (worker_running && playing)
        pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
}

void audio_event_stop(void)
{
    if (!enabled)
        return;

    pthread_mutex_lock(&lock);
    /* 대기 중인 요청도 함께 버린다 - 안 그러면 방금 끊은 자리에 곧바로
     * 다음 재생이 들어와 "멈췄다"가 성립하지 않는다. paused 는 손대지
     * 않는다(그건 방송이 장치를 쥐고 있는 동안만 쓰는 별개 상태다). */
    pending_scenario = -1;
    const bool must_stop = playing;
    pthread_mutex_unlock(&lock);

    if (!must_stop)
        return;

    rpic_audio_stop();
    pthread_mutex_lock(&lock);
    while (worker_running && playing)
        pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
}

void audio_event_cleanup(void)
{
    if (!enabled)
        return;

    pthread_mutex_lock(&lock);
    worker_running = false;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);

    rpic_audio_stop();          /* 진행 중 재생이 있으면 즉시 끊는다 */
    pthread_join(worker, NULL);
    rpic_audio_close();         /* 워커가 이미 닫지만 오류 경로 대비 */
    enabled = false;
}
