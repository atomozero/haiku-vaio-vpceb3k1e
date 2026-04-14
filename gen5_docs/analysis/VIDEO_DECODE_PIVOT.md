# Strategic pivot: video decode as primary goal, LLM as phase 2

## Context

Earlier documents (`LLM_FEASIBILITY.md`, `REFERENCE_PROJECTS.md`) positioned small-LLM inference as the target workload that justifies bringing up the Gen5 media pipeline. On reflection, **hardware H.264 decode is a strictly better primary goal** for this project, with LLM inference naturally falling into place as a phase 2 follow-on. This document records the rationale and the revised roadmap.

## Why video decode is a better primary goal than LLM inference

### 1. We use the hardware as intended, not as a creative repurposing
Gen5's media pipeline (`MEDIA_VFE_STATE`, `MEDIA_CURBE_LOAD`, `MEDIA_OBJECT`) was literally designed to do video decode. Every transistor was laid out with MPEG-2/VC-1/H.264 as the day job. Running LLM matmul on it is valid but is a *secondary* use pattern that nobody validated silicon against. Running video decode is the *primary* use pattern.

### 2. A complete, production-hardened reference implementation exists
`libva-intel-driver` (`intel-vaapi-driver`) contains:
- The full Gen5 media-pipeline programming sequence
- Gen5-specific EU kernels for MC, IDCT, deblocking (as `.asm` source)
- The `intel-gen4asm` assembler that produced them
- 15 years of production bug fixes from real users

For the LLM goal we would have had to **invent** every compute kernel from scratch and validate numerically against a CPU reference. For video decode we **port** known-good code and validate against the obvious "does the video play" criterion. Risk drops dramatically.

### 3. User value is orders of magnitude higher
- A small LLM at 5–15 tok/s is a technical curiosity used occasionally.
- A 2010 laptop that plays 1080p H.264 smoothly instead of stuttering is a **daily quality-of-life improvement** for every video the user watches.

The marginal utility of the two outcomes is not comparable.

### 4. Impact on Haiku as an operating system is much larger
Haiku currently has **zero hardware video decode on any Intel GPU**. Shipping one would be:
- The first hardware-accelerated video decoder on Haiku for Intel
- Reusable across the Intel Arrandale/Clarkdale installed base (2009–2011), not just the VAIO VPCEB3K1E
- A structural contribution to the OS, not a personalization of one machine

An LLM runtime on Haiku is a niche demo; a working video decoder is infrastructure other software can build on.

### 5. Memory constraints disappear
H.264 1080p needs ~30–50 MB for reference frames and working buffers. The 128 MB stolen + GTT-mappable memory is comfortable. The "does a 1B-parameter quantized model fit in 200 MB?" worry from the LLM plan vanishes.

### 6. The success criterion is unambiguous
"Does the video play smoothly?" — immediately obvious. "Is the matmul kernel numerically correct?" — requires CPU reference, epsilon tolerance, careful validation. Clearer success criteria make iteration faster.

### 7. A driver that does video decode feels complete
A GPU driver that only does 2D modeset feels half-finished. One that adds hardware video decode feels like a real driver. This changes the perception of the project and the motivation to keep working on it.

## Dimension-by-dimension comparison

| Dimension | LLM inference (old plan) | H.264 decode (new plan) |
|---|---|---|
| Hardware fit | Creative repurposing | Intended use |
| Reference code | Invent kernels from scratch | Port from libva-intel-driver |
| Technical risk | High (novel territory) | Medium (porting known-good) |
| User value | Occasional demo | Every video file |
| Memory budget | Tight (≤1B params) | Comfortable (30–50 MB) |
| Success criterion | Ambiguous (numerical correctness) | Unambiguous (plays or stutters) |
| Haiku ecosystem impact | Minor | Major — first of its kind |
| "Feels like a real driver" | No (2D + experiment) | Yes (2D + video) |
| Future scaling | Could grow | Capped at H.264 (no HEVC) |

## The critical insight: LLM doesn't go away, it moves to phase 2

Everything built for the video decoder is **the same infrastructure needed for GPGPU/LLM**:

- Media pipeline bring-up (`PIPELINE_SELECT`, `MEDIA_VFE_STATE`, `MEDIA_OBJECT`)
- Thread dispatch on the EU array
- EU kernel authoring toolchain (`intel-gen4asm` in-tree)
- Surface state, binding tables, sampler
- `PIPE_CONTROL` and sync primitives
- Proper GTT memory allocator
- Debug instrumentation

Once the video decoder works, reaching a compute matmul kernel is **a small additional step**, not a leap into the unknown. Every piece of the stack will already have been validated on a real workload with a canonical reference implementation. Writing the LLM phase on top of that is additive: 3–4 new kernels (matmul, norm, softmax, RoPE) over working infrastructure, instead of building infrastructure and kernels simultaneously under the risk of "is it even possible".

**LLM-as-phase-2 is significantly easier than LLM-as-phase-1.**

## Honest caveats

