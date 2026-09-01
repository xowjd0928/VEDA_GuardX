/*
 * rpic_audio.c - GuardX RPi C I2S 오디오 재생 구현 (MAX98357A / ALSA)
 *
 * 검증된 max98357a_i2s_test.c 의 ALSA 사용법(snd_pcm_set_params +
 * snd_pcm_writei + snd_pcm_recover)을 그대로 계승하되, 재사용 가능한
 * 모듈로 정리했다. 상세/사용법은 rpic_audio.h 주석 참조.
 *
 * 구조:
 *   - 지속 핸들(g_pcm): open()에서 48kHz/스테레오로 한 번 열어 톤·raw
 *     PCM 재생에 재사용.
 *   - WAV: 파일마다 파라미터가 달라 별도 임시 핸들을 잠깐 열어 재생
 *     (지속 핸들과 독립). 그래서 device 이름을 기억해 둔다(g_device).
 *   - 핵심 재생 primitive는 pcm_write_frames() 하나. 추후 어떤 디코더든
 *     여기로 PCM을 흘려보내면 된다.
 */

#define _POSIX_C_SOURCE 200809L

#include <alsa/asoundlib.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpic_audio.h"
#include "audio_ref.h"

#define FRAMES_PER_BLOCK  1024U
#define AUDIO_PI          3.14159265358979323846
#define FADE_MS           50
#define ALSA_LATENCY_US   100000U   /* set_params 목표 지연 */

/* 지속 핸들(톤/raw PCM용) + 열 때 쓴 카드 이름(WAV 임시 핸들 재사용). */
static snd_pcm_t *g_pcm;
static char g_device[128] = RPIC_AUDIO_DEFAULT_DEV;

/* 진행 중 재생 중단 플래그. rpic_audio_stop()이 세우고, 각 재생 시작 시
 * 자동 해제된다. 시그널 핸들러에서도 안전하게 세울 수 있도록 sig_atomic_t. */
static volatile sig_atomic_t g_stop;

/* ---------------------------------------------------------------------
 * 내부 헬퍼
 * --------------------------------------------------------------------- */

/* device 를 rate/channels/S32_LE 로 열어 설정까지 마친 핸들 반환.
 * 실패 시 NULL.
 *
 * 하드웨어로 나가는 포맷이 S32_LE 인 것이 중요하다. TOIMIC(arecord)이 캡처를
 * S32_LE 로 열면 I2S 프레임이 32비트 슬롯으로 고정되는데, 재생만 16비트로
 * 쓰면 샘플이 슬롯 하위쪽에 실려 진폭이 2^16 배 줄어든다 - 에러 없이 무음처럼
 * 들린다. 양방향 프레임 길이를 맞춰야 마이크와 스피커를 동시에 쓸 수 있다.
 *
 * 앱 내부(톤 생성/WAV)는 그대로 S16 이고, pcm_write_frames()가 write 직전에
 * S32 로 올린다. */
static snd_pcm_t *pcm_open_configured(const char *device,
                                      unsigned int rate,
                                      unsigned int channels)
{
    snd_pcm_t *pcm = NULL;
    int rc = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);

    if (rc < 0) {
        fprintf(stderr,
                "rpic_audio: '%s' open 실패: %s\n"
                "  (aplay -L 또는 aplay -l 로 I2S 카드 확인)\n",
                device, snd_strerror(rc));
        return NULL;
    }

    rc = snd_pcm_set_params(pcm,
                            SND_PCM_FORMAT_S32_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            channels,
                            rate,
                            1,                 /* soft resample 허용 */
                            ALSA_LATENCY_US);
    if (rc < 0) {
        fprintf(stderr,
                "rpic_audio: '%s' 설정 실패(%uHz, 32bit, %uch): %s\n",
                device, rate, channels, snd_strerror(rc));
        snd_pcm_close(pcm);
        return NULL;
    }
    return pcm;
}

/* 인터리브 S16 프레임을 S32 로 올려 xrun 복구까지 하며 전부 write.
 * g_stop 이 서면 중단하고 0 반환(정상 중단). 실패 시 -1.
 *
 * 핸들이 S32_LE 로 열려 있으므로(pcm_open_configured 주석 참조) 여기서
 * 변환한다. 16비트 값을 상위로 16칸 올리면 원래 진폭이 그대로 유지된다. */
