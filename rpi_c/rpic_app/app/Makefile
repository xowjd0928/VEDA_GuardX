# rpic_app/app/Makefile
# GuardX RPi C 유저스페이스 App 빌드
#
# 네이티브 빌드:      make            # rpic_subscriber (MQTT 구독자 + I2S 알림음)
#                     make tools      # audio_test (오디오 단독 검증 도구)
# 크로스컴파일 예시:  make CC=aarch64-linux-gnu-gcc
#
# 의존 패키지:
#   libmosquitto-dev  (MQTT)
#   libasound2-dev    (I2S/ALSA 알림음 - rpic_subscriber에 포함됨)
#   -lpthread, -lm    (오디오 워커/사인 생성, 표준 라이브러리)

CC      ?= gcc
CFLAGS  := -Wall -Wextra -O2 \
           -I include \
           -I ../hal/include \
           -I ../modbus/include \
           -I ../../common/include \
           -I ../../../shared
LDFLAGS := -lmosquitto -lasound -lm -lpthread

SRCS := src/main.c \
        src/actuator_registry.c \
        src/cmd_parser.c \
        src/mqtt_sub.c \
        src/rpic_audio.c \
        src/audio_event.c \
        src/audio_ref.c \
        src/fan_control.c \
        src/audio_arbiter.c \
        src/matrix_link.c \
        ../modbus/src/modbus_rtu.c \
        ../hal/src/guardx_hal.c

HDRS := $(wildcard include/*.h ../hal/include/*.h ../modbus/include/*.h ../../common/include/*.h ../../../shared/*.h)

TARGET := rpic_subscriber

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# --- 오디오 단독 검증 도구 (톤/WAV 재생, 구독자 없이 소리만 확인) ---
tools: audio_test

audio_test: src/audio_test.c src/rpic_audio.c include/rpic_audio.h $(HDRS)
	$(CC) $(CFLAGS) -o $@ src/audio_test.c src/rpic_audio.c -lasound -lm

clean:
	rm -f $(TARGET) audio_test

.PHONY: all tools clean
