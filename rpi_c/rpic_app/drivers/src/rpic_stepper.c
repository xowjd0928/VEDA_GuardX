/*
 * rpic_stepper.c - GuardX RPi C 스텝모터 드라이버 (28BYJ-48 + ULN2003, 문 개폐)
 *                  + 방향별 리밋 리드센서 정지 (CW:GPIO17, CCW:GPIO27)
 *
 * GuardX_Driver_Convention.md 컨벤션 준수:
 *  - 1번: 모듈명 rpic_stepper
 *  - 2번: class_create() 독립 호출
 *  - 3번: /dev/rpic_stepper 자동 생성
 *  - 4번: major 동적 할당, minor 0 고정
 *  - 6번: ioctl 매직넘버 0xC5
 *  - 7번: gpiolib API + gpio_to_irq()/request_irq() 인터럽트
 *
 * 하프스텝(8상) 시퀀스로 28BYJ-48을 구동한다(문 개폐 기구). 연속 회전:
 * write()로 stepper_data_t(부호=방향, 0=정지)를 받아 방향만 설정하고 즉시
 * 반환한다. 실제 회전은 커널 스레드(stepper_thread_fn)가 백그라운드로 계속
 * 돌리며, 정지 명령(value=0) 또는 그 방향 리밋 감지까지 이어간다.
 *
 * 방향별 리밋 정지(원본 파이썬 gpiozero 리드센서를 커널로 이관):
 *   - CW(+)로 돌 때  GPIO17 감지(LOW) -> 즉시 정지
 *   - CCW(-)로 돌 때 GPIO27 감지(LOW) -> 즉시 정지
 * 각 센서는 IRQ(양 에지)로 감시. 반대 방향으로는 그 리밋에 걸려 있어도
 * 빠져나갈 수 있다. 감지/해제는 dmesg로 확인. 상세는 rpic_stepper.h.
 *
 * !!! GPIO17/GPIO27은 풀업이 필요하다(접점이 GND). config.txt에
 *     'gpio=17=ip,pu' 와 'gpio=27=ip,pu' 를 넣거나 리드센서 모듈의 온보드
 *     풀업을 쓸 것. 없으면 감지 상태가 플로팅으로 튈 수 있다.
 *
 * 실물 미연결 상태에서는 simulate=1로 로드 - GPIO/스레드/IRQ 없이 로그만.
 * "SIMULATION MODE" 표시 블록이 실물 연결 시 제거 대상.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "rpic_stepper.h"

#define DEVICE_NAME "rpic_stepper"

/* class_create()가 커널 6.4부터 owner 인자를 받지 않음 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define GUARDX_CLASS_CREATE(name) class_create(name)
#else
#define GUARDX_CLASS_CREATE(name) class_create(THIS_MODULE, name)
#endif

/* BCM 핀 번호 (배선 기준, rpic_stepper.h) */
static const unsigned int pins[RPIC_STEPPER_PIN_COUNT] = {
    RPIC_STEPPER_GPIO_IN1,
    RPIC_STEPPER_GPIO_IN2,
    RPIC_STEPPER_GPIO_IN3,
    RPIC_STEPPER_GPIO_IN4,
};

/* gpiolib에 넘길 실효 전역 GPIO 번호 = gpio_base + BCM. init에서 채운다. */
static unsigned int line[RPIC_STEPPER_PIN_COUNT];

/* 하프스텝 8상 시퀀스 (검증 코드 stepper.c와 동일) */
static const u8 half_step_seq[STEPPER_HALF_STEPS][RPIC_STEPPER_PIN_COUNT] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1},
};

static int major;
static struct class *rpic_stepper_class;
static struct device *rpic_stepper_device;

static stepper_data_t state;   /* 마지막 설정한 방향값(부호=방향, 0=정지) */

/* 회전 스레드가 참조하는 현재 방향: +1=CW, -1=CCW, 0=정지.
 * write/ioctl(프로세스), 리밋 IRQ(인터럽트)가 WRITE_ONCE로 갱신하고
 * 스레드가 READ_ONCE로 읽는다. 단일 int 원자 접근이라 별도 락 불필요. */
static int direction;
static struct task_struct *stepper_thread;
static DECLARE_WAIT_QUEUE_HEAD(stepper_wq);