static int pcm_write_frames(snd_pcm_t *pcm,
                            const int16_t *samples,
                            snd_pcm_uframes_t frame_count,
                            unsigned int channels)
{
    int32_t conv[FRAMES_PER_BLOCK * RPIC_AUDIO_CHANNELS];
    snd_pcm_uframes_t offset = 0;

    if (channels == 0)
        return -1;

    /* 한 번에 변환할 프레임 수 - conv 버퍼를 넘지 않게 채널 수로 나눈다. */
    const snd_pcm_uframes_t max_chunk =
        (sizeof(conv) / sizeof(conv[0])) / channels;

    if (max_chunk == 0) {
        fprintf(stderr, "rpic_audio: 채널 수(%u)가 너무 큼\n", channels);
        return -1;
    }

    while (offset < frame_count) {
        if (g_stop)
            return 0;

        snd_pcm_uframes_t chunk = frame_count - offset;
        if (chunk > max_chunk)
            chunk = max_chunk;

        for (snd_pcm_uframes_t i = 0; i < chunk * channels; i++)
            conv[i] = (int32_t)samples[(offset * channels) + i] << 16;

        /* 스피커로 나갈 것과 **같은** 샘플을 감지기에도 한 부 보낸다.
         * ALSA 에 쓰기 직전이라 기준 신호가 실제 소리보다 항상 앞선다 -
         * AEC 의 적응 필터는 그 앞섬을 흡수할 수 있지만 뒤처짐은 못
         * 흡수하므로, 이 순서가 규약이다. 실패는 조용히 무시된다
         * (audio_ref.h — 감지기 때문에 사이렌이 끊기면 안 된다). */
        audio_ref_feed(samples + (offset * channels), chunk, channels,
                       RPIC_AUDIO_SAMPLE_RATE);

        snd_pcm_uframes_t done = 0;
        while (done < chunk) {
            if (g_stop)
                return 0;

            snd_pcm_sframes_t r =
                snd_pcm_writei(pcm, conv + (done * channels), chunk - done);
            if (r < 0) {
                r = snd_pcm_recover(pcm, (int)r, 1);
                if (r < 0) {
                    fprintf(stderr, "rpic_audio: write 실패: %s\n",
                            snd_strerror((int)r));
                    return -1;
                }
                continue;
            }
            done += (snd_pcm_uframes_t)r;
        }
        offset += chunk;
    }
    return 0;
}

/* 재생 마무리: 정상 완주면 drain(버퍼 소진까지 재생), 중단이면 drop. */
static guardx_err_t pcm_finish(snd_pcm_t *pcm)
{
    int rc = g_stop ? snd_pcm_drop(pcm) : snd_pcm_drain(pcm);

    if (rc < 0) {
        fprintf(stderr, "rpic_audio: drain/drop 실패: %s\n", snd_strerror(rc));
        return GUARDX_ERR_WRITE;
    }
    return GUARDX_OK;
}

/* 재생 직전 PREPARED 상태로 만든다. 지속 핸들을 재사용할 때 이전 재생이
 * drain/drop으로 SETUP/XRUN 상태에 있으면, prepare 없이 writei하면
 * -EBADFD로 실패한다(두 번째 재생부터 안 남). 그래서 매 재생 시작 시 호출. */
static guardx_err_t pcm_prepare(snd_pcm_t *pcm)
{
    int rc = snd_pcm_prepare(pcm);

    if (rc < 0) {
        fprintf(stderr, "rpic_audio: prepare 실패: %s\n", snd_strerror(rc));
        return GUARDX_ERR_WRITE;
    }
    return GUARDX_OK;
}

/* 리틀엔디안 정수 읽기(WAV 헤더 파싱용). */
static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ---------------------------------------------------------------------
 * 공개 API
 * --------------------------------------------------------------------- */

guardx_err_t rpic_audio_open(const char *device)
{
    if (g_pcm)
        return GUARDX_OK;   /* 이미 열려있으면 성공 취급 */

    if (device && device[0]) {
        strncpy(g_device, device, sizeof(g_device) - 1);
        g_device[sizeof(g_device) - 1] = '\0';
    }

    g_pcm = pcm_open_configured(g_device,
                                RPIC_AUDIO_SAMPLE_RATE,
                                RPIC_AUDIO_CHANNELS);
    return g_pcm ? GUARDX_OK : GUARDX_ERR_OPEN;
}

