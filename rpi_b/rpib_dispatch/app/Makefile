# rpib_dispatch/app/Makefile
# GuardX RPi B - 화재 대응 명령 실제 발행 (rpib_engine 3분할)

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
        src/cmd_builder.c \
        ../../rpib_common/src/sensor_parser.c \
        ../../rpib_common/src/fire_internal.c

HDRS := $(wildcard include/*.h ../../rpib_common/include/*.h ../../common/include/*.h)

TARGET := rpib_dispatch

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
