# rpib_ingest/app/Makefile
# GuardX RPi B - 센서 원시값/버튼 수집 (rpib_engine 3분할, A->B 구간)
#
# 네이티브 빌드:      make
# 크로스컴파일 예시:  make CC=aarch64-linux-gnu-gcc
#   apt install libmosquitto-dev libpq-dev (rpib_app/app/Makefile과 동일 의존)

CC      ?= gcc
CFLAGS  := -Wall -Wextra -O2 \
           -I include \
           -I ../../rpib_common/include \
           -I ../../common/include \
           -I /usr/include/postgresql
LDFLAGS := -lmosquitto -lpq

SRCS := src/main.c \
        src/mqtt_sub.c \
        src/db_writer.c \
        ../../rpib_common/src/sensor_parser.c \
        ../../rpib_common/src/zone_loader.c

HDRS := $(wildcard include/*.h ../../rpib_common/include/*.h ../../common/include/*.h)

TARGET := rpib_ingest

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
