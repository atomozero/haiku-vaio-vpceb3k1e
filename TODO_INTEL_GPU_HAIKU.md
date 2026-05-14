# TODO: Accelerazione GPU Intel Gen5 su Haiku

**Hardware:** Intel HD Graphics 0x0046 (Ironlake Mobile, Gen5), Sony Vaio VPCEB3K1E
**OS:** Haiku R1~beta5 (hrev59506+)
**Direzione strategica:** Video decode hardware (MPEG-2 → H.264) come obiettivo primario,
compute/LLM come fase successiva. Vedi `gen5_docs/analysis/VIDEO_DECODE_PIVOT.md`.

**Ultimo aggiornamento:** 2026-05-14 (GEM_EXECBUFFER2 working, Mesa crocus loads + creates resources)

---

## Legenda

- [x] Completato e verificato su hardware
- [-] Parzialmente completato / funzionante con limitazioni
- [ ] Da fare

---

## Fase 0: Infrastruttura e preparazione — COMPLETATA

- [x] Studio architettura X547/RadeonGfx, libdrm2, accelerant2
- [x] Studio driver i915 Linux (crocus winsys, GEM, execbuffer, DRM ioctl)
- [x] Documentazione hardware Gen5 (register map, ring buffer, PRM)
- [x] Patch display LVDS (EDID fallback, dual/single channel, IBX watermark)
- [x] Display 1366x768 32bit 59.9Hz funzionante
- [x] 2D BLT acceleration (XY_SOURCE_BLIT, XY_COLOR_BLIT, pattern fill)
- [x] Port intel-gen4asm, correzione opcode Gen5

---

## Fase 1: Pipeline 3D (Render Engine) — COMPLETATA (base)

- [x] PIPELINE_SELECT 3D, STATE_BASE_ADDRESS, URB_FENCE, 3DPRIMITIVE
- [x] SF kernel (7 istr) + WM kernel (6 istr) — solid fill SIMD16
- [x] Workaround Ironlake (RC6 wake, MI_MODE, _3D_CHICKEN2)
- [x] render_fill_rect funzionante con marker diagnostici
- [ ] 3D avanzato: alpha blending, texture sampling, compositing

---

## Fase 2: Media Pipeline (Compute) — COMPLETATA

- [x] Infrastruttura: gpu_bo allocator, gpu_debug, bench module
- [x] Phase I.B: primo kernel EU (hello_world.g4a, 10-command batch)
- [x] Phase 1.2: 48-thread parallel dispatch
- [x] Phase 1.3: per-thread memory write (OWord Block Write, SURFTYPE_BUFFER)
- [x] Phase 2.1: SAXPY FP32 bit-exact (384 elementi, benchmark MFLOPS)
- [x] Phase 2.2: MEDIA_BLOCK_READ sampler cache, CURBE 30 GRF, libva ABI
- [x] Phase 2.3b: MPEG-2 IQ kernel (quantizzazione inversa, bit-exact)

---

## Fase 3: Decoder MPEG-2 — FASE ATTIVA

### 3.1 IDCT kernel standalone — COMPLETATO
- [x] idct_single.g4a — IDCT 2-pass dp4 (109 istruzioni)
- [x] DO_IDCT subroutine con jmpi/ip, a0.0 dual-half addressing
- [x] Riferimento CPU (idct_ref.h) bit-exact

### 3.2 Kernel IQ + IDCT combinato — COMPLETATO
- [x] iq_idct_intra.g4a (149 istruzioni) con output U8 a SURFTYPE_2D
- [x] Media Block Write (msg_type=2) con coordinate (x,y) da inline data
- [x] Versione no-level-shift per decode reale (145 istruzioni)
- [x] Bug scoperti e corretti:
  - {compr} UB→UW widening scrive solo metà GRF (dati stale)
  - gen4asm .N subregister è in BYTE senza -a flag
  - {compr} su W add/mov scrive solo metà GRF

