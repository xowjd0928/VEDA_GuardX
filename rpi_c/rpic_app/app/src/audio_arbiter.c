/*
 * audio_arbiter.c - 방송/사이렌 배타 점유 조율 (규칙은 audio_arbiter.h 참조)
 *
 * 사이렌은 "한 번 재생"이 아니라 화재가 해제될 때까지 반복이라, 워커 스레드가
 * audio_event_play(FIRE) 를 주기적으로 다시 넣는다. audio_event 쪽 워커가
 * 실제 ALSA 재생을 담당하므로 여기서는 장치를 직접 열지 않는다.
 *
 * 같은 워커가 KEEPALIVE 만료도 감시한다. 타이머 스레드를 따로 두지 않으려고
 * 조건변수 대기에 시한을 걸어 두 가지를 한 루프에서 처리한다.
 *
 * RTP 수신기 정지에 systemctl 을 쓰는 것은 그것이 그 프로세스의 수명을 쥔
 * 주체이기 때문이다. 직접 kill 하면 Restart=always 가 곧바로 되살려서 장치를
 * 다시 뺏는다.
 */

#define _POSIX_C_SOURCE 200809L

#include "audio_arbiter.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "audio_event.h"
#include "broadcast_protocol.h"
#include "mqtt_sub.h"

/* 사이렌 한 번 재생이 끝난 뒤 다음 재생까지의 간격(ms).
 * 음원 길이는 audio_event 가 알고 여기서는 모른다 - 짧게 잡아 촘촘히 넣되,
 * 재생 중이면 audio_event 가 알아서 무시하므로 겹치지 않는다. */
#define SIREN_REPEAT_MS         1500

/* KEEPALIVE 만료 검사 주기. 만료 판정 자체는 프로토콜의 TIMEOUT_MS 다. */
#define KEEPALIVE_CHECK_MS      1000

/* 방송 정지 후 ALSA 장치가 실제로 반납될 때까지의 상한.
 * 초과해도 사이렌은 그냥 시도한다 - 안전 기능이 대기로 지연되면 안 된다. */
#define DEVICE_RELEASE_MAX_MS   300
#define DEVICE_RELEASE_STEP_MS   20

/* 수신기 기동 후 스피커를 실제로 잡을 때까지의 상한(READY 응답 전). */
#define DEVICE_ACQUIRE_MAX_MS  2500
#define DEVICE_ACQUIRE_STEP_MS   50

/* RTP 수신기 유닛. receive.sh 를 띄우는 그 유닛이다. */
#define RTP_UNIT                "guardx-broadcast-rtp"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;

static bool fire_active;
static bool broadcast_active;
static unsigned long broadcast_session;      /* 진행 중 방송의 session_id */
static char broadcast_owner[64];             /* 그 방송을 건 VMS 의 client id */
static struct timespec last_keepalive;       /* 그 세션의 마지막 수신 시각 */
static bool worker_running;
static pthread_t worker;

/* ------------------------------------------------------------------ 시간 */

