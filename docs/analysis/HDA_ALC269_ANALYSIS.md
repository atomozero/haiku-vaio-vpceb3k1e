# HDA Driver Analysis — Realtek ALC269 on Sony Vaio VPCEB3K1E

| | |
|---|---|
| **Status** | 📄 Reference |
| **Category** | Analysis |
| **Target** | Sony Vaio VPCEB3K1E — Realtek ALC269 (Intel HDA) |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---

**Date:** March 18, 2026
**Codec:** Realtek ALC269 (Vendor 0x10ec, Product 0x0269)
**Subsystem:** Sony 0x104d:0x4600
**Revision:** 1.0.0.4
**Controller:** Intel 5 Series/3400 HD Audio (0x8086:0x3b56)
**Current Haiku quirks:** 0x0700 (global HDA_QUIRK_IVREF)

---

## 1. CURRENT STATUS

### What works
- Analog audio playback (internal speakers)
- DAC widgets 2 and 3 correctly associated with output pins
- Playback stream: 192kHz/24bit, 2 channels

### What does NOT work
- **Recording (microphone):** the ALC269 input tree NOW builds correctly
  (selector fix, commit 7bd9965). The `build input tree failed` still
  present in the syslog belongs to the **Intel HDMI** codec (function group
  with no microphone), not to the ALC269. The remaining bug was in the
  capture stream binding: both ADCs (7 and 8) were being bound to the same
  stream tag and channel 0, colliding on the HDA serial link. See section 4. FIXED.
- **Headphone jack detection:** no speaker automute when headphones are plugged in
- **HDMI audio:** `Failed to setup new audio function group` (second function group)
- **VREF pin 0x19:** configured incorrectly (VREF_80 instead of VREF_GRD)

---

## 2. CODEC WIDGET MAP FOR THE ALC269

### DAC (Digital to Analog Converter)
| NID | Type | Format | Notes |
|-----|------|---------|------|
| 2 | Audio Output | 16/20/24bit, 44-192kHz | Primary DAC (speaker) |
| 3 | Audio Output | 16/20/24bit, 44-192kHz | Secondary DAC (headphones) |
| 6 | Audio Output | Digital, 32-192kHz | SPDIF |
| 16 (0x10) | Audio Output | Digital, 32-192kHz | SPDIF/HDMI |

### ADC (Analog to Digital Converter)
| NID | Type | Format | Connections |
|-----|------|---------|-------------|
| 7 | Audio Input | 16/20/24bit, 44-96kHz | Input from NID 36 (selector 0x24) |
| 8 | Audio Input | 16/20/24bit, 44-96kHz | Input from NID 35 (selector 0x23) |

### Mixer
| NID | Inputs | Function |
|-----|----------|----------|
| 11 (0x0b) | 24, 25, 26, 27, 29 | Analog mixer (all input pins) |
| 12 (0x0c) | DAC 2, Mixer 11 | Mix for speaker |
| 13 (0x0d) | DAC 3, Mixer 11 | Mix for headphones |
| 14 (0x0e) | Mixer 12, 13 | Master selector |

### Pin Complex (physical connectors)
| NID | BIOS Config | Type | Device | Association |
|-----|------------|------|--------|-------------|
| 17 (0x11) | - | Output | N/C (disabled) | 15 |
| 18 (0x12) | Fixed | Input | **Internal mic (digital)** | 2 |
| 20 (0x14) | Fixed | Output | **Internal speakers** | 1 |
| 21 (0x15) | Jack | Output | **Headphones (3.5mm jack)** | 1, jack detect |
| 22 (0x16) | - | Output | N/C (disabled) | 15 |
| 24 (0x18) | Jack | I/O | **External mic (3.5mm jack)** | 3, jack detect |
| 25 (0x19) | - | I/O | **N/C but VREF active!** | 15 |
| 26 (0x1a) | - | I/O | N/C (disabled) | 15 |
| 27 (0x1b) | - | I/O | N/C (disabled) | 15 |
| 29 (0x1d) | Fixed | Input | Internal ATAPI (CD audio) | 0 |
| 30 (0x1e) | - | Output | Digital out | 15 |

