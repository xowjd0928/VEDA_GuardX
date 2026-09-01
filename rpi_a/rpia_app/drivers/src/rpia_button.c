/*
 * rpia_button.c - GuardX RPi A 비상 버튼 드라이버 (GPIO 인터럽트)
 *
 * GuardX_Driver_Convention.md 컨벤션 준수:
 *  - 1번: 모듈명 rpia_button
 *  - 2번: class_create() 독립 호출
 *  - 3번: /dev/rpia_button 자동 생성
 *  - 4번: major 동적 할당, minor 0 고정
 *  - 6번: ioctl 매직넘버 0xA4
 *  - 7번: gpiolib + gpio_to_irq()/request_irq() 인터럽트 기반
 *
 * 유일한 인터럽트 기반 디바이스. IRQ 핸들러에서 이벤트 카운터를 올리고
 * wake_up_interruptible()로 poll 대기자를 깨운다. App은 poll(POLLIN) ->
 * read()로 눌림 횟수를 가져간다(카운터 리셋).
 *
 * 이 드라이버는 비상 버튼의 "로깅 경로"(-> RPi B MQTT QoS2) 전용이며,
 * "제어 경로"(-> RPi C 유선 GPIO/릴레이 인터락)는 커널/App과 무관하게
 * 하드웨어로 즉시 동작한다.
 *
 * !!! GPIO 번호 주의 (2026-07-24, Pi4 실기 확인) !!!
 *   최신 커널은 pinctrl-bcm2711 gpiochip의 base가 512다. 그래서 legacy
 *   gpio_request/gpio_to_irq에 넘길 전역 번호는 BCM 번호 + 512 (rpia_button.h
 *   의 RPIA_BUTTON_GPIO 참조). 처음에 BCM 번호(23)를 그대로 넘겨서
 *   gpio_request가 -EPROBE_DEFER(517)로 실패했었다. SPI(rpia_adc)와 달리
 *   GPIO는 legacy API가 살아있어 오버레이 없이 번호만 맞추면 된다.
 *
 * 현재 실제 버튼 미연결 상태이면 simulate=1로 로드하면 GPIO IRQ 대신
 * debugfs(/sys/kernel/debug/rpia_button/press)에 write하는 것으로 가짜
 * 눌림 이벤트를 발생시킨다. 코드 내 "SIMULATION MODE" 표시된 블록이
 * 실제 버튼 연결 시 제거 대상이다.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/moduleparam.h>
#include <linux/debugfs.h>
#include <linux/version.h>

#include "rpia_button.h"

#define DEVICE_NAME "rpia_button"

/* class_create()가 커널 6.4부터 owner 인자를 받지 않음 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define GUARDX_CLASS_CREATE(name) class_create(name)
#else
#define GUARDX_CLASS_CREATE(name) class_create(THIS_MODULE, name)
#endif

/* del_timer_sync()가 커널 6.10부터 timer_delete_sync()로 이름 변경 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#define GUARDX_TIMER_DELETE_SYNC(t) timer_delete_sync(t)
#else
#define GUARDX_TIMER_DELETE_SYNC(t) del_timer_sync(t)
#endif

static int major;
static struct class *rpia_button_class;
static struct device *rpia_button_device;
static int button_irq = -1;

/* 이벤트 상태: IRQ 핸들러(인터럽트 컨텍스트)와 read/poll(프로세스
 * 컨텍스트)이 공유하므로 spinlock으로 보호한다. */
static DEFINE_SPINLOCK(button_lock);
static u32 press_count;
static DECLARE_WAIT_QUEUE_HEAD(button_waitq);

/* 기계식 버튼 채터링(bouncing)/노이즈 방지: retriggerable(trailing-edge)
 * 디바운스. 엣지가 들어올 때마다 타이머를 BUTTON_DEBOUNCE_MS 뒤로 재무장하고,
 * 그 시간 동안 조용해야 그때 딱 1회로 확정한다. 튀는 구간이 고정창(400ms)
 * 보다 길게 늘어져서 leading-edge 방식으로는 한 번에 여러 번 잡히던 문제
 * (2026-08-04 실측) 를 버스트 길이에 무관하게 해결한다. */
#define BUTTON_DEBOUNCE_MS 400
static struct timer_list debounce_timer;

static void debounce_timer_cb(struct timer_list *t)
{
    unsigned long flags;

    spin_lock_irqsave(&button_lock, flags);
    press_count++;
    spin_unlock_irqrestore(&button_lock, flags);

    wake_up_interruptible(&button_waitq);
}

