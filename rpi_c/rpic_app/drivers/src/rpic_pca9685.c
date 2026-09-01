/*
 * rpic_pca9685.c - GuardX RPi C PWM 액추에이터 드라이버 (PCA9685, I2C)
 *
 * GuardX_Driver_Convention.md 컨벤션 준수:
 *  - 1번: 모듈명 rpic_pca9685
 *  - 2번: class_create() 독립 호출
 *  - 3번: /dev/rpic_pca9685 자동 생성
 *  - 4번: major 동적 할당, minor 0 고정
 *  - 6번: ioctl 매직넘버 0xC1
 *
 * 서보 2개(문/가스밸브)를 PCA9685 한 칩으로 구동한다. 팬은 하드웨어
 * PWM(GPIO12)으로 분리되어 별도 드라이버(rpic_fan) 담당이다 - 배경은
 * rpic_pca9685.h 상단 주석 참조.
 *
 * write()로 pca9685_cmd_t {channel, value}를 받아 해당 채널의 PWM을
 * 갱신한다. read()는 마지막 적용 상태(pca9685_state_t)를 반환한다
 * (하드웨어를 다시 읽지 않음 - PCA9685는 출력 전용으로 쓰는 중).
 *
 * 실제 칩 미연결 상태에서는 simulate=1로 로드하면 I2C 접근 없이
 * 명령을 pr_info 로그로만 남긴다. dmesg로 App→드라이버 경로를 검증할
 * 수 있다. "SIMULATION MODE" 표시 블록이 실 칩 연결 시 제거 대상.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/moduleparam.h>
#include <linux/version.h>

#include "rpic_pca9685.h"

#define DEVICE_NAME "rpic_pca9685"

/* class_create()가 커널 6.4부터 owner 인자를 받지 않음 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define GUARDX_CLASS_CREATE(name) class_create(name)
#else
#define GUARDX_CLASS_CREATE(name) class_create(THIS_MODULE, name)
#endif

/* PCA9685 레지스터 (데이터시트 7.3절) */
#define REG_MODE1        0x00
#define REG_MODE2        0x01
#define REG_LED0_ON_L    0x06   /* 채널 n = LED0_ON_L + 4*n */
#define REG_PRESCALE     0xFE

#define MODE1_SLEEP      0x10
#define MODE1_AI         0x20   /* 레지스터 auto-increment (여기선 미사용이지만 관례상 설정) */
#define MODE1_RESTART    0x80
#define MODE2_OUTDRV     0x04   /* totem-pole 출력 (외부 드라이버단 입력에 적합) */

#define PCA_INTERNAL_OSC_HZ  25000000UL
#define PCA_COUNTER_MAX      4096

/* 논리 채널 -> PCA9685 물리 채널. 배선 바꾸면 여기만 수정. */
static const int phys_ch[RPIC_PCA_CH_COUNT] = {
    [RPIC_PCA_CH_SERVO1] = 0,
    [RPIC_PCA_CH_SERVO2] = 1,
};

static int major;
static struct class *rpic_pca9685_class;
static struct device *rpic_pca9685_device;
static struct i2c_adapter *pca_adapter;
static struct i2c_client *pca_client;

/* 마지막 적용 상태 (read()/ioctl GET으로 노출) */
static pca9685_state_t state = {
    .servo1_angle = SERVO1_SAFE_ANGLE,
    .servo2_angle = SERVO2_SAFE_ANGLE,
};

static DEFINE_MUTEX(pca_lock);

/* =====================================================================
 * [SIMULATION MODE] PCA9685 실물이 없어도 App/MQTT 경로를 테스트하기
 * 위한 모드. insmod 시 simulate=1로 켠다.
 *   insmod rpic_pca9685.ko simulate=1
 *
 * >>> 실제 칩 연결 시 할 일 <<<
 *   1) 아래 simulate / simulate_fail 두 module_param 블록 삭제
 *   2) 코드 내 "SIMULATION MODE" 분기(if (simulate){...}) 전부 삭제
 *   3) rpic_pca9685_init() 안의 "if (!simulate)" 조건 제거하고
 *      I2C attach/칩 초기화를 항상 실행하도록 되돌리기
 * ===================================================================== */