static void now_mono(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static long elapsed_ms(const struct timespec *since)
{
    struct timespec now;

    now_mono(&now);
    return (now.tv_sec - since->tv_sec) * 1000L +
           (now.tv_nsec - since->tv_nsec) / 1000000L;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------- systemctl / 장치 */

/* systemctl 을 fork/exec 로 부른다. system() 을 피하는 것은 셸을 거치면
 * 인자 해석이 끼어들고 SIGCHLD 처리가 프로세스 전역에 영향을 주기 때문이다.
 *
 * root 가 아니면 systemctl 이 권한 오류로 실패하므로 sudo -n 으로 한 번 더
 * 시도한다(-n: 비밀번호를 물으면 그냥 실패). 서비스로 돌 때는 첫 번째가,
 * 개발자가 손으로 띄웠을 때는 두 번째가 통한다.
 *
 * 반환: true=성공. 호출부는 이 값을 반드시 봐야 한다 - 실패를 무시하면
 * "방송을 껐다고 믿는데 실제로는 스피커를 쥐고 있는" 상태가 된다. */
static bool run_systemctl(const char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "arbiter: fork 실패: %s\n", strerror(errno));
        return false;
    }
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        continue;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool systemctl(const char *verb)
{
    const char *direct[] = { "systemctl", verb, RTP_UNIT, NULL };
    const char *elevated[] = { "sudo", "-n", "systemctl", verb, RTP_UNIT, NULL };

    if (run_systemctl(direct))
        return true;
    if (geteuid() != 0 && run_systemctl(elevated))
        return true;

    fprintf(stderr, "arbiter: systemctl %s %s 실패 (uid=%u)\n",
            verb, RTP_UNIT, (unsigned)geteuid());
    return false;
}

/* 재생 서브스트림이 열려 있는가. 열려 있으면 hw_params 가 실제 파라미터를
 * 담고, 닫혀 있으면 "closed" 한 줄이다. 카드 번호가 환경마다 달라 0~3 을 훑는다. */
static bool speaker_held(void)
{
    char path[64];
    int card;

    for (card = 0; card <= 3; card++) {
        FILE *fp;
        char buf[64];

        snprintf(path, sizeof(path),
                 "/proc/asound/card%d/pcm1p/sub0/hw_params", card);
        fp = fopen(path, "r");
        if (!fp)
            continue;
        if (fgets(buf, sizeof(buf), fp) && strstr(buf, "closed") == NULL) {
            fclose(fp);
            return true;
        }
        fclose(fp);
    }
    return false;
}

static bool wait_device_release(void)
{
    int waited = 0;

    while (speaker_held()) {
        if (waited >= DEVICE_RELEASE_MAX_MS) {
            fprintf(stderr,
                    "arbiter: %dms 내에 스피커가 반납되지 않음\n",
                    DEVICE_RELEASE_MAX_MS);
            return false;
        }
        sleep_ms(DEVICE_RELEASE_STEP_MS);
        waited += DEVICE_RELEASE_STEP_MS;
    }
    return true;
}

static bool wait_device_acquire(void)
{
    int waited = 0;

    while (!speaker_held()) {
        if (waited >= DEVICE_ACQUIRE_MAX_MS)
            return false;
        sleep_ms(DEVICE_ACQUIRE_STEP_MS);
        waited += DEVICE_ACQUIRE_STEP_MS;
    }
    return true;
}

/* ----------------------------------------------------------- READY 응답 */

static void publish_result(unsigned long session, const char *result,
                           const char *reason, const char *owner)
{
    char json[256];
    int n = snprintf(json, sizeof(json),
                     "{\"node_id\":\"rpic\",\"session_id\":%lu,"
                     "\"result\":\"%s\",\"reason\":\"%s\","
                     "\"owner\":\"%s\"}",
                     session, result, reason ? reason : "",
                     owner ? owner : "");

    if (n <= 0)
        return;
    if (n > (int)sizeof(json))
        n = (int)sizeof(json);
    (void)mqtt_sub_publish(GUARDX_BROADCAST_READY_TOPIC, json, n);
}

static void publish_ready(unsigned long session, bool ok, const char *reason)
{
    publish_result(session,
                   ok ? GUARDX_BROADCAST_RESULT_READY
                      : GUARDX_BROADCAST_RESULT_ERROR,
                   reason, "");
}

/* ------------------------------------------------------- 점유 상태 발행 */

/* 지금 누가 스피커를 쥐고 있는가를 retained 로 알린다.
 *
 * VMS 가 여러 대면 "다른 VMS 가 방송 중인가"를 물어볼 곳이 필요하고, 그
 * 답을 아는 것은 장치를 실제로 쥔 여기뿐이다. VMS 끼리 서로 알리게 하면
 * 죽은 VMS 의 마지막 주장이 영원히 남는다.
 *
 * lock 을 잡지 않는다 - 호출부가 이미 쥐고 있거나(상태를 바꾼 직후) 값을
 * 인자로 복사해서 넘긴다. 여기서 다시 잡으면 그 자리들이 전부 데드락이다. */
static void publish_state(bool active, unsigned long session,
                          const char *owner, const char *reason)
{
    char json[256];
    int n = snprintf(json, sizeof(json),
                     "{\"node_id\":\"rpic\",\"timestamp\":%lld,"
                     "\"active\":%s,\"session_id\":%lu,"
                     "\"owner\":\"%s\",\"reason\":\"%s\"}",
                     (long long)time(NULL) * 1000LL,
                     active ? "true" : "false", session,
                     owner ? owner : "", reason ? reason : "");

    if (n <= 0)
        return;
    if (n > (int)sizeof(json))
        n = (int)sizeof(json);
    /* retained - 나중에 켜진 VMS 도 구독 즉시 현재 소유자를 받는다. */
    (void)mqtt_sub_publish_retained(GUARDX_BROADCAST_STATE_TOPIC, json, n);
}

/* --------------------------------------------------------------- 사이렌 */

/* 사이렌을 내보내야 하는 조건. 화재 중이고 방송이 없을 때만. */
static bool siren_wanted_locked(void)
{
    return fire_active && !broadcast_active;
}

/* 워커를 깨운다.
 *
 * 상태를 바꾸는 자리에서 바로 깨우면 안 된다 - 수신기 정지·장치 반납 대기·
 * 일시정지 해제 같은 뒷정리는 잠금을 푼 뒤에 하는데, 그 사이 워커가 먼저
 * 사이렌을 시도해 EBUSY 로 튕기거나(장치가 아직 방송 것) 일시정지에 걸려
 * 조용히 버려진다. 그래서 뒷정리가 끝난 다음에만 이걸 부른다. */
static void wake_worker(void)
{
    pthread_mutex_lock(&lock);
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
}

/* KEEPALIVE 가 끊긴 방송을 정리한다. lock 을 쥔 채 호출하고, 실제 정지가
 * 필요하면 true 를 돌려 호출부가 lock 밖에서 처리하게 한다. */
static bool keepalive_expired_locked(void)
{
    if (!broadcast_active)
        return false;
    return elapsed_ms(&last_keepalive) > GUARDX_BROADCAST_TIMEOUT_MS;
}

static void *worker_main(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&lock);
    while (worker_running) {
        if (keepalive_expired_locked()) {
            unsigned long dead = broadcast_session;
            char dead_owner[sizeof(broadcast_owner)];

            memcpy(dead_owner, broadcast_owner, sizeof(dead_owner));
            broadcast_active = false;
            broadcast_session = 0;
            broadcast_owner[0] = '\0';
            pthread_mutex_unlock(&lock);

            /* 방송을 걸었던 VMS 는 대개 이 시점에 이미 못 듣는다(그래서
             * 만료된 것이다). 그래도 발행해야 **다른** VMS 들이 "이제
             * 비었다"를 알고, 되살아난 그 VMS 도 retained 로 받는다. */
            publish_state(false, dead, dead_owner, "timeout");

            fprintf(stderr,
                    "arbiter: KEEPALIVE %dms 만료 - 방송 세션 %lu 종료\n",
                    GUARDX_BROADCAST_TIMEOUT_MS, dead);
            (void)systemctl("stop");
            (void)wait_device_release();
            audio_event_set_paused(false);

            pthread_mutex_lock(&lock);
            continue;   /* 화재 중이면 아래 루프가 곧바로 사이렌을 재개한다 */
        }

        if (!siren_wanted_locked()) {
            /* 방송 중에는 KEEPALIVE 만료를 봐야 하므로 무기한 대기하지 않는다.
             * 유휴 상태(방송도 화재도 없음)에서만 신호를 기다린다. */
            if (broadcast_active) {
                struct timespec until;

                clock_gettime(CLOCK_REALTIME, &until);
                until.tv_sec += KEEPALIVE_CHECK_MS / 1000;
                pthread_cond_timedwait(&cond, &lock, &until);
            } else {
                pthread_cond_wait(&cond, &lock);
            }
            continue;
        }
        pthread_mutex_unlock(&lock);

        /* 사이렌이 안 울릴 때 원인을 로그로 좁힐 수 있게, 시도 시점의 장치
         * 상태를 같이 남긴다. held 면 누군가 아직 스피커를 쥔 것이다. */
        printf("arbiter: 사이렌 재생 시도 (스피커 %s)\n",
               speaker_held() ? "점유중" : "여유");
        audio_event_play(AUDIO_SCENE_FIRE);
        sleep_ms(SIREN_REPEAT_MS);

        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* ------------------------------------------------------------------ API */

guardx_err_t audio_arbiter_init(void)
{
    pthread_mutex_lock(&lock);
    if (worker_running) {
        pthread_mutex_unlock(&lock);
        return GUARDX_OK;
    }
    worker_running = true;
    fire_active = false;
    broadcast_active = false;
    broadcast_session = 0;
    pthread_mutex_unlock(&lock);

    /* 기동 시 알려진 안전 상태로 맞춘다.
     *
     * 앞선 실행이 방송 중에 죽었거나 유닛이 enable 된 채 부팅했으면 수신기가
     * 스피커를 쥔 채로 남아 있다. 우리 상태는 "방송 없음"으로 시작하므로 그
     * 불일치를 그대로 두면 첫 화재에서 사이렌이 EBUSY 로 막힌다.
     * 액추에이터가 apply_safe_state() 로 시작하는 것과 같은 원칙이다. */
    if (!systemctl("stop"))
        fprintf(stderr,
                "arbiter: 경고 - 기동 시 %s 정지에 실패했습니다.\n"
                "  root 로 실행 중이 아니면 사이렌이 스피커를 못 잡을 수 있습니다.\n",
                RTP_UNIT);

    if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
        pthread_mutex_lock(&lock);
        worker_running = false;
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "arbiter: 워커 시작 실패 - 사이렌 반복이 비활성\n");
        return GUARDX_ERR_OPEN;
    }
    printf("arbiter: 오디오 조율 시작 (사이렌 <-> 방송 배타 점유)\n");
    return GUARDX_OK;
}