/* 눌림 이벤트 공통 처리: 디바운스 타이머 재무장 (매 엣지마다 연장) */
static void button_event(void)
{
    mod_timer(&debounce_timer, jiffies + msecs_to_jiffies(BUTTON_DEBOUNCE_MS));
}

/* ---------------------------------------------------------------------
 * [REAL HARDWARE] IRQ 핸들러 - 실제 버튼 연결 시 그대로 유지
 * --------------------------------------------------------------------- */
static irqreturn_t rpia_button_irq_handler(int irq, void *dev_id)
{
    button_event();
    return IRQ_HANDLED;
}

/* =====================================================================
 * [SIMULATION MODE] 실제 버튼이 없어 debugfs write로 눌림 이벤트를
 * 흉내낸다.
 *   insmod rpia_button.ko simulate=1
 *   echo 1 > /sys/kernel/debug/rpia_button/press   # 눌림 1회 발생
 *
 * >>> 실제 버튼 연결 시 할 일 <<<
 *   1) simulate module_param 블록 삭제
 *   2) press_write()/debugfs 초기화·해제 함수 삭제
 *   3) rpia_button_init()의 "if (!simulate)" 조건 제거하고 GPIO+IRQ
 *      블록을 항상 실행하도록 되돌리기
 * ===================================================================== */
static bool simulate = false;
module_param(simulate, bool, 0644);
MODULE_PARM_DESC(simulate, "1=GPIO IRQ 대신 debugfs write로 눌림 이벤트 발생");

static struct dentry *button_debug_dir;

static ssize_t press_write(struct file *file, const char __user *buf,
                            size_t count, loff_t *ppos)
{
    button_event();
    return count;
}

static const struct file_operations press_fops = {
    .owner = THIS_MODULE,
    .write = press_write,
};

static void rpia_button_debugfs_init(void)
{
    button_debug_dir = debugfs_create_dir(DEVICE_NAME, NULL);
    if (IS_ERR_OR_NULL(button_debug_dir)) {
        pr_warn("%s: debugfs_create_dir failed, simulation trigger disabled\n",
                DEVICE_NAME);
        button_debug_dir = NULL;
        return;
    }
    debugfs_create_file("press", 0200, button_debug_dir, NULL, &press_fops);
}

static void rpia_button_debugfs_exit(void)
{
    debugfs_remove_recursive(button_debug_dir);
}
/* ===================== [SIMULATION MODE 끝] ========================= */

/* ---------------------------------------------------------------------
 * file_operations
 * --------------------------------------------------------------------- */

static int rpia_button_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int rpia_button_release(struct inode *inode, struct file *file)
{
    return 0;
}

/* read: 마지막 read 이후의 눌림 횟수를 반환하고 카운터를 리셋.
 * 눌림이 없으면 O_NONBLOCK이면 -EAGAIN, 블로킹이면 대기. */
static ssize_t rpia_button_read(struct file *file, char __user *buf,
                                 size_t count, loff_t *ppos)
{
    button_data_t data;
    unsigned long flags;

    if (count < sizeof(button_data_t))
        return -EINVAL;

    spin_lock_irqsave(&button_lock, flags);
    while (press_count == 0) {
        spin_unlock_irqrestore(&button_lock, flags);

        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        if (wait_event_interruptible(button_waitq, press_count > 0))
            return -ERESTARTSYS;

        spin_lock_irqsave(&button_lock, flags);
    }
    data = press_count;
    press_count = 0;
    spin_unlock_irqrestore(&button_lock, flags);

    if (copy_to_user(buf, &data, sizeof(button_data_t)))
        return -EFAULT;

    return sizeof(button_data_t);
}

static __poll_t rpia_button_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;
    unsigned long flags;

    poll_wait(file, &button_waitq, wait);

    spin_lock_irqsave(&button_lock, flags);
    if (press_count > 0)
        mask |= POLLIN | POLLRDNORM;
    spin_unlock_irqrestore(&button_lock, flags);

    return mask;
}

