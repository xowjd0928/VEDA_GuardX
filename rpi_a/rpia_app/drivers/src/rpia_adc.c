/*
 * rpia_adc.c - GuardX RPi A ADC 드라이버 (MCP3008, SPI)
 *
 * GuardX_Driver_Convention.md 컨벤션 준수:
 *  - 1번/1-1번: 모듈명 rpia_adc. MCP3008 칩 1개가 불꽃(CH0)/가스(CH1)
 *    두 센서의 AO를 겸하므로 칩 기준으로 모듈 하나로 통합
 *  - 2번: class_create() 독립 호출
 *  - 3번: /dev/rpia_adc 자동 생성, read() 한 번으로 두 채널 동시 반환
 *  - 4번: major 동적 할당, minor 0 고정
 *  - 6번: ioctl 매직넘버 0xA5
 *  - 8-2번: SPI 서브시스템(spi_sync) 사용
 *
 * [구조] SPI 디바이스 드라이버(spi_driver) + 캐릭터 디바이스 이중 구조:
 *  - 아래(SPI): 디바이스 트리 오버레이(rpia-adc-overlay.dts)가 spi0/CE0에
 *    "guardx,rpia-adc" 노드를 선언하면 커널이 probe()를 호출하며 struct
 *    spi_device를 붙여준다. 컨트롤러를 직접 찾을 필요가 없다.
 *  - 위(캐릭터): probe에서 /dev/rpia_adc를 만들어 App에 read()/ioctl()로
 *    raw 채널 값을 넘긴다.
 *
 * !!! 구 방식 폐기 이력 (2026-07-24) !!!
 *   최신 커널(6.18.34+rpt-rpi-v8 실측)은 spi_busnum_to_master()가 제거됐고,
 *   외부 모듈이 버스번호로 spi_controller를 직접 얻는 표준 API가 없다.
 *   기존 bus_find_device_by_name(&spi_bus_type, "spi0") 방식은 spi_bus_type이
 *   컨트롤러가 아닌 자식(spi0.0)만 담고 있어 런타임에 실패했다. 그래서
 *   init/exit에서 직접 attach하던 구조를 버리고, DT 오버레이 + spi_driver
 *   probe(커널이 컨트롤러를 붙여주는 정석 경로)로 전환했다. (I2C/GPIO는
 *   i2c_get_adapter()/gpio_request()로 직접 획득이 가능하지만 SPI는 이 커널
 *   에서 그 경로가 막혀 있어 유일하게 probe 기반이 됐다.)
 *
 * 빌드/로드:
 *   make                                         # rpia_adc.ko
 *   dtc -@ -I dts -O dtb -o rpia-adc.dtbo rpia-adc-overlay.dts
 *   sudo cp rpia-adc.dtbo /boot/firmware/overlays/
 *   # /boot/firmware/config.txt 에 dtoverlay=rpia-adc 추가 후 재부팅
 *   sudo modprobe rpia_adc      # 또는 insmod rpia_adc.ko
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/spi/spi.h>
#include <linux/mutex.h>
#include <linux/mod_devicetable.h>
#include <linux/version.h>

#include "rpia_adc.h"

#define DEVICE_NAME "rpia_adc"

/* class_create()가 커널 6.4부터 owner 인자를 받지 않음 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define GUARDX_CLASS_CREATE(name) class_create(name)
#else
#define GUARDX_CLASS_CREATE(name) class_create(THIS_MODULE, name)
#endif

static int major;
static struct class *rpia_adc_class;
static struct device *rpia_adc_device;
static struct spi_device *adc_spi;   /* probe에서 커널이 붙여준 SPI 디바이스 */

static DEFINE_MUTEX(adc_lock);

/* ---------------------------------------------------------------------
 * MCP3008 SPI 트랜잭션 (rpia_adc.h 상단 프로토콜 주석 참조)
 * --------------------------------------------------------------------- */
static int mcp3008_read_channel(u8 channel, u16 *out)
{
    u8 tx[3] = { 0x01, (u8)(0x80 | (channel << 4)), 0x00 };
    u8 rx[3] = { 0, 0, 0 };
    struct spi_transfer t = {
        .tx_buf = tx,
        .rx_buf = rx,
        .len    = sizeof(tx),
    };
    struct spi_message m;
    int ret;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);

    ret = spi_sync(adc_spi, &m);
    if (ret < 0)
        return ret;

    *out = (u16)(((rx[1] & 0x03) << 8) | rx[2]);
    return 0;
}