### Input Selectors (for ADC)
| NID | Function |
|-----|----------|
| 35 (0x23) | Input Source Selector for ADC 8 |
| 36 (0x24) | Input Source Selector for ADC 7 |

---

## 3. CRITICAL BUG: VREF ON PIN 0x19 (NID 25)

### The problem
In Linux, the fundamental fix for ALL Sony Vaio machines with an ALC269 is:

```c
// sound/pci/hda/patch_realtek.c
[ALC269_FIXUP_SONY_VAIO] = {
    .type = HDA_FIXUP_VERBS,
    .v.verbs = (const struct hda_verb[]) {
        { 0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREFGRD },
        { }
    }
},
```

Pin 0x19 (NID 25) is electrically connected to the audio path even though
it is logically "disabled" (config 0x411111f0). If its VREF is set to 80%
(as Haiku does with the global IVREF quirk), a signal loss/crosstalk occurs
that causes distortion or muting of the speakers.

### Status in Haiku
The global quirk on lines 82-83 of `hda_codec.cpp`:
```cpp
{ HDA_ALL, HDA_ALL, HDA_ALL, HDA_ALL, HDA_QUIRK_IVREF, 0 },
```
Applies `HDA_QUIRK_IVREF` (50+80+100) to ALL codecs. This causes
`hda_widget_prepare_pin_ctrl()` (line 695) to set VREF_80 on pin 0x19,
which is EXACTLY the problem that Linux fixes.

### Required fix
Add a Sony quirk that removes IVREF from pin 0x19, or (simpler) add support
for codec-specific initialization verbs.

---

## 4. BUG: MICROPHONE NOT WORKING (ADC collision on the capture stream)

### Diagnosis update (from real syslog)
The ALC269 input tree builds correctly after the selector fix:
```
build input tree
  look at input widget 7 -> selector 36 -> input pin 24 (external mic)
  look at input widget 8 -> selector 35 -> input pin 18 (internal mic)
build tree!
```
The remaining `build input tree failed` in the syslog belongs to the
Intel HDMI codec (function group with no inputs), not to the ALC269.

### The real bug
The record stream binds BOTH ADCs (7 and 8) to the same stream tag and
channel 0 (`hda_stream_start`, `channelNum += 2` commented out). Two input
converters on the same (tag, channel) collide on the HDA serial link ->
corrupted/silent capture. In addition, ADC 7 (used first) only routes the
external mic (selector 36 does not contain pin 18): the internal mic was
never captured.

### Fix applied
In `hda_audio_group_get_widgets`, for record streams a SINGLE converter is
now used, preferring the ADC whose active input path terminates on a Fixed
(internal) mic. This way the internal mic records by default; the mixer MUX
(selector 35: inputs 24 25 26 27 29 18 11) allows switching to the jack.

### The problem (historical)
The syslog showed:
```
hda: build input tree
hda: build input tree failed
```

### Path analysis
The correct input path would be:
```
Pin 18 (internal mic) --> Selector 36 (0x24) --> ADC 7
Pin 24 (external mic) --> Selector 36 (0x24) --> ADC 7
```

The function `hda_audio_group_build_input_tree()` (line 1081) looks for:
1. Widgets of type `WT_AUDIO_INPUT` (ADC 7, 8)
2. For each ADC, it looks at its inputs: ADC 7 has input `36`
3. It calls `hda_widget_find_input_path()` on widget 36

Widget 36 (NID 0x24) is a selector that connects the various mic pins to
the ADC. But in the syslog dump, widgets 35 and 36 are NOT printed (the
dump stops at 34). Possible causes:

