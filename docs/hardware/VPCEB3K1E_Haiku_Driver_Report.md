# Haiku Driver Compatibility Report — Sony Vaio VPCEB3K1E

| | |
|---|---|
| **Status** | ✅ Complete |
| **Category** | Hardware report |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---

**Analysis date:** March 18, 2026
**System:** Haiku R1~beta5+development, x86_64 kernel (build Feb 14, 2026)

---

## Detected Hardware Configuration

| Component | Detail |
|---|---|
| **CPU** | Intel Core i3 M 370 @ 2.40 GHz (2 cores / 4 threads) |
| **RAM** | ~3.7 GB (3940 MB total) |
| **Chipset** | Intel HM55 (5 Series/3400) |
| **Display** | 15.5" 1366x768 |

---

## Driver Status by Component

### 1. CPU - WORKING
- All 4 threads recognized and active
- Variable frequency (SpeedStep active): from ~1217 to ~2394 MHz
- `x86_cstates` module loaded for power saving (C-states)
- **Status: Full support**

### 2. GPU - Intel HD Graphics (Ironlake) - WORKING (with patches)
- `intel_extreme` driver loaded (device 0046)
- **Native resolution: 1366x768 at 59.9 Hz** (32-bit) - FIXED with patched accelerant
- Patched accelerant installed at `/boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant`
- **Patches applied:**
  1. `LVDSPort::IsConnected()` - Added VESA EDID / VBT / BIOS fallback for PCH platforms (GMBUS DDC was failing on this panel)
  2. `LVDSPort::SetDisplayMode()` - Preserves BIOS dual/single channel LVDS configuration (ref: 9front fix)
  3. `Pipe::Enable()` - Added watermark support for PCH Ibex Peak (IBX)
  4. `LVDSPort::SetDisplayMode()` - Disables panel fitter at native resolution
  5. `commands.h` - Fixed COMMAND_BLIT_RGBA: bits 21:20 indicate tiling on Gen5, not RGBA
  6. `engine.cpp` - Fixed uninitialized flush variable + enabled engine trace
- **Status: Native resolution working. 2D acceleration under test.**

### 3. Audio - Intel HDA - WORKING (with patches)
- Controller: Intel 5 Series/3400 HD Audio (device 3b56)
- Codec: Realtek ALC269 (subsystem Sony 104d:4600)
- Device node present: `/dev/audio/hmulti/hda`
- Patched driver installed via HPKG with BlockedEntries to override the system driver
- **Patches applied:**
  1. Sony VAIO quirk: VREF_GRD on pin 0x19 to eliminate speaker crosstalk
  2. Widget type override for NID 0x23/0x24 as WT_AUDIO_SELECTOR for the microphone
- Headphone/speaker automute already implemented in the base driver
- **Status: Audio working, microphone detected, VREF fix active (Quirks: 0x4000)**

### 4. Ethernet - Marvell Yukon 88E8059 - WORKING
- `marvell_yukon` driver loaded
- Interface `/dev/net/marvell_yukon/0` present
- **Status: Full support**

### 5. WiFi - Intel Centrino Wireless-N 1000 - WORKING
- `iprowifi4965` driver loaded
- Connected and operating via WPA2/802.11n(g)
- **Status: Full support**

### 6. USB - Intel EHCI - WORKING
- 2 EHCI controllers (device 3b3c and 3b34) with integrated hubs
- USB devices correctly detected
- **Status: Full support** (USB 2.0 only)

### 7. Keyboard and Touchpad (PS/2) - WORKING
- Device nodes: `/dev/input/keyboard/at` and `/dev/input/mouse/ps2`
- **Status: Full support**

### 8. SATA Storage (AHCI) - WORKING
- Intel AHCI controller (device 3b29) supported
- Disk detected at `/dev/disk/scsi/0`
- **Status: Full support**

### 9. Webcam - Microdia (0c45:6409) - NOT WORKING
- Detected over USB as "Webcam" (Video Streaming class)
- No `/dev/video/` device node present
- Haiku has no working UVC (USB Video Class) driver for this device
- **External project underway** to develop the UVC driver
- **Status: Not supported (external development underway)**

