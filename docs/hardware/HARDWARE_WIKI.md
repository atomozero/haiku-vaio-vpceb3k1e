# Intel Gen5-Gen8 Hardware Wiki

| | |
|---|---|
| **Status** | 📄 Reference |
| **Category** | Hardware reference |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---


Empirical knowledge base collected during development of the Haiku driver.
This information is NOT in the Intel PRMs — it was discovered
through testing on real hardware (Sony Vaio VPCEB3K1E, Gen5 Ironlake).

---

## 1. Ring Buffer Rules

### 1.1 NEVER reset the ring after boot
The kernel driver initializes the ring at boot (`setup_ring_buffer`).
Disabling + re-enabling the ring (write 0 to RING_CTL, then re-enable)
kills the Command Streamer **permanently**. HEAD stays at 0x0
and never advances again. Only a reboot restores the CS.

**Workaround**: Use `gpu_ring_init()`, which reads the hardware TAIL
and syncs the position without touching RING_CTL.

### 1.2 MMIO writes are ignored from userspace
All MMIO writes from userspace (via `clone_area` or `/dev/misc/poke`
BAR0 0xF0000000) are **silently ignored**. The kernel maps the
registers with `B_KERNEL_WRITE_AREA` without `B_WRITE_AREA`.

Verified: wrote 0xDEADBEEF to a scratch register, read back 0x0.
RING_BUFFER_HEAD stuck at 0x5134, not resettable.

**Workaround**: All MMIO writes must go through the
kernel ioctl (`INTEL_RING_WRITE_TAIL`, `INTEL_RING_INIT_3D`).
Ring memory (graphics_memory) IS writable from userspace.

### 1.3 MI_LOAD_REGISTER_IMM (LRI) does not work on Gen5
LRI (opcode 0x22) in the ring hangs the CS after 2 DW on Ironlake.
Masked registers (MI_MODE, _3D_CHICKEN2, CACHE_MODE_0) get corrupted
by raw writes via LRI.

**Workaround**: Use the `INTEL_RING_INIT_3D` kernel ioctl — the kernel
writes the workaround registers via direct MMIO.

### 1.4 Ring wrap-around
The ring is circular (typically 64KB). When `pos + needed > size`,
you must:
1. Wait for HEAD to pass our position (GPU has consumed it)
2. Fill with MI_NOOP up to the end of the ring
3. Reset pos to 0
4. Do NOT use RING_RESET!

### 1.5 Coexistence with app_server
The ring is shared between the accelerant (app_server) and userspace tools/plugins.
Two strategies:
- **Lock-based**: `acquire_lock(&ring->lock)` — serializes access
  but requires tracking a shared position
- **Stateless** (preferred): `gpu_ring` maintains a local position,
  syncing with the HW TAIL. No lock needed because writes
  to ring memory are atomic at the DWORD level.

---

## 2. Media Pipeline (EU Kernel Dispatch)

### 2.1 10-command preamble
Mandatory sequence before MEDIA_OBJECT:
```
1. MI_FLUSH
2. 3DSTATE_DEPTH_BUFFER (null)
3. PIPELINE_SELECT MEDIA
4. STATE_BASE_ADDRESS (8 DW Ironlake, 10 DW SNB+)
5. URB_FENCE (VFE_REALLOC + CS_REALLOC)
6. MEDIA_STATE_POINTERS (VFE state + IDRT)
7. CS_URB_STATE (entry_size=16, num_entries=1)
8. CONSTANT_BUFFER (CURBE data pointer + size)
9. N x MEDIA_OBJECT (inline data per thread)
10. MI_FLUSH (final)
```

### 2.2 Max 48 EU threads per submission
VFE num_urb_entries cannot exceed 48 on Gen5. The 49th dispatch
causes a permanent IS stall (Instruction Stream stall).

### 2.3 Single submission mandatory
After MEDIA_OBJECT + MI_FLUSH, a second media submission in the ring
causes an IS stall. Everything (preamble + N dispatches + flush) must go
in a single gpu_ring_advance().

### 2.4 Completion tracking via marker
Do NOT use HEAD polling for completion (fragile with a shared ring).
Use MI_STORE_DATA_IMM to write a tag into a GTT buffer:
```
DW0: 0x10400002 (MI_STORE_DATA_IMM, GGTT, 4 DW)
DW1: 0
DW2: marker_bo GTT offset
DW3: tag value (e.g. 0xBEEF0042)
```
The CPU reads the marker address until it finds the tag.

### 2.5 CURBE layout for IDCT
- curbe_read_len = 20 (20 GRF of 32 bytes = 640 bytes)
- g1-g4: padding (128 bytes)
- g5-g20: IDCT cosine table (512 bytes, kIdctTableGpu)
- CONSTANT_BUFFER: 10 units of 64 bytes = 640 bytes

### 2.6 Inline data position
With `const_urb_entry_read_len = N`, MEDIA_OBJECT's inline data
arrives at the EU kernel at `g(1 + N)`, NOT g1. CURBE occupies g1..gN.

---

## 3. EU Kernel Assembly (gen4asm)

### 3.1 Subregister .N is in BYTES
Without the `-a` flag, gen4asm interprets `.N` as a byte offset:
```
g90.0<1>UW   → byte 0  = UW element 0
g90.8<1>UW   → byte 8  = UW element 4 (NOT element 8!)
g90.16<1>UW  → byte 16 = UW element 8
```

### 3.2 {compr} widening UB->UW is broken
`mov (16) g70<1>UW g1.0<16,16,1>UB {compr}` writes ONLY bytes 0-15
of g70. Bytes 16-31 remain stale (data from the previous thread).