static long rpia_button_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    button_data_t data;
    unsigned long flags;

    switch (cmd) {
    case BUTTON_IOC_READ:
        spin_lock_irqsave(&button_lock, flags);
        data = press_count;
        press_count = 0;
        spin_unlock_irqrestore(&button_lock, flags);

        if (copy_to_user((void __user *)arg, &data, sizeof(button_data_t)))
            return -EFAULT;
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations rpia_button_fops = {
    .owner          = THIS_MODULE,
    .open           = rpia_button_open,
    .release        = rpia_button_release,
    .read           = rpia_button_read,
    .poll           = rpia_button_poll,
    .unlocked_ioctl = rpia_button_ioctl,
};

/* ---------------------------------------------------------------------
 * module init / exit
 * --------------------------------------------------------------------- */

static int __init rpia_button_init(void)
{
    int ret;
    dev_t devno;

    major = register_chrdev(0, DEVICE_NAME, &rpia_button_fops);
    if (major < 0) {
        pr_err("%s: register_chrdev failed (%d)\n", DEVICE_NAME, major);
        return major;
    }

    rpia_button_class = GUARDX_CLASS_CREATE(DEVICE_NAME);
    if (IS_ERR(rpia_button_class)) {
        ret = PTR_ERR(rpia_button_class);
        goto err_unregister;
    }

    devno = MKDEV(major, 0);
    rpia_button_device = device_create(rpia_button_class, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(rpia_button_device)) {
        ret = PTR_ERR(rpia_button_device);
        goto err_class_destroy;
    }

    timer_setup(&debounce_timer, debounce_timer_cb, 0);

    /* [SIMULATION MODE] simulate=1이면 GPIO/IRQ 대신 debugfs 트리거 사용.
     * >>> 실제 버튼 연결 시: if/else 제거하고 GPIO+IRQ 블록을
     *     무조건 실행하도록 되돌릴 것 <<< */
    if (!simulate) {
        /* [REAL HARDWARE]
         * RPIA_BUTTON_GPIO = BCM23 + 512 (gpiochip base). 번호만 맞으면
         * legacy gpio_request/gpio_to_irq가 정상 동작한다. */
        ret = gpio_request(RPIA_BUTTON_GPIO, DEVICE_NAME);
        if (ret < 0) {
            pr_err("%s: gpio_request(%d) failed (%d)\n",
                   DEVICE_NAME, RPIA_BUTTON_GPIO, ret);
            goto err_device_destroy;
        }
        gpio_direction_input(RPIA_BUTTON_GPIO);

        /* 내부 풀업(눌림 시 GND=falling edge) - 외부 저항 불필요 */
        gpiod_set_config(gpio_to_desc(RPIA_BUTTON_GPIO),
                         PIN_CONF_PACKED(PIN_CONFIG_BIAS_PULL_UP, 0));

        button_irq = gpio_to_irq(RPIA_BUTTON_GPIO);
        if (button_irq < 0) {
            pr_err("%s: gpio_to_irq failed (%d)\n", DEVICE_NAME, button_irq);
            ret = button_irq;
            goto err_gpio_free;
        }

        /* 내부 풀업 + 눌림 시 GND -> falling edge 트리거.
         * 실제 배선이 반대(풀다운+눌림 시 VCC)면 IRQF_TRIGGER_RISING으로
         * 변경할 것 */
        ret = request_irq(button_irq, rpia_button_irq_handler,
                          IRQF_TRIGGER_FALLING, DEVICE_NAME, NULL);
        if (ret < 0) {
            pr_err("%s: request_irq failed (%d)\n", DEVICE_NAME, ret);
            goto err_gpio_free;
        }
    } else {
        rpia_button_debugfs_init();
        pr_info("%s: SIMULATION MODE enabled (debugfs press trigger)\n", DEVICE_NAME);
    }

    press_count = 0;

    pr_info("%s: loaded, major=%d, gpio=%d, irq=%d, simulate=%d\n",
             DEVICE_NAME, major, RPIA_BUTTON_GPIO, button_irq, simulate);
    return 0;

err_gpio_free:
    gpio_free(RPIA_BUTTON_GPIO);
err_device_destroy:
    device_destroy(rpia_button_class, devno);
err_class_destroy:
    class_destroy(rpia_button_class);
err_unregister:
    unregister_chrdev(major, DEVICE_NAME);
    return ret;
}

static void __exit rpia_button_exit(void)
{
    /* 대기 중인 디바운스 타이머부터 정리 (콜백이 이후 해제될 자원을
     * 건드리지 않도록 IRQ/GPIO 해제보다 먼저) */
    GUARDX_TIMER_DELETE_SYNC(&debounce_timer);

    /* [SIMULATION MODE] IRQ/GPIO 대신 debugfs를 썼으면 그쪽을 정리 */
    if (!simulate) {
        free_irq(button_irq, NULL);
        gpio_free(RPIA_BUTTON_GPIO);
    } else {
        rpia_button_debugfs_exit();
    }

    device_destroy(rpia_button_class, MKDEV(major, 0));
    class_destroy(rpia_button_class);
    unregister_chrdev(major, DEVICE_NAME);
    pr_info("%s: unloaded\n", DEVICE_NAME);
}

module_init(rpia_button_init);
module_exit(rpia_button_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GuardX");
MODULE_DESCRIPTION("GuardX RPi A Emergency Button Driver (GPIO interrupt)");
