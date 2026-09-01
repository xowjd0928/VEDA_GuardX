/*
 * rpia_temphum.c - GuardX RPi A 온습도 센서 드라이버 (SHT30, I2C)
 *
 * GuardX_Driver_Convention.md 컨벤션 준수:
 *  - 1번: 모듈명 rpia_temphum, snake_case
 *  - 2번: class_create() 독립 호출, class 이름 = 모듈명
 *  - 3번: /dev/rpia_temphum 자동 생성, read() 한 번으로 temphum_data_t 반환
 *  - 3-1: 정수 x10 스케일 (float 미사용 - 커널 공간은 부동소수점 자체가 금지)
 *  - 4번: major 동적 할당, minor 0 고정
 *  - 6번: ioctl 매직넘버 0xA2
 *  - 8-1번: I2C 서브시스템 사용. rpic_pca9685.c(어댑터 획득 -> 클라이언트
 *    등록 -> simulate 스킵 -> 에러 시 goto 역순 정리) 패턴을 그대로 따름
 *
 * 4단계 개정: 기존 DHT22류 단일 GPIO 비트뱅킹 타이밍 프로토콜을 전부
 * 폐기하고 SHT30(I2C, addr 0x44) 싱글샷 측정으로 교체했다. 클럭
 * 스트레칭 방식 대신 "커맨드 전송 -> 고정 지연(15ms) -> 결과 읽기"
 * 방식을 쓴다 (BCM283x I2C 컨트롤러가 슬레이브의 클럭 스트레칭을
 * 안정적으로 못 다루는 경우가 있어 회피).
 *
 * 커널 공간은 부동소수점을 쓸 수 없으므로, SHT30 raw 16bit 값을 x10
 * 정수로 변환하는 계산도 정수 연산만으로 한다(분자 최대값이 32bit
 * 범위 안이라 64bit 중간값도 불필요 - 아래 sht30_*_raw_to_x10 참조).
 *
 * 현재 실제 센서 미연결 상태이므로 simulate=1로 로드하면 I2C 대신
 * debugfs(/sys/kernel/debug/rpia_temphum/)로 주입한 값을 반환한다.
 * 코드 내 "SIMULATION MODE" 표시된 블록이 실제 센서 연결 시 제거 대상이다.
 *
 *   insmod rpia_temphum.ko simulate=1
 *   echo 235 > /sys/kernel/debug/rpia_temphum/temperature_x10   # 23.5도
 *   echo 602 > /sys/kernel/debug/rpia_temphum/humidity_x10      # 60.2%
 *
 * >>> 실제 센서 연결 시 할 일 <<<
 *   1) 아래 simulate module_param, debug_temp_x10/debug_hum_x10,
 *      rpia_temphum_debugfs_init()/_exit() 블록 전부 삭제
 *   2) do_temphum_read() 안의 "SIMULATION MODE" 분기 삭제
 *   3) rpia_temphum_init()에서 "if (!simulate)" 조건 제거하고 I2C
 *      attach를 무조건 실행하도록 되돌리기
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/debugfs.h>
#include <linux/version.h>

#include "rpia_temphum.h"

#define DEVICE_NAME "rpia_temphum"

/* class_create()가 커널 6.4부터 owner 인자를 받지 않음.
 * 6.4 미만(예: 6.12 이전 커스텀 커널)과 이후 둘 다 지원. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define GUARDX_CLASS_CREATE(name) class_create(name)
#else
#define GUARDX_CLASS_CREATE(name) class_create(THIS_MODULE, name)
#endif

/* SHT30 싱글샷 측정 커맨드: High repeatability, 클럭 스트레칭 비활성
 * (데이터시트 4.3절 커맨드 테이블). 측정 소요시간 최대 15ms. */
#define SHT30_CMD_MEASURE_HI_NOSTRETCH_MSB  0x24
#define SHT30_CMD_MEASURE_HI_NOSTRETCH_LSB  0x00
#define SHT30_MEASURE_DELAY_MS              15

static int major;
static struct class *rpia_temphum_class;
static struct device *rpia_temphum_device;
static struct i2c_adapter *temphum_adapter;
static struct i2c_client *temphum_client;

/* 동시 접근(open 후 read) 직렬화용. I2C 트랜잭션 + msleep 구간을
 * 한 번에 한 요청만 처리하게 한다. */
static DEFINE_MUTEX(temphum_lock);

/* 마지막으로 읽은 값 캐시. SHT30 싱글샷 측정도 최소 간격을 두는 게
 * 안전해 기존 DHT22류와 동일한 캐싱 정책을 유지한다. */
static temphum_data_t last_data;
static unsigned long last_read_jiffies;
#define MIN_READ_INTERVAL_MS 2000

/* =====================================================================
 * [SIMULATION MODE] 실제 온습도 센서가 아직 없어, I2C 트랜잭션 대신
 * debugfs로 최종 온습도 값을 직접 주입해 테스트한다.
 * ===================================================================== */