guardx_err_t rpic_audio_play_pcm(const int16_t *interleaved_lr, size_t frames)
{
    if (!g_pcm)
        return GUARDX_ERR_NOT_OPEN;
    if (!interleaved_lr || frames == 0)
        return GUARDX_ERR_INVALID;

    g_stop = 0;
    if (pcm_prepare(g_pcm) != GUARDX_OK)
        return GUARDX_ERR_WRITE;
    if (pcm_write_frames(g_pcm, interleaved_lr,
                         (snd_pcm_uframes_t)frames, RPIC_AUDIO_CHANNELS) < 0)
        return GUARDX_ERR_WRITE;
    return pcm_finish(g_pcm);
}

guardx_err_t rpic_audio_stream_begin(const char *device)
{
    guardx_err_t ret = rpic_audio_open(device);
    if (ret != GUARDX_OK)
        return ret;

    g_stop = 0;
    ret = pcm_prepare(g_pcm);
    if (ret != GUARDX_OK)
        (void)rpic_audio_close();
    return ret;
}

guardx_err_t rpic_audio_stream_write(const int16_t *interleaved_lr,
                                     size_t frames)
{
    if (!g_pcm)
        return GUARDX_ERR_NOT_OPEN;
    if (!interleaved_lr || frames == 0)
        return GUARDX_ERR_INVALID;

    return pcm_write_frames(g_pcm, interleaved_lr,
                            (snd_pcm_uframes_t)frames,
                            RPIC_AUDIO_CHANNELS) < 0
               ? GUARDX_ERR_WRITE : GUARDX_OK;
}

guardx_err_t rpic_audio_stream_end(void)
{
    if (!g_pcm)
        return GUARDX_ERR_NOT_OPEN;

    guardx_err_t finish = pcm_finish(g_pcm);
    guardx_err_t close = rpic_audio_close();
    return finish != GUARDX_OK ? finish : close;
}

guardx_err_t rpic_audio_play_tone(double freq_hz, double seconds,
                                  double volume_pct)
{
    if (!g_pcm)
        return GUARDX_ERR_NOT_OPEN;
    if (freq_hz < 20.0 || freq_hz > 20000.0 ||
        seconds < 0.1 || seconds > 3600.0 ||
        volume_pct < 0.1 || volume_pct > 100.0)
        return GUARDX_ERR_INVALID;

    const uint64_t total_frames =
        (uint64_t)(seconds * (double)RPIC_AUDIO_SAMPLE_RATE);
    const uint64_t fade_frames =
        (uint64_t)RPIC_AUDIO_SAMPLE_RATE * FADE_MS / 1000;
    const double peak = 32767.0 * (volume_pct / 100.0);
    const double phase_step =
        (2.0 * AUDIO_PI * freq_hz) / (double)RPIC_AUDIO_SAMPLE_RATE;

    int16_t block[FRAMES_PER_BLOCK * RPIC_AUDIO_CHANNELS];
    double phase = 0.0;
    uint64_t generated = 0;

    g_stop = 0;
    if (pcm_prepare(g_pcm) != GUARDX_OK)
        return GUARDX_ERR_WRITE;

    while (generated < total_frames && !g_stop) {
        uint64_t remaining = total_frames - generated;
        snd_pcm_uframes_t n = remaining < FRAMES_PER_BLOCK
                                  ? (snd_pcm_uframes_t)remaining
                                  : FRAMES_PER_BLOCK;

        for (snd_pcm_uframes_t i = 0; i < n; i++) {
            uint64_t fn = generated + i;
            double env = 1.0;

            if (fn < fade_frames)
                env = (double)fn / (double)fade_frames;
            uint64_t from_end = total_frames - fn;
            if (from_end < fade_frames) {
                double fo = (double)from_end / (double)fade_frames;
                if (fo < env)
                    env = fo;
            }

            int16_t s = (int16_t)lrint(sin(phase) * peak * env);
            block[(i * RPIC_AUDIO_CHANNELS) + 0] = s;   /* L */
            block[(i * RPIC_AUDIO_CHANNELS) + 1] = s;   /* R (동일) */

            phase += phase_step;
            if (phase >= 2.0 * AUDIO_PI)
                phase -= 2.0 * AUDIO_PI;
        }

        if (pcm_write_frames(g_pcm, block, n, RPIC_AUDIO_CHANNELS) < 0)
            return GUARDX_ERR_WRITE;
        generated += n;
    }

    return pcm_finish(g_pcm);
}

