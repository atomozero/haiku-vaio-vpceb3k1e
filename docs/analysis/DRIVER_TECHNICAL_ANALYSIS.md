# Technical Comparison: intel_extreme (Haiku) vs i915 (Linux)

| | |
|---|---|
| **Status** | 📄 Reference |
| **Category** | Analysis |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---

# GPU Ironlake Mobile (0x0046) - Sony Vaio VPCEB3K1E

**Date:** March 18, 2026

---

## 1. THE CRITICAL BUG: LVDSPort::IsConnected() - Ports.cpp:1134-1180

### Evidence from the syslog

```
intel_extreme: found LFP of size 1366 x 768 in BIOS VBT tables  ← Knows the resolution!
intel_extreme: LVDS C: no EDID information found.                ← DDC/GMBUS fails
intel_extreme: dump_ports: No ports connected                    ← No ports!
intel_extreme: Warning: zero active displays were found!         ← VESA fallback
```

The LVDS register reads `0x80308302`:
- Bit 31 (`LVDS_PORT_EN`): **SET** - the BIOS has enabled the LVDS panel
- Bit 1 (`PCH_LVDS_DETECTED`): **SET** - hardware confirms the panel is present

### The problematic code

```cpp
// Ports.cpp:1134
bool LVDSPort::IsConnected()
{
    if (gInfo->shared_info->pch_info != INTEL_PCH_NONE) {
        uint32 registerValue = read32(_PortRegister());
        if ((registerValue & PCH_LVDS_DETECTED) == 0) {
            return false;  // This check PASSES (bit is set)
        }
    }
    // ...falls through to here...
    return HasEDID();  // ← RETURNS FALSE! GMBUS DDC fails!
}
```

### Why it fails

For PCH platforms (Ironlake+), the method:
1. Correctly checks the `PCH_LVDS_DETECTED` bit - **OK, passes**
2. Then falls through to the final `return HasEDID()` (line 1179) - **FAILS**
3. `HasEDID()` calls the DDC read via GMBUS on `INTEL_I2C_IO_C` (0x5018)
4. The EDID read via I2C fails (many LVDS panels have no EDID EEPROM)
5. Result: the port is considered "not connected" even though it is active!

### Comparison with pre-PCH code (Gen <= 4)

The code for earlier generations (lines 1148-1174) **already** has the correct logic:
```cpp
} else if (gInfo->shared_info->device_type.Generation() <= 4) {
    if (!HasEDID()) {
        if (gInfo->shared_info->has_vesa_edid_info) {
            // Fallback to VESA EDID ← IMPLEMENTED for Gen<=4
        } else if (gInfo->shared_info->got_vbt) {
            return true;  // Force connected if VBT is present ← IMPLEMENTED for Gen<=4
        }
    }
}
```

This same fallback logic is **completely missing** from the PCH path!

### Comparison with Linux i915

Linux `intel_lvds_init()` uses this hierarchy:
1. OpRegion/VBT panel data
2. EDID via GMBUS/DDC
3. Current hardware state (reads timings from the active pipe)
4. VBT BIOS fallback

Linux **never requires EDID** to consider LVDS connected. If the LVDS register
shows the port enabled and/or the VBT contains panel data, the display is
considered connected.

### Proposed fix

```cpp
bool LVDSPort::IsConnected()
{
    if (gInfo->shared_info->pch_info != INTEL_PCH_NONE) {
        uint32 registerValue = read32(_PortRegister());
        if ((registerValue & PCH_LVDS_DETECTED) == 0) {
            TRACE("LVDS: Not detected\n");
            return false;
        }

        // Try EDID via GMBUS
        if (HasEDID())
            return true;

        // Fallback: VESA EDID from the bootloader
        if (gInfo->shared_info->has_vesa_edid_info) {
            TRACE("LVDS: Using VESA edid info\n");
            memcpy(&fEDIDInfo, &gInfo->shared_info->vesa_edid_info,
                sizeof(edid1_info));
            if (fEDIDState != B_OK) {
                fEDIDState = B_OK;
                edid_dump(&fEDIDInfo);
            }
            return true;
        }

        // Fallback: VBT has valid panel data
        if (gInfo->shared_info->got_vbt) {
            TRACE("LVDS: No EDID but VBT present, force connected\n");
            return true;
        }

        // Last resort: the BIOS has already enabled the port
        if (registerValue & LVDS_PORT_EN) {
            TRACE("LVDS: No EDID/VBT but port enabled by BIOS, "
                "force connected\n");
            return true;
        }

        return false;
    }
    // ...rest of the code unchanged for Gen<=4...
}
```

