# rpib_decision/app/Makefile
# GuardX RPi B - 화재 판정 (rpib_engine 3분할)

CC      ?= gcc
CFLAGS  := -Wall -Wextra -O2 \
           -I include \
           -I ../../rpib_common/include \
           -I ../../common/include \
           -I /usr/include/postgresql
# -lm: threshold_loader.c의 fabsf (rpib_app/app/Makefile과 동일 근거)
LDFLAGS := -lmosquitto -lpq -lm

SRCS := src/main.c \
        src/mqtt_bus.c \
        src/db_writer.c \
        src/decision.c \
        src/threshold_loader.c \
        ../../rpib_common/src/sensor_parser.c \
        ../../rpib_common/src/zone_loader.c \
        ../../rpib_common/src/fire_internal.c

HDRS := $(wildcard include/*.h ../../rpib_common/include/*.h ../../common/include/*.h)

TARGET := rpib_decision

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