1. **Unrecognized widget type:** if widget 36 is parsed as
   "Vendor defined" (unknown type), `hda_widget_find_input_path()` does not
   traverse it (the `switch` on type falls through to the default case and
   returns `false`)

2. **Widget out of range:** if `widget_count` does not include widgets
   35-36, `hda_audio_group_get_widget()` returns NULL

3. **Pin already in use:** if pin 24 (external mic) is already marked as
   `WIDGET_FLAG_OUTPUT_PATH` (because it has I/O capability), the check at
   line 972-973 excludes it from the input path

### Required fix
Check the parsing of widgets 35-36. If they are mis-parsed selectors, force
the correct type. Otherwise, verify that the I/O pin flags are not blocking
the input path.

---

## 5. BUG: HDMI AUDIO FUNCTION GROUP FAILING

### The problem
```
hda: hda_audio_group_get_widgets failed for playback stream
hda: hda_audio_group_get_widgets failed for record stream
hda: Failed to setup new audio function group (No such device)!
```

### Analysis
The Intel 5 Series controller has TWO function groups:
1. **FG 1:** Realtek ALC269 analog codec (works partially)
2. **FG 2:** Intel HDMI digital codec (fails)

The second function group contains the HDMI/DP widgets (NID 4, 5, 6 in the
second FG), which are digital pin complexes. The tree builder cannot build
either the playback or record path because:
- The widgets are "Digital" with Content Protection (HDCP)
- The tree builder looks for standard audio widgets (DAC/mixer) that do not
  exist in the HDMI topology (the HDMI codec uses a direct pin→output path)

### Priority
Low. HDMI audio is not essential. The error message does not affect analog
audio operation. Linux handles the Intel HDMI codec with a separate driver
(`snd-hda-codec-hdmi`).

---

## 6. HEADPHONE AUTOMUTE - ALREADY IMPLEMENTED

### In-depth analysis
Contrary to the initial analysis, the Haiku HDA driver **already has**
automute implemented:

- `hda_audio_group_switch_init()` (line 1212): enables unsolicited responses
  on all pin complexes with presence detect
- `hda_codec_switch_handler()` (line 1282): kernel thread that receives
  unsolicited events via semaphore
- `hda_audio_group_check_sense()` (line 1233): reads the presence detect of
  the HP pin (PIN_DEV_HEAD_PHONE_OUT) and mutes/unmutes the speaker pins

Pin 21 (HP, config 0x0221101f, device=PIN_DEV_HEAD_PHONE_OUT) and pin 20
(Speaker, config 0x90170110, device=PIN_DEV_SPEAKER) have the correct
device types to be handled by the existing automute logic.

### Status: NO FIX NEEDED
Automute should work correctly once the VREF and input tree fixes are
applied. To be verified with a practical test.

---

## 7. PIN CONFIG SUMMARY: BIOS vs LINUX

| Pin NID (Dec) | Pin NID (Hex) | BIOS Config | Type | Linux Override |
|-------|-------|-------------|------|---------------|
| 17 | 0x11 | 0x411111f0 | Disabled | None |
| 18 | 0x12 | 0x90a60920 | Internal mic | None |
| 20 | 0x14 | 0x90170110 | Speaker | None |
| 21 | 0x15 | 0x0221101f | Headphones | None |
| 24 | 0x18 | 0x02a15830 | External mic | None |
| 25 | 0x19 | 0x411111f0 | Disabled | **VREF_GRD** |
| 26 | 0x1a | 0x411111f0 | Disabled | None |
| 27 | 0x1b | 0x411111f0 | Disabled | None |
| 29 | 0x1d | 0x4015812d | Internal BIOS | None |
| 30 | 0x1e | 0x411111f0 | Disabled | None |

Linux does NOT override the BIOS pin configs - it only sends the VREF_GRD
verb to pin 0x19. The VPCEB3K1E's BIOS pin configs are correct.

---

## 8. FIX PLAN