### 3.3 Parser MPEG-2 bitstream — COMPLETATO
- [x] Bitstream reader, start code scanner
- [x] Parser: sequence_header, picture_header, picture_coding_extension, slice_header
- [x] DC VLC decoder (Table B-12 luma, B-13 chroma)
- [x] AC VLC decoder (Table B-14 completa, 112 entries + escape codes)
- [x] Table B-15 per intra_vlc_format=1 — implementata
- [x] Macroblock address increment (Table B-1, fino a 33 + escape + stuffing)
- [x] Macroblock type I-picture (Table B-2) e P-picture (Table B-3)
- [x] Coded Block Pattern Table B-9 completa (64 entries)
- [x] Intra macroblock decoder (6 blocchi: 4Y + Cb + Cr, DC prediction)
- [x] Non-intra macroblock decoder (P-frame inter blocks)
- [x] Motion vector decode (Table B-10 + f_code expansion)
- [x] IQ inline nel parser (come ffmpeg): segno applicato DOPO >>4
- [x] Mismatch control §7.4.3: block[63] ^= 1
- [x] Error recovery: continue su MB fail (non resync_to_next_slice)
- [x] Fix EOB dopo blocco pieno (idx>=64): consumare trailing EOB
- [-] IDCT precision: ±1-3 pixel vs ffmpeg (cosine table libva, non IEEE 1180)

### 3.4 Integrazione parser → GPU decode — COMPLETATO
- [x] Parse I-frame → dispatch batch GPU → verifica pixel
- [x] 100% coverage su tutti i file di test (gray, dark, testsrc, real, mandel)

### 3.5 Output visibile — COMPLETATO
- [x] Decode Y su GPU, Cb/Cr su CPU, YCbCr→RGB32 (BT.601)
- [x] Output PPM visualizzabile con ShowImage
- [x] Test results finali:
  | File | Risoluzione | MB | Copertura | PSNR vs ffmpeg |
  |------|------------|:---:|:---------:|:-----------:|
  | Gray q31 | 64×64 | 16/16 | **100%** | exact |
  | Dark q20 | 320×240 | 300/300 | **100%** | 44+ dB |
  | Real q31 | 640×480 | 1200/1200 | **100%** | 44.5 dB |
  | Testsrc q15 | 320×240 | 300/300 | **100%** | 44.3 dB |
  | Mandel q2 | 320×240 | 300/300 | **100%** | 50.0 dB |
  | Mandel q5 | 320×240 | 300/300 | **100%** | 49.4 dB |

### 3.6 GPU path IDCT-only — COMPLETATO
- [x] Kernel idct_to_u8.g4a: IDCT + clamp + Media Block Write U8
- [x] Batch dispatch submit_blocks_batch_gpu() fino a 400 blocchi

### 3.7 Motion Compensation P-frame — CPU COMPLETATA
- [x] test_mc_decode.cpp: decoder multi-frame I+P con MC CPU
- [x] Half-pel bilinear interpolation (4 casi)
- [x] Chroma MC con scaling MV /2
- [x] Skipped MB handling, reference frame management

### 3.8 media_kit codec add-on — COMPLETATO (I+P)
- [x] Plugin MPEG2DecoderPlugin + MPEG2Decoder (DecoderPlugin API)
- [x] Registrazione formato B_MPEG_2_VIDEO via BMediaFormats
- [x] Output B_YCbCr420 (Y + Cb + Cr planes)
- [x] I-frame decode via mpeg2_parser + compute_idct_reference
- [x] P-frame decode con motion compensation (half-pel bilinear)
- [x] Reference frame management (I/P → reference per P successivi)
- [x] Skipped MB handling (copia da reference, zero MV)
- [x] Build come .so, installato in ~/config/non-packaged/add-ons/media/plugins/
- [x] Viewer standalone (mpeg2_viewer) con I+P playback visuale
- [x] Test multi-frame: 10 frame (2 I + 8 P), 320x240, 100% MB coverage
- [ ] Test con MediaPlayer su file .m2v
- [ ] B-frame decode (output nero per ora)

### 3.9 GPU Motion Compensation kernel — COMPLETATO
- [x] mc_fullpel_only.g4b.gen5: kernel MC full-pel (13 istruzioni)
  - Media Block Read da reference surface (BTI 2)
  - Media Block Write a output surface (BTI 1)
- [x] Test boot-time: MC su reference sintetico 32x32 gradient
  - Verifica pixel-exact vs CPU mc_block_cpu(): 64/64 correct
- [x] Benchmark GPU vs CPU (48 dispatch batch, single submission)
  - GPU: 17 µs / 48 blocchi = 2.8M blocks/s
  - CPU: 7 µs / 48 blocchi = 6.9M blocks/s
  - Ratio: 0.41x (CPU vince per blocco singolo 8×8, overhead dispatch domina)
  - Limiti Gen5 scoperti:
    - URB recycling rotto: iterations ≤ num_urb_entries (max 48)
    - No second ring submission per media: IS stall dopo MI_FLUSH
    - Soluzione: tutto in singola submission ring (state + N dispatch + flush)