/* 문 위치는 센서의 순간 ACTIVE/RELEASE 상태와 분리해 관리한다.
 * TOP/BOTTOM은 해당 끝 센서에 도달했을 때 고정한다. 새 회전 명령에서는
 * 그 방향 센서를 직접 읽어 실제로 해제돼 있으면 BETWEEN으로 최신화한다.
 * 따라서 센서 해제 IRQ를 놓쳐도 과거 active 값 때문에 막히지 않는다. */
enum door_position {
    DOOR_BETWEEN = 0,
    DOOR_TOP,
    DOOR_BOTTOM,
};

static int door_position = DOOR_BETWEEN;

/* --- 방향별 리밋 리드센서 (CW:GPIO17, CCW:GPIO27) --- */
struct limit_sensor {
    unsigned int   line;          /* 실효 GPIO (base + BCM) */
    int            irq;           /* -1 = 미등록 */
    int            active;        /* 1 = 감지(LOW), 0 = 해제 */
    int            dir_sign;      /* 이 센서가 막는 방향: +1=CW, -1=CCW */
    int            end_position;  /* 감지 시 고정할 논리 위치(TOP/BOTTOM) */
    struct delayed_work debounce_work; /* 에지 발생 시 다중 표본으로 상태 재확인 */
    const char    *label;
};

static struct limit_sensor cw_limit = {
    .irq = -1, .dir_sign = 1,  .end_position = DOOR_TOP,
    .label = "CW(GPIO17)",
};
static struct limit_sensor ccw_limit = {
    .irq = -1, .dir_sign = -1, .end_position = DOOR_BOTTOM,
    .label = "CCW(GPIO27)",
};

static DEFINE_MUTEX(stepper_lock);

/* =====================================================================
 * [SIMULATION MODE] 실물(모터+ULN2003+리드센서) 없이 App/MQTT 경로 테스트용.
 *   insmod rpic_stepper.ko simulate=1
 *
 * >>> 실물 연결 시 할 일 <<<
 *   1) simulate / simulate_fail 두 module_param 블록 삭제
 *   2) 코드 내 "SIMULATION MODE" 분기 전부 삭제
 *   3) init의 "if (!simulate)" 조건 제거하고 gpio_request + 스레드 + IRQ를
 *      항상 실행하도록 되돌리기
 * ===================================================================== */
static bool simulate = false;
module_param(simulate, bool, 0644);
MODULE_PARM_DESC(simulate, "1=GPIO/스레드/IRQ 대신 로그만 출력 (모터 미연결 상태 테스트용)");

static bool simulate_fail = false;
module_param(simulate_fail, bool, 0644);
MODULE_PARM_DESC(simulate_fail, "1=write 강제 실패(-EIO), rmmod 없이 장애 재현용");

/* gpiolib 전역 GPIO base. 최신 커널은 메인 gpiochip base가 0이 아니다
 * (Pi4/BCM2711 + 6.x = 512). 실효 번호 = gpio_base + BCM.
 * 확인: cat /sys/class/gpio/gpiochipN/base  또는  gpioinfo
 * 커널이 base를 바꾸면 insmod ... gpio_base=NNN 으로 덮어쓴다. */
static int gpio_base = 512;
module_param(gpio_base, int, 0644);
MODULE_PARM_DESC(gpio_base, "메인 gpiochip 전역 base (기본 512, BCM에 더해짐)");

/* ---------------------------------------------------------------------
 * 스텝모터 제어
 * --------------------------------------------------------------------- */

/* 한 스텝의 4핀 코일 상태 출력 */
static void set_coils(unsigned int seq_index)
{
    unsigned int i;

    for (i = 0; i < RPIC_STEPPER_PIN_COUNT; i++)
        gpio_set_value(line[i], half_step_seq[seq_index][i]);
}

/* 4핀 전부 LOW - 코일 해제(정지 상태에서 발열/소비 방지) */
static void release_coils(void)
{
    unsigned int i;

    for (i = 0; i < RPIC_STEPPER_PIN_COUNT; i++)
        gpio_set_value(line[i], 0);
}

/* 회전 스레드: direction이 0이면 코일 해제 후 대기, 0이 아니면 그 방향
 * 으로 하프스텝을 계속 진행한다. 정지/방향 변경/리밋 감지/언로드 시 wq로 깨어난다. */