### Expected impact

With this fix:
- The LVDS panel will be recognized as connected
- `create_mode_list()` in mode.cpp will use the VBT path (lines 236-255) to build the
  mode list with a resolution of 1366x768 (already read correctly from the VBT)
- `intel_set_display_mode()` will process the LVDS port (no longer skipping it)
- `LVDSPort::SetDisplayMode()` will configure the PLL, FDI, transcoder, and panel

---

## 2. SECONDARY ISSUES IDENTIFIED

### 2.1 Watermark only for CPT, not for IBX - Pipes.cpp:641

```cpp
void Pipe::Enable(bool enable)
{
    if (enable) {
        // Watermark only for CPT (Sandy Bridge PCH)
        if (gInfo->shared_info->pch_info == INTEL_PCH_CPT) {
            write32(INTEL_DISPLAY_A_PIPE_WATERMARK, 0x0783818);
        }
        // IBX (Ironlake PCH) receives no watermark!
    }
}
```

**Problem:** Watermarks control the buffering of the display pipeline.
Without correct watermarks on IBX, artifacts or flickering may occur.

**Linux:** Configures generation-specific watermarks in `ironlake_update_wm()`.

**Priority:** Medium (basic functionality not compromised)

### 2.2 Dual/Single Channel based solely on the P2 divisor - Ports.cpp:1313

```cpp
// LVDSPort::SetDisplayMode()
if (divisors.p2 == 5 || divisors.p2 == 7) {
    lvds |= LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP;  // dual
} else {
    lvds &= ~(LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP); // single
}
```

**Problem:** The 9front driver fix (by Michael Forney) demonstrated that overwriting
the BIOS's dual-channel configuration causes a black screen on some laptops. The BIOS
may configure dual-channel for hardware-specific reasons.

**Proposed fix:** Preserve the BIOS setting for dual/single channel by reading
the current register value before modifying it:
```cpp
// Preserve the BIOS dual/single channel configuration
uint32 bios_lvds = read32(_PortRegister());
lvds = (lvds & ~(LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP))
     | (bios_lvds & (LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP));
```

**Priority:** High (potential black screen)

### 2.3 Missing transcoder mapping for IBX - Ports.cpp:1303

```cpp
if (gInfo->shared_info->pch_info == INTEL_PCH_CPT) {
    lvds &= ~PORT_TRANS_SEL_MASK;
    if (fPipe->Index() == INTEL_PIPE_A)
        lvds |= PORT_TRANS_A_SEL_CPT;
    else
        lvds |= PORT_TRANS_B_SEL_CPT;
}
// For IBX: NO explicit transcoder mapping
```

**Problem:** On IBX the transcoder mapping uses the `DISPLAY_MONITOR_PIPE_B` bit in
the LVDS register (bit 30). The current code does not set it explicitly for IBX.
This works only because the BIOS value is preserved by the initial `read32()`.

**Priority:** Low (works as long as the same pipe as the BIOS is used)

### 2.4 Generation check for panel power - Ports.cpp:1223

```cpp
if (gInfo->shared_info->device_type.Generation() != 4) {
    // Power off Panel
    write32(panelControl, ...);
}
```

**Note:** Gen 5 (Ironlake) does perform the power off/on. This is correct according
to Intel documentation. It is not a bug, but is worth noting.

---

## 3. PLL ANALYSIS - Haiku vs Linux Comparison

### Ironlake LVDS Single-Channel PLL Limits (120MHz ref)

The comment in pll.cpp line 42 says: *"we use the values of N+2, M1+2 and M2+2"*

| Param | Haiku (with +2 offset) | Haiku (actual value) | Linux i915 |
|-------|----------------------|---------------------|------------|
| N min | 3 | 1 | 1 |
| N max | 5 | 3 | 3 |
| M1 min | 14 | 12 | 12 |
| M1 max | 24 | 22 | 22 |
| M2 min | 7 | 5 | 5 |
| M2 max | 11 | 9 | 9 |