static int do_adc_read(adc_data_t *result)
{
    int ret;
    u16 gas, spark;

    ret = mcp3008_read_channel(RPIA_ADC_CH_GAS, &gas);
    if (ret < 0)
        return ret;

    ret = mcp3008_read_channel(RPIA_ADC_CH_SPARK, &spark);
    if (ret < 0)
        return ret;

    result->gas_raw   = gas;
    result->spark_raw = spark;
    return 0;
}

/* ---------------------------------------------------------------------
 * file_operations (캐릭터 디바이스 = App 인터페이스)
 * --------------------------------------------------------------------- */

static int rpia_adc_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int rpia_adc_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t rpia_adc_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    adc_data_t data;
    int ret;

    if (count < sizeof(adc_data_t))
        return -EINVAL;

    mutex_lock(&adc_lock);
    ret = do_adc_read(&data);
    mutex_unlock(&adc_lock);

    if (ret < 0)
        return ret;

    if (copy_to_user(buf, &data, sizeof(adc_data_t)))
        return -EFAULT;

    return sizeof(adc_data_t);
}

static long rpia_adc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    adc_data_t data;
    int ret;

    switch (cmd) {
    case ADC_IOC_READ:
        mutex_lock(&adc_lock);
        ret = do_adc_read(&data);
        mutex_unlock(&adc_lock);

        if (ret < 0)
            return ret;

        if (copy_to_user((void __user *)arg, &data, sizeof(adc_data_t)))
            return -EFAULT;

        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations rpia_adc_fops = {
    .owner          = THIS_MODULE,
    .open           = rpia_adc_open,
    .release        = rpia_adc_release,
    .read           = rpia_adc_read,
    .unlocked_ioctl = rpia_adc_ioctl,
};

/* ---------------------------------------------------------------------
 * spi_driver probe / remove (SPI 클라이언트 = 칩 통신)
 * --------------------------------------------------------------------- */

static int rpia_adc_probe(struct spi_device *spi)
{
    int ret;
    dev_t devno;

    adc_spi = spi;

    /* MCP3008: SPI mode 0, 8bit. 클럭은 오버레이의 spi-max-frequency 사용 */
    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    ret = spi_setup(spi);
    if (ret < 0) {
        dev_err(&spi->dev, "spi_setup failed (%d)\n", ret);
        return ret;
    }

    major = register_chrdev(0, DEVICE_NAME, &rpia_adc_fops);
    if (major < 0) {
        dev_err(&spi->dev, "register_chrdev failed (%d)\n", major);
        return major;
    }

    rpia_adc_class = GUARDX_CLASS_CREATE(DEVICE_NAME);
    if (IS_ERR(rpia_adc_class)) {
        ret = PTR_ERR(rpia_adc_class);
        goto err_unregister;
    }

    devno = MKDEV(major, 0);
    rpia_adc_device = device_create(rpia_adc_class, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(rpia_adc_device)) {
        ret = PTR_ERR(rpia_adc_device);
        goto err_class_destroy;
    }

    dev_info(&spi->dev, "%s: loaded, major=%d, speed=%uHz\n",
             DEVICE_NAME, major, spi->max_speed_hz);
    return 0;

err_class_destroy:
    class_destroy(rpia_adc_class);
err_unregister:
    unregister_chrdev(major, DEVICE_NAME);
    return ret;
}

static void rpia_adc_remove(struct spi_device *spi)
{
    device_destroy(rpia_adc_class, MKDEV(major, 0));
    class_destroy(rpia_adc_class);
    unregister_chrdev(major, DEVICE_NAME);
    dev_info(&spi->dev, "%s: unloaded\n", DEVICE_NAME);
}

/* ---------------------------------------------------------------------
 * 매칭 테이블 - 오버레이의 compatible = "guardx,rpia-adc" 와 연결
 * --------------------------------------------------------------------- */
static const struct of_device_id rpia_adc_of_match[] = {
    { .compatible = "guardx,rpia-adc" },
    { }
};
MODULE_DEVICE_TABLE(of, rpia_adc_of_match);

static const struct spi_device_id rpia_adc_spi_id[] = {
    { "rpia-adc", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, rpia_adc_spi_id);

static struct spi_driver rpia_adc_driver = {
    .driver = {
        .name           = DEVICE_NAME,
        .of_match_table = rpia_adc_of_match,
    },
    .probe    = rpia_adc_probe,
    .remove   = rpia_adc_remove,
    .id_table = rpia_adc_spi_id,
};
module_spi_driver(rpia_adc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GuardX");
MODULE_DESCRIPTION("GuardX RPi A ADC Driver (MCP3008: gas + spark AO)");
