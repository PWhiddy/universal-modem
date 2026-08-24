CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?= -Iinclude
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
CSTD := -std=c11
LDLIBS := -lm
SYSTEM_NAME := $(shell uname -s)

ifeq ($(SYSTEM_NAME),Linux)
LDLIBS += -lasound -pthread
CFLAGS += -pthread
endif

ifeq ($(SYSTEM_NAME),Darwin)
LDLIBS += -framework CoreAudio -framework AudioToolbox
LDLIBS += -framework CoreFoundation -pthread
CFLAGS += -pthread
endif

LIB_SOURCES := \
	src/audio.c \
	src/calibration.c \
	src/channel.c \
	src/crc.c \
	src/distortion.c \
	src/fec.c \
	src/fft.c \
	src/interleave.c \
	src/live.c \
	src/live_wire.c \
	src/modem.c \
	src/qam.c

LIB_SOURCES += src/session.c src/validation.c
LIB_OBJECTS := $(LIB_SOURCES:.c=.o)

.PHONY: all clean test check

all: universal-modem

universal-modem: src/main.o $(LIB_OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_modem: tests/test_modem.o $(LIB_OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test check: test_modem
	./test_modem

%.o: %.c include/um.h src/um_internal.h
	$(CC) $(CPPFLAGS) $(CSTD) $(CFLAGS) $(WARNINGS) -c $< -o $@

src/audio.o src/live.o: src/audio.h
src/live.o src/live_wire.o tests/test_modem.o: src/live_wire.h

clean:
	rm -f $(LIB_OBJECTS) src/main.o tests/test_modem.o universal-modem test_modem
