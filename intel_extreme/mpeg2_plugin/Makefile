## Makefile for MPEG-2 decoder media_kit plugin and viewer
## Builds the plugin .so and the standalone GPU-accelerated viewer.

CXX = g++
CXXFLAGS = -Wall -O2 -fPIC \
	-I../accelerant \
	-I/boot/system/develop/headers \
	-I/boot/system/develop/headers/os \
	-I/boot/system/develop/headers/os/support \
	-I/boot/system/develop/headers/os/media \
	-I/boot/system/develop/headers/os/interface \
	-I/boot/system/develop/headers/os/app \
	-I/boot/system/develop/headers/private/media \
	-I/boot/system/develop/headers/private/graphics/intel_extreme \
	-I/boot/system/develop/headers/private/graphics/common \
	-I/boot/system/develop/headers/os/add-ons/graphics \
	-I/boot/system/develop/headers/os/drivers \
	-I/boot/system/develop/headers/private/graphics \
	-I/boot/system/develop/headers/private/shared

LDFLAGS_PLUGIN = -shared
LIBS_PLUGIN = -lbe -lmedia -lstdc++
LIBS_VIEWER = -lbe -lstdc++

PARSER_SRC = ../accelerant/mpeg2_parser.cpp
GPU_RING_SRC = ../accelerant/gpu_ring.cpp

PLUGIN_SOURCES = mpeg2_decoder_plugin.cpp gpu_idct.cpp $(GPU_RING_SRC) $(PARSER_SRC)
PLUGIN_TARGET = mpeg2_decoder.so

VIEWER_SOURCES = mpeg2_viewer.cpp gpu_idct.cpp $(GPU_RING_SRC) $(PARSER_SRC)
VIEWER_TARGET = mpeg2_viewer

INSTALL_DIR = /boot/home/config/non-packaged/add-ons/media/plugins

.PHONY: all clean install viewer

all: $(PLUGIN_TARGET) $(VIEWER_TARGET)

$(PLUGIN_TARGET): $(PLUGIN_SOURCES)
	$(CXX) $(CXXFLAGS) $(LDFLAGS_PLUGIN) -o $@ $^ $(LIBS_PLUGIN)

$(VIEWER_TARGET): $(VIEWER_SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS_VIEWER)

viewer: $(VIEWER_TARGET)

install: $(PLUGIN_TARGET)
	mkdir -p $(INSTALL_DIR)
	cp $(PLUGIN_TARGET) $(INSTALL_DIR)/
	@echo ""
	@echo "=== Installed to $(INSTALL_DIR)/$(PLUGIN_TARGET) ==="
	@echo "Restart media_server: pkill -f media_server"

clean:
	rm -f $(PLUGIN_TARGET) $(VIEWER_TARGET)