### 10. Bluetooth - BCM2070 (Foxconn) - PARTIAL
- Adapter detected over USB: "Foxconn T77H114 BCM2070"
- Kernel modules `btCoreData` and `hci` loaded
- Bluetooth device nodes present (`/dev/bluetooth/h2..h5`)
- Haiku's Bluetooth stack is historically incomplete
- **External project underway** to complete the BT stack
- **Status: Detected, limited/experimental functionality (external development underway)**

### 11. Card Reader - Ricoh SDHCI + Memory Stick - PARTIAL
- 2x Ricoh SD Host Controller (device e822) detected
- 1x Ricoh Memory Stick Host Controller (device e230) detected
- `sdhci` and `mmc` modules loaded in the kernel
- **Status: Driver loaded, functionality potentially limited**

### 12. Power Management (ACPI) - PARTIAL
- `acpi_battery`: battery monitoring present
- `acpi_thermal`: temperature monitoring present
- `acpi_button`: power/lid button present
- Missing: suspend/hibernate (not supported in Haiku)
- **Status: Basic monitoring working. No suspend/hibernate.**

### 13. Intel MEI (Management Engine) - NOT SUPPORTED
- Device 3b64 detected but no driver loaded
- Not critical for day-to-day operation
- **Status: Not supported (irrelevant)**

### 14. SMBus - NOT SUPPORTED
- Intel SMBus Controller (device 3b30) detected
- Used for hardware sensors (temperatures, fans)
- **Status: Not supported**

---

## Summary Table

| Component | Status | Notes |
|---|---|---|
| CPU (i3 M370) | **Working** | 4 threads, SpeedStep active |
| GPU (Intel HD) | **Working** | Native 1366x768, 6 patches applied |
| Audio (HDA) | **Working** | ALC269 with VREF fix + mic fix |
| Ethernet (Marvell) | **Working** | |
| WiFi (Intel N1000) | **Working** | Connected and operating |
| USB 2.0 | **Working** | |
| Keyboard/Touchpad | **Working** | |
| SATA/HDD | **Working** | |
| Webcam | **Not working** | External project underway |
| Bluetooth | **Partial** | External project underway |
| Card Reader | **Partial** | Driver loaded, to be verified |
| Power Mgmt | **Partial** | No suspend/hibernate |
| Intel MEI | **Not supported** | Not critical |
| SMBus | **Not supported** | Not critical |

---

## Implementation Plan (from easiest to hardest)

### Level 1 - Configuration / Quick fixes

**1. GPU resolution (1366x768)**
- **Difficulty:** Low
- **Description:** The resolution is stuck at 1024x768 even though the `intel_extreme` driver is loaded. Forcing the correct mode via `screenmode` or adding an EDID override might be enough. Check whether the LVDS panel is correctly detected and whether a manual modeline configuration fixes the issue.
- **Action:** Test `screenmode --set 1366x768x32` or check the driver logs with `syslog` to understand why the native resolution is not being offered.

**2. Verify Card Reader (SDHCI) operation**
- **Difficulty:** Low
- **Description:** The `sdhci` and `mmc` drivers are already loaded. Insert an SD card and check whether it mounts automatically. If the Ricoh controller requires specific quirks, small patches may be needed.
- **Action:** Test with a physical SD card, review syslog for any initialization errors.

### Level 2 - Improvements to existing drivers

**3. Audio - Output/mixer configuration**
- **Difficulty:** Low-Medium
- **Description:** The HDA driver is loaded and working, but the specific codec on the VPCEB3K1E (likely Realtek ALC275) may require specific pin configuration to correctly handle routing between internal speakers, headphones, and microphone.
- **Action:** Verify the HDA codec in use, test headphone/speaker switching, configure the mixer.

**4. ACPI Battery - Advanced monitoring**
- **Difficulty:** Medium
- **Description:** The `acpi_battery` driver is loaded. Verify that charge/discharge data is read correctly. Possible improvements for low-battery notifications or Deskbar integration.
- **Action:** Read `/dev/power/acpi_battery` and verify the accuracy of the reported data.

### Level 3 - Driver development / external project integration

