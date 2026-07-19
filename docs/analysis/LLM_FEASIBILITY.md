# Can a small LLM run on Intel Ironlake Gen5 with hardware acceleration?

| | |
|---|---|
| **Status** | 📄 Reference |
| **Category** | Analysis |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---


**Target hardware**: Intel HD Graphics, device 0x0046 (Arrandale mobile), in the Sony VAIO VPCEB3K1E.
**Platform**: Haiku R1~beta5, custom `intel_extreme` accelerant.
**Goal**: run a small LLM (0.5B–3B params) with the GPU doing the matmul-heavy work, not the CPU.

This document is an honest feasibility assessment. The short version is: **it is technically possible, but the right pipeline is the media/GPGPU pipeline, not the 3D render pipeline we're currently debugging.** The rest of this document explains why and what that implies for the project direction.

---

## 1. The hardware budget

### 1.1 Compute
- 12 Execution Units × 4 threads/EU = **48 concurrent hardware threads**.
- Each EU has two 4-wide FP SIMD units → effective 8-wide SIMD per EU per cycle (SIMD16 dispatch is also supported but internally pipelined).
- Peak theoretical FP32: roughly 12 EU × 8 lanes × 2 ops (FMA) × ~0.5–0.9 GHz = **~80–170 GFLOPS FP32**.
- No FP16, no BF16, no INT8 native — this is Gen5. Quantization has to happen in software (e.g. INT8 weights dequantized to FP32 in the kernel), which eats some of the budget.
- No dedicated matrix units (no XMX, no Tensor Cores — those arrive from Gen12 / Xe).

For context: a modern CPU like the i3 M370 in this machine peaks around 20 GFLOPS FP32 (2 cores × 4-wide SSE × 2 × 2.4 GHz). So the GPU offers **roughly a 4–8× compute speedup over the CPU** if we can actually feed it. That is the prize.

### 1.2 Memory
This is where it gets harder.
- **No VRAM**. Everything lives in system DDR3-1066 dual-channel RAM, shared with the CPU. Peak platform bandwidth: ~8.5 GB/s.
- The GPU reaches memory through the **GTT aperture** (256 MB on this machine), with 128 MB pre-reserved as stolen memory.
- **L3 cache**: 256 KB shared across all EUs. Tiny compared to modern chips.
- No unified memory model like HSA — GPU can't snoop CPU caches cleanly on Gen5. Every GPU read of data touched by the CPU needs explicit flush/coherency via PIPE_CONTROL.

**Implication**: the workload must be **compute-bound, not memory-bound**. Matmul is compute-bound at large enough tile sizes (N³ ops vs N² memory). LLM inference at batch=1 is normally memory-bound on most hardware, but on Gen5 the CPU/GPU bandwidth gap is zero (same DDR3), so moving the matmul to the GPU mainly wins through the wider SIMD and higher thread count, not through higher bandwidth.

### 1.3 Model size budget
With 128 MB stolen + ~128 MB of additional GTT-mappable memory available, we can realistically put **~200 MB of weights** in GPU-addressable memory before fighting the kernel for pages. That's enough for:
- A **0.5B–1B parameter model in INT4** (~250–500 MB raw, tight but feasible with streaming)
- A **0.3B parameter model in INT8** (~300 MB, borderline)
- A **0.1B parameter model in FP16** (~200 MB, comfortable — but no native FP16 means we burn FP32 math)

Realistic sweet spot: **TinyLlama-class (~1.1B) quantized to INT4, or a distilled 300–500M model**. GPT-2 small (124M) would run comfortably. Anything beyond 1.5B is probably a fight against memory.

### 1.4 Expected tokens/second
Back-of-envelope for a 1B INT4 model at batch=1, sequence length 512:
- Per token: ~2 × params FLOPs = ~2 GFLOP
- At effective 50 GFLOPS (half of theoretical, generous for Gen5 with non-ideal scheduling): **~25 tokens/s** compute-bound
- Memory-bound: 500 MB / 8.5 GB/s = ~60ms/token → ~16 tokens/s

Realistic expectation: **5–15 tokens/s** for a 1B-class model, once everything is working. That's slow but usable for local chat. For comparison, `llama.cpp` CPU-only on this i3 M370 would be around 1–3 tokens/s for the same model. So a **3–5× practical speedup** is the prize.

This is not about building a fast LLM rig. It is about **making a dead laptop do something it can't do today**.

---

## 2. Which pipeline? The critical architectural choice

This is the point where the project direction must be decided, because the current work is on the wrong pipeline for the stated goal.

Gen5 exposes **two command-streamer pipelines** sharing the same EU array:

### 2.1 The 3D render pipeline (what we're currently debugging)
Path: `3DSTATE_URB → 3DSTATE_VS → 3DSTATE_GS → 3DSTATE_CLIP → 3DSTATE_SF → 3DSTATE_WM → 3DPRIMITIVE`.

