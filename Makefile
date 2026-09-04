CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?= -Iinclude
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
CSTD := -std=c11
LDLIBS := -lm
SYSTEM_NAME := $(shell uname -s)
LIGHT_VIDEO_PLATFORM_OBJECTS :=

ifeq ($(SYSTEM_NAME),Linux)
LDLIBS += -lasound -pthread
CFLAGS += -pthread
X11_LIBS := $(shell pkg-config --libs x11 2>/dev/null)
ifneq ($(strip $(X11_LIBS)),)
CPPFLAGS += -DUM_HAVE_X11=1 $(shell pkg-config --cflags x11 2>/dev/null)
LDLIBS += $(X11_LIBS)
endif
endif

ifeq ($(SYSTEM_NAME),Darwin)
LDLIBS += -framework CoreAudio -framework AudioToolbox
LDLIBS += -framework CoreFoundation -framework SystemConfiguration -pthread
LDLIBS += -framework AVFoundation -framework CoreMedia -framework CoreVideo
LDLIBS += -framework Cocoa
CFLAGS += -pthread
LIGHT_VIDEO_PLATFORM_OBJECTS += src/light_video_macos.o
endif

LIB_SOURCES := \
	src/audio.c \
	src/calibration.c \
	src/calibration_config.c \
	src/channel.c \
	src/crc.c \
	src/distortion.c \
	src/fec.c \
	src/fft.c \
	src/interleave.c \
	src/light.c \
	src/light_live.c \
	src/light_network.c \
	src/light_packet.c \
	src/light_session.c \
	src/light_video.c \
	src/live.c \
	src/live_wire.c \
	src/modem.c \
	src/network.c \
	src/qam.c

LIB_SOURCES += src/session.c src/tcp_relay.c src/traffic_policy.c \
	src/validation.c
LIB_OBJECTS := $(LIB_SOURCES:.c=.o)
LIB_OBJECTS += $(LIGHT_VIDEO_PLATFORM_OBJECTS)
SIM_LIB_OBJECTS := $(filter-out src/audio.o src/network.o src/tcp_relay.o,\
	$(LIB_OBJECTS))

.PHONY: all clean test check

all: universal-modem

universal-modem: src/main.o $(LIB_OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_modem: tests/test_modem.o $(LIB_OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_live_audio: tests/test_live_audio.o $(SIM_LIB_OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_tcp_relay: tests/test_tcp_relay.o src/network.o src/tcp_relay.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_light: tests/test_light.o src/light.o src/crc.o src/fec.o \
	src/interleave.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_light_session: tests/test_light_session.o src/light_session.o \
	src/light_packet.o src/light.o src/crc.o src/fec.o src/interleave.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_light_packet: tests/test_light_packet.o src/light_session.o \
	src/light_packet.o src/light.o src/crc.o src/fec.o src/interleave.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_light_network: tests/test_light_network.o src/light_network.o \
	src/light_session.o src/light_packet.o src/light.o src/crc.o src/fec.o \
	src/interleave.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test_light_video: tests/test_light_video.o src/light_video.o \
	$(LIGHT_VIDEO_PLATFORM_OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test check: test_modem test_light test_light_session test_light_packet \
	test_light_network \
	test_light_video test_live_audio test_tcp_relay
	./test_modem
	./test_light
	./test_light_session
	./test_light_packet
	./test_light_network
	./test_light_video
	./test_live_audio
	./test_tcp_relay

%.o: %.c include/um.h src/um_internal.h
	$(CC) $(CPPFLAGS) $(CSTD) $(CFLAGS) $(WARNINGS) -c $< -o $@

%.o: %.m include/um.h src/light_video.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -fobjc-arc -fblocks \
		-c $< -o $@

src/audio.o src/live.o: src/audio.h
src/live.o src/network.o tests/test_live_audio.o tests/test_tcp_relay.o: \
	src/network.h
src/network.o src/tcp_relay.o tests/test_tcp_relay.o: src/tcp_relay.h
src/live.o src/live_wire.o tests/test_modem.o tests/test_live_audio.o: src/live_wire.h
src/live.o src/traffic_policy.o tests/test_modem.o: src/traffic_policy.h
tests/test_live_audio.o: src/audio.h
src/main.o src/light_live.o src/light_video.o tests/test_light_video.o: \
	src/light_video.h
src/light_live.o: src/network.h src/traffic_policy.h
src/light_packet.o src/light_session.o tests/test_light_packet.o: \
	src/light_packet.h

clean:
	rm -f $(LIB_OBJECTS) src/main.o tests/test_modem.o tests/test_live_audio.o \
		tests/test_light.o tests/test_light_session.o \
		tests/test_light_packet.o \
		tests/test_light_network.o tests/test_light_video.o \
		tests/test_tcp_relay.o $(LIGHT_VIDEO_PLATFORM_OBJECTS) universal-modem \
		test_modem test_light test_light_session test_light_packet \
		test_light_network \
		test_light_video \
		test_live_audio test_tcp_relay
