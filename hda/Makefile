## Makefile for HDA audio kernel driver (Haiku)
## Compiles the patched HDA driver with ALC269 fixes

CXX = /boot/system/develop/tools/bin/g++

SYSHDRS = /boot/system/develop/headers
PRIVHDRS = $(SYSHDRS)/private

CXXFLAGS = -Wall -O2 -fPIC -std=c++17 \
	-D_KERNEL_MODE \
	-I. \
	-I$(PRIVHDRS)/kernel \
	-I$(PRIVHDRS)/kernel/arch/x86 \
	-I$(PRIVHDRS)/system/arch/x86_64 \
	-I$(PRIVHDRS)/system/arch/x86 \
	-I$(PRIVHDRS)/system \
	-I$(PRIVHDRS) \
	-I$(PRIVHDRS)/audio \
	-I$(SYSHDRS)/os/drivers \
	-I$(SYSHDRS)/os/kernel \
	-I$(SYSHDRS)/os/support \
	-I$(SYSHDRS)/posix \
	-fno-exceptions -fno-rtti -ffreestanding -fno-builtin -fno-plt

SRCS = driver.cpp device.cpp hda_multi_audio.cpp hda_controller.cpp hda_codec.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = hda

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -shared -nostdlib -o $@ $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	mkdir -p /boot/system/non-packaged/add-ons/kernel/drivers/bin
	cp $(TARGET) /boot/system/non-packaged/add-ons/kernel/drivers/bin/
	mkdir -p /boot/system/non-packaged/add-ons/kernel/drivers/dev/audio/hmulti
	cp $(TARGET) /boot/system/non-packaged/add-ons/kernel/drivers/dev/audio/hmulti/hda
	@echo "Installed to /boot/system/non-packaged/add-ons/kernel/drivers/bin/$(TARGET)"
	@echo "Reboot to activate the patched driver."