static bool simulate = false;
module_param(simulate, bool, 0644);
MODULE_PARM_DESC(simulate, "1=I2C 대신 debugfs 주입값 사용");

static struct dentry *temphum_debug_dir;
static u16 debug_temp_x10 = 235;   /* 기본값 23.5도, s16으로 재해석해서 사용 */
static u16 debug_hum_x10  = 602;   /* 기본값 60.2% */

static void rpia_temphum_debugfs_init(void)
{
    temphum_debug_dir = debugfs_create_dir(DEVICE_NAME, NULL);
    if (IS_ERR_OR_NULL(temphum_debug_dir)) {
        pr_warn("%s: debugfs_create_dir failed, simulation value injection disabled\n",
                DEVICE_NAME);
        temphum_debug_dir = NULL;
        return;
    }
    debugfs_create_u16("temperature_x10", 0644, temphum_debug_dir, &debug_temp_x10);
    debugfs_create_u16("humidity_x10", 0644, temphum_debug_dir, &debug_hum_x10);
}

static void rpia_temphum_debugfs_exit(void)
{
    debugfs_remove_recursive(temphum_debug_dir);
}

/* ---------------------------------------------------------------------
 * SHT30 CRC8 (Sensirion 앱노트: 다항식 0x31, 초기값 0xFF, 반사 없음)
 * --------------------------------------------------------------------- */
static u8 sht30_crc8(const u8 *data, int len)
{
    u8 crc = 0xFF;
    int i, j;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (u8)((crc << 1) ^ 0x31) : (u8)(crc << 1);
    }
    return crc;
}

/* raw 16bit -> x10 정수 변환. 분자 최대값(1750*65535 ≈ 1.15e8,
 * 1000*65535 ≈ 6.6e7)이 모두 32bit int 범위 안이라 64bit 연산 불필요. */
static s16 sht30_temp_raw_to_x10(u16 raw)
{
    /* 섭씨 = -45 + 175 * raw/65535, x10 스케일: -450 + 1750*raw/65535 */
    return (s16)(-450 + (1750 * (int)raw) / 65535);
}

static s16 sht30_hum_raw_to_x10(u16 raw)
{
    /* %RH = 100 * raw/65535, x10 스케일: 1000*raw/65535 */
    return (s16)((1000 * (int)raw) / 65535);
}

/* ---------------------------------------------------------------------
 * SHT30 싱글샷 측정: 커맨드 전송 -> 15ms 대기 -> 6바이트 응답
 * (온도 MSB/LSB/CRC + 습도 MSB/LSB/CRC)
 * --------------------------------------------------------------------- */
static int read_temphum_raw(temphum_data_t *result)
{
    u8 cmd[2] = { SHT30_CMD_MEASURE_HI_NOSTRETCH_MSB,
                  SHT30_CMD_MEASURE_HI_NOSTRETCH_LSB };
    u8 resp[6];
    int ret;

    ret = i2c_master_send(temphum_client, cmd, sizeof(cmd));
    if (ret < 0)
        return ret;
    if (ret != sizeof(cmd))
        return -EIO;

    msleep(SHT30_MEASURE_DELAY_MS);

    ret = i2c_master_recv(temphum_client, resp, sizeof(resp));
    if (ret < 0)
        return ret;
    if (ret != sizeof(resp))
        return -EIO;

    if (sht30_crc8(&resp[0], 2) != resp[2])
        return -EIO;
    if (sht30_crc8(&resp[3], 2) != resp[5])
        return -EIO;

    result->temperature = sht30_temp_raw_to_x10((u16)((resp[0] << 8) | resp[1]));
    result->humidity    = sht30_hum_raw_to_x10((u16)((resp[3] << 8) | resp[4]));
    return 0;
}

static int do_temphum_read(temphum_data_t *result)
{
    int ret;

    /* [SIMULATION MODE] - 실제 센서 연결 시 이 블록 삭제 */
    if (simulate) {
        result->temperature = (s16)debug_temp_x10;
        result->humidity    = (s16)debug_hum_x10;
        last_data = *result;
        last_read_jiffies = jiffies;
        return 0;
    }

    /* [REAL HARDWARE] - 아래는 실제 SHT30 I2C 경로, 그대로 유지 */

    /* 너무 잦은 재요청 시 캐시된 값을 반환 (DHT22류 시절과 동일 정책 유지) */
    if (last_read_jiffies != 0 &&
        jiffies_to_msecs(jiffies - last_read_jiffies) < MIN_READ_INTERVAL_MS) {
        *result = last_data;
        return 0;
    }

    ret = read_temphum_raw(result);
    if (ret < 0)
        return ret;

    last_data = *result;
    last_read_jiffies = jiffies;

    return 0;
}

/* ---------------------------------------------------------------------
 * file_operations
 * --------------------------------------------------------------------- */