static int stepper_thread_fn(void *data)
{
    unsigned int idx = 0;

    while (!kthread_should_stop()) {
        int d = READ_ONCE(direction);

        if (d == 0) {
            release_coils();
            wait_event_interruptible(stepper_wq,
                READ_ONCE(direction) != 0 || kthread_should_stop());
            continue;
        }

        set_coils(idx);
        usleep_range(STEPPER_STEP_DELAY_US, STEPPER_STEP_DELAY_US + 100);

        /* CW: 시퀀스 정방향, CCW: 역방향(= +7 mod 8) */
        idx = (idx + (d > 0 ? 1u : (STEPPER_HALF_STEPS - 1u)))
              % STEPPER_HALF_STEPS;
    }

    release_coils();
    return 0;
}

/* 방향 설정. 부호만 보고 +1/0/-1로 정규화한 뒤 스레드를 깨운다. */
static int apply_stepper(stepper_data_t value)
{
    int dir = (value > 0) - (value < 0);   /* +1 / 0 / -1 */

    /* 회전 시작 전에 그 방향의 센서를 직접 읽어 논리 위치를 최신화한다.
     * LOW면 실제 끝에 있으므로 TOP/BOTTOM으로 고정하고 같은 방향을 막는다.
     * HIGH면 저장된 끝 상태가 오래된 것이므로 BETWEEN으로 바꾸고 허용한다.
     * 회전 중 끝 도달은 기존 리밋 IRQ가 처리한다. */
    if (!simulate && dir != 0) {
        struct limit_sensor *target = dir > 0 ? &cw_limit : &ccw_limit;
        int active = (gpio_get_value(target->line) == 0);

        WRITE_ONCE(target->active, active);
        if (active) {
            WRITE_ONCE(door_position, target->end_position);
            pr_info("%s: at %s - %s rotate ignored\n",
                    DEVICE_NAME,
                    target->end_position == DOOR_TOP ? "TOP" : "BOTTOM",
                    dir > 0 ? "CW" : "CCW");
            return -EBUSY;
        }

        WRITE_ONCE(door_position, DOOR_BETWEEN);
    }

    /* [SIMULATION MODE] - 실물 연결 시 이 블록 삭제 */
    if (simulate) {
        if (simulate_fail)
            return -EIO;
        if (dir != 0)
            WRITE_ONCE(door_position, DOOR_BETWEEN);
        state = value;
        pr_info("%s: %s (simulate)\n", DEVICE_NAME,
                dir > 0 ? "CW start" : dir < 0 ? "CCW start" : "STOP");
        return 0;
    }

    /* [REAL HARDWARE] 방향만 갱신하고 스레드에 맡긴다(비블로킹) */
    WRITE_ONCE(direction, dir);
    state = value;
    wake_up_interruptible(&stepper_wq);

    pr_info("%s: %s\n", DEVICE_NAME,
            dir > 0 ? "CW start" : dir < 0 ? "CCW start" : "STOP");
    return 0;
}

/* ---------------------------------------------------------------------
 * [REAL HARDWARE] 방향별 리밋 리드센서 IRQ (양 에지)
 *   LOW(접점 GND) = 감지. 안정 상태 확인 뒤 그 방향으로 돌고 있으면 정지.
 * --------------------------------------------------------------------- */
static void limit_debounce_work(struct work_struct *work)
{
    struct limit_sensor *s = container_of(to_delayed_work(work),
                                          struct limit_sensor,
                                          debounce_work);
    int low_count = 0;
    int i;
    int val;

    /* IRQ 에지가 계속 들어와도 확인 시점을 뒤로 미루지 않는다. 짧은 간격으로
     * 여러 번 읽어 대부분 LOW일 때만 실제 접촉으로 확정한다. 화재 동시 부하의
     * 순간 LOW는 탈락하고, 진짜 리드센서의 채터링은 LOW 다수결로 잡힌다. */
    for (i = 0; i < STEPPER_LIMIT_SAMPLES; i++) {
        if (gpio_get_value(s->line) == 0)
            low_count++;
        if (i + 1 < STEPPER_LIMIT_SAMPLES)
            msleep(STEPPER_LIMIT_SAMPLE_MS);
    }
    val = low_count >= STEPPER_LIMIT_LOW_MIN ? 0 : 1;

    mutex_lock(&stepper_lock);
    if (val == 0) {
        int moving_dir = READ_ONCE(direction);

        /* active는 ioctl 진단용 전기 상태로만 유지한다.
         * 논리 위치는 그 센서 방향으로 이동하다 실제 도달했을 때만 고정한다.
         * 반대 방향으로 빠져나가는 중 발생한 접점 바운스는 위치를 되돌리지 않는다. */
        WRITE_ONCE(s->active, 1);
        if (moving_dir == s->dir_sign) {
            WRITE_ONCE(door_position, s->end_position);
            WRITE_ONCE(direction, 0);
            wake_up_interruptible(&stepper_wq);
            pr_info("%s: %s limit hit -> stepper stop, position=%s\n",
                    DEVICE_NAME, s->label,
                    s->end_position == DOOR_TOP ? "TOP" : "BOTTOM");
        } else if (moving_dir == 0) {
            WRITE_ONCE(door_position, s->end_position);
            pr_info("%s: %s limit detected, position=%s\n",
                    DEVICE_NAME, s->label,
                    s->end_position == DOOR_TOP ? "TOP" : "BOTTOM");
        } else {
            pr_info("%s: %s limit detected while moving away (position unchanged)\n",
                    DEVICE_NAME, s->label);
        }
    } else {
        /* RELEASE는 전기 상태/로그만 갱신한다. 논리 위치는 다음 회전
         * 명령을 받았을 때 해당 방향 센서를 직접 읽어 최신화한다. */
        WRITE_ONCE(s->active, 0);
        pr_info("%s: %s limit cleared\n", DEVICE_NAME, s->label);
    }
    mutex_unlock(&stepper_lock);
}