**Result: the PLL values are CORRECT.** The +2 offset is documented and applied
correctly. This is not the cause of the resolution problem.

### SSC (Spread Spectrum Clock)

```cpp
// pll.cpp:476-479
if (gInfo->shared_info->pch_info == INTEL_PCH_IBX) {
    hasCK505 = false;
    wantsSSC = hasCK505;  // = false
}
```

For IBX, SSC is **already disabled**. Linux has an explicit quirk for the Sony Vaio
(device 0x0046, vendor 0x104d, subsystem 0x9076) that forces SSC off. Haiku's behavior
is already aligned.

---

## 4. GROUNDWORK FOR ITEM 7 (GPU Acceleration)

### 4.1 Current state of the infrastructure

| Component | Status | File |
|---|---|---|
| Register mapping (CPU/PCH split) | Implemented | intel_extreme.cpp |
| VBT/OpRegion parsing | Implemented | bios.cpp |
| FDI Link Training (ILK-specific) | Implemented | FlexibleDisplayInterface.cpp |
| Panel Fitter (CPU-side) | Implemented | PanelFitter.cpp |
| PLL computation (ILK limits) | Implemented | pll.cpp |
| DPLL programming (PCH DPLLs) | Implemented | Pipes.cpp |
| Basic mode-setting sequence | Implemented | Ports.cpp, Pipes.cpp |
| 2D BLT commands | Defined (header) | intel_extreme.h:1536-1547 |
| Ring Buffer | Basic infrastructure | intel_extreme.h:1522-1526 |
| Overlay (video) | Structures defined | intel_extreme.h:1754-1919 |

### 4.2 What's needed for 2D acceleration (BLT engine)

The `intel_extreme.h` header already defines the commands:
```cpp
#define XY_COMMAND_SOURCE_BLIT       0x54c00006
#define XY_COMMAND_COLOR_BLIT        0x54000004
#define XY_COMMAND_SETUP_MONO_PATTERN 0x44400007
#define XY_COMMAND_SCANLINE_BLIT     0x49400001
#define COMMAND_COLOR_BLIT           0x50000003
```

To implement 2D acceleration the following are needed:
1. **Ring Buffer Manager** - Initialization and management of the GPU's
   command ring buffer. The `hardware_status` structure is already defined.
2. **Accelerant hooks** - Implement `intel_fill_rectangle()`, `intel_blit()`,
   `intel_screen_to_screen_blit()` in the accelerant.
3. **Fence registers** - For memory tiling (already partially supported).
4. **GTT (Graphics Translation Table)** - To map graphics memory.

**Difficulty:** Medium. The infrastructure is already sketched out; what's needed
is the concrete implementation of the blit functions.

### 4.3 What's needed for 3D acceleration (OpenGL/Mesa)

For Ironlake (Gen5) the 3D pipeline is EU (Execution Units) based:
1. **Mesa Gallium driver** - Port Mesa's `i915g` driver (covers up to Gen5)
2. **GEM/GTT memory management** - GPU buffer object management
3. **Batch buffer submission** - Sending shader commands to the GPU
4. **Shader compiler** - Gen5 has a limited set of shader instructions

**Difficulty:** Very high. Requires a full Mesa port and integration with
Haiku's graphics system. This is an OS-level project, not specific to
this laptop.

### 4.4 Recommended preparatory work (feasible now)

The following groundwork can be implemented already at this stage,
improving performance and preparing the ground for item 7:

1. **Fix the IsConnected() bug** (this document, section 1)
   - Prerequisite for everything else: without a working display, nothing can be tested

2. **Ring Buffer initialization**
   - Allocate and initialize the GPU's command ring buffer
   - Write the basic submit/wait functions
   - This is the foundation for both 2D and 3D

3. **GTT setup**
   - Configure the Graphics Translation Table for Gen5
   - Map graphics memory regions
   - Prerequisite for any accelerated operation

4. **Correct watermarks for IBX**
   - Improve display stability
   - Necessary to avoid corruption when enabling asynchronous operations

5. **BLT engine test**
   - Implement a simple color fill via BLT
   - Validate that the ring buffer and GTT work
   - First step toward the 2D acceleration hooks

---

## 5. ACTION SUMMARY

### Immediate action (1366x768 resolution fix)