static int rpia_temphum_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int rpia_temphum_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t rpia_temphum_read(struct file *file, char __user *buf,
                                  size_t count, loff_t *ppos)
{
    temphum_data_t data;
    int ret;

    if (count < sizeof(temphum_data_t))
        return -EINVAL;

    mutex_lock(&temphum_lock);
    ret = do_temphum_read(&data);
    mutex_unlock(&temphum_lock);

    if (ret < 0)
        return ret;

    if (copy_to_user(buf, &data, sizeof(temphum_data_t)))
        return -EFAULT;

    return sizeof(temphum_data_t);
}

static long rpia_temphum_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    temphum_data_t data;
    int ret;

    switch (cmd) {
    case TEMPHUM_IOC_READ:
        mutex_lock(&temphum_lock);
        ret = do_temphum_read(&data);
        mutex_unlock(&temphum_lock);

        if (ret < 0)
            return ret;

        if (copy_to_user((void __user *)arg, &data, sizeof(temphum_data_t)))
            return -EFAULT;

        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations rpia_temphum_fops = {
    .owner          = THIS_MODULE,
    .open           = rpia_temphum_open,
    .release        = rpia_temphum_release,
    .read           = rpia_temphum_read,
    .unlocked_ioctl = rpia_temphum_ioctl,
};

/* ---------------------------------------------------------------------
 * module init / exit
 * --------------------------------------------------------------------- */

static int __init rpia_temphum_init(void)
{
    int ret;
    dev_t devno;

    /* 4번: major 동적 할당 */
    major = register_chrdev(0, DEVICE_NAME, &rpia_temphum_fops);
    if (major < 0) {
        pr_err("%s: register_chrdev failed (%d)\n", DEVICE_NAME, major);
        return major;
    }

    /* 2번: class_create 독립 호출, class 이름 = 모듈명 */
    rpia_temphum_class = GUARDX_CLASS_CREATE(DEVICE_NAME);
    if (IS_ERR(rpia_temphum_class)) {
        ret = PTR_ERR(rpia_temphum_class);
        goto err_unregister;
    }

    devno = MKDEV(major, 0);
    rpia_temphum_device = device_create(rpia_temphum_class, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(rpia_temphum_device)) {
        ret = PTR_ERR(rpia_temphum_device);
        goto err_class_destroy;
    }

    /* [SIMULATION MODE] simulate=1이면 물리 SHT30이 없을 수 있으므로
     * I2C attach 자체를 생략한다.
     * >>> 실제 센서 연결 시: 이 if문 제거하고 아래 블록을
     *     무조건 실행하도록 되돌릴 것 <<< */
    if (!simulate) {
        struct i2c_board_info info = {
            I2C_BOARD_INFO(DEVICE_NAME, RPIA_TEMPHUM_I2C_ADDR),
        };

        temphum_adapter = i2c_get_adapter(RPIA_TEMPHUM_I2C_BUS);
        if (!temphum_adapter) {
            pr_err("%s: i2c adapter %d not found (raspi-config에서 I2C 활성화 확인)\n",
                   DEVICE_NAME, RPIA_TEMPHUM_I2C_BUS);
            ret = -ENODEV;
            goto err_device_destroy;
        }

        temphum_client = i2c_new_client_device(temphum_adapter, &info);
        if (IS_ERR(temphum_client)) {
            ret = PTR_ERR(temphum_client);
            goto err_put_adapter;
        }
    } else {
        rpia_temphum_debugfs_init();
        pr_info("%s: SIMULATION MODE enabled (debugfs value injection)\n", DEVICE_NAME);
    }

    last_read_jiffies = 0;

    pr_info("%s: loaded, major=%d, i2c=%d-0x%02x, simulate=%d\n",
             DEVICE_NAME, major, RPIA_TEMPHUM_I2C_BUS, RPIA_TEMPHUM_I2C_ADDR, simulate);
    return 0;

err_put_adapter:
    i2c_put_adapter(temphum_adapter);
err_device_destroy:
    device_destroy(rpia_temphum_class, devno);
err_class_destroy:
    class_destroy(rpia_temphum_class);
err_unregister:
    unregister_chrdev(major, DEVICE_NAME);
    return ret;
}

static void __exit rpia_temphum_exit(void)
{
    /* [SIMULATION MODE] I2C attach를 안 했으면 detach도 생략 */
    if (!simulate) {
        i2c_unregister_device(temphum_client);
        i2c_put_adapter(temphum_adapter);
    } else {
        rpia_temphum_debugfs_exit();
    }

    device_destroy(rpia_temphum_class, MKDEV(major, 0));
    class_destroy(rpia_temphum_class);
    unregister_chrdev(major, DEVICE_NAME);
    pr_info("%s: unloaded\n", DEVICE_NAME);
}

module_init(rpia_temphum_init);
module_exit(rpia_temphum_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GuardX");
MODULE_DESCRIPTION("GuardX RPi A Temperature/Humidity Sensor Driver (SHT30, I2C)");
