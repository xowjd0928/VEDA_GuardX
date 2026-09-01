/*
 * rpia_irtemp.c - GuardX RPi A 비접촉 온도 센서 드라이버 (MLX90614, I2C)
 *
 * GuardX_Driver_Convention.md 컨벤션 준수:
 *  - 1번: 모듈명 rpia_irtemp
 *  - 2번: class_create() 독립 호출
 *  - 3번: /dev/rpia_irtemp 자동 생성, read() 한 번으로 irtemp_data_t 반환
 *  - 3-1: 정수 x10 스케일 (float 미사용)
 *  - 4번: major 동적 할당, minor 0 고정
 *  - 6번: ioctl 매직넘버 0xA6
 *  - 8-1번: I2C 서브시스템 사용. rpic_pca9685.c/rpia_temphum.c와 동일한
 *    어댑터 획득 -> 클라이언트 등록 -> simulate 스킵 -> 에러 goto
 *    역순 정리 구조를 따름
 *
 * 5단계: 다이어그램(RPi-S) 대조로 새로 추가된 센서. MLX90614는
 * SMBus PEC(CRC-8)를 기본 지원하므로, SHT30처럼 raw i2c_master_send/
 * recv로 직접 CRC 검증할 필요 없이 i2c_smbus_read_word_data() +
 * I2C_CLIENT_PEC 플래그만으로 커널 SMBus 코어가 자동 검증해준다.
 *
 * 현재 실제 센서 미연결 상태이므로 simulate=1로 로드하면 I2C 대신
 * debugfs(/sys/kernel/debug/rpia_irtemp/)로 주입한 값을 반환한다.
 * 코드 내 "SIMULATION MODE" 표시된 블록이 실제 센서 연결 시 제거 대상이다.
 *
 *   insmod rpia_irtemp.ko simulate=1
 *   echo 250 > /sys/kernel/debug/rpia_irtemp/ambient_x10   # 주변 25.0도
 *   echo 235 > /sys/kernel/debug/rpia_irtemp/object_x10    # 대상 23.5도
 *
 * >>> 실제 센서 연결 시 할 일 <<<
 *   1) 아래 simulate module_param, debug_temp_x10,
 *      rpia_irtemp_debugfs_init()/_exit() 블록 전부 삭제
 *   2) do_irtemp_read() 안의 "SIMULATION MODE" 분기 삭제
 *   3) rpia_irtemp_init()에서 "if (!simulate)" 조건 제거하고 I2C
 *      attach를 무조건 실행하도록 되돌리기
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/moduleparam.h>
#include <linux/debugfs.h>
#include <linux/version.h>

#include "rpia_irtemp.h"

#define DEVICE_NAME "rpia_irtemp"

/* class_create()가 커널 6.4부터 owner 인자를 받지 않음 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define GUARDX_CLASS_CREATE(name) class_create(name)
#else
#define GUARDX_CLASS_CREATE(name) class_create(THIS_MODULE, name)
#endif

/* MLX90614 RAM 레지스터 (데이터시트 8.3.1절)
 *   Tamb (0x06)  = 주변(칩 내부 기준) 온도
 *   Tobj1 (0x07) = 대상 표면 온도 */
#define MLX90614_REG_TAMB    0x06
#define MLX90614_REG_TOBJ1   0x07

static int major;
static struct class *rpia_irtemp_class;
static struct device *rpia_irtemp_device;
static struct i2c_adapter *irtemp_adapter;
static struct i2c_client *irtemp_client;

static DEFINE_MUTEX(irtemp_lock);

/* =====================================================================
 * [SIMULATION MODE] 실제 MLX90614 미연결 상태에서 I2C 대신 debugfs로
 * 값을 직접 주입해 테스트한다.
 * ===================================================================== */
static bool simulate = false;
module_param(simulate, bool, 0644);
MODULE_PARM_DESC(simulate, "1=I2C 대신 debugfs 주입값 사용");

