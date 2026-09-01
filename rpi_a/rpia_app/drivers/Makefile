# rpia_app/drivers/Makefile
# GuardX RPi A 커널 드라이버 통합 빌드
#
# src/ 아래 소스 파일당 독립된 .ko 하나씩 생성된다.
# 새 디바이스 추가 시 obj-m / <모듈명>-objs 두 줄만 추가하면 됨.
#
# !!! 구 rpia_gas/rpia_spark(DO 기반 개별 드라이버)는 삭제됨 !!!
# MQ-2/TS0226이 AO+MCP3008로 배선이 바뀌면서 rpia_adc.ko 하나가 두
# 센서의 아날로그 값을 겸한다(컨벤션 1-1번).

obj-m += rpia_temphum.o
rpia_temphum-objs := src/rpia_temphum.o

obj-m += rpia_button.o
rpia_button-objs := src/rpia_button.o

obj-m += rpia_adc.o
rpia_adc-objs := src/rpia_adc.o

obj-m += rpia_irtemp.o
rpia_irtemp-objs := src/rpia_irtemp.o

ccflags-y := -I$(src)/include

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