It's designed to rasterize triangles. To do compute with it you have to:
- Draw a fullscreen quad or rectlist covering a render target shaped like your output tensor.
- Encode input tensors as textures sampled by the fragment shader (WM kernel).
- Read back from the render target.

**This is how GPGPU was done before compute shaders existed** (the "GPGPU era", ~2005–2009). It works but has massive overhead: fixed-function rasterizer runs, depth/color interpolators burn cycles, and every "compute launch" is a draw call with ~15 state commands preceding it.

**Verdict for LLM**: possible but very inefficient. You'd waste 30–50% of the budget on rasterizer housekeeping, and matrix layouts have to be tortured into texture coordinates.

### 2.2 The media pipeline (MEDIA_OBJECT / thread dispatch)
Path: `PIPELINE_SELECT(media) → MEDIA_VFE_STATE → MEDIA_CURBE_LOAD → MEDIA_INTERFACE_DESCRIPTOR_LOAD → MEDIA_OBJECT`.

This is Gen5's **direct thread dispatch** interface. You hand it:
- A kernel binary (EU ISA machine code)
- A thread count and SIMD mode
- A constant buffer (CURBE)
- Surface state pointers

And it launches threads directly on the EU array, bypassing the entire 3D rasterizer. **This is the actual GPGPU path on Gen5**, documented in **Vol 2 Part 2** of the PRM. It's what Intel's own `libva` / video decode used, and what OpenCL implementations on Gen5 (there was never a production one, but Beignet for Gen6+ follows the same model) would have used.

**Verdict for LLM**: this is the right path. Fewer state commands, no rasterizer overhead, direct control over thread geometry and SIMD width, direct access to the sampler for weight loads.

### 2.3 Consequence

**The current work debugging 3DPRIMITIVE is not aligned with the LLM goal.** Even if we get the 3D pipeline working, it's the wrong pipeline for compute. The pragmatic move is:

- **Abandon** the 3D pipeline work as a dead end *for the LLM goal*. It's still useful if the goal were 2D render acceleration via 3D (alpha blending, scaling), but we've just said that's not the goal.
- **Keep** the BLT engine code for actual 2D acceleration in app_server — it's the pragmatic path to a responsive desktop, and it's simple.
- **Start fresh** on the media pipeline for compute. The existing work on ring buffer, GTT mapping, PIPE_CONTROL sync, and Gen5 workarounds **is all reusable** — that's infrastructure, not pipeline-specific.

This is the central strategic decision. The ring, GTT, interrupts, workarounds, and debug instrumentation we've built are the hard part. The pipeline choice on top is by comparison straightforward — if we pick the right one.

---

## 3. What has to be built

Assuming we pick the media pipeline, here is an honest list of what stands between us and a running LLM. Ordered by dependency.

### 3.1 Infrastructure (already exists or half-exists)
- ✅ Ring buffer command submission
- ✅ GTT allocation / mapping
- ✅ PIPE_CONTROL sync primitives
- ✅ Gen5 ring init workarounds
- ⚠️ Interrupt-driven completion (currently appears to be polling — needs to become IRQ + seqno for efficiency, but works as polling for bring-up)
- ❌ Proper GPU memory allocator (we currently hand-place things at fixed GTT offsets — this doesn't scale to an LLM's worth of buffers)
- ❌ Debugging instrumentation (ACTHD, per-command markers, GTT batch readback — see previous analysis)

### 3.2 Media pipeline bring-up
- ❌ `PIPELINE_SELECT` media mode
- ❌ `MEDIA_VFE_STATE` (VFE = Video Front End, the thread dispatcher)
- ❌ `MEDIA_CURBE_LOAD` (push constants equivalent)
- ❌ `MEDIA_INTERFACE_DESCRIPTOR_LOAD` (kernel descriptors — ISA pointer, sampler count, binding table)
- ❌ `MEDIA_OBJECT` or `MEDIA_OBJECT_WALKER` (thread launch)
- ❌ First "hello world" kernel: one thread writes a known value to a surface, CPU reads back. **This is the milestone that unblocks everything else.**

### 3.3 EU ISA toolchain (the real unknown)
This is the hardest non-obvious problem.

**We need EU machine code.** The EUs execute Intel's proprietary EU ISA — not x86, not a public VM bytecode. To run a compute kernel we need to produce binary EU instructions. Options, from most to least practical:

**Option A — Hand-assemble kernels.**
Feasible for tiny kernels (a few dozen instructions: matmul inner loop, vector add, softmax). The ISA is documented in **PRM Vol 4 Part 2**. Doable but painful and error-prone. This is the realistic path for a proof of concept and for the first 3–5 critical kernels.

**Option B — Use Mesa's i965 compiler backend (`brw_eu_emit.c`, `brw_eu.h`).**
Mesa has a C API that emits EU instructions programmatically. We could port just the emitter (not the full GLSL→NIR→i965 stack) and call it from Haiku. Medium effort, gives us a real assembler without a full compiler.

