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

# Gen4/5 EU assembler (in-tree, ported from igt-gpu-tools). Used to compile
# .g4a kernel sources into .g4b.gen5 C array initializer fragments that are
# #include'd by media_pipeline.cpp.
GEN4ASM = ../tools/gen4asm/gen4asm

# Kernel sources live in kernels/ and are compiled into kernels/*.g4b.gen5.
KERNEL_SRCS = kernels/hello_world.g4a kernels/memset_indexed.g4a \
	kernels/saxpy_simd8.g4a kernels/iq_intra_single.g4a kernels/sampler_read.g4a \
	kernels/sampler_read_4row.g4a kernels/curbe_read.g4a \
	kernels/libva_probe.g4a kernels/idct_single.g4a \
	kernels/iq_idct_intra.g4a kernels/mc_forward.g4a kernels/idct_to_u8.g4a kernels/iq_idct_intra_nolevelshift.g4a
KERNEL_BINS = $(KERNEL_SRCS:.g4a=.g4b.gen5)

# Accelerant source files
ACCEL_SRCS = accelerant.cpp bench.cpp cursor.cpp dpms.cpp engine.cpp \
	gpu_bo.cpp gpu_debug.cpp gpu_ring.cpp gen5_ops.cpp gen6_ops.cpp gen7_ops.cpp \
	hooks.cpp media_pipeline.cpp \
	memory.cpp mode.cpp mpeg2_parser.cpp overlay.cpp pll.cpp render.cpp \
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

.PHONY: all clean install kernels test test-install revert-test

all: kernels $(TARGET)

# Build with the media-pipeline hello-world probe enabled. This is the
# only sanctioned way to produce a test-enabled accelerant; the flag is
# deliberately not a runtime toggle so that a hanging probe cannot brick
# subsequent boots silently.
test:
	$(MAKE) clean
	$(MAKE) all CFLAGS_COMMON="$(CFLAGS_COMMON) -DMEDIA_PIPELINE_HELLO_TEST"
	@echo ""
	@echo "=== TEST-ENABLED ACCELERANT BUILT ==="
	@echo "  binary: $(TARGET)"
	@echo "  probe fires once per library load at intel_init_accelerant"
	@echo "  install: sudo make test-install  (saves a safety backup)"
	@echo "  rollback: sudo make revert-test"
	@echo ""

# Install the test-enabled accelerant to the user override path, first
# saving whatever is currently there as .revert-backup so a rollback is
# one command away.
INSTALL_DIR  = /boot/system/non-packaged/add-ons/accelerants
INSTALL_NAME = intel_extreme.accelerant
test-install: $(TARGET)
	@mkdir -p $(INSTALL_DIR)
	@if [ -f $(INSTALL_DIR)/$(INSTALL_NAME) ]; then \
		cp $(INSTALL_DIR)/$(INSTALL_NAME) $(INSTALL_DIR)/$(INSTALL_NAME).revert-backup; \
		echo "backup saved: $(INSTALL_DIR)/$(INSTALL_NAME).revert-backup"; \
	fi
	cp $(TARGET) $(INSTALL_DIR)/$(INSTALL_NAME)
	@echo ""
	@echo "=== TEST ACCELERANT INSTALLED ==="
	@echo "  reboot to run the probe"
	@echo "  after boot:  grep 'intel_extreme media' /boot/system/var/log/syslog"
	@echo "  rollback: sudo make revert-test"

revert-test:
	@if [ -f $(INSTALL_DIR)/$(INSTALL_NAME).revert-backup ]; then \
		cp $(INSTALL_DIR)/$(INSTALL_NAME).revert-backup $(INSTALL_DIR)/$(INSTALL_NAME); \
		echo "reverted from backup"; \
	else \
		rm -f $(INSTALL_DIR)/$(INSTALL_NAME); \
		echo "removed override, will fall back to packaged accelerant"; \
	fi

# Build the in-tree EU assembler if missing, then all kernel binaries.
kernels: $(KERNEL_BINS)

$(GEN4ASM):
	$(MAKE) -C ../tools/gen4asm

kernels/%.g4b.gen5: kernels/%.g4a $(GEN4ASM)
	$(GEN4ASM) -g5 -o $@ $<

# Build common library
../libaccelerantscommon.a: $(COMMON_OBJS)
	$(AR) rcs $@ $^

# Build accelerant shared library
$(TARGET): $(ACCEL_OBJS) ../libaccelerantscommon.a
	$(CXX) -shared -o $@ $(ACCEL_OBJS) ../libaccelerantscommon.a -lbe -lstdc++

# media_pipeline.cpp #include's assembled kernel binaries; force it to
# rebuild when any kernel source changes.
media_pipeline.o: $(KERNEL_BINS)

# Compile rules
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

../common_src/%.o: ../common_src/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

../common_src/%.o: ../common_src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(ACCEL_OBJS) $(COMMON_OBJS) ../libaccelerantscommon.a $(TARGET) \
		$(KERNEL_BINS)

install: $(TARGET)
	mkdir -p /boot/system/non-packaged/add-ons/accelerants
	cp $(TARGET) /boot/system/non-packaged/add-ons/accelerants/
	@echo "Installed to /boot/system/non-packaged/add-ons/accelerants/$(TARGET)"
	@echo "Reboot to activate the patched driver."