| # | What | File | Lines | Difficulty |
|---|------|------|-------|-----------|
| 1 | Fix `IsConnected()` EDID/VBT fallback | Ports.cpp | 1134-1180 | Low |
| 2 | Preserve BIOS dual-channel setting | Ports.cpp | 1292-1319 | Low |
| 3 | Add IBX watermark | Pipes.cpp | 640-646 | Medium |

### Groundwork for acceleration (Item 7)

| # | What | Difficulty | Dependencies |
|---|------|-----------|------------|
| 4 | Ring Buffer init | Medium | Fix #1 working |
| 5 | GTT setup Gen5 | Medium | Fix #1 working |
| 6 | BLT color fill test | Medium | #4, #5 |
| 7 | 2D accelerant hooks | Medium-High | #4, #5, #6 |
| 8 | Mesa/Gallium port | Very High | All of the above |

### Recommended tests after the fix

1. Verify that `screenmode --list` shows 1366x768
2. Verify that the display turns on at native resolution
3. Test resolution changes via Screen preferences
4. Verify FDI link training works correctly in the logs
5. Test external HDMI/VGA output (if available)

---

## 6. PATCHES IMPLEMENTED (March 18, 2026)

### Patch 1: LVDSPort::IsConnected() - Ports.cpp
**Lines modified:** 1148-1179 (32 lines added)

Added a fallback chain for PCH platforms (Ironlake+) for when the
EDID read via GMBUS/DDC fails:
1. Try EDID via GMBUS (original behavior)
2. Fall back to VESA EDID from the bootloader
3. Fall back to VBT (which contains 1366x768 from the BIOS)
4. Last resort: port enabled by BIOS (LVDS_PORT_EN bit)

The same logic already existed for Gen<=4 but was missing from the PCH path.

### Patch 2: LVDSPort::SetDisplayMode() - Ports.cpp
**Lines modified:** 1343-1351 (comment and condition changed)

Replaced the dual/single channel determination based on the P2 divisor
with preservation of the BIOS configuration. The LVDS register is
read to check whether `LVDS_CLKB_POWER_UP` was set by the BIOS.
Reference: Michael Forney's fix for the 9front igfx driver.

### Patch 3: Pipe::Enable() - Pipes.cpp
**Line modified:** 641-642

Added `INTEL_PCH_IBX` to the condition for display watermarks.
Previously only `INTEL_PCH_CPT` (Sandy Bridge PCH) received watermarks.
Now Ibex Peak (Ironlake PCH) is configured correctly as well.

### Status: TESTED AND WORKING (March 18, 2026)

The patched accelerant was successfully compiled and installed:
- **Build:** `make` with GCC 13.3 on Haiku (custom Makefile created for standalone build)
- **Installation:** `/boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant`
- **Result after reboot:** Display at **1366x768 32-bit 59.9 Hz** (native resolution)

**Evidence from the syslog after the fix:**
```
LVDS: No EDID, but force enabled as we have a VBT          ← Patch 1 active
Hardware mode will actually be 1366x768 at 59Hz             ← Native resolution!
LVDS: single channel (preserving BIOS setting)              ← Patch 2 active
Port configuration completed successfully!                  ← All OK
```

**Notes:**
- The panel has no EDID EEPROM (GMBUS DDC fails), so the VBT mode is the correct one
- The panel fitter is active (PCH_PANEL_FITTER_CONTROL = 0x80800000)
- FDI link training completed successfully (4 lanes, 18-bit color depth, 270MHz ref)
- PLL: p=28 (p1=2, p2=14), n=5, m=84 (m1=15, m2=9) — pixel clock 72MHz

---

## 7. KEY REGISTERS FOR DEBUGGING

For future diagnostics, these registers are useful to read:

```
LVDS Port Register:        0xe1180 (PCH_LVDS)
PCH Panel Status:          0xc7200
PCH Panel Control:         0xc7204
PCH DPLL A:                0xc6014
PCH DPLL B:                0xc6018
FDI TX A Control:          0x60100
FDI RX A Control:          0xf000c
FDI RX A IIR:              0xf0014
PCH DREF Control:          0xc6200
Pipe A Config:             0x70008
Transcoder A HTOTAL:       0xe0000
Transcoder A VTOTAL:       0xe000c
Panel Fitter A Control:    0x68080
Panel Fitter A Window Size: 0x68074
```
