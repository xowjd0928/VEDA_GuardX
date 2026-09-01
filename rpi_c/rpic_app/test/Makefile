# test/Makefile - 액추에이터 테스트 도구(actuator_test) 빌드
#
#   make          # actuator_test 생성
#   make clean    # 삭제
#
# 필요 패키지: sudo apt install -y libmosquitto-dev

CC     ?= gcc
CFLAGS := -O2 -Wall -Wextra
LDLIBS := -lmosquitto

all: actuator_test test_matrix_link fan_control_test fan_path_test

actuator_test: actuator_test.c
	$(CC) $(CFLAGS) -o $@ actuator_test.c $(LDLIBS)

# RPi C <-> STM32 표시 계약 테스트. modbus_rtu.c를 링크하지 않는다 -
# 테스트 파일 안의 스텁이 그 심볼을 제공해 시리얼 없이 돈다.
test_matrix_link: test_matrix_link.c ../app/src/matrix_link.c
	$(CC) $(CFLAGS) -pthread \
		-I ../app/include -I ../hal/include -I ../modbus/include \
		-I ../../common/include -I ../../../shared \
		-o $@ test_matrix_link.c ../app/src/matrix_link.c

# 팬 우선순위(화재 > AUTO > 수동) 테스트. HAL 과 MQTT 발행은 테스트 파일
# 안의 스텁이 대신하므로 팬도 브로커도 없이 돈다.
fan_control_test: fan_control_test.c ../app/src/fan_control.c
	$(CC) $(CFLAGS) -pthread \
		-I ../app/include -I ../hal/include \
		-I ../../common/include -I ../../../shared \
		-o $@ fan_control_test.c ../app/src/fan_control.c

# VMS AUTO 명령이 RPi C 까지 닿는지 - cmd_parser + actuator_registry 를
# 실제로 링크해 JSON 부터 팬까지 한 번에 통과시킨다.
fan_path_test: fan_path_test.c ../app/src/fan_control.c \
		../app/src/cmd_parser.c ../app/src/actuator_registry.c
	$(CC) $(CFLAGS) -pthread \
		-I ../app/include -I ../hal/include \
		-I ../../common/include -I ../../../shared \
		-o $@ fan_path_test.c ../app/src/fan_control.c \
		../app/src/cmd_parser.c ../app/src/actuator_registry.c

# 자동 검증용 - 빌드하고 바로 돌린다(실패하면 make가 멈춘다).
check: test_matrix_link fan_control_test fan_path_test
	./test_matrix_link
	./fan_control_test
	./fan_path_test

clean:
	rm -f actuator_test test_matrix_link fan_control_test fan_path_test

.PHONY: all check clean