### Fix 1: Sony Vaio VREF (Critical) - Difficulty: Low
Add to the quirk table `kCodecQuirks[]` in `hda_codec.cpp`:
```cpp
{ 0x104d, HDA_ALL, REALTEK_VENDORID, 0x0269,
    0, HDA_QUIRK_IVREF },  // Sony Vaio ALC269: remove global IVREF
```
And after applying the quirk, send the specific verb:
```cpp
// SET_PIN_WIDGET_CONTROL(0x19, PIN_VREFGRD)
// NID 25 (0x19), verb 0x707, value 0x01
```

### Fix 2: Input tree (Medium) - Difficulty: Low - IMPLEMENTED
Forced widget type WT_AUDIO_SELECTOR for NID 0x23 and 0x24 of the ALC269.
These widgets are reported by the hardware as "vendor defined" but function
as input selectors for mic → ADC routing.

### Fix 3: Headphone automute - NOT NEEDED
Automute is already implemented in the Haiku HDA driver via:
- `hda_audio_group_switch_init()` to enable unsolicited responses
- `hda_codec_switch_handler()` thread to receive events
- `hda_audio_group_check_sense()` to toggle speaker/HP mute

### Fix 4: HDMI audio (Optional) - Difficulty: High
Handle the second Intel HDMI function group. Requires dedicated support
for the HDMI codec (direct path, content protection, ELD). Not a priority
for this laptop.

---

## 9. QUIRK SYSTEM COMPARISON: HAIKU vs LINUX

| Feature | Haiku | Linux |
|---|---|---|
| Quirk type | Global GPIO + VREF | Verb, pin config, custom functions |
| Granularity | Per codec/vendor | Per specific subsystem ID |
| Pin config override | No | Yes (full) |
| Init verbs | No | Yes (verb array) |
| Jack detection | Yes (basic) | Yes (unsolicited events) |
| Automute | Yes (basic) | Yes (generic + per-codec) |
| Input switching | No | Yes (automatic) |

Haiku's quirk system is simpler than Linux's but functional. For the
ALC269, the applied fixes (VREF + selector type) are sufficient for basic
operation. Advanced fixes would require:
1. Per-pin VREF control (not global) - WORKED AROUND with a dedicated quirk
2. Automatic input source switching (internal/external mic)

---

## 10. KEY REGISTERS AND VERBS FOR DEBUGGING

```
# Read pin config (GET_CONFIG_DEFAULT)
Verb: MAKE_VERB(codec_addr, NID, 0xf1c, 0)  // byte 0
Verb: MAKE_VERB(codec_addr, NID, 0xf1d, 0)  // byte 1
Verb: MAKE_VERB(codec_addr, NID, 0xf1e, 0)  // byte 2
Verb: MAKE_VERB(codec_addr, NID, 0xf1f, 0)  // byte 3

# Read pin widget control
Verb: MAKE_VERB(codec_addr, NID, 0xf07, 0)

# Set VREF_GRD on pin 0x19
Verb: MAKE_VERB(codec_addr, 0x19, 0x707, 0x01)

# Read presence detect
Verb: MAKE_VERB(codec_addr, NID, 0xf09, 0)  // bit 31 = jack present

# Codec addresses
ALC269 analog: codec_addr = 0
Intel HDMI:    codec_addr = 3 (typically)
```

---

## 11. HAIKU HDA DRIVER SOURCE FILES

Downloaded into `hda/`:
- `hda_codec.cpp` - Codec parsing, widget tree, quirk table (MAIN FILE)
- `hda_codec_defs.h` - Verb definitions, widget types, pin capabilities
- `driver.h` - Driver data structures
- `hda_controller.cpp` - Controller init, DMA, interrupt
- `hda_multi_audio.cpp` - Multi-channel mixer/audio interface

Repository: `https://github.com/haiku/haiku/tree/master/src/add-ons/kernel/drivers/audio/hda`