static irqreturn_t cw_irq_handler(int irq, void *dev_id)
{
    (void)irq;
    (void)dev_id;
    queue_delayed_work(system_wq, &cw_limit.debounce_work, 0);
    return IRQ_HANDLED;
}

static irqreturn_t ccw_irq_handler(int irq, void *dev_id)
{
    (void)irq;
    (void)dev_id;
    queue_delayed_work(system_wq, &ccw_limit.debounce_work, 0);
    return IRQ_HANDLED;
}

/* ---------------------------------------------------------------------
 * file_operations
 * --------------------------------------------------------------------- */

static int rpic_stepper_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int rpic_stepper_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t rpic_stepper_write(struct file *file, const char __user *buf,
                                  size_t count, loff_t *ppos)
{
    stepper_data_t value;
    int ret;

    if (count < sizeof(stepper_data_t))
        return -EINVAL;

    if (copy_from_user(&value, buf, sizeof(stepper_data_t)))
        return -EFAULT;

    mutex_lock(&stepper_lock);
    ret = apply_stepper(value);
    mutex_unlock(&stepper_lock);

    if (ret < 0)
        return ret;

    return sizeof(stepper_data_t);
}

static ssize_t rpic_stepper_read(struct file *file, char __user *buf,
                                 size_t count, loff_t *ppos)
{
    stepper_data_t snap;

    if (count < sizeof(stepper_data_t))
        return -EINVAL;

    mutex_lock(&stepper_lock);
    snap = state;
    mutex_unlock(&stepper_lock);

    if (copy_to_user(buf, &snap, sizeof(stepper_data_t)))
        return -EFAULT;

    return sizeof(stepper_data_t);
}

