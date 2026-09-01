# rpic_app/drivers/Makefile
# GuardX RPi C 커널 드라이버 통합 빌드
#
# src/ 아래 소스 파일당 독립된 .ko 하나씩 생성된다.
# 새 디바이스 추가 시 obj-m / <모듈명>-objs 두 줄만 추가하면 됨.

obj-m += rpic_pca9685.o
rpic_pca9685-objs := src/rpic_pca9685.o

obj-m += rpic_stepper.o
rpic_stepper-objs := src/rpic_stepper.o

obj-m += rpic_pump.o
rpic_pump-objs := src/rpic_pump.o

# rpic_amp(LM386 전원스위치) 제거됨 - MAX98357A(I2S)는 상시 ON이라 제어할
# GPIO 전원핀이 없다. 소리는 유저스페이스 rpic_audio(ALSA)가 낸다.

ccflags-y := -I$(src)/include

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