**Fix**: 2 × explicit SIMD8:
```asm
mov (8) g70.0<1>UW  g1.0<8,8,1>UB  { align1 };
mov (8) g70.16<1>UW g1.8<8,8,1>UB  { align1 };
```

### 3.3 {compr} on D destination DOES increment the register
`mul (16) g80<1>D ... {compr}` writes correctly: 8 D to g80 +
8 D to g81. {compr} increments because D fills the entire GRF.

### 3.4 Send: header from GRF, payload from MRF
On Gen4/5, send reads the header from the source GRF register and the payload
from MRF (m1, m2, ...). The source GRF is NOT used for payload.

### 3.5 MEDIA_BLOCK_READ: msg_type=2
The PRM DevILK shows msg_type=4 (3-bit), but gen4asm uses 2-bit
encoding where msg_type=2 is correct. msg_type=4 fails silently.

### 3.6 OWord Block R/W: SURFTYPE_BUFFER mandatory
SURFTYPE_2D causes a silent drop for OWord Block operations.
Always use SURFTYPE_BUFFER (type 4).

---

## 4. MPEG-2 Parser

### 4.1 DC level shift in dc_pred
The initial dc_pred value `2^(7 + intra_dc_precision)` = 128 for
dc_prec=0 already includes the +128 level shift. Do NOT add +128
after IDCT. The EU kernel must NOT have the +128 add.

### 4.2 EOB after a full block (64 coefficients)
When all 64 coefficients are coded (idx reaches 63),
the bitstream STILL contains an EOB marker (`10`, 2 bits).
It MUST be consumed. Otherwise: a 2-bit drift per block,
corrupting the entire slice. The #1 cause of "75% coverage".

### 4.3 Error recovery: skip+continue, NEVER resync
When `mpeg2_decode_intra_macroblock()` fails, skip the MB
and continue (`mbx++; continue;`). Do NOT call
`mpeg2_resync_to_next_slice()` — it loses all remaining MBs.
Fix: coverage from 76-82% to 100%.

### 4.4 Table B-14 12-bit VLC codes
The 12-bit VLC codes in Table B-14 are NOT sequential.
The order is by statistical frequency. Use the correct mapping
from ISO 13818-2 or from ffmpeg's `ff_mpeg1_vlc_table`.

### 4.5 Full CBP Table B-9
All 64 entries are needed. An incomplete table causes silent
corruption in P-frame inter MBs.

### 4.6 Per-slice and per-MB quantiser_scale
Must be computed from `q_scale_code` using
`mpeg2_compute_quantiser_scale()`. Updated for every slice header
and for every MB with the quant flag.

---

## 5. Display Engine (Gen5 Ironlake)

### 5.1 LVDS EDID fallback
On PCH (Ironlake+), GMBUS DDC fails on many LVDS panels.
Fallback chain:
1. GMBUS EDID (standard)
2. VESA EDID from the bootloader
3. VBT data from the BIOS
4. Port enabled by the BIOS (LVDS_PORT_EN bit)

### 5.2 Dual/single channel LVDS
Do NOT derive from the PLL's P2. Preserve the BIOS configuration by reading
the `LVDS_CLKB_POWER_UP` bit from the current LVDS register.

### 5.3 Panel fitter
Disable when the requested resolution == the panel's native resolution.
1366 pixels (not divisible) + 1:1 panel fitter = artifacts.

### 5.4 IBX watermark
The original code configures watermarks only for CPT (Sandy Bridge).
Add `INTEL_PCH_IBX` to the condition.

---

## 6. Mesa / OpenGL Integration

### 6.1 glapi dispatch bridge
Mesa 25 uses `_mesa_glapi`, the `_glapi` system. Find libglapi.so
already loaded via `get_next_image_info`, NOT `load_add_on`.

### 6.2 ISL format table corruption
The ISL table's C99 sparse array gets corrupted by the linker.
170/918 valid entries. The B8G8R8X8_UNORM entry has bpb=0 → crash.
Fix: binary patch or compile ISL with -O0.

### 6.3 SET_TILING via intel_ioctl
crocus_bufmgr.c uses a raw `ioctl()` that bypasses the DRM shim.
Change it to `intel_ioctl()` to go through the translation layer.

### 6.4 Batch #3 hang (OPEN)
EXECBUF2 #1 (state setup): OK. #2 (glClear): OK.
#3 (readback): HANGS on 3DSTATE_GLOBAL_DEPTH_OFFSET_CLAMP.
Difference: DW2 = 0x00c7012b (batch #2) vs 0x00000000 (batch #3).
A DW-by-DW debug of batch #3 vs #2 is needed.

---

## 7. Kernel Driver

### 7.1 Ioctl for TAIL write
`INTEL_RING_WRITE_TAIL`: the only way to write TAIL from userspace.
The kernel receives tail_value and writes the MMIO register.

### 7.2 Ioctl for 3D workaround
`INTEL_RING_INIT_3D`: writes MI_MODE (0x209C), _3D_CHICKEN2 (0x208C),
CACHE_MODE_0 (0x2120) via direct MMIO in the kernel.

### 7.3 Building the kernel driver
ABI differences between the build tree and the runtime kernel (e.g. `_mutex_unlock`
vs `mutex_unlock`) require a mutex compatibility shim. Compile
with `-fPIC`. Blacklist the system driver via
`/boot/system/settings/packages`.

### 7.4 RING_RESET — DO NOT USE
`INTEL_RING_RESET` works ONCE after boot. The second
time, the CS dies permanently. The kernel calls it at boot
in `setup_ring_buffer()` — from that point on, never again.
