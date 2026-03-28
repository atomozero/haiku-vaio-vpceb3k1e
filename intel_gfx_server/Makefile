## IntelGfx Server - GPU acceleration server for Intel Gen5 on Haiku

ACCEL_DIR = ../intel_extreme/accelerant

INCLUDES = -I$(ACCEL_DIR) \
	-I/boot/system/develop/headers/os/add-ons/graphics \
	-I/boot/system/develop/headers/os/drivers \
	-I/boot/system/develop/headers/private/graphics \
	-I/boot/system/develop/headers/private/graphics/intel_extreme \
	-I/boot/system/develop/headers/private/graphics/common \
	-I/boot/system/develop/headers/private/shared \
	-I/boot/system/develop/headers/private/system \
	-I/boot/system/develop/headers/os \
	-I/boot/system/develop/headers/os/support \
	-I/boot/system/develop/headers/os/kernel \
	-I/boot/system/develop/headers/os/app \
	-I/boot/system/develop/headers/posix

CXXFLAGS = -Wall -O2 $(INCLUDES) -std=c++17
LDFLAGS = -lbe

TARGET = IntelGfxServer

all: $(TARGET)

$(TARGET): IntelGfxServer.cpp
	g++ $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