void audio_arbiter_set_fire(bool active)
{
    pthread_mutex_lock(&lock);
    /* 같은 화재 메시지가 반복 도착해도 여기서 걸러진다 - 전이일 때만 움직인다.
     * 이게 없으면 화재 중 운영자가 다시 시작한 방송이 다음 재발행에 끊긴다. */
    if (active == fire_active) {
        pthread_mutex_unlock(&lock);
        return;
    }
    fire_active = active;
    unsigned long preempted = 0;
    char preempted_owner[sizeof(broadcast_owner)];

    preempted_owner[0] = '\0';
    if (active) {
        preempted = broadcast_session;
        memcpy(preempted_owner, broadcast_owner, sizeof(preempted_owner));
        broadcast_active = false;
        broadcast_session = 0;
        broadcast_owner[0] = '\0';
    }
    printf("arbiter: 화재 %s\n", active ? "발생 - 사이렌 우선" : "해제");
    pthread_mutex_unlock(&lock);

    /* 화재가 방송을 선점했으면 그 사실을 알린다. 방송하던 VMS 는 이걸 보고
     * 마이크를 접는다 - 안 그러면 소리는 안 나가는데 화면만 "방송 중"이다. */
    if (preempted)
        publish_state(false, preempted, preempted_owner, "fire");

    if (!active) {
        audio_event_stop();   /* 화재 해제 - 울리던 사이렌을 바로 끊는다 */
        wake_worker();
        return;
    }

    /* broadcast_active 를 보지 않고 무조건 정지시킨다.
     *
     * 우리가 기억하는 상태와 실제가 어긋날 수 있기 때문이다: RTP 수신기가
     * 살아 있는 채로 rpic_subscriber 만 재시작되면 broadcast_active 는 false
     * 로 시작하는데, 그때 화재가 나면 "방송 없음"이라 믿고 장치를 열다 EBUSY
     * 로 사이렌을 통째로 놓친다. 안전 경로에서는 기억보다 실제를 믿는다 -
     * 이미 멈춰 있으면 systemctl stop 은 무해하다. */
    printf("arbiter: 방송 선점 - %s 정지\n", RTP_UNIT);
    audio_event_set_paused(false);   /* 방송 때 걸어둔 일시정지를 푼다 */

    if (!systemctl("stop"))
        fprintf(stderr,
                "arbiter: 위험 - 방송 정지 실패. 사이렌이 장치를 못 열 수 있습니다.\n");
    if (!wait_device_release())
        fprintf(stderr,
                "arbiter: 위험 - 스피커가 반납되지 않았습니다. 사이렌을 그대로 시도합니다.\n");

    /* 장치가 비고 일시정지가 풀린 지금에서야 깨운다. */
    wake_worker();
}