static struct dentry *irtemp_debug_dir;
static u16 debug_amb_x10 = 250;   /* 기본값 25.0도, s16으로 재해석해서 사용 */
static u16 debug_obj_x10 = 235;   /* 기본값 23.5도 */

static void rpia_irtemp_debugfs_init(void)
{
    irtemp_debug_dir = debugfs_create_dir(DEVICE_NAME, NULL);
    if (IS_ERR_OR_NULL(irtemp_debug_dir)) {
        pr_warn("%s: debugfs_create_dir failed, simulation value injection disabled\n",
                DEVICE_NAME);
        irtemp_debug_dir = NULL;
        return;
    }
    debugfs_create_u16("ambient_x10", 0644, irtemp_debug_dir, &debug_amb_x10);
    debugfs_create_u16("object_x10", 0644, irtemp_debug_dir, &debug_obj_x10);
}

static void rpia_irtemp_debugfs_exit(void)
{
    debugfs_remove_recursive(irtemp_debug_dir);
}

/* ---------------------------------------------------------------------
 * MLX90614 읽기 - rpia_irtemp.h 상단 변환식 참조
 * --------------------------------------------------------------------- */
/* 지정 RAM 레지스터 워드를 읽어 x10 스케일 섭씨로 변환.
 * SMBus PEC는 클라이언트 flags(I2C_CLIENT_PEC)로 켜져 있어 커널 SMBus
 * 코어가 CRC-8 검증까지 자동으로 해준다 - 실패하면 음수 errno로 반환됨 */
static int read_reg_x10(u8 reg, s16 *out)
{
    s32 raw;
    s32 millideg_k;
    s32 millideg_c;

    raw = i2c_smbus_read_word_data(irtemp_client, reg);
    if (raw < 0)
        return raw;

    if (raw & 0x8000)
        return -EIO;   /* MLX90614 에러 플래그 비트 */

    millideg_k = raw * 20;              /* raw * 0.02K, mK 단위 (정확한 정수) */
    millideg_c = millideg_k - 273150;   /* -273.15K를 mK로 빼줌 */
    *out = (s16)(millideg_c / 100);     /* mC -> x10(deci) 스케일 */

    return 0;
}

static int read_irtemp_raw(irtemp_data_t *out)
{
    int ret;

    ret = read_reg_x10(MLX90614_REG_TAMB, &out->ambient);
    if (ret < 0)
        return ret;

    ret = read_reg_x10(MLX90614_REG_TOBJ1, &out->object);
    if (ret < 0)
        return ret;

    return 0;
}

static int do_irtemp_read(irtemp_data_t *result)
{
    /* [SIMULATION MODE] - 실제 센서 연결 시 이 블록 삭제 */
    if (simulate) {
        result->ambient = (s16)debug_amb_x10;
        result->object  = (s16)debug_obj_x10;
        return 0;
    }

    /* [REAL HARDWARE] - MLX90614 SMBus 읽기, 그대로 유지 */
    return read_irtemp_raw(result);
}

/* ---------------------------------------------------------------------
 * file_operations
 * --------------------------------------------------------------------- */

static int rpia_irtemp_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int rpia_irtemp_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t rpia_irtemp_read(struct file *file, char __user *buf,
                                 size_t count, loff_t *ppos)
{
    irtemp_data_t data;
    int ret;

    if (count < sizeof(irtemp_data_t))
        return -EINVAL;

    mutex_lock(&irtemp_lock);
    ret = do_irtemp_read(&data);
    mutex_unlock(&irtemp_lock);

    if (ret < 0)
        return ret;

    if (copy_to_user(buf, &data, sizeof(irtemp_data_t)))
        return -EFAULT;

    return sizeof(irtemp_data_t);
}

