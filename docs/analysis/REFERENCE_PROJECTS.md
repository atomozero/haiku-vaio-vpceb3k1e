# Reference projects beyond Mesa & clean-room feasibility

| | |
|---|---|
| **Status** | 📄 Reference |
| **Category** | Analysis |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---


Two questions answered here:
1. What other codebases (besides Mesa i965) can we learn from for Gen5 / Ironlake?
2. Can we ship a driver without pulling in Linux-derived code or runtime blobs?

---

## 1. Reference projects, ranked by usefulness

### Tier 1 — directly relevant

#### **libva-intel-driver** (a.k.a. `intel-vaapi-driver`, `i965_drv_video.so`)
- Repos: `github.com/intel/intel-vaapi-driver` and the historical `github.com/gbeauchesne/libva-intel-driver`.
- **Why it matters**: this is the VA-API backend that implements H.264 / VC-1 / MPEG-2 decode on Intel integrated GPUs, **explicitly including Ironlake**. It contains real production code for `shaders/h264/mc/*.gen5`, `*.asm`, and full `MEDIA_OBJECT` dispatch on Gen5.
- **For us**: this is arguably **more valuable than Mesa i965** for the LLM goal because video decode on Gen5 uses exactly the pipeline we need — the media pipeline with `MEDIA_VFE_STATE` / `MEDIA_CURBE_LOAD` / `MEDIA_OBJECT`. It's the only real-world codebase that shows how to launch compute threads on Ironlake EUs outside of rasterization.
- **Shader format**: the driver ships **EU assembly** (`.asm`) for each kernel, assembled at build time by a small tool (`intel-gen4asm`, also in the tree). This means we get both:
  - working Gen5 kernels in human-readable form to study
  - a minimal open-source Gen4/Gen5 EU assembler to bootstrap from
- License: MIT.
- **Action**: download this next. It's probably the single most useful codebase we haven't grabbed yet.

#### **Mesa crocus** (modern replacement for i965 classic)
- Path: `src/gallium/drivers/crocus/` in mainline Mesa.
- **What it is**: a Gallium3D driver covering **Gen4 through Gen7** (i965, GM45, **Ironlake**, Sandybridge, Ivybridge, Haswell). Merged into Mesa 21.2 (2021). Still maintained, still in main branch. Authored by Ilia Mirkin and Dave Airlie as a fork of `iris` (the modern Gen8+ driver) with old relocation-based batchbuffer management added back in.
- **Why it matters**: same hardware coverage as i965 classic but **cleaner modern code**. Uses the NIR compiler infrastructure, genxml for state packing, and a much more consistent style than the 20-year-old i965 tree. Bugs in old i965 that were never fixed are often fixed here.
- **For us**: second-best reference after libva-intel-driver, and **strictly better than i965 classic for bit-level state layout** because it uses genxml packers (fewer hand-coded bitfields = fewer bugs we could inherit).
- License: MIT.
- **Action**: grab `src/gallium/drivers/crocus/` alongside the existing `mesa_refs/`.

#### **intel-gpu-tools (IGT)**
- Repo: `gitlab.freedesktop.org/drm/igt-gpu-tools`.
- **What it is**: Intel's in-house test suite for i915. Contains hundreds of tiny C programs that do **one specific low-level thing each** — "send this batch buffer, expect this register value". The `lib/` directory has `intel_batchbuffer.c`, `intel_chipset.c`, helpers that encapsulate the minimum viable sequence to talk to the hardware.
- **For us**: when we're stuck on "how do I make the ring accept this one command", IGT almost certainly has a 50-line test that does exactly that. It's **the best debugging reference available** — much more focused than reading kernel code.
- Coverage of Gen5: Ironlake is supported in the older commits (pre-2018); newer versions have dropped some Gen5-specific tests but `lib/` is still fine.
- License: MIT.
- **Action**: grab `lib/intel_batchbuffer.*`, `lib/intel_reg.h`, and a handful of `tests/gem_*.c` that exercise render/media.

### Tier 2 — partially relevant

#### **xf86-video-intel** (DDX — the X server driver)
- Repo: `gitlab.freedesktop.org/xorg/driver/xf86-video-intel`.
- **What it is**: the historical X.Org 2D acceleration driver for Intel GPUs. Contains **UXA** and **SNA** acceleration backends. Covers Ironlake with real, production-hardened BLT paths.
- **For us**: this is the **gold reference for the BLT engine** (useful for the parallel 2D accel track). Specifically `src/sna/gen5_render.c` has Gen5-specific render paths, and the UXA code has straightforward `XY_COLOR_BLT` / `XY_SRC_COPY_BLT` emitters we can learn from.
- License: MIT.
- **Action**: grab the Gen5-relevant files if we keep the BLT track alive.