- [ ] mc_idct_inter.g4a: MC + IDCT residual addition combinato
- [ ] Integrazione batch dispatch per P-frame (MC per ogni block)
- [x] Benchmark IDCT 400 blocchi — **GPU VINCE 4×**
      GPU: 114-126 µs, CPU: 458-475 µs → speedup **4.01×**
      Fix: busy-wait puro (no snooze), timing solo ring kick→marker.
      Tool standalone: tests/gpu_idct_bench (no reboot, clona accelerant)

### 3.10 GPU 3D Cube Demo — FUNZIONANTE
- [x] Clone accelerant da userspace (device open + shared_info + registers)
- [x] Compute rasterizer: cubo 3D con 48 EU thread paralleli
  - Rotazione + proiezione prospettica + backface culling + lighting
  - Painter's algorithm (Z-sort) per 6 facce colorate
  - Tile fill kernel via media pipeline
  - BWindow + BBitmap + Invalidate display loop
- [x] Benchmark IDCT 400 blocchi — **GPU 4× faster than CPU**
  - GPU: 100-126 µs, CPU: 351-475 µs → speedup 3.5-4.0×
  - Tool standalone: tests/gpu_idct_bench (no reboot)
- [x] Cubo 3D demo: 480×480 raster, 720×720 finestra, 21 FPS
  - GPU tile fill: ~100 µs/frame (260 tiles)
  - Bottleneck: app_server Invalidate→Draw round-trip (~47 ms)
  - BDirectWindow tentato ma VRAM write-combining più lento (11 FPS)
- [-] Ring clone da userspace: ring reset funziona (HEAD/TAIL=0) ma CS
      non riparte dopo boot-time media pipeline test.
      **Root cause trovata** (sessione 2026-05-09, analisi i915):
      1. ILK_GDSR media domain reset necessario (reg 0x12ca4) — ring
         soft-reset non basta per IS stall da MEDIA_OBJECT+MI_FLUSH
      2. FORCEWAKE (reg 0xA18C) non asserted — GPU in RC6, TAIL drops
      3. i915 Linux non permette MAI write RING_TAIL da userspace —
         solo kernel via execbuffer ioctl
- [x] **Fase A.1**: ILK_GDSR media domain reset in render_init_clone
- [x] **Fase A.2**: FORCEWAKE assert prima di ring operations
- [x] **Fase A.3**: GPU plasma demo visibile sullo schermo (177 FPS)
      GPU IDCT 0.1ms/frame, CPU blit 4.3ms/frame, 300 blocchi/frame
      Scoperta: graphics_memory accessibile dal clone, framebuffer
      write diretto funziona. ILK_GDSR non necessario per media pipeline
      (solo per 3D pipeline ring recovery).
      Tool: tests/gpu_plasma_screen
- [x] **Cubo 3D diretto a framebuffer** — 60 FPS stabile
      CPU raster (per-pixel edge test) → memcpy a graphics_memory.
      815 FPS raw, 60 FPS con frame limiter.
- [x] **BLT engine via kernel ioctl** — 60 FPS stabile
      XY_SRC_COPY_BLT (0x54F00006) scritto nella ring memory,
      TAIL kick via INTEL_RING_WRITE_TAIL ioctl. GPU esegue il BLT,
      copia da buffer GTT a screen framebuffer. Tool: tests/gpu_triangle

### 3.10.1 Scoperta critica: MMIO read-only da userspace (2026-05-11)
Le scritture MMIO sono **silenziosamente ignorate** sia via `clone_area` che
via driver `/dev/misc/poke` (mapping diretto BAR0 0xF0000000). Verificato:
- Scratch register: write 0xDEADBEEF, read back 0x0
- RING_BUFFER_HEAD bloccato a 0x5134 — non resettabile
- RING_BUFFER_CONTROL: write 0, read back 0xF001 — non disabilitabile
- Syslog conferma: `engine stalled, head 5134` (anche app_server fallisce)
- Render ring HEAD identico in 5+ esecuzioni successive

**Implicazione:** tutta la pipeline ring→GPU (BLT, media pipeline, 3D
TRILIST) è inaccessibile da userspace. Il kernel driver DEVE fare le
scritture TAIL/HEAD/CTL. Questo è il prerequisito assoluto per:
- BLT engine da userspace
- Media pipeline da userspace (dopo boot)
- Mesa/crocus (GEM_EXECBUFFER2)
- Qualsiasi operazione GPU che non sia framebuffer diretto