static bool simulate = false;
module_param(simulate, bool, 0644);
MODULE_PARM_DESC(simulate, "1=I2C 대신 로그만 출력 (PCA9685 미연결 상태 테스트용)");

/* App이 fd를 물고 있는 동안 write 실패 격리 로직을 테스트하기 위한
 * 강제 실패 스위치.
 *   echo 1 | sudo tee /sys/module/rpic_pca9685/parameters/simulate_fail */
static bool simulate_fail = false;
module_param(simulate_fail, bool, 0644);
MODULE_PARM_DESC(simulate_fail, "1=write 강제 실패(-EIO), rmmod 없이 장애 재현용");

/* ---------------------------------------------------------------------
 * PCA9685 접근 헬퍼
 * --------------------------------------------------------------------- */

static int pca_write_reg(u8 reg, u8 val)
{
    /* [SIMULATION MODE] - 실제 칩 연결 시 이 블록 삭제 */
    if (simulate)
        return 0;

    return i2c_smbus_write_byte_data(pca_client, reg, val);
}

/* 채널 하나의 ON/OFF tick(12bit) 설정. 서보는 on=0 고정, off로 펄스폭
 * 지정. (팬의 full-on/full-off 정적 출력 경로는 팬 분리로 제거됨) */
static int pca_set_pwm(int logical_ch, u16 on, u16 off)
{
    u8 base = REG_LED0_ON_L + 4 * phys_ch[logical_ch];
    int ret;

    ret = pca_write_reg(base,     on & 0xFF);
    if (ret < 0) return ret;
    ret = pca_write_reg(base + 1, (on >> 8) & 0x1F);
    if (ret < 0) return ret;
    ret = pca_write_reg(base + 2, off & 0xFF);
    if (ret < 0) return ret;
    return pca_write_reg(base + 3, (off >> 8) & 0x1F);
}

/* 칩 초기화: SLEEP 진입 -> 프리스케일 설정(SLEEP 중에만 가능) ->
 * 기상 -> 오실레이터 안정화 대기 -> RESTART (데이터시트 7.3.1.1) */
static int pca_chip_init(void)
{
    /* prescale = round(25MHz / (4096 * freq)) - 1, 50Hz면 121 */
    u8 prescale = DIV_ROUND_CLOSEST(PCA_INTERNAL_OSC_HZ,
                                    (unsigned long)PCA_COUNTER_MAX * PCA9685_PWM_FREQ_HZ) - 1;
    int ret;

    ret = pca_write_reg(REG_MODE1, MODE1_SLEEP);
    if (ret < 0) return ret;
    ret = pca_write_reg(REG_PRESCALE, prescale);
    if (ret < 0) return ret;
    ret = pca_write_reg(REG_MODE1, MODE1_AI);
    if (ret < 0) return ret;

    usleep_range(500, 1000);   /* 오실레이터 기동 최소 500us */

    ret = pca_write_reg(REG_MODE1, MODE1_RESTART | MODE1_AI);
    if (ret < 0) return ret;
    return pca_write_reg(REG_MODE2, MODE2_OUTDRV);
}

/* ---------------------------------------------------------------------
 * 명령 적용
 * --------------------------------------------------------------------- */

static int apply_servo(int logical_ch, s32 angle)
{
    /* 각도 -> 펄스폭 tick 선형 변환 */
    u16 off = SERVO_COUNT_MIN +
              (u32)angle * (SERVO_COUNT_MAX - SERVO_COUNT_MIN) / SERVO_ANGLE_MAX;

    return pca_set_pwm(logical_ch, 0, off);
}