#### **Linux i915 (older kernels)**
- We already have v4.19 excerpts. Not worth pulling more.
- License: **GPL-2.0** — we must treat this as "read for understanding only, never copy".
- The key insight: i915 is a kernel driver managing memory, contexts, interrupts, scheduling. The *programming of the GPU pipeline itself* happens in userspace (Mesa, libva, etc.). So for our accelerant work, Mesa/libva are actually closer to what we're writing than i915 is.

#### **Beignet**
- Repo: archived, `github.com/intel/beignet` / Freedesktop cgit.
- **What it is**: Intel's open-source OpenCL runtime for Gen7+ (Ivybridge and newer). **Does not support Gen5.**
- **For us**: Gen5 unsupported, so not a direct reference, but the *architecture* of Beignet (how it maps OpenCL concepts to MEDIA_OBJECT-style dispatch, kernel compilation, constant buffers, event/sync model) is the clearest example of "GPGPU on Intel integrated graphics using the media pipeline". Worth reading for structural ideas even though the code won't run on our chip.
- License: LGPL-2.1.
- **Action**: optional, read-only reference for architecture decisions.

### Tier 3 — inspiration / cross-validation

- **xen-vgt / gvt-g** — virtualization layer, not useful for bring-up.
- **NetBSD/OpenBSD/FreeBSD DRM forks** — ports of Linux i915, same code, no additional value.
- **ChromeOS mini-i915** — same, stripped-down Linux.
- **Intel IGA (Intel Graphics Assembler)** — Gen9+ only, ISA too different from Gen5.
- **Intel IGC (Graphics Compiler)** — Gen8+ only.
- **POCL** — portable OpenCL, no Gen5 backend, pure software.
- **Haiku's `radeon_hd` and `nvidia` accelerants** — structural reference for how Haiku's `intel_extreme` neighbors organize their code. Worth a look to check for GPGPU patterns, though I'd expect neither to implement compute. Pure architectural reference, not code to copy.

### Hidden gems worth mentioning

- **`gen4asm` / `intel-gen4asm`** — shipped inside libva-intel-driver. This is a **standalone Gen4/Gen5 EU assembler**, ~3000 lines of flex/bison + C. If we want to write our own kernels from scratch in readable form instead of hand-emitting binary, this is the tool. **Potentially a huge productivity win** and the single most important thing to find outside of Mesa.
- **`libdrm_intel` (`libdrm/intel/`)** — userspace helpers for batch buffer relocation, GEM ioctl wrappers. GPL'd (LGPL actually), but the patterns it encodes are useful to understand.
- **`cairo-drm`** — historical, long-dead experimental 2D backend talking directly to Intel GPUs from Cairo. Contains some elegant minimal examples of "here is the smallest possible working BLT".

---

## 2. Can we write a driver without Linux blobs?

Short answer: **yes, cleanly, with no runtime dependency on any Linux code or firmware.** Here's the detailed breakdown.

### 2.1 Runtime firmware — Gen5 is free

This is the simplest piece. Modern Intel GPUs (Gen9+ and especially Gen12+) need multiple signed firmware blobs at runtime: **DMC** (display microcontroller), **GuC** (graphics microcontroller scheduler), **HuC** (HEVC/media offload), **CSR**. Without them the GPU is hobbled or inoperative.

**Ironlake predates all of this.** Gen5 has:
- No DMC — display is driven directly by MMIO writes (which is exactly what our current accelerant does).
- No GuC — scheduling is host-side, the kernel/userspace builds batch buffers and writes them into the ring directly.
- No HuC — media decode is done by the same EU array via MEDIA_OBJECT kernels (which ship as *source* in libva-intel-driver, not as signed blobs).
- No CSR / no PMU firmware.

So a Gen5 driver is **firmware-free**. There is literally nothing signed by Intel that needs to load at runtime. This is a real advantage of targeting 2010-era hardware: we get full hardware access with zero vendor-blob baggage. Nothing to redistribute, nothing to license, nothing to audit.

The only "blobs" anywhere in the picture are **our own compiled EU kernels** (see §2.3), which we generate from our own source.

### 2.2 Source code licenses — what we can legally copy vs read

