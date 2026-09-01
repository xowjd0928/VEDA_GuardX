# rpia_app/app/Makefile
# GuardX RPi A 유저스페이스 App 빌드
#
# 네이티브 빌드:      make
# 크로스컴파일 예시:  make CC=aarch64-linux-gnu-gcc
# (libmosquitto는 타겟 sysroot에 있어야 함: apt install libmosquitto-dev)

CC      ?= gcc
CFLAGS  := -Wall -Wextra -O2 \
           -I include \
           -I ../hal/include \
           -I ../../common/include \
           $(EXTRA_CFLAGS)
LDFLAGS := -lmosquitto

SRCS := src/main.c \
        src/device_registry.c \
        src/json_builder.c \
        src/mqtt_pub.c \
        ../hal/src/guardx_hal.c

HDRS := $(wildcard include/*.h ../hal/include/*.h ../../common/include/*.h)

TARGET := rpia_publisher

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