static int apply_cmd(const pca9685_cmd_t *cmd)
{
    int ret;

    /* [SIMULATION MODE] - 실제 칩 연결 시 이 블록 삭제 */
    if (simulate && simulate_fail)
        return -EIO;

    switch (cmd->channel) {
    case RPIC_PCA_CH_SERVO1:
    case RPIC_PCA_CH_SERVO2:
        if (cmd->value < SERVO_ANGLE_MIN || cmd->value > SERVO_ANGLE_MAX)
            return -EINVAL;
        ret = apply_servo(cmd->channel, cmd->value);
        if (ret < 0)
            return ret;
        if (cmd->channel == RPIC_PCA_CH_SERVO1)
            state.servo1_angle = cmd->value;
        else
            state.servo2_angle = cmd->value;
        break;

    default:
        return -EINVAL;
    }

    /* 액추에이터 노드 특성상 명령 빈도가 낮아(사람/판단로직 단위)
     * 매 명령을 로그로 남긴다 - 시뮬레이션 검증도 이 로그로 한다 */
    pr_info("%s: ch=%d value=%d applied (simulate=%d)\n",
            DEVICE_NAME, cmd->channel, cmd->value, simulate);
    return 0;
}

/* 부팅/종료 공통 안전 상태 적용 (실패해도 진행 - best effort) */
static void apply_safe_state(void)
{
    const pca9685_cmd_t safe[] = {
        { RPIC_PCA_CH_SERVO1, SERVO1_SAFE_ANGLE },
        { RPIC_PCA_CH_SERVO2, SERVO2_SAFE_ANGLE },
    };
    int i;

    for (i = 0; i < (int)ARRAY_SIZE(safe); i++)
        if (apply_cmd(&safe[i]) < 0)
            pr_warn("%s: safe state ch=%d failed\n", DEVICE_NAME, safe[i].channel);
}

/* ---------------------------------------------------------------------
 * file_operations
 * --------------------------------------------------------------------- */

static int rpic_pca9685_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int rpic_pca9685_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t rpic_pca9685_write(struct file *file, const char __user *buf,
                                  size_t count, loff_t *ppos)
{
    pca9685_cmd_t cmd;
    int ret;

    if (count < sizeof(pca9685_cmd_t))
        return -EINVAL;

    if (copy_from_user(&cmd, buf, sizeof(pca9685_cmd_t)))
        return -EFAULT;

    mutex_lock(&pca_lock);
    ret = apply_cmd(&cmd);
    mutex_unlock(&pca_lock);

    if (ret < 0)
        return ret;

    return sizeof(pca9685_cmd_t);
}

static ssize_t rpic_pca9685_read(struct file *file, char __user *buf,
                                 size_t count, loff_t *ppos)
{
    pca9685_state_t snap;

    if (count < sizeof(pca9685_state_t))
        return -EINVAL;

    mutex_lock(&pca_lock);
    snap = state;
    mutex_unlock(&pca_lock);

    if (copy_to_user(buf, &snap, sizeof(pca9685_state_t)))
        return -EFAULT;

    return sizeof(pca9685_state_t);
}