**Option C — `intel-beignet` (OpenCL on Gen7+).**
Beignet has an EU assembler and LLVM-based kernel compiler, but Gen5 is not supported. Porting would be large.

**Option D — Precompile on Linux, extract blob.**
On a Linux machine with i965 / Mesa, compile a GLSL compute shader targeted at Ironlake, dump the generated EU binary from `INTEL_DEBUG=vs,wm`, and embed the blob in our driver. Low-effort for static kernels, no-effort if we already have Linux access. **Best early-stage strategy**: get the pipeline running with precompiled blobs, figure out the compiler question later.

Recommended approach: **D now, B later**. Start with a handful of precompiled blob kernels (matmul tile, RMSNorm, softmax, RoPE), prove the pipeline works end-to-end, then if it does and we want to iterate on kernels without leaving Haiku, port Mesa's EU emitter.

### 3.4 Numerics and model runtime
- ❌ Weight loading (mmap file into GTT-addressable buffer)
- ❌ Quantized matmul kernel (INT4/INT8 weights, FP32 accumulator)
- ❌ Layernorm / RMSNorm kernel
- ❌ Softmax kernel
- ❌ RoPE / positional embedding kernel
- ❌ KV-cache management in GPU memory
- ❌ Tokenizer (CPU-side, pre-existing crates — BPE / SentencePiece)
- ❌ Orchestrator: a program that walks the model graph and dispatches kernels

This is the "LLM runtime" layer. It's substantial but well-understood work — llama.cpp, ggml, mlc-llm, candle all have reference implementations. The question is not how to do it but how much porting effort. A minimal GPT-2 inference loop is ~1500 lines of C; a minimal Llama loop is ~3000. The GPU backend is extra.

---

## 4. Timeline honest assessment

Grouped by effort, assuming one person working part-time.

| Phase | What | Rough effort |
|---|---|---|
| A | Instrumentation + GTT memory allocator + media pipeline bring-up + "hello world" kernel | 2–4 weeks of focused work |
| B | Matmul kernel (precompiled), verify numerical correctness | 2–3 weeks |
| C | Remaining kernels (norm, softmax, RoPE) | 1–2 weeks |
| D | GPT-2-small runtime end-to-end, first generated token | 2–3 weeks |
| E | INT4 matmul, larger model, optimization | open-ended |

**Total to "first token from a GPU-accelerated LLM on a VAIO VPCEB3K1E running Haiku": ~2–3 months of focused work.** Not 2–3 weeks, not 2–3 years. It's a real project but it's bounded.

The largest risks:
1. **Coherency / cache issues** on Gen5. No IOMMU, limited snooping — getting CPU→GPU→CPU data flow right on an 2009-era platform has footguns we haven't hit yet.
2. **PRM ambiguity / errata**. Intel 2010-era PRMs have gaps and bugs. Some things only Mesa's i965 source knows.
3. **GTT memory pressure**. 128–256 MB is tight for an LLM. Possible showstopper for anything >1B params.
4. **EU ISA assembly effort** if Option D (precompiled blobs) doesn't work out for some reason.

None of these are fundamental. They're engineering.

---

## 5. Recommendation

**Yes, do this, but redirect the work.**

Specifically:
1. **Freeze** the 3D render pipeline debugging. It's a dead end for the LLM goal. Document where it is and why it's parked.
2. **Refocus** on the media pipeline. Use Vol 2 Part 2 as the primary spec.
3. **Reuse** all existing infrastructure (ring, GTT, PIPE_CONTROL, workarounds).
4. **First milestone**: MEDIA_OBJECT launches a single-thread kernel that writes a constant to a surface, CPU verifies. That's the "transistors doing what we say" moment.
5. **Second milestone**: 48-thread matmul kernel correct against CPU reference.
6. **Third milestone**: GPT-2-124M inference loop producing text.
7. **Keep the BLT engine** work separately as the actual 2D acceleration path. Don't couple the two.

The instrumentation plan from the previous analysis (ACTHD, per-command markers, GTT batch dumps) applies unchanged — it's pipeline-agnostic. Do it first either way. Everything downstream depends on being able to see what the GPU is actually doing when it gets stuck.

---

## 6. One thing to be clear about

This will never be a "fast" LLM rig. A 2015 low-end laptop will beat it. A Raspberry Pi 5 will beat it on a good day. The value is not the benchmark number. The value is:

- **Making a 2009 laptop do inference it can't do today.** A class of machine that has effectively been abandoned by every modern ML stack.
- **Writing the first GPGPU driver for Gen5 on any non-Linux OS.** The documentation and code produced along the way has reuse value: Haiku's entire `intel_extreme` stack gets an acceleration option it has never had, and other Gen5 laptops (2009–2011 Arrandale/Clarkdale, huge installed base still exists) benefit.
- **Actually understanding a GPU end-to-end.** This is the kind of project that makes you good at graphics / compute work in a way that no tutorial can.

Those are the real reasons to do it. The LLM on top is the forcing function that makes the work concrete and testable.
