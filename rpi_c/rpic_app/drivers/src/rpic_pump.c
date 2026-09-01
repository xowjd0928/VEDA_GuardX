/*
 * rpic_pump.c - GuardX RPi C 워터펌프 드라이버 (HG7881 경유)
 *
 * GuardX_Driver_Convention.md 컨벤션 준수:
 *  - 1번: 모듈명 rpic_pump
 *  - 2번: class_create() 독립 호출
 *  - 3번: /dev/rpic_pump 자동 생성
 *  - 4번: major 동적 할당, minor 0 고정
 *  - 6번: ioctl 매직넘버 0xC2
 *  - 7번: gpiolib API 사용
 *
 * HG7881(L9110S) 모터 드라이버 채널 B로 펌프를 ON/OFF 스위칭한다.
 * IA/IB 논리와 배선은 rpic_pump.h 참조. write()로 pump_data_t(0/1)를
 * 받고, read()는 마지막 적용 상태를 반환한다.
 *
 * 실물 미연결 상태에서는 simulate=1로 로드 - GPIO 접근 없이 로그만.
 * "SIMULATION MODE" 표시 블록이 실물 연결 시 제거 대상.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/mutex.h>
#include <linux/moduleparam.h>
#include <linux/version.h>

#include "rpic_pump.h"

#define DEVICE_NAME "rpic_pump"

/* class_create()가 커널 6.4부터 owner 인자를 받지 않음 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define GUARDX_CLASS_CREATE(name) class_create(name)
#else
#define GUARDX_CLASS_CREATE(name) class_create(THIS_MODULE, name)
#endif

static int major;
static struct class *rpic_pump_class;
static struct device *rpic_pump_device;

static pump_data_t state;   /* 마지막 적용 상태 (0=OFF) */

static DEFINE_MUTEX(pump_lock);

/* =====================================================================
 * [SIMULATION MODE] 실물(HG7881+펌프) 없이 App/MQTT 경로 테스트용.
 *   insmod rpic_pump.ko simulate=1
 *
 * >>> 실물 연결 시 할 일 <<<
 *   1) simulate / simulate_fail 두 module_param 블록 삭제
 *   2) 코드 내 "SIMULATION MODE" 분기 전부 삭제
 *   3) init의 "if (!simulate)" 조건 제거하고 gpio_request를
 *      항상 실행하도록 되돌리기
 * ===================================================================== */
static bool simulate = false;
module_param(simulate, bool, 0644);
MODULE_PARM_DESC(simulate, "1=GPIO 대신 로그만 출력 (펌프 미연결 상태 테스트용)");

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

/* gpiolib에 넘길 실효 전역 GPIO 번호 = gpio_base + BCM. init에서 채운다. */
static unsigned int pump_ia;
static unsigned int pump_ib;

/* ---------------------------------------------------------------------
 * 펌프 제어 - HG7881 IA/IB 출력
 * --------------------------------------------------------------------- */
static int apply_pump(pump_data_t on)
{
    if (on != 0 && on != 1)
        return -EINVAL;

    /* [SIMULATION MODE] - 실물 연결 시 이 블록 삭제 */
    if (simulate) {
        if (simulate_fail)
            return -EIO;
        state = on;
        pr_info("%s: %s (simulate)\n", DEVICE_NAME, on ? "ON" : "OFF");
        return 0;
    }

    /* [REAL HARDWARE] IA=H/IB=L 정방향 구동, 둘 다 L이면 정지 */
    gpio_set_value(pump_ia, on ? 1 : 0);
    gpio_set_value(pump_ib, 0);
    state = on;
    pr_info("%s: %s\n", DEVICE_NAME, on ? "ON" : "OFF");
    return 0;
}

/* ---------------------------------------------------------------------
 * file_operations
 * --------------------------------------------------------------------- */

static int rpic_pump_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int rpic_pump_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t rpic_pump_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    pump_data_t on;
    int ret;

    if (count < sizeof(pump_data_t))
        return -EINVAL;

    if (copy_from_user(&on, buf, sizeof(pump_data_t)))
        return -EFAULT;

    mutex_lock(&pump_lock);
    ret = apply_pump(on);
    mutex_unlock(&pump_lock);

    if (ret < 0)
        return ret;

    return sizeof(pump_data_t);
}

static ssize_t rpic_pump_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    pump_data_t snap;

    if (count < sizeof(pump_data_t))
        return -EINVAL;

    mutex_lock(&pump_lock);
    snap = state;
    mutex_unlock(&pump_lock);

    if (copy_to_user(buf, &snap, sizeof(pump_data_t)))
        return -EFAULT;

    return sizeof(pump_data_t);
}