### 3.11 Prossimi passi
- [x] **Kernel ioctl per TAIL write** — FUNZIONANTE (GPU esegue comandi)
- [x] **BLT blit al framebuffer** — FUNZIONANTE (60 FPS, cubo 3D visibile)
- [x] **Media pipeline via ioctl** — FUNZIONANTE (GPU IDCT 3.5x, 400 blocchi)
      ring_submit_ioctl() in media_pipeline.cpp, fallback a QueueCommands.
      Ring reset via ioctl in media_pipeline_init per cloni.
- [ ] GPU IDCT nel plugin (sostituire compute_idct_reference con GPU dispatch)
- [ ] GPU MC+IDCT combinato per P-frame decode completo su GPU
- [ ] Gouraud shading WM kernel (interpolazione colore per vertice)
- [ ] IDCT IEEE 1180 (sostituire cosine table)
- [ ] B-frame support

---

## Fase 4: Decoder H.264 — dopo MPEG-2

- [ ] Parser H.264 (NAL, SPS, PPS, slice, CAVLC/CABAC)
- [ ] Kernel GPU H.264 (MC quarter-pel, IDCT 4x4/8x8, deblocking)
- [ ] Reference frame management (DPB, MMCO)
- [ ] Profili Baseline → Main → High

---

## Fase 5: Compute / LLM — dopo video decode

- [ ] Kernel compute (matmul, softmax, RMSNorm, RoPE)
- [ ] Runtime inferenza (GPT-2 small / TinyLlama)

---

## Fase 6: OpenGL via Mesa crocus — ROADMAP

### Fase A: Ring clone funzionante (prerequisito per tutto)
- [-] A.1: ILK_GDSR media domain reset — non necessario (scoperta 2026-05-10)
- [-] A.2: FORCEWAKE — non esiste su ILK Gen5 (solo SNB+)
- [-] A.3: MMIO write da userspace impossibile — HEAD bloccato, TAIL ignorato
      **Root cause 2026-05-11:** tutte le scritture MMIO (clone_area + poke
      BAR0 0xF0000000) silenziosamente ignorate. Serve kernel ioctl.
- [x] A.4: Cubo 3D visibile via framebuffer diretto (CPU raster, 60 FPS)

### Fase B: Kernel ioctl per ring submission — COMPLETATA (base)
- [x] B.1: INTEL_RING_RESET + INTEL_RING_WRITE_TAIL ioctl nel kernel driver
      Kernel scrive TAIL/HEAD/CTL via MMIO (userspace è R/O).
      Build manuale con -fPIC + mutex ABI shim (_mutex→mutex) per hrev59669.
      Blacklist driver di sistema via /boot/system/settings/packages.
      **TEST PASS**: HEAD avanza dopo TAIL write → GPU esegue MI_NOOP!
      CRITICAL: never RING_RESET after boot — kills CS permanently.
      Use ring sync (read HW TAIL, set sw pos) instead.
- [x] B.2: BLT via ioctl — 3D cubo 480×480 a 60 FPS, GPU BLT a schermo
- [ ] B.3: GPU hang detection + ILK_GDSR recovery nel kernel

### Fase C: Interfaccia DRM minimale per Mesa crocus
- [x] C.1: GEM_CREATE / GEM_CLOSE — wrapper su INTEL_ALLOCATE_GRAPHICS_MEMORY ✅
- [x] C.2: GEM_MMAP — already mapped by allocator, just return addr ✅
- [x] C.2b: GETPARAM + GET_APERTURE + GEM_BUSY + SET_DOMAIN ✅
- [x] C.2c: GEM_CONTEXT_CREATE/DESTROY (stub) ✅
- [x] C.3: **GEM_EXECBUFFER2** — FUNZIONANTE! (2026-05-13)
      DRM shim: MI_BATCH_BUFFER_START nel ring + TAIL kick via ioctl.
      Ring sync (non reset!) con TAIL hardware — RING_RESET uccide il CS.
      Relocation patching, EXEC_HANDLE_LUT, EXEC_BATCH_FIRST supportati.
      Completion marker via MI_STORE_DATA_IMM nel ring (non nel batch).
      **CRITICAL**: RING_RESET (disable→re-enable) uccide il CS dopo il
      primo uso. Fix: sync con TAIL hw senza reset (render_init_clone).
      **gl_test risultati** (2026-05-13):
      - Ring sync OK: marker=0xBEEF0001, GPU WORKS!
      - Mesa crocus init OK: screen, context, resource_create, textures
      - Crash in ralloc.c canary (Mesa memory allocator) during
        crocus_batch_add_syncobj → validate_program_pipeline
      - Prossimo: debug syncobj/fence handling nel DRM shim
      - Causa: 3D pipeline state issue (non ring submission)
      - Next: debug 3D pipeline state o ISL surface encoding
      EXECBUF2 #2+ (glClear/3D render) hang: IPEHR=0x02000000 (MI_FLUSH
      after batch return), INSTDONE=0xFFFFFFFF, EIR=0x0.
      HEAD advances through MI_BATCH_BUFFER_START but stalls on
      post-batch MI_FLUSH. The 3D pipeline state from the batch
      leaves the CS unable to flush. Need to debug pipeline state.
      **Scoperta critica:** RING_RESET (disable→re-enable) uccide il CS
      permanentemente. Soluzione: ring sync (leggere TAIL HW, mai resettare).