| Project | License | Can we copy code? | Can we read for understanding? |
|---|---|---|---|
| Intel PRMs | Freely redistributable, Intel Open Source docs | n/a (documentation) | ✓ — yes, this is the authoritative reference |
| **Mesa (i965, crocus, genxml)** | **MIT** | **✓ yes, with attribution** | ✓ |
| **libva-intel-driver** | **MIT** | **✓ yes, with attribution** | ✓ |
| **intel-gpu-tools (lib/)** | **MIT** | **✓ yes, with attribution** | ✓ |
| **xf86-video-intel** | **MIT** | **✓ yes, with attribution** | ✓ |
| `intel-gen4asm` (in libva-intel-driver) | MIT | ✓ yes | ✓ |
| Linux i915 | **GPL-2.0** | ✗ no — incompatible with Haiku's MIT base | read-only for understanding |
| libdrm | LGPL-2.1 | ⚠ linking caveats, best avoided | ✓ read |
| Beignet | LGPL-2.1 | ⚠ linking caveats, best avoided | ✓ read |

**Conclusion**: **every codebase in Tier 1 and Tier 2 of §1 is MIT-licensed.** Mesa, libva-intel-driver, IGT, xf86-video-intel, gen4asm — all MIT. We can copy code from any of them into our accelerant, with proper attribution, and ship it under Haiku's MIT-compatible licensing without any legal or philosophical conflict.

**We never need to touch Linux kernel code.** Everything about *how to program the GPU* lives in the userspace MIT projects. Linux i915 only handles *managing* the GPU (contexts, scheduling, memory). On Haiku, **that management layer is what we're writing ourselves** — there's nothing to import from i915 anyway. We'd read it for understanding the occasional obscure quirk, not copy.

This is a cleaner starting position than many ports. There is no GPL contamination risk because the authoritative references are already MIT.

### 2.3 Precompiled EU kernel blobs — can we avoid them?

The earlier feasibility doc suggested, as a bootstrap tactic, precompiling compute kernels on Linux and embedding the binary. That's pragmatic but **the user is right to push back on it** — embedded binary blobs from Linux builds are exactly the kind of thing that makes a driver feel "tainted" even when it's legally clean. We can do better.

Three honest options:

**Option A — Ship EU assembly source, assemble at build time.**
Port `intel-gen4asm` (from libva-intel-driver, MIT, ~3000 lines) into our tree. Write our compute kernels in `.asm` files. Build system runs the assembler at compile time and generates `.h` files with the resulting binary. **No precompiled blobs anywhere** — the binary is regenerated every build from human-readable source that we (or libva authors) wrote.

This is how libva-intel-driver itself ships Gen5 kernels. It's the canonical clean approach. **Strongly recommended.**

**Option B — Hand-write EU binary in C.**
Feasible for the first "hello world" kernel (a few dozen instructions). Painful for anything real. Worth doing once, to prove the pipeline works with zero toolchain dependency, then switch to A.

**Option C — Runtime code generation.**
Build a tiny EU instruction emitter in C (along the lines of Mesa's `brw_eu_emit.c` but much smaller), generate kernels at driver-load time. Most flexible, lets us specialize kernels per-model. Higher effort, similar license story to A (we'd adapt the Mesa emitter, MIT). Worth doing **after** A is working, if kernel specialization turns out to matter.

**Recommended path**: A for bring-up and production, C as a later optimization if needed. **We never ship a precompiled blob.** Every byte of GPU code in the system is generated from source we can read and modify.

### 2.4 What about the CPU-side runtime?

For the LLM runtime itself (tokenizer, graph walker, weight loader) there's no Linux dependency to worry about — ggml, llama.cpp, candle, and similar projects are MIT/Apache-licensed and portable C/C++/Rust. The work is porting effort, not license friction.

---

## 3. Updated recommendation

Refine the feasibility doc's roadmap with these findings:

1. **Download libva-intel-driver** as the primary compute-path reference — it's closer to what we're building than Mesa i965.
2. **Download Mesa crocus** as the modern state-packing reference — cleaner bitfield handling than i965 classic.
3. **Download IGT `lib/`** as the debugging / minimum-viable-batch reference.
4. **Port `intel-gen4asm`** into the tree early. Having an in-tree EU assembler removes the biggest unknown and eliminates any need for Linux-generated blobs.
5. **Keep xf86-video-intel SNA** bookmarked for the BLT-engine track if we continue 2D accel work in parallel.

Net result: **a fully MIT, firmware-free, blob-free driver stack**, assembled from MIT references and our own code, authoritative hardware specs from Intel, and everything buildable in-tree on Haiku.

---

## 4. Concrete next step (if agreed)

Pull these into `gen5_docs/`:

```
gen5_docs/
├── libva_intel/          ← i965_drv_video src tree (Gen5 media kernels + gen4asm)
├── mesa_crocus/          ← src/gallium/drivers/crocus/ (Gen5-relevant files)
├── igt/                  ← lib/intel_batchbuffer.*, lib/intel_reg.h, sample tests
└── xf86_video_intel/     ← src/sna/gen5_*.c (optional, for BLT track)
```

All MIT, all ~few MB total, all things we'll actually read repeatedly.