**5. Bluetooth - External project integration**
- **Difficulty:** Medium-High
- **Description:** Haiku's BT stack has the basics (`btCoreData`, `hci`) and the device nodes exist. The BCM2070 chip is recognized. An external project is working on completing the stack. Integration requires: completing the USB-HCI transport layer, implementing minimal BT profiles (at least HID and file transfer), and testing with the specific BCM2070 chip.
- **Dependencies:** External project for the BT stack.
- **Action:** Monitor the external project, test intermediate builds with the BCM2070 hardware.

**6. Webcam - UVC driver integration**
- **Difficulty:** Medium-High
- **Description:** The Microdia webcam (0c45:6409) is a standard USB Video Class device. An external project is developing a UVC driver for Haiku. Integration requires: a video capture framework (`/dev/video/`), implementation of the UVC protocol, and handling of compression formats (MJPEG/YUV).
- **Dependencies:** External project for the UVC driver, media_kit video framework.
- **Action:** Monitor the external project, test with this specific webcam once available.

**7. GPU - 2D/3D acceleration and improved mode-setting**
- **Difficulty:** High
- **Description:** The `intel_extreme` driver for the Ironlake generation (Gen5) has basic but incomplete support. Possible improvements: correct LVDS/eDP handling for the native panel, 2D acceleration via the BLT engine, and possible OpenGL support via Mesa/Gallium (very complex).
- **Action:** Contribute to Haiku's upstream `intel_extreme` driver to improve Ironlake support.

### Level 4 - Complex system features

**8. SMBus - Hardware sensor monitoring**
- **Difficulty:** High
- **Description:** The Intel SMBus controller (device 3b30) would allow reading temperature sensors, fan speed, and voltage. Requires implementing an SMBus driver for the Intel 5 Series chipset and an hwmon framework.
- **Action:** Implement an i2c/smbus driver, sensor interface.

**9. Suspend/Hibernate (ACPI S3/S4)**
- **Difficulty:** Very High
- **Description:** Haiku does not support suspend/hibernate. This is an operating-system-level limitation that requires: saving/restoring the state of all drivers, ACPI S3/S4 handling, and hardware reinitialization on resume. This is a kernel/OS-level project, not specific to this laptop.
- **Action:** Upstream contribution to the Haiku project. Not solvable at the level of a single machine.

---

## Patches Developed (March 18, 2026)

3 patches were developed to resolve item 1 (GPU resolution):

### Patch 1 - `Ports.cpp:LVDSPort::IsConnected()` [CRITICAL]
Bug: the PCH path (Ironlake+) fell through to `return HasEDID()` with no fallback.
Fix: added a fallback chain VESA EDID -> VBT -> BIOS port enabled.
This is the main fix that resolves the "not connected" panel issue.

### Patch 2 - `Ports.cpp:LVDSPort::SetDisplayMode()` [PREVENTIVE]
Bug: dual/single channel LVDS was being forced based on the P2 divisor.
Fix: preserves the BIOS configuration by reading the current LVDS register.
Prevents potential black screens (ref: 9front igfx fix).

### Patch 3 - `Pipes.cpp:Pipe::Enable()` [ENHANCEMENT]
Bug: watermarks were set only for CPT (Sandy Bridge PCH), not for IBX.
Fix: added INTEL_PCH_IBX to the watermark condition.
Improves display stability on Ironlake.

**Status:** Awaiting compilation and testing.
**Details:** See [`../analysis/DRIVER_TECHNICAL_ANALYSIS.md`](../analysis/DRIVER_TECHNICAL_ANALYSIS.md) for the complete analysis.

---

## BT Notes

Note: during testing a kernel panic was observed in the Bluetooth stack
(`bt_a2dp_source_test` - spinlock deadlock in btCoreData). This is a
separate issue from the GPU driver, related to the experimental BT stack.

---

## Final Notes

The Sony Vaio VPCEB3K1E is **well supported for everyday use** on Haiku. Core functionality (CPU, network, audio, disk, input) is all operational. The immediate critical items to address are:

1. **High priority:** Fix the GPU resolution to 1366x768 - **PATCHES READY**
2. **Medium priority:** Verify the card reader and advanced audio configuration
3. **Low priority:** Wait for/contribute to external projects for webcam and Bluetooth
4. **Long term:** GPU acceleration (groundwork identified), suspend/hibernate