- [x] C.4: GET_RESET_STATS, SET_TILING, GET_TILING, SET_CACHING, GEM_WAIT,
      CONTEXT_GETPARAM/SETPARAM, MADVISE — tutti implementati nel dispatcher

### Fase D: Mesa crocus winsys per Haiku — IN CORSO
- [x] D.1: libdrm shim (xf86drm.h, _IOC compat, drmDevice, libdrm_shim.so)
- [x] D.2: crocus_bufmgr → Haiku GEM shim via haiku_drm_intel
- [x] D.3: Mesa 25.3.3 compilata con crocus (libcrocus.a OK)
- [-] D.4: CrocusRenderer + "Crocus Pipe" addon (128MB, statically linked)
      **Stato 2026-05-14:**
      - GLInfo/gl_test carica Crocus Pipe OK, crocus_screen_create OK
      - Ring sync (no reset): GPU WORKS, ring test marker OK
      - GEM_EXECBUFFER2 #1 (state setup): completato dalla GPU!
      - gl_test: resource_create OK, validate_textures OK, crash in ralloc
      - OpenGL 2.1, GLSL 1.20, Mesa Intel(R) HD Graphics (ILK)
      - Next: debug crash dopo validate_textures (ralloc/syncobj)
      **Prossimo blocco:** 3D pipeline hang al batch #3 (vedi E.2)

### Fase E: GLInfo e rendering visibile — PIANIFICAZIONE

#### E.1: GLInfo mostra dati GL corretti — COMPLETATA (2026-05-11)
- [x] Fix `st_api_make_current`: dichiarata bool (era int, ABI mismatch)
- [x] glapi dispatch bridge: Mesa 25 `_mesa_glapi` → system `_glapi`
      Trovare libglapi.so GIÀ caricata (get_next_image_info, non load_add_on)
- [x] `hgl_buffer->newWidth/Height` inizializzati da BGLView bounds
- [x] GLInfo mostra: **Mesa Intel(R) HD Graphics (ILK), OpenGL 2.1 Mesa 25.3.3**
      Texture 2D max: 8192, tutte le capacità popolate

#### E.1.1: ISL format table fix (2026-05-11)
- [x] **ISL format table corrotta nel binario linkato** — 170 entry valide su 918.
      Entry 233 (B8G8R8X8_UNORM) aveva bpb=0 → crash `bs > 0` in isl_tiling_get_info.
      Root cause: linker non ha scritto correttamente la sparse array C99.
      Fix: binary patch del Crocus Pipe con tabella corretta dal .o (36720 bytes).
- [x] **gl_test non crasha più** — finestra nera (glClear non visibile, serve SwapBuffers)

#### E.2: SwapBuffers GPU→Screen (rendering visibile) + 3D pipeline debug
- [-] **gl_test sessione 2026-05-13 (nuova, con ring sync fix):**
      OpenGL 2.1 Mesa 25.3.3 Intel(R) HD Graphics (ILK) — init OK
      EXECBUF2 #1: 9 cmds state setup → **GPU completed!** (seq=1)
      EXECBUF2 #2: 65 cmds glClear → **GPU completed!** (seq=2)
      EXECBUF2 #3: 67 cmds readback → **GPU HANG** HEAD=0x254, TAIL=0x3e0
      EXECBUF2 #4+: tutti in timeout, HEAD bloccato a 0x254
      **Diagnostica hang #3:**
      - IPEHR=0x79000002 (3DSTATE_GLOBAL_DEPTH_OFFSET_CLAMP)
      - INSTDONE=0xFFFFFFFF (pipeline completato, nessuno stage bloccato)
      - EIR=0x0 ESR=0x0 (nessun errore HW)
      - ACTHD=0x254 (GPU ferma nella ring, non nel batch)
      - Batch #3 inizia con: 79000002 00000000 00000000 00000000
        poi 61010006 (STATE_BASE_ADDRESS), 78080007 (PIPELINED_POINTERS)
      - Batch #2 (che funziona) ha comandi quasi identici — differenza?
      **Analisi batch #2 vs #3:**
      Batch #2 (OK):  79000002 00000000 **00c7012b** 00000000 61010006...
      Batch #3 (HANG): 79000002 00000000 **00000000** 00000000 61010006...
      DW2 di 3DSTATE_GLOBAL_DEPTH_OFFSET_CLAMP: 0x00c7012b vs 0x00000000
      La differenza potrebbe essere significativa (depth buffer config).