static long rpic_pump_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    pump_data_t val;
    int ret;

    switch (cmd) {
    case PUMP_IOC_SET:
        if (copy_from_user(&val, (void __user *)arg, sizeof(val)))
            return -EFAULT;

        mutex_lock(&pump_lock);
        ret = apply_pump(val);
        mutex_unlock(&pump_lock);
        return ret;

    case PUMP_IOC_GET:
        mutex_lock(&pump_lock);
        val = state;
        mutex_unlock(&pump_lock);

        if (copy_to_user((void __user *)arg, &val, sizeof(val)))
            return -EFAULT;
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations rpic_pump_fops = {
    .owner          = THIS_MODULE,
    .open           = rpic_pump_open,
    .release        = rpic_pump_release,
    .read           = rpic_pump_read,
    .write          = rpic_pump_write,
    .unlocked_ioctl = rpic_pump_ioctl,
};

/* ---------------------------------------------------------------------
 * module init / exit
 * --------------------------------------------------------------------- */

static int __init rpic_pump_init(void)
{
    int ret;
    dev_t devno;

    major = register_chrdev(0, DEVICE_NAME, &rpic_pump_fops);
    if (major < 0) {
        pr_err("%s: register_chrdev failed (%d)\n", DEVICE_NAME, major);
        return major;
    }

    rpic_pump_class = GUARDX_CLASS_CREATE(DEVICE_NAME);
    if (IS_ERR(rpic_pump_class)) {
        ret = PTR_ERR(rpic_pump_class);
        goto err_unregister;
    }

    devno = MKDEV(major, 0);
    rpic_pump_device = device_create(rpic_pump_class, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(rpic_pump_device)) {
        ret = PTR_ERR(rpic_pump_device);
        goto err_class_destroy;
    }

    /* [SIMULATION MODE] simulate=1이면 물리 GPIO가 없을 수 있으므로
     * gpio_request 자체를 생략한다.
     * >>> 실물 연결 시: 이 if문 제거하고 아래 블록을
     *     무조건 실행하도록 되돌릴 것 <<< */
    /* BCM 핀 -> 실효 전역 GPIO 번호 (base + BCM) */
    pump_ia = (unsigned int)gpio_base + RPIC_PUMP_GPIO_IA;
    pump_ib = (unsigned int)gpio_base + RPIC_PUMP_GPIO_IB;

    if (!simulate) {
        ret = gpio_request(pump_ia, DEVICE_NAME "_ia");
        if (ret < 0) {
            pr_err("%s: gpio_request(IA=%u=base %d+BCM %d) failed (%d)\n",
                   DEVICE_NAME, pump_ia, gpio_base, RPIC_PUMP_GPIO_IA, ret);
            goto err_device_destroy;
        }
        ret = gpio_request(pump_ib, DEVICE_NAME "_ib");
        if (ret < 0) {
            pr_err("%s: gpio_request(IB=%u=base %d+BCM %d) failed (%d)\n",
                   DEVICE_NAME, pump_ib, gpio_base, RPIC_PUMP_GPIO_IB, ret);
            gpio_free(pump_ia);
            goto err_device_destroy;
        }
        /* 초기값 LOW = 펌프 OFF 보장 상태로 출력 설정 */
        gpio_direction_output(pump_ia, 0);
        gpio_direction_output(pump_ib, 0);
    } else {
        pr_info("%s: SIMULATION MODE enabled (no real GPIO access)\n", DEVICE_NAME);
    }

    pr_info("%s: loaded, major=%d, base=%d, gpio BCM IA=%d IB=%d, simulate=%d\n",
            DEVICE_NAME, major, gpio_base, RPIC_PUMP_GPIO_IA, RPIC_PUMP_GPIO_IB, simulate);
    return 0;

err_device_destroy:
    device_destroy(rpic_pump_class, devno);
err_class_destroy:
    class_destroy(rpic_pump_class);
err_unregister:
    unregister_chrdev(major, DEVICE_NAME);
    return ret;
}

static void __exit rpic_pump_exit(void)
{
    /* 언로드 시 펌프 OFF 보장 */
    mutex_lock(&pump_lock);
    apply_pump(0);
    mutex_unlock(&pump_lock);

    /* [SIMULATION MODE] gpio_request를 안 했으면 gpio_free도 생략 */
    if (!simulate) {
        gpio_free(pump_ia);
        gpio_free(pump_ib);
    }
    device_destroy(rpic_pump_class, MKDEV(major, 0));
    class_destroy(rpic_pump_class);
    unregister_chrdev(major, DEVICE_NAME);
    pr_info("%s: unloaded\n", DEVICE_NAME);
}

module_init(rpic_pump_init);
module_exit(rpic_pump_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GuardX");
MODULE_DESCRIPTION("GuardX RPi C Water Pump Driver (HG7881)");