void audio_arbiter_broadcast_keepalive(unsigned long session)
{
    pthread_mutex_lock(&lock);
    if (broadcast_active && broadcast_session == session)
        now_mono(&last_keepalive);
    /* 진행 중이 아닌 세션의 KEEPALIVE 는 버린다(헤더 주석 참조). 화재 선점
     * 뒤에도 VMS 가 한동안 보내는데, 그걸 받아 방송을 되살리면 안 된다. */
    pthread_mutex_unlock(&lock);
}

void audio_arbiter_set_broadcast(bool active, unsigned long session,
                                 const char *owner, bool takeover)
{
    bool start_receiver = false;
    bool stop_receiver = false;
    unsigned long taken_session = 0;
    char taken_owner[sizeof(broadcast_owner)];
    char busy_owner[sizeof(broadcast_owner)];

    taken_owner[0] = '\0';
    busy_owner[0] = '\0';

    pthread_mutex_lock(&lock);
    if (active) {
        if (broadcast_active && broadcast_session == session) {
            /* 같은 세션의 재발행 = QoS1 중복 START 이거나 KEEPALIVE.
             * 상태는 그대로 두고 만료 시계만 되감는다. */
            now_mono(&last_keepalive);
            pthread_mutex_unlock(&lock);
            return;
        }
        if (broadcast_active && !takeover) {
            /* 다른 VMS 가 이미 쥐고 있다. 예전에는 나중 START 가 무조건
             * 이겨서, 서로를 모르는 VMS 두 대가 상대의 방송을 예고 없이
             * 끊었다. 이제는 거절하고 누가 쥐고 있는지만 알려준다 -
             * 그 VMS 가 확인창을 띄우고, 운영자가 고르면 takeover 로 다시
             * 온다. */
            unsigned long held = broadcast_session;

            memcpy(busy_owner, broadcast_owner, sizeof(busy_owner));
            pthread_mutex_unlock(&lock);
            /* 값은 잠금 안에서 복사해 두고 로그는 밖에서 찍는다 - 잠금을 푼
             * 뒤에 전역을 읽으면 그 사이 바뀐 값을 찍게 된다. */
            fprintf(stderr,
                    "arbiter: 방송 거절 - 이미 %s 가 방송 중 "
                    "(요청 session %lu, 진행 중 %lu)\n",
                    busy_owner[0] ? busy_owner : "(무명)", session, held);
            publish_result(session, GUARDX_BROADCAST_RESULT_BUSY,
                           "another VMS is broadcasting", busy_owner);
            return;
        }
        if (broadcast_active) {
            /* 인수. 이전 세션은 여기서 버려지므로 그 STOP 이 늦게 와도 아래
             * session 대조에서 걸린다. 인수당한 VMS 가 화면에 사유를 적을 수
             * 있도록 상태를 따로 한 번 발행한다. */
            taken_session = broadcast_session;
            memcpy(taken_owner, broadcast_owner, sizeof(taken_owner));
        }
        broadcast_active = true;
        broadcast_session = session;
        snprintf(broadcast_owner, sizeof(broadcast_owner), "%s",
                 owner ? owner : "");
        now_mono(&last_keepalive);
        start_receiver = true;
    } else {
        if (!broadcast_active) {
            pthread_mutex_unlock(&lock);
            return;
        }
        /* 낡은 STOP 차단. 이게 없으면 이전 방송의 늦은 STOP 하나가 방금 시작한
         * 방송을 끊고 사이렌을 되살린다. */
        if (session != broadcast_session) {
            fprintf(stderr,
                    "arbiter: 낡은 STOP 무시 (session %lu, 진행 중 %lu)\n",
                    session, broadcast_session);
            pthread_mutex_unlock(&lock);
            return;
        }
        memcpy(taken_owner, broadcast_owner, sizeof(taken_owner));
        broadcast_active = false;
        broadcast_session = 0;
        broadcast_owner[0] = '\0';
        stop_receiver = true;
    }
    printf("arbiter: 방송 %s (session %lu, owner %s)%s\n",
           active ? "시작" : "종료", session,
           active ? (owner && owner[0] ? owner : "(무명)")
                  : (taken_owner[0] ? taken_owner : "(무명)"),
           (!active && fire_active) ? " - 화재 지속, 사이렌 재개" : "");
    pthread_mutex_unlock(&lock);

    /* 인수당한 세션의 종료를 먼저 알린다. 새 소유자를 발행한 뒤에 보내면
     * 두 메시지의 순서가 뒤집혀, 인수당한 VMS 가 "내 방송이 끝났다"를 본 뒤
     * 다시 "누군가 방송 중"을 보게 되어 화면 문구가 흔들린다. */
    if (taken_session)
        publish_state(false, taken_session, taken_owner, "taken_over");

    if (start_receiver) {
        /* 방송이 장치를 열기 전에 사이렌을 확실히 내린다. set_paused(true) 는
         * 새 재생을 막고 진행 중인 것을 끊은 뒤 워커가 실제로 멈출 때까지
         * 기다려 준다 - 그래서 여기 한 줄이면 충분하다. */
        audio_event_set_paused(true);
        wake_worker();   /* 사이렌이 멎었음을 워커가 반영하게 한다 */
        if (!wait_device_release()) {
            publish_ready(session, false, "speaker busy");
            return;
        }
        /* 수신기는 방송 중에만 살아 있는다. 상시 실행이 유휴 상태에서도
         * 스피커를 물고 있어 사이렌을 막던 것이 이 결함의 출발점이었다. */
        if (!systemctl("start")) {
            char failed_owner[sizeof(broadcast_owner)];

            publish_ready(session, false, "receiver start failed");
            failed_owner[0] = '\0';
            pthread_mutex_lock(&lock);
            if (broadcast_session == session) {
                memcpy(failed_owner, broadcast_owner, sizeof(failed_owner));
                broadcast_active = false;
                broadcast_session = 0;
                broadcast_owner[0] = '\0';
            }
            pthread_mutex_unlock(&lock);
            /* 점유를 잡았다고 알린 적이 없으므로 여기서 "비었다"를 알려야
             * 다른 VMS 가 계속 busy 로 막히지 않는다. */
            publish_state(false, session, failed_owner, "error");
            audio_event_set_paused(false);
            wake_worker();   /* 일시정지를 푼 뒤에 깨운다(위 wake_worker 주석) */
            return;
        }
        /* 수신기가 실제로 스피커를 잡은 뒤에 READY 를 보낸다. 프로세스가 떴다는
         * 것만으로는 부족하다 - alsasink 가 장치를 열기 전에 VMS 가 쏘면 앞부분이
         * 잘린다. */
        if (!wait_device_acquire()) {
            fprintf(stderr,
                    "arbiter: 수신기가 %dms 내에 스피커를 잡지 못했습니다\n",
                    DEVICE_ACQUIRE_MAX_MS);
            publish_ready(session, false, "receiver not ready");
            return;
        }
        publish_ready(session, true, "");
        /* 점유 확정은 READY 뒤에 알린다. 먼저 알리면 다른 VMS 가 "방송 중"으로
         * 보는데 정작 소리는 아직 안 나가는 구간이 생긴다. */
        publish_state(true, session, owner, "");
        return;
    }

    if (stop_receiver) {
        (void)systemctl("stop");
        (void)wait_device_release();
        audio_event_set_paused(false);
        /* 여기서 깨워야 화재가 계속일 때 사이렌이 재개된다. 상태를 바꾸는
         * 자리에서 미리 깨우면 아직 paused 라 요청이 조용히 버려진다. */
        wake_worker();
        publish_state(false, session, taken_owner, "stopped");
    }
}

void audio_arbiter_republish_state(void)
{
    bool active;
    unsigned long session;
    char owner[sizeof(broadcast_owner)];

    pthread_mutex_lock(&lock);
    active = broadcast_active;
    session = broadcast_session;
    memcpy(owner, broadcast_owner, sizeof(owner));
    pthread_mutex_unlock(&lock);

    publish_state(active, session, owner, "");
}

void audio_arbiter_cleanup(void)
{
    pthread_mutex_lock(&lock);
    if (!worker_running) {
        pthread_mutex_unlock(&lock);
        return;
    }
    worker_running = false;
    broadcast_active = false;
    broadcast_session = 0;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);

    pthread_join(worker, NULL);

    /* 우리가 띄운 수신기는 우리가 내린다. 남겨두면 다음 기동까지 스피커를
     * 물고 있어 그 사이 화재 사이렌이 막힌다. */
    (void)systemctl("stop");
}