static long rpic_pca9685_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    pca9685_cmd_t c;
    pca9685_state_t snap;
    int ret;

    switch (cmd) {
    case PCA9685_IOC_SET:
        if (copy_from_user(&c, (void __user *)arg, sizeof(c)))
            return -EFAULT;

        mutex_lock(&pca_lock);
        ret = apply_cmd(&c);
        mutex_unlock(&pca_lock);
        return ret;

    case PCA9685_IOC_GET:
        mutex_lock(&pca_lock);
        snap = state;
        mutex_unlock(&pca_lock);

        if (copy_to_user((void __user *)arg, &snap, sizeof(snap)))
            return -EFAULT;
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations rpic_pca9685_fops = {
    .owner          = THIS_MODULE,
    .open           = rpic_pca9685_open,
    .release        = rpic_pca9685_release,
    .read           = rpic_pca9685_read,
    .write          = rpic_pca9685_write,
    .unlocked_ioctl = rpic_pca9685_ioctl,
};

/* ---------------------------------------------------------------------
 * module init / exit
 * --------------------------------------------------------------------- */

static int __init rpic_pca9685_init(void)
{
    int ret;
    dev_t devno;

    major = register_chrdev(0, DEVICE_NAME, &rpic_pca9685_fops);
    if (major < 0) {
        pr_err("%s: register_chrdev failed (%d)\n", DEVICE_NAME, major);
        return major;
    }

    rpic_pca9685_class = GUARDX_CLASS_CREATE(DEVICE_NAME);
    if (IS_ERR(rpic_pca9685_class)) {
        ret = PTR_ERR(rpic_pca9685_class);
        goto err_unregister;
    }

    devno = MKDEV(major, 0);
    rpic_pca9685_device = device_create(rpic_pca9685_class, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(rpic_pca9685_device)) {
        ret = PTR_ERR(rpic_pca9685_device);
        goto err_class_destroy;
    }

    /* [SIMULATION MODE] simulate=1이면 I2C 버스에 칩이 없을 수 있으므로
     * attach 자체를 생략한다.
     * >>> 실제 칩 연결 시: 이 if문 제거하고 아래 블록을
     *     무조건 실행하도록 되돌릴 것 <<< */
    if (!simulate) {
        struct i2c_board_info info = {
            I2C_BOARD_INFO(DEVICE_NAME, RPIC_PCA9685_I2C_ADDR),
        };

        pca_adapter = i2c_get_adapter(RPIC_PCA9685_I2C_BUS);
        if (!pca_adapter) {
            pr_err("%s: i2c adapter %d not found (raspi-config에서 I2C 활성화 확인)\n",
                   DEVICE_NAME, RPIC_PCA9685_I2C_BUS);
            ret = -ENODEV;
            goto err_device_destroy;
        }

        pca_client = i2c_new_client_device(pca_adapter, &info);
        if (IS_ERR(pca_client)) {
            ret = PTR_ERR(pca_client);
            goto err_put_adapter;
        }

        ret = pca_chip_init();
        if (ret < 0) {
            pr_err("%s: chip init failed (%d) - 배선/주소(0x%02x) 확인\n",
                   DEVICE_NAME, ret, RPIC_PCA9685_I2C_ADDR);
            goto err_unregister_client;
        }
    } else {
        pr_info("%s: SIMULATION MODE enabled (no real I2C access)\n", DEVICE_NAME);
    }

    /* 전원 인가 직후 서보를 정의된 위치로 - 이전 상태 불명이므로 */
    mutex_lock(&pca_lock);
    apply_safe_state();
    mutex_unlock(&pca_lock);

    pr_info("%s: loaded, major=%d, i2c=%d-0x%02x, freq=%dHz, simulate=%d\n",
            DEVICE_NAME, major, RPIC_PCA9685_I2C_BUS, RPIC_PCA9685_I2C_ADDR,
            PCA9685_PWM_FREQ_HZ, simulate);
    return 0;

err_unregister_client:
    i2c_unregister_device(pca_client);
err_put_adapter:
    i2c_put_adapter(pca_adapter);
err_device_destroy:
    device_destroy(rpic_pca9685_class, devno);
err_class_destroy:
    class_destroy(rpic_pca9685_class);
err_unregister:
    unregister_chrdev(major, DEVICE_NAME);
    return ret;
}

static void __exit rpic_pca9685_exit(void)
{
    /* 언로드 시 액추에이터를 물리적으로 안전 상태로 되돌리고 나간다 */
    mutex_lock(&pca_lock);
    apply_safe_state();
    mutex_unlock(&pca_lock);

    /* [SIMULATION MODE] attach를 안 했으면 detach도 생략 */
    if (!simulate) {
        i2c_unregister_device(pca_client);
        i2c_put_adapter(pca_adapter);
    }
    device_destroy(rpic_pca9685_class, MKDEV(major, 0));
    class_destroy(rpic_pca9685_class);
    unregister_chrdev(major, DEVICE_NAME);
    pr_info("%s: unloaded\n", DEVICE_NAME);
}

module_init(rpic_pca9685_init);
module_exit(rpic_pca9685_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GuardX");
MODULE_DESCRIPTION("GuardX RPi C Servo Driver (PCA9685: servo x2)");
