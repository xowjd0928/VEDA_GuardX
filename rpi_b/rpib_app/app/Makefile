# rpib_app/app/Makefile
# GuardX RPi B 판단 엔진 빌드
#
# 네이티브 빌드:      make
# 크로스컴파일 예시:  make CC=aarch64-linux-gnu-gcc
# (libmosquitto는 타겟 sysroot에 있어야 함: apt install libmosquitto-dev)
#
# PHASE 4: fire_threshold 핫리로드용 libpq 추가
#   Pi B에서 최초 1회: sudo apt install libpq-dev
#   (libmosquitto와 마찬가지로 네이티브 빌드 시 apt 패키지로 해결,
#   크로스컴파일 시엔 타겟 sysroot에도 있어야 함)

CC      ?= gcc
CFLAGS  := -Wall -Wextra -O2 \
           -I include \
           -I ../../common/include \
           -I /usr/include/postgresql
# -lm: threshold_loader.c의 fabsf(가중치 합 검증). -O2에서는 컴파일러
# 빌트인으로 인라인돼 실제로는 안 쓰이지만, 최적화를 끄면(-O0 디버깅)
# 진짜 심볼 참조가 생겨 링크가 깨진다 - 미리 걸어둔다.
# -lpthread: PHASE 3 DB 기록 워커 스레드. libmosquitto도 내부적으로
# 스레드를 쓰지만 그건 라이브러리가 알아서 링크하는 것이므로, 우리가
# pthread_create를 직접 부르는 이상 명시해야 한다.
LDFLAGS := -lmosquitto -lpq -lm -lpthread

SRCS := src/main.c \
        src/sensor_parser.c \
        src/decision.c \
        src/cmd_builder.c \
        src/db_writer.c \
        src/db_writer_pg.c \
        src/db_queue.c \
        src/mqtt_bus.c \
        src/threshold_loader.c \
        src/zone_loader.c

HDRS := $(wildcard include/*.h ../../common/include/*.h)

TARGET := rpib_engine

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# db_queue 단위 테스트. 엔진 본체와 달리 libmosquitto/libpq가 필요 없어
# (자료구조 + decision.h 타입만 씀) 따로 빌드한다.
test: ../test/test_db_queue.c src/db_queue.c src/decision.c
	$(CC) $(CFLAGS) -o test_db_queue $^ -lpthread
	./test_db_queue

clean:
	rm -f $(TARGET) test_db_queue

.PHONY: all clean test