- [ ] **Debug 3D pipeline hang** — BLOCCO ATTIVO
      **Sessione 2026-05-14:** Confermato con kernel ioctl patchato.
      EXECBUF2 #1 (9 DW state setup) hang su PIPE_CONTROL (0x7a000002).
      HEAD avanza a TAIL (ring consumato) ma GPU bloccata nel batch.
      INSTDONE=0xFFFFFFFE (quasi completato). Batch decodifica:
      MI_FLUSH + PIPE_CONTROL + 3DSTATE cmds (790a, 7906).
      Prossimo passo:
      1. Confrontare batch #2 (OK) e #3 (hang) DW per DW
      2. Verificare surface state encoding (0x42b040, 0x42b080 refs)
      3. Verificare se manca un PIPE_CONTROL/MI_FLUSH tra batch
      4. Testare: forzare stessi comandi di batch #2 in batch #3
      5. Provare ad aggiungere PIPE_CONTROL con CS stall prima di batch #3
- [ ] Opzione A (rapida): readback GPU surface → memcpy a BBitmap (CPU, lento)
- [ ] Opzione B (veloce): BLT da GPU surface a framebuffer diretto
- [ ] Opzione C (corretta): DRI-style direct rendering

#### E.3: Robustezza DRM shim
- [ ] Ring wrap: gestire wrap-around senza RING_RESET (sync HEAD, wait drain)
- [ ] GEM_BUSY: controllare se BO ha comandi pendenti nel ring
- [ ] SYNCOBJ: implementare wait reale (poll su completion marker)
- [ ] Multiple EXECBUFFER2 in sequenza senza ring overflow

#### E.4: Mesa crocus completeness
- [ ] Verificare crocus batch submit path (crocus_batch.c) end-to-end
- [ ] Test con programma GL semplice (glClear + glFinish)
- [ ] Test con glxgears o equivalente Haiku
- [ ] Profile performance: GPU batch throughput

### Scoperte propedeutiche
- **(2026-05-08)** Ring buffer accessibile da clone userspace, ma NON resettare.
  render_init_clone(): alloca state GPU, sincronizza ring position.
- **(2026-05-11)** **MMIO write broken**: scritture a registri GPU ignorate
  da userspace (clone_area + poke). Root cause: kernel mappa con
  B_KERNEL_WRITE_AREA senza B_WRITE_AREA. Risolto con kernel ioctl.
- **(2026-05-11)** **Ring reset kills CS**: disable+re-enable del ring
  uccide il Command Streamer dopo il primo uso. Funziona solo 1 volta
  dal boot, poi il CS non riparte. Usare sync-only (leggere HW TAIL).
- **(2026-05-11)** **GEM_EXECBUFFER2 funzionante**: DRM shim invia batch
  via MI_BATCH_BUFFER_START nel ring + TAIL kick ioctl. Mesa crocus
  sottomette batch e GPU li esegue. Ring sync senza reset.
- **(2026-05-11)** **GLInfo analysis**: tutti i 18 ioctl sono gestiti.
  `st_api_make_current` ritorna true (ABI: dichiarata int, è bool).
  Manca bridge GPU surface → BBitmap per rendering visibile.

### Roadmap Mesa (stato attuale)
- [x] **Step 1**: GETPARAM + GET_APERTURE (chipset_id=0x0046) ✅
- [x] **Step 2**: GEM_CREATE/CLOSE/MMAP ✅
- [x] **Step 3**: GEM_CONTEXT_CREATE/DESTROY (stub) ✅
- [x] **Step 4**: **GEM_EXECBUFFER2** ✅ (clflush + INTEL_EXEC_BATCH ioctl)
- [x] **Step 5**: SET_TILING, GET_TILING, GEM_WAIT, CONTEXT_PARAM, RESET_STATS ✅
- [x] **Step 6**: Shim libdrm per Haiku — xf86drm.h con _IOC compat, drmDevice,
      syncobj stubs, drmPrime, drmAuth. libdrm_shim.so con extern "C" exports.
