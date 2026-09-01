CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  ?= -lmosquitto

TARGET := fan_test actuator_test

all: $(TARGET)

fan_test: fan_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

actuator_test: actuator_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
