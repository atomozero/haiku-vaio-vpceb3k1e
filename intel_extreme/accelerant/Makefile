## Makefile for intel_extreme accelerant (Haiku)
## Compiles the patched accelerant with LVDS fixes

CXX = /boot/system/develop/tools/bin/g++
CC  = /boot/system/develop/tools/bin/gcc
AR  = /boot/system/develop/tools/bin/gcc-ar

SYSHDRS = /boot/system/develop/headers
PRIVHDRS = $(SYSHDRS)/private

CFLAGS_COMMON = -Wall -O2 \
	-I. \
	-I$(SYSHDRS)/os/add-ons/graphics \
	-I$(SYSHDRS)/os/drivers \
	-I$(PRIVHDRS)/graphics \
	-I$(PRIVHDRS)/graphics/intel_extreme \
	-I$(PRIVHDRS)/graphics/common \
	-I$(PRIVHDRS)/shared \
	-I$(PRIVHDRS)/system \
	-I$(PRIVHDRS)/kernel/boot \
	-I$(SYSHDRS)/os \
	-I$(SYSHDRS)/os/support \
	-I$(SYSHDRS)/os/interface \
	-I$(SYSHDRS)/os/kernel \
	-I$(SYSHDRS)/os/storage \
	-I$(SYSHDRS)/os/app \
	-I$(SYSHDRS)/posix

CXXFLAGS = $(CFLAGS_COMMON) -std=c++17 -fPIC
CFLAGS   = $(CFLAGS_COMMON) -std=c11 -fPIC

# Accelerant source files
ACCEL_SRCS = accelerant.cpp cursor.cpp dpms.cpp engine.cpp hooks.cpp \
	memory.cpp mode.cpp overlay.cpp pll.cpp render.cpp \
	FlexibleDisplayInterface.cpp PanelFitter.cpp Ports.cpp Pipes.cpp \
	TigerLakePLL.cpp

ACCEL_OBJS = $(ACCEL_SRCS:.cpp=.o)

# Common library source files
COMMON_CPP_SRCS = ../common_src/compute_display_timing.cpp \
	../common_src/create_display_modes.cpp \
	../common_src/dp.cpp \
	../common_src/validate_display_mode.cpp \
	../common_src/video_configuration.cpp

COMMON_C_SRCS = ../common_src/ddc.c \
	../common_src/decode_edid.c \
	../common_src/dump_edid.c \
	../common_src/i2c.c

COMMON_OBJS = $(COMMON_CPP_SRCS:.cpp=.o) $(COMMON_C_SRCS:.c=.o)

TARGET = ../intel_extreme.accelerant

.PHONY: all clean install

all: $(TARGET)

# Build common library
../libaccelerantscommon.a: $(COMMON_OBJS)
	$(AR) rcs $@ $^

# Build accelerant shared library
$(TARGET): $(ACCEL_OBJS) ../libaccelerantscommon.a
	$(CXX) -shared -o $@ $(ACCEL_OBJS) ../libaccelerantscommon.a -lbe -lstdc++

# Compile rules
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

../common_src/%.o: ../common_src/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

../common_src/%.o: ../common_src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(ACCEL_OBJS) $(COMMON_OBJS) ../libaccelerantscommon.a $(TARGET)

install: $(TARGET)
	mkdir -p /boot/system/non-packaged/add-ons/accelerants
	cp $(TARGET) /boot/system/non-packaged/add-ons/accelerants/
	@echo "Installed to /boot/system/non-packaged/add-ons/accelerants/$(TARGET)"
	@echo "Reboot to activate the patched driver."
