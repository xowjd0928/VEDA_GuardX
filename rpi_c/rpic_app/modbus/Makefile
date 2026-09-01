# rpic_app/modbus/Makefile
# GuardX RPi C <-> STM32 Modbus RTU 통신 시험 도구 빌드
#
# 네이티브 빌드:      make            # modbus_test 생성
# 크로스컴파일 예시:  make CC=aarch64-linux-gnu-gcc
#
# 의존성 없음(표준 라이브러리 + termios 만 사용). MQTT/ALSA 불필요.

CC      ?= gcc
CFLAGS  := -Wall -Wextra -O2 -I include
SRCS    := src/modbus_rtu.c \
           src/modbus_test.c
HDRS    := $(wildcard include/*.h)

TARGET  := modbus_test

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