- [x] **Step 7**: **Mesa 25.3.3 compilata con crocus!**
      `meson -Dgallium-drivers=softpipe,crocus -Dplatforms=haiku -Dwerror=false`
      Patch: system_has_kms_drm += haiku, intel_perf MIN/MAX guard,
      build_id stub, libsync _IOC compat. 1339 file compilati.
      Output: libgallium-25.3.3.so (134MB) con softpipe + crocus.
- [-] **Step 8**: CrocusRenderer addon per Haiku GL stack
      Addon installato e funzionante (GLInfo carica, crocus screen + EXECBUF OK).
      Manca bridge GPU surface → BBitmap per display visibile.
      Vedi Fase E per la pianificazione dettagliata.

### Scoperte sessione 2026-05-10

**IDCT benchmark corretto con busy-wait:**
- snooze(500) nel wait loop falsava il timing GPU (60ms misurati vs ~100µs reali)
- Fix: busy-wait puro (spin loop senza snooze) nel benchmark
- Risultato: **GPU 3.5-4.0× più veloce della CPU** su 400 blocchi IDCT 8×8
  - GPU: 100-126 µs (400 blocchi in parallelo, 48 EU thread + URB recycling)
  - CPU: 351-475 µs (400 blocchi sequenziali, compute_idct_reference -O2)
- Tool standalone: tests/gpu_idct_bench (no reboot, output su terminale)

**Ring clone — analisi completa:**
- Ring funziona dal clone per media pipeline + BLT (cube demo OK)
- Ring NON funziona dopo che il cube demo (o qualsiasi media pipeline)
  ha riempito il ring → CS si blocca su MI_FLUSH post-MEDIA_OBJECT
  (stesso bug IS stall documentato in sessione precedente)
- Diagnostica: HEAD=0x5134 fisso con TAIL=0xCA20 (31KB di comandi pending)
- MMIO register writes funzionano dal clone (TAIL si aggiorna)
- ring.base == graphics_memory (VA corretto, non serve remap)
- HWS_PGA=0x1ffff000 (HW Status Page configurato dal kernel)
- Problema fondamentale: **il CS si blocca dopo N media dispatches** perché
  MI_FLUSH non completa (IS stall). Una volta bloccato, il ring è morto.
  Solo un reboot o un ILK_GDSR reset può riportarlo in vita.

**Triangolo 3D TRILIST — stato:**
- render_draw_triangle implementato con CLIP ACCEPT_ALL + VUE header
- Non testabile finché il ring è dead (serve boot fresco senza cube demo)
- Approccio: boot pulito → test TRILIST prima di qualsiasi media pipeline

### Prossimo passo immediato
- [ ] Boot fresco (no test accelerant, no cube demo) → test _tri_test
      per verificare TRILIST su ring pulito
- [ ] Se TRILIST funziona: integrare in demo visiva (cubo con 3D pipeline)
- [ ] MI_BATCH_BUFFER_START dal clone — prerequisito per EXECBUFFER2
- [ ] Fix CS hang: trovare modo di resettare CS dopo media pipeline
      senza reboot (ILK_GDSR o kernel ioctl)

---

## Bug Gen5 scoperti e documentati