guardx_err_t rpic_audio_play_wav(const char *path)
{
    if (!path || !path[0])
        return GUARDX_ERR_INVALID;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("rpic_audio: WAV open");
        return GUARDX_ERR_OPEN;
    }

    /* --- RIFF/WAVE 헤더 파싱 (PCM 16-bit) --- */
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "rpic_audio: '%s' RIFF/WAVE 아님\n", path);
        fclose(fp);
        return GUARDX_ERR_INVALID;
    }

    unsigned int rate = 0, channels = 0, bits = 0;
    uint16_t audio_format = 0;
    long data_pos = -1;
    uint32_t data_size = 0;
    uint8_t ch[8];

    /* 청크를 순회하며 fmt / data 를 찾는다. */
    while (fread(ch, 1, 8, fp) == 8) {
        uint32_t sz = rd_u32le(ch + 4);

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (sz < 16 || fread(fmt, 1, 16, fp) != 16) {
                fprintf(stderr, "rpic_audio: fmt 청크 손상\n");
                fclose(fp);
                return GUARDX_ERR_INVALID;
            }
            audio_format = rd_u16le(fmt + 0);
            channels     = rd_u16le(fmt + 2);
            rate         = rd_u32le(fmt + 4);
            bits         = rd_u16le(fmt + 14);
            /* fmt 청크가 16보다 크면 나머지 건너뛰기 */
            if (sz > 16)
                fseek(fp, (long)(sz - 16), SEEK_CUR);
        } else if (memcmp(ch, "data", 4) == 0) {
            data_pos = ftell(fp);
            data_size = sz;
            break;   /* data 이후는 스트리밍으로 읽는다 */
        } else {
            fseek(fp, (long)sz, SEEK_CUR);   /* 관심 없는 청크 스킵 */
        }
        if (sz & 1)
            fseek(fp, 1, SEEK_CUR);          /* 청크는 2바이트 정렬 */
    }

    if (data_pos < 0 || channels == 0 || rate == 0) {
        fprintf(stderr, "rpic_audio: '%s' fmt/data 청크 누락\n", path);
        fclose(fp);
        return GUARDX_ERR_INVALID;
    }
    if (audio_format != 1 || bits != 16) {
        fprintf(stderr,
                "rpic_audio: '%s' 미지원 포맷(format=%u, bits=%u). "
                "PCM 16-bit WAV만 지원.\n", path, audio_format, bits);
        fclose(fp);
        return GUARDX_ERR_INVALID;
    }

    /* --- 파일 파라미터로 임시 핸들 열고 data 청크를 스트리밍 재생 --- */
    snd_pcm_t *pcm = pcm_open_configured(g_device, rate, channels);
    if (!pcm) {
        fclose(fp);
        return GUARDX_ERR_OPEN;
    }
    if (pcm_prepare(pcm) != GUARDX_OK) {
        snd_pcm_close(pcm);
        fclose(fp);
        return GUARDX_ERR_WRITE;
    }

    const size_t block_align = (size_t)channels * (bits / 8);
    int16_t block[FRAMES_PER_BLOCK * RPIC_AUDIO_CHANNELS];
    /* 안전: 채널 수가 스테레오를 넘으면 블록 프레임 수를 줄여 버퍼 초과 방지 */
    const snd_pcm_uframes_t max_frames =
        (sizeof(block) / sizeof(block[0])) / channels;

    g_stop = 0;
    guardx_err_t ret = GUARDX_OK;
    uint32_t remaining = data_size;

    while (remaining > 0 && !g_stop) {
        snd_pcm_uframes_t want = max_frames;
        size_t want_bytes = (size_t)want * block_align;
        if (want_bytes > remaining) {
            want = remaining / block_align;
            want_bytes = (size_t)want * block_align;
        }
        if (want == 0)
            break;   /* data_size가 프레임 경계로 안 떨어지는 자투리 */

        size_t got = fread(block, 1, want_bytes, fp);
        if (got < want_bytes) {
            if (got < block_align)
                break;          /* 더 읽을 완전한 프레임 없음 */
            want = (snd_pcm_uframes_t)(got / block_align);
        }

        if (pcm_write_frames(pcm, block, want, channels) < 0) {
            ret = GUARDX_ERR_WRITE;
            break;
        }
        remaining -= (uint32_t)(want * block_align);
    }

    if (ret == GUARDX_OK)
        ret = pcm_finish(pcm);
    else
        snd_pcm_drop(pcm);

    snd_pcm_close(pcm);
    fclose(fp);
    return ret;
}

void rpic_audio_stop(void)
{
    g_stop = 1;
}

guardx_err_t rpic_audio_close(void)
{
    if (!g_pcm)
        return GUARDX_OK;
    snd_pcm_close(g_pcm);
    g_pcm = NULL;
    return GUARDX_OK;
}