static long rpic_stepper_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    stepper_data_t val;
    __s32 limits;
    int ret;

    switch (cmd) {
    case STEPPER_IOC_SET:
        if (copy_from_user(&val, (void __user *)arg, sizeof(val)))
            return -EFAULT;

        mutex_lock(&stepper_lock);
        ret = apply_stepper(val);
        mutex_unlock(&stepper_lock);
        return ret;

    case STEPPER_IOC_GET:
        mutex_lock(&stepper_lock);
        val = state;
        mutex_unlock(&stepper_lock);

        if (copy_to_user((void __user *)arg, &val, sizeof(val)))
            return -EFAULT;
        return 0;

    case STEPPER_IOC_GET_LIMITS:
        /* bit0 = CW(GPIO17) 감지, bit1 = CCW(GPIO27) 감지 */
        limits = (READ_ONCE(cw_limit.active) ? 1 : 0) |
                 (READ_ONCE(ccw_limit.active) ? 2 : 0);
        if (copy_to_user((void __user *)arg, &limits, sizeof(limits)))
            return -EFAULT;
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations rpic_stepper_fops = {
    .owner          = THIS_MODULE,
    .open           = rpic_stepper_open,
    .release        = rpic_stepper_release,
    .read           = rpic_stepper_read,
    .write          = rpic_stepper_write,
    .unlocked_ioctl = rpic_stepper_ioctl,
};

/* ---------------------------------------------------------------------
 * module init / exit
 * --------------------------------------------------------------------- */

/* 요청한 개수만큼 gpio_request + 출력(LOW) 설정. 실패 시 이미 잡은
 * 것들을 되감고 음수 반환. */
static int stepper_request_gpios(unsigned int upto)
{
    unsigned int i;
    int ret;

    for (i = 0; i < upto; i++) {
        ret = gpio_request(line[i], DEVICE_NAME);
        if (ret < 0) {
            pr_err("%s: gpio_request(%u = base %d + BCM %u) failed (%d)\n",
                   DEVICE_NAME, line[i], gpio_base, pins[i], ret);
            while (i > 0)
                gpio_free(line[--i]);
            return ret;
        }
        gpio_direction_output(line[i], 0);
    }
    return 0;
}

/* [REAL HARDWARE] 리밋 센서 하나: GPIO 요청 + 입력 설정 + 양 에지 IRQ.
 * 실패 시 이미 잡은 GPIO를 되감고 음수 반환. */
static int stepper_request_limit(struct limit_sensor *s, unsigned int bcm,
                                 irq_handler_t handler)
{
    int ret;

    INIT_DELAYED_WORK(&s->debounce_work, limit_debounce_work);

    ret = gpio_request(s->line, DEVICE_NAME);
    if (ret < 0) {
        pr_err("%s: gpio_request(%s=%u=base %d+BCM %u) failed (%d)\n",
               DEVICE_NAME, s->label, s->line, gpio_base, bcm, ret);
        return ret;
    }
    gpio_direction_input(s->line);

    s->irq = gpio_to_irq(s->line);
    if (s->irq < 0) {
        pr_err("%s: gpio_to_irq(%s) failed (%d)\n", DEVICE_NAME, s->label, s->irq);
        ret = s->irq;
        s->irq = -1;
        goto err_free;
    }

    /* 초기 상태 실측(부팅 시 이미 걸려 있을 수 있음) */
    s->active = (gpio_get_value(s->line) == 0) ? 1 : 0;

    ret = request_irq(s->irq, handler,
                      IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                      DEVICE_NAME, s);
    if (ret < 0) {
        pr_err("%s: request_irq(%s) failed (%d)\n", DEVICE_NAME, s->label, ret);
        s->irq = -1;
        goto err_free;
    }
    return 0;

err_free:
    gpio_free(s->line);
    return ret;
}

static void stepper_free_limit(struct limit_sensor *s)
{
    if (s->irq >= 0) {
        free_irq(s->irq, s);
        cancel_delayed_work_sync(&s->debounce_work);
        gpio_free(s->line);
        s->irq = -1;
    }
}

static int __init rpic_stepper_init(void)
{
    int ret;
    dev_t devno;

    major = register_chrdev(0, DEVICE_NAME, &rpic_stepper_fops);
    if (major < 0) {
        pr_err("%s: register_chrdev failed (%d)\n", DEVICE_NAME, major);
        return major;
    }

    rpic_stepper_class = GUARDX_CLASS_CREATE(DEVICE_NAME);
    if (IS_ERR(rpic_stepper_class)) {
        ret = PTR_ERR(rpic_stepper_class);
        goto err_unregister;
    }

    devno = MKDEV(major, 0);
    rpic_stepper_device = device_create(rpic_stepper_class, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(rpic_stepper_device)) {
        ret = PTR_ERR(rpic_stepper_device);
        goto err_class_destroy;
    }

    /* BCM 핀 -> 실효 전역 GPIO 번호 (base + BCM) */
    {
        unsigned int i;
        for (i = 0; i < RPIC_STEPPER_PIN_COUNT; i++)
            line[i] = (unsigned int)gpio_base + pins[i];
    }
    cw_limit.line  = (unsigned int)gpio_base + RPIC_STEPPER_GPIO_CW_LIMIT;
    ccw_limit.line = (unsigned int)gpio_base + RPIC_STEPPER_GPIO_CCW_LIMIT;

    /* [SIMULATION MODE] simulate=1이면 물리 GPIO가 없을 수 있으므로
     * gpio_request/회전 스레드/IRQ 자체를 생략한다.
     * >>> 실물 연결 시: 이 if문 제거하고 아래 블록을
     *     무조건 실행하도록 되돌릴 것 <<< */
    if (!simulate) {
        ret = stepper_request_gpios(RPIC_STEPPER_PIN_COUNT);
        if (ret < 0)
            goto err_device_destroy;

        ret = stepper_request_limit(&cw_limit, RPIC_STEPPER_GPIO_CW_LIMIT,
                                    cw_irq_handler);
        if (ret < 0)
            goto err_gpio_free;

        ret = stepper_request_limit(&ccw_limit, RPIC_STEPPER_GPIO_CCW_LIMIT,
                                    ccw_irq_handler);
        if (ret < 0)
            goto err_cw_free;

        /* 모듈 적재 시 실제 센서를 한 번 읽어 초기 논리 위치를 정한다. */
        if (READ_ONCE(cw_limit.active) && READ_ONCE(ccw_limit.active)) {
            WRITE_ONCE(door_position, DOOR_BETWEEN);
            pr_warn("%s: both limit sensors active at load - position unknown\n",
                    DEVICE_NAME);
        } else if (READ_ONCE(cw_limit.active)) {
            WRITE_ONCE(door_position, DOOR_TOP);
            pr_info("%s: initial position=TOP\n", DEVICE_NAME);
        } else if (READ_ONCE(ccw_limit.active)) {
            WRITE_ONCE(door_position, DOOR_BOTTOM);
            pr_info("%s: initial position=BOTTOM\n", DEVICE_NAME);
        } else {
            WRITE_ONCE(door_position, DOOR_BETWEEN);
            pr_info("%s: initial position=BETWEEN\n", DEVICE_NAME);
        }

        stepper_thread = kthread_run(stepper_thread_fn, NULL, DEVICE_NAME);
        if (IS_ERR(stepper_thread)) {
            ret = PTR_ERR(stepper_thread);
            stepper_thread = NULL;
            pr_err("%s: kthread_run failed (%d)\n", DEVICE_NAME, ret);
            goto err_ccw_free;
        }
    } else {
        pr_info("%s: SIMULATION MODE enabled (no real GPIO/IRQ access)\n", DEVICE_NAME);
    }

    pr_info("%s: loaded, major=%d, base=%d, gpio BCM IN1~4=%d/%d/%d/%d, CW_lim=%d, CCW_lim=%d, simulate=%d\n",
            DEVICE_NAME, major, gpio_base,
            RPIC_STEPPER_GPIO_IN1, RPIC_STEPPER_GPIO_IN2,
            RPIC_STEPPER_GPIO_IN3, RPIC_STEPPER_GPIO_IN4,
            RPIC_STEPPER_GPIO_CW_LIMIT, RPIC_STEPPER_GPIO_CCW_LIMIT, simulate);
    return 0;

err_ccw_free:
    stepper_free_limit(&ccw_limit);
err_cw_free:
    stepper_free_limit(&cw_limit);
err_gpio_free:
    {
        unsigned int i;
        for (i = 0; i < RPIC_STEPPER_PIN_COUNT; i++)
            gpio_free(line[i]);
    }
err_device_destroy:
    device_destroy(rpic_stepper_class, devno);
err_class_destroy:
    class_destroy(rpic_stepper_class);
err_unregister:
    unregister_chrdev(major, DEVICE_NAME);
    return ret;
}

static void __exit rpic_stepper_exit(void)
{
    unsigned int i;

    /* 회전 스레드 정지 -> 스레드가 종료 직전 release_coils()로 코일 해제.
     * WRITE_ONCE(0)로 방향을 먼저 끄고, kthread_stop이 wq를 깨워 종료시킨다. */
    if (stepper_thread) {
        WRITE_ONCE(direction, 0);
        kthread_stop(stepper_thread);   /* 스레드 종료까지 블로킹 */
        stepper_thread = NULL;
    }

    /* [SIMULATION MODE] gpio_request/IRQ를 안 했으면 해제도 생략.
     * (코일 해제는 스레드가 이미 처리했지만, 안전하게 한 번 더) */
    if (!simulate) {
        stepper_free_limit(&ccw_limit);
        stepper_free_limit(&cw_limit);
        release_coils();
        for (i = 0; i < RPIC_STEPPER_PIN_COUNT; i++)
            gpio_free(line[i]);
    }
    device_destroy(rpic_stepper_class, MKDEV(major, 0));
    class_destroy(rpic_stepper_class);
    unregister_chrdev(major, DEVICE_NAME);
    pr_info("%s: unloaded\n", DEVICE_NAME);
}

module_init(rpic_stepper_init);
module_exit(rpic_stepper_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GuardX");
MODULE_DESCRIPTION("GuardX RPi C Stepper Motor Driver (28BYJ-48, continuous + CW/CCW limit sensors)");