### 1. HEVC/H.265 is permanently out of reach
Gen5 silicon has no H.265, VP9, or AV1 hardware support. HEVC decode arrives on Broadwell/Gen8. What we *can* decode:
- ✅ MPEG-2 (all profiles)
- ✅ H.264 up to High Profile, 1080p ~30fps
- ✅ VC-1 (supported, but obsolete — skip)
- ❌ H.265 / HEVC
- ❌ VP9
- ❌ AV1

For modern streaming services this is a real limitation. For local files, older YouTube streams (H.264 fallback path still served to older clients), DVB/DVD MPEG-2, and archive content, it covers a huge amount of real-world use.

### 2. The bitstream parser is CPU-side and must be written
The parser is spec-heavy and error-prone: NAL units, slice headers, CABAC/CAVLC, sequence/picture parameter sets. Three strategies, in increasing independence:
- Port the parser from libva-intel-driver (MIT, already targets Gen5, already validated). **Likely the right choice.**
- Port an MIT/BSD-licensed standalone parser.
- Write from scratch following the H.264 spec (ISO/IEC 14496-10). Long but in-tree.

### 3. Integration with Haiku's `media_kit`
The GPU decoder is one thing; exposing it as a `BMediaNode` / media add-on that VideoPlayer and other consumers can use is another. Haiku-specific wrapper work sits on top of the GPU work. Not hard, but real effort that's separate from the Gen5 programming.

### 4. Conformance testing is the long tail
Video codecs have endless edge cases: interlaced content, field pictures, B-frame patterns, weighted prediction, CABAC corner cases. Realistic rule of thumb: 20% of the time goes into bring-up, 80% into conformance and edge-case fixes. This is the dominant cost of the project and should be planned for explicitly.

### 5. Start with MPEG-2, not H.264
MPEG-2 is roughly ten times simpler than H.264: no CABAC, simpler motion compensation, no deblocking filter, no B-frame weirdness. libva-intel-driver has the Gen5 MPEG-2 decoder in source. Doing MPEG-2 first:
- Validates the entire media pipeline on a simple workload
- Delivers a first decoded frame in weeks, not months
- Debugs the foundation without H.264's complexity piled on top
- Then H.264 becomes "add missing kernels + more complex parser", not "everything at once"

This is exactly how libva-intel-driver itself was built historically.

## Revised roadmap

### Phase 0 — Infrastructure (pipeline-agnostic, do first regardless)
1. Freeze the 3D render pipeline debugging work. Document where it is and why it is parked.
2. Build debug instrumentation: ACTHD/INSTDONE/IPEHR dumps, per-command markers in batches, GTT readback of submitted batches.
3. Implement a proper GTT memory allocator (replacing fixed-offset placement).
4. Port `intel-gen4asm` from libva-intel-driver into the tree. No precompiled kernel blobs ever ship.

### Phase 1 — Media pipeline bring-up
5. `PIPELINE_SELECT` media mode, `MEDIA_VFE_STATE`, `MEDIA_INTERFACE_DESCRIPTOR_LOAD`, first `MEDIA_OBJECT` launching a one-thread "hello world" kernel that writes a constant to a surface. First sign of life from the EUs.
6. Parallel memset kernel across 48 threads. Validates thread dispatch, CURBE, surface state, sync.

### Phase 2 — MPEG-2 decoder
7. Port the MPEG-2 bitstream parser from libva-intel-driver (or write minimal subset).
8. Port Gen5 MPEG-2 kernels (IDCT, MC) from libva-intel-driver.
9. First decoded frame displayed via the existing `intel_extreme` framebuffer.
10. Wrap as a Haiku `media_kit` codec add-on; play a real `.mpg` file end-to-end in MediaPlayer.

### Phase 3 — H.264 decoder
11. Extend parser to H.264 Baseline → Main → High profile incrementally.
12. Port H.264 MC/IDCT/deblocking kernels from libva.
13. Conformance testing against H.264 test bitstreams.

### Phase 4 — LLM compute kernels on the now-working infrastructure
14. Write matmul kernel(s) in `.asm`, assembled by the in-tree `gen4asm`.
15. Write RMSNorm, softmax, RoPE kernels.
16. Port a minimal inference runtime (GPT-2 small → TinyLlama) that dispatches the kernels via the same `MEDIA_OBJECT` path the video decoder uses.

Phase 4 is now **small** because phases 0–3 built and validated 90% of its prerequisites on a different workload.

## What we give up

The only thing we lose with this pivot is the unique "cool factor" of running an LLM on a 2010 laptop as the primary goal. We recover it in phase 4, where it becomes incrementally easier than it would have been as phase 1. In exchange we get:
- A working video decoder on real content, every day
- A structural contribution to Haiku
- Dramatically lower risk of the project stalling
- Reference code for every step instead of invention from scratch
- A driver that feels complete instead of experimental

## Decision

Adopt this as the new project direction. Subsequent work plans and analysis documents should assume the video-decode-first roadmap.