| # | Bug | Impatto | Fix |
|---|-----|---------|-----|
| 1 | {compr} UB→UW widening scrive solo metà GRF | Dati stale non deterministici | Widening esplicita 2×SIMD8 |
| 2 | gen4asm .N subreg è in BYTE (senza -a) | Pack D→W alla posizione sbagliata | Usare .16 per UW elem 8 |
| 3 | {compr} su W add/mov scrive solo metà GRF | +128 applicato solo a metà blocco | 4× add(16) senza {compr} |
| 4 | dc_pred init include level shift | +128 dopo IDCT raddoppia l'offset | NON aggiungere +128 |
| 5 | GOP code 0xB8 > SLICE_START_MAX | Parser esce dal loop header | Check <= SLICE_MAX |
| 6 | EOB '10' non controllato per primo AC | Blocchi DC-only non terminano | EOB check prima di '1s' |
| 7 | URB entry stale data oltre 48 dispatch | Thread leggono dati sbagliati | Padding 16 DW per inline |
| 8 | SURFTYPE_2D per OWord Block R/W | Silent OOB drops | Usare SURFTYPE_BUFFER |
| 9 | MEDIA_BLOCK_READ msg_type=4 (PRM) | Silently fails | Usare msg_type=2 |
| 10 | Send payload da GRF (non MRF) | Output corrotto | Header da GRF, payload da MRF |
| 11 | Table B-14 VLC codes 12-bit SBAGLIATI | Coefficienti AC decodificati errati | Codici corretti da ffmpeg ff_mpeg1_vlc_table |
| 12 | Start code 00 00 01 dentro macroblocco | Parser legge start code come DC VLC | Check peek(23)==0 prima di ogni blocco |
| 13 | quantiser_scale mai aggiornato | AC IQ con q_scale=1 (hardcoded) | Compute da q_scale_code per-slice |
| 14 | EOB non consumato dopo blocco pieno | Drift 2 bit per blocco con 64 coeff | Leggere trailing EOB quando idx>=64 |
| 15 | CBP Table B-9 incompleta | Silent corruption P-frame inter MB | Implementata tabella completa 64 entries |
| 16 | resync_to_next_slice su MB fail | Perde tutti i MB rimanenti della slice | Sostituito con continue (skip+retry) |
| 17 | VFE num_urb_entries=1, >6 dispatch | IS stall al 7° MEDIA_OBJECT | num_urb_entries = dispatch_count (max 48) |
| 18 | Second ring submission media post-MI_FLUSH | IS stall, thread EU non dispatcha | Tutto in singola ring submission |
| 19 | 3D pipeline dal clone: 0 pixel output | State block clone ignorato dalla 3D pipeline | Usare media pipeline (compute) per rasterizzazione |
| 20 | 3D comandi nel ring (non batch): parsati ma non dispatchati | 3DPRIMITIVE nel ring non produce pixel | Sempre usare MI_BATCH_BUFFER_START |
| 21 | MC kernel EOT non rilascia URB entry | URB recycling rotto per MC kernel | IDCT kernel funziona, MC no |
| 22 | gpu_debug_wait_value snooze(500) | Timing benchmark falsato (60ms vs µs) | Busy-wait puro per benchmark |
| 23 | Media pipeline hung → CS morto per tutti | Cube demo riempie ring, HEAD fermo, ring dead | Boot fresco, o ILK_GDSR / kernel ioctl |
| 24 | Ring reset userspace non ripristina CS | HEAD=0 ma CS non riparte dopo disable/re-enable | Solo kernel driver può fare GPU reset |

---

## Milestone chiave

| # | Milestone | Stato |
|---|---|---|
| M1 | Display 1366x768 funzionante | ✅ |
| M2 | 2D BLT acceleration | ✅ |
| M3 | 3D solid fill via render engine | ✅ |
| M4 | Primo kernel EU via media pipeline | ✅ |
| M5 | 48-thread parallel dispatch | ✅ |
| M6 | Aritmetica FP32 bit-exact (SAXPY) | ✅ |
| M7 | Sampler cache + CURBE + libva ABI | ✅ |
| M8 | MPEG-2 IQ kernel | ✅ |
| M9 | MPEG-2 IDCT kernel (2-pass dp4) | ✅ |
| M10 | IQ+IDCT+clamp+U8+Media Block Write | ✅ |
| M11 | Parser MPEG-2 + GPU decode I-frame | ✅ |
| M12 | Primo frame MPEG-2 visibile (PPM) | ✅ |
| M13 | I-frame 100% decode (tutti i file) | ✅ 100%, PSNR 44-50 dB vs ffmpeg |
| M14 | Motion compensation P-frame (CPU) | ✅ MC + parser fix EOB/CBP |
| M14b | Plugin media_kit I+P decode | ✅ 10 frame, 100% MB coverage |
| M14c | GPU IDCT batch benchmark | ✅ 400 blocks, GPU 4× faster than CPU |
| M14d | GPU 3D cube demo (compute rasterizer) | ✅ 480×480, 21 FPS, 48 EU threads |
| M15 | BLT framebuffer blit (bypass app_server) | ⬜ |
| M16 | GPU MC+IDCT P-frame decode completo | ⬜ |
| M17 | Playback MPEG-2 in MediaPlayer | ⬜ |
| M18 | H.264 decode | ⬜ |
| M17 | LLM inference su GPU | ⬜ |