static long rpia_irtemp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    irtemp_data_t data;
    int ret;

    switch (cmd) {
    case IRTEMP_IOC_READ:
        mutex_lock(&irtemp_lock);
        ret = do_irtemp_read(&data);
        mutex_unlock(&irtemp_lock);

        if (ret < 0)
            return ret;

        if (copy_to_user((void __user *)arg, &data, sizeof(irtemp_data_t)))
            return -EFAULT;

        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations rpia_irtemp_fops = {
    .owner          = THIS_MODULE,
    .open           = rpia_irtemp_open,
    .release        = rpia_irtemp_release,
    .read           = rpia_irtemp_read,
    .unlocked_ioctl = rpia_irtemp_ioctl,
};

/* ---------------------------------------------------------------------
 * module init / exit
 * --------------------------------------------------------------------- */

static int __init rpia_irtemp_init(void)
{
    int ret;
    dev_t devno;

    major = register_chrdev(0, DEVICE_NAME, &rpia_irtemp_fops);
    if (major < 0) {
        pr_err("%s: register_chrdev failed (%d)\n", DEVICE_NAME, major);
        return major;
    }

    rpia_irtemp_class = GUARDX_CLASS_CREATE(DEVICE_NAME);
    if (IS_ERR(rpia_irtemp_class)) {
        ret = PTR_ERR(rpia_irtemp_class);
        goto err_unregister;
    }

    devno = MKDEV(major, 0);
    rpia_irtemp_device = device_create(rpia_irtemp_class, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(rpia_irtemp_device)) {
        ret = PTR_ERR(rpia_irtemp_device);
        goto err_class_destroy;
    }

    /* [SIMULATION MODE] simulate=1이면 물리 MLX90614가 없을 수 있으므로
     * I2C attach 자체를 생략한다.
     * >>> 실제 센서 연결 시: 이 if문 제거하고 아래 블록을
     *     무조건 실행하도록 되돌릴 것 <<< */
    if (!simulate) {
        struct i2c_board_info info = {
            I2C_BOARD_INFO(DEVICE_NAME, RPIA_IRTEMP_I2C_ADDR),
            .flags = I2C_CLIENT_PEC,   /* MLX90614 SMBus PEC 자동 검증 */
        };

        irtemp_adapter = i2c_get_adapter(RPIA_IRTEMP_I2C_BUS);
        if (!irtemp_adapter) {
            pr_err("%s: i2c adapter %d not found (raspi-config에서 I2C 활성화 확인)\n",
                   DEVICE_NAME, RPIA_IRTEMP_I2C_BUS);
            ret = -ENODEV;
            goto err_device_destroy;
        }

        irtemp_client = i2c_new_client_device(irtemp_adapter, &info);
        if (IS_ERR(irtemp_client)) {
            ret = PTR_ERR(irtemp_client);
            goto err_put_adapter;
        }
    } else {
        rpia_irtemp_debugfs_init();
        pr_info("%s: SIMULATION MODE enabled (debugfs value injection)\n", DEVICE_NAME);
    }

    pr_info("%s: loaded, major=%d, i2c=%d-0x%02x, simulate=%d\n",
             DEVICE_NAME, major, RPIA_IRTEMP_I2C_BUS, RPIA_IRTEMP_I2C_ADDR, simulate);
    return 0;

err_put_adapter:
    i2c_put_adapter(irtemp_adapter);
err_device_destroy:
    device_destroy(rpia_irtemp_class, devno);
err_class_destroy:
    class_destroy(rpia_irtemp_class);
err_unregister:
    unregister_chrdev(major, DEVICE_NAME);
    return ret;
}

static void __exit rpia_irtemp_exit(void)
{
    /* [SIMULATION MODE] I2C attach를 안 했으면 detach도 생략 */
    if (!simulate) {
        i2c_unregister_device(irtemp_client);
        i2c_put_adapter(irtemp_adapter);
    } else {
        rpia_irtemp_debugfs_exit();
    }

    device_destroy(rpia_irtemp_class, MKDEV(major, 0));
    class_destroy(rpia_irtemp_class);
    unregister_chrdev(major, DEVICE_NAME);
    pr_info("%s: unloaded\n", DEVICE_NAME);
}

module_init(rpia_irtemp_init);
module_exit(rpia_irtemp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GuardX");
MODULE_DESCRIPTION("GuardX RPi A Non-contact Temperature Sensor Driver (MLX90614, I2C)");
