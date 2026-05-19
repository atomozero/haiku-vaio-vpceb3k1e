# Differenze rispetto ai driver originali Haiku
# Sony Vaio VPCEB3K1E — 19 Maggio 2026 (aggiornato)

Questo documento elenca tutte le modifiche apportate ai driver
`intel_extreme` (GPU) e `hda` (audio) rispetto alla versione
originale inclusa in Haiku R1~beta5 (hrev59506).

---

## 1. GPU — intel_extreme accelerant

Il driver accelerant originale e in:
`/boot/system/add-ons/accelerants/intel_extreme.accelerant` (194179 byte)

Il nostro driver patchato e in:
`/boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant`

### 1.1 Ports.cpp — LVDSPort::IsConnected()

**Problema:** Su piattaforme PCH (Ironlake+), la lettura EDID via GMBUS
DDC fallisce su molti pannelli LVDS. Il driver considera la porta "non
connessa" e cade in fallback VESA a 1024x768.

**Fix:** Aggiunta catena di fallback dopo il fallimento di HasEDID():
1. Prova VESA EDID dal bootloader (`has_vesa_edid_info`)
2. Fallback a dati VBT (`got_vbt`) — contiene 1366x768 dal BIOS
3. Ultimo fallback: porta abilitata dal BIOS (bit `LVDS_PORT_EN`)

La stessa logica esisteva gia per Gen<=4 ma mancava nel path PCH.

### 1.2 Ports.cpp — LVDSPort::SetDisplayMode()

**Problema:** La configurazione dual/single channel LVDS veniva derivata
dal divisore P2 del PLL, potenzialmente sovrascrivendo la configurazione
BIOS corretta e causando schermo nero.

**Fix:** Preserva la configurazione BIOS leggendo il bit `LVDS_CLKB_POWER_UP`
dal registro LVDS corrente. Riferimento: fix di Michael Forney per il
driver igfx di 9front.

### 1.3 Ports.cpp — LVDSPort::SetDisplayMode()

**Problema:** Il panel fitter veniva abilitato incondizionatamente, anche
quando la risoluzione richiesta corrispondeva a quella nativa del pannello.
Con 1366 pixel (non divisibile uniformemente), il panel fitter a 1:1
introduceva artefatti visivi.

**Fix:** Disabilitare il panel fitter quando `needsScaling` e false
(risoluzione nativa).

### 1.4 Pipes.cpp — Pipe::Enable()

**Problema:** I watermark del display venivano configurati solo per
PCH Cougar Point (CPT/Sandy Bridge), non per Ibex Peak (IBX/Ironlake).

**Fix:** Aggiunto `INTEL_PCH_IBX` alla condizione watermark.

### 1.5 engine.cpp — QueueCommands (HWS Sync)

**Problema:** `intel_wait_engine_idle()` usava polling MMIO del registro
HEAD del ring buffer (~100-500ns per lettura), causando latenza eccessiva
durante la sincronizzazione GPU.

**Fix:** Implementato sync tramite Hardware Status Page:
- Ogni 8 comandi, emette `MI_STORE_DWORD_INDEX` che scrive un sequence
  number nella HWS page
- `intel_wait_engine_idle()` legge la HWS page (memoria cached, ~1-2ns)
  e verifica se il sequence number ha raggiunto il target
- Fallback automatico a polling HEAD se la HWS non e disponibile

### 1.6 engine.cpp — Ottimizzazioni BLT

- Costruzione comandi BLT fuori dal loop (opcode/mode/stride costanti)
- Fast path `memcpy` in `QueueCommands::Put()` per comandi senza wrap
- Fix variabile `flush` non inizializzata nel distruttore
- Fix `intel_fill_span`: aggiunta chiamata `queue.Put()` mancante nel loop

### 1.7 engine.cpp — Batch Buffer (infrastruttura, disabilitato)

Implementata classe `BatchCommands` e funzioni `init_batch_buffer()`/
`uninit_batch_buffer()` per sottomissione comandi via `MI_BATCH_BUFFER_START`.
Attualmente disabilitata nelle funzioni BLT perche l'overhead per
operazioni singole (count=1) supera il beneficio.

### 1.8 render.cpp/render.h — Render Engine 3D Gen5

Implementato rendering 2D via pipeline 3D Gen5 (Ironlake):
- Re-inizializzazione ring buffer (disable/reset HEAD/re-enable) matching
  Linux i915 `init_ring_common()` — necessario per comandi Type 3
- Workaround Gen5: MI_MODE, _3D_CHICKEN2, CACHE_MODE_0
- Macro `GEN5_3D(pipeline, opcode, subopcode)` per encoding comandi
- SF kernel da intel-vaapi-driver (7 istruzioni: delta attributi + URB_WRITE)
- WM kernel solid fill (6 istruzioni: MOV immediati RGBA + FB_WRITE SIMD8)
- Color patching: converte BGRA uint32 a 4 float e patcha il kernel binario
- URB_FENCE + CS_URB_STATE per partizionamento URB (VS:256, SF:64)
- State setup: VS/SF/WM/CC, binding table, surface state, CC viewport
- Funzione `render_fill_rect()` con sequenza comandi completa:
  MI_FLUSH → MI_LRI workarounds → PIPELINE_SELECT → STATE_BASE_ADDRESS →
  URB_FENCE → PIPELINED_POINTERS → DRAWING_RECTANGLE → BINDING_TABLE_PTRS →
  VERTEX_BUFFERS → VERTEX_ELEMENTS → 3DPRIMITIVE → PIPE_CONTROL → MI_FLUSH
- Diagnostica GPU: dump INSTDONE/IPEIR/IPEHR/EIR, PIPE_CONTROL marker

**Bug critici corretti:**
- `CMD_STATE_BASE_ADDRESS` era `0x69000006` (DRAWING_RECTANGLE) anziche
  `0x61010006` — le basi indirizzi non venivano mai impostate
- `CMD_PIPELINE_SELECT` aveva `(0x1<<29)` impostando type=001 anziche
  type=000 (MI) — la GPU non entrava mai in modalita 3D
- Tutti gli opcode 3D avevano SubOpcode nel campo Opcode (bit positions errate)
- SEND `msg_reg_nr` era 0 in SF e WM kernel (doveva essere 1)

### 1.9 hooks.cpp — Fill Span abilitato

Il hook `B_FILL_SPAN` era disabilitato (`return NULL`). Riabilitato
ora che il bug nel loop di `intel_fill_span` e stato corretto.

### 1.10 hooks.cpp — Overlay abilitato per Ironlake

`INTEL_GROUP_ILK` rimosso dalla blacklist overlay. L'overlay hardware
legacy (MI_OVERLAY_FLIP) e supportato su Gen3-Gen5. I registri overlay
sono allocati correttamente dal kernel driver.

### 1.11 intel_extreme.h — Nuove definizioni

- `MI_STORE_DWORD_INDEX`, `MI_BATCH_BUFFER_START`, `MI_BATCH_BUFFER_END`
- `HWS_SYNC_SEQUENCE_INDEX`
- `DISPLAY_CONTROL_TILED` (bit 10 di DSPCNTR)
- `INTEL_FENCE_BASE_965` (0x03000), `INTEL_FENCE_BASE_GEN6` (0x100000)
- Costanti fence register (FENCE_REG_VALID, FENCE_REG_PITCH_SHIFT)

### 1.12 memory.cpp — Allocazione con alignment

Aggiunto overload `intel_allocate_memory(size, alignment, flags, base)`
che propaga il parametro di allineamento al kernel GART allocator.

### 1.13 accelerant.h — Stato tiling

Aggiunti campi `frame_buffer_tiled` e `fence_register_index` alla
struct `accelerant_info` (privata dell'accelerant, NON in `intel_shared_info`
per evitare ABI break con il kernel driver).

### 1.14 media_pipeline.cpp/h — Media Pipeline & EU Kernel Dispatch

Implementata infrastruttura completa per il compute engine Gen5 via
MEDIA_OBJECT command:
- **gpu_bo allocator** (gpu_bo.cpp/h): buffer GTT con alloc/free/write/clear
- **Batch writer** (batch_writer in gen_ops.h): accumula comandi DWORD
  in array statico, poi sottomette via ring
- **10-command preamble**: MI_FLUSH → DEPTH_BUFFER_NULL → PIPELINE_SELECT →
  URB_FENCE → STATE_BASE_ADDRESS → MEDIA_STATE_POINTERS → CS_URB_STATE →
  CONSTANT_BUFFER → N × MEDIA_OBJECT → MI_FLUSH
- **Surface state**: SURFTYPE_BUFFER per OWord R/W, SURFTYPE_2D per Media Block
- **CURBE**: fino a 30 GRF push-to-thread via CONSTANT_BUFFER
- **Marker system**: MI_STORE_DATA_IMM scrive tag in marker_bo per debug/sync
- **Kernel dispatch**: fino a 48 EU thread paralleli, URB recycling

**EU kernels implementati** (kernels/):
- `idct_single.g4a` — IDCT standalone S16→S16 (109 istruzioni)
- `idct_to_u8.g4a` — IDCT + clamp + Media Block Write U8
- `iq_idct_intra.g4a` — IQ + IDCT combinato per I-frame
- `mc_forward.g4a` — P-frame forward MC (Media Block Read/Write)

**Benchmark**: GPU IDCT 4× piu veloce della CPU su 400 blocchi 8×8.

### 1.15 gpu_ring.cpp/h — Ring Submission via Kernel Ioctl

Layer di ring submission indipendente dalla generazione. Gestisce:
- Apertura device, clone shared_info e registri
- Sync con TAIL hardware (mai RING_RESET — uccide il CS)
- `gpu_ring_begin/emit/advance`: scrittura comandi + TAIL kick via
  `INTEL_RING_WRITE_TAIL` ioctl (unico modo per scrivere MMIO da userspace)
- `gpu_ring_submit`: sottomette array di comandi pre-costruito
- `gpu_ring_wait_idle`: attende che HEAD raggiunga TAIL

Usato da media_pipeline.cpp, test standalone, e gpu_idct nel plugin.

### 1.16 gen_ops.h, gen5/6/7_ops.cpp — Astrazione Multi-Generazione

Vtable `gen_ops` con function pointer per emissione comandi
generazione-specifica:
- `emit_pipeline_select_media/3d`, `emit_state_base_address`,
  `emit_urb_fence`, `emit_mi_flush`, `emit_constant_buffer`,
  `emit_media_state_pointers`, `emit_cs_urb_state`, markers
- `init_gen_ops(ops, generation)` seleziona automaticamente
- **Gen5** (Ironlake): testato, STATE_BASE_ADDRESS 8DW, MI_FLUSH
- **Gen6** (Sandy Bridge): non testato, STATE_BASE_ADDRESS 10DW, PIPE_CONTROL
- **Gen7** (Ivy Bridge/Haswell): non testato

Guida contributor: `PORTING.md`.

### 1.17 gpu_debug.cpp/h — Diagnostica GPU

- Dump registri: INSTDONE, IPEHR, ACTHD, EIR, ESR
- `gpu_debug_wait_value()`: busy-wait su indirizzo GTT per marker sync
- Ring health check: confronto HEAD/TAIL, detect stall

### 1.18 DRM Shim per Mesa crocus

Libreria `libdrm_shim.so` che traduce ioctl DRM Linux in ioctl
intel_extreme Haiku:
- **GEM**: CREATE/CLOSE/MMAP/BUSY/WAIT, SET_DOMAIN, MADVISE
- **GEM_EXECBUFFER2**: inline batch nel ring + MI_STORE_DATA_IMM marker +
  TAIL ioctl. Relocation patching, EXEC_HANDLE_LUT, EXEC_BATCH_FIRST.
- **GETPARAM/GET_APERTURE**: chipset_id=0x0046, aperture size
- **SET_TILING/GET_TILING**: tiling mode tracking
- **CONTEXT**: CREATE/DESTROY stub, GETPARAM/SETPARAM

Mesa 25.3.3 compilata con driver crocus (Gallium), CrocusRenderer addon
per Haiku GL stack. OpenGL 2.1, GLSL 1.20.

### Prestazioni GPU vs driver originale

| Test | Originale | Patchato | Differenza |
|------|-----------|----------|------------|
| FillRect | 57,294/s | 67,316/s | **+18%** |
| CopyBits | 9,614/s | 9,648/s | = |
| InvertRect | 56,715/s | 61,943/s | **+9%** |
| FullClear | 70,827/s | 71,412/s | +1% |
| SyncLatency | 22.4 us | 19.2 us | **+14%** |
| SmallRect 16x16 | 52,093/s | 48,902/s | -6% |
| LargeRect | 176 MB/s | 162 MB/s | -8% |

---

## 2. Audio — HDA kernel driver

Il driver originale e in:
`/boot/system/add-ons/kernel/drivers/bin/hda` (dal pacchetto haiku)

Il nostro driver patchato e installato via HPKG:
`/boot/system/packages/hda_patched-1.0-1-x86_64.hpkg`

Con `BlockedEntries` in `/boot/system/settings/packages` per
sovrascrivere il driver di sistema.

### 2.1 hda_codec.cpp — Quirk Sony Vaio ALC269 (VREF fix)

**Problema:** Il quirk globale `HDA_QUIRK_IVREF` imposta VREF_80 su
tutti i pin con capacita VREF. Il pin 0x19 dell'ALC269 e logicamente
disabilitato ma elettricamente connesso al percorso audio. VREF_80
causa crosstalk/distorsione sugli speaker interni.

**Fix:** Aggiunto quirk specifico per Sony (vendor 0x104d) con ALC269:
```cpp
{ SONY_VENDORID, HDA_ALL, REALTEK_VENDORID, 0x0269,
    HDA_QUIRK_SONY_VAIO, HDA_QUIRK_IVREF },
```
Dopo il tree build, invia `SET_PIN_WIDGET_CONTROL(0x19, VREF_GRD)`
per impostare il pin a VREF Ground. Equivalente al fix Linux
`ALC269_FIXUP_SONY_VAIO` in `patch_realtek.c`.

### 2.2 hda_codec.cpp — Override tipo widget per selettori input ALC269

**Problema:** I widget NID 0x23 e 0x24 (selettori input per ADC) sono
riportati dall'hardware come "Vendor Defined" (tipo 15). La funzione
`hda_widget_find_input_path()` li ignora nel `default: return false`,
causando `build input tree failed` e microfono non funzionante.

**Fix:** Aggiunto caso nel `switch(codec_id)` per ALC269:
```cpp
case 0x10ec0269:
    if (nodeID == 0x23 || nodeID == 0x24)
        widget.type = WT_AUDIO_SELECTOR;
    break;
```

### Risultato HDA

- Quirks: 0x4000 (HDA_QUIRK_SONY_VAIO, IVREF rimosso)
- Mic interno (NID 18) e mic esterno (NID 24) rilevati
- Automute cuffie/speaker gia presente nel driver base
- Audio funzionante senza distorsione

---

## 3. MPEG-2 Decoder Plugin (media_kit)

Plugin `mpeg2_decoder.so` per il media_kit di Haiku. Decodifica video
MPEG-2 con accelerazione GPU opzionale.

Installato in: `~/config/non-packaged/add-ons/media/plugins/`

### 3.1 mpeg2_decoder_plugin.cpp — Plugin DecoderPlugin

- Registra formato `B_MPEG_2_VIDEO` via BMediaFormats
- Output `B_YCbCr420` (Y + Cb + Cr planes separati)
- Decode I, P e B frame con motion compensation half-pel
- **Batch IDCT**: accumula blocchi 8×8, flush via GPU (fallback CPU)
- Reference frame management: I/P → forward ref, shift a backward

### 3.2 gpu_idct.cpp/h — GPU IDCT per il Plugin

Reimplementazione standalone della pipeline media per uso dal plugin:
- Apre `/dev/graphics/intel_extreme_000200`, clona shared_info e registri
- Alloca buffer GTT: kernel, CURBE, input (S16 coeff), output (S16 IDCT),
  batch (comandi), VFE state, IDRT, surface state, binding table
- Batch GPU: preamble 10 comandi + N MEDIA_OBJECT, sottomesso via
  MI_BATCH_BUFFER_START nel ring + TAIL ioctl
- Kernel EU `idct_single.g4b.gen5`: IDCT 2-pass dp4, output S16
- Fallback CPU automatico se GPU non disponibile
- Thread-safe: usa benaphore del ring buffer (condiviso con app_server)

### 3.3 mpeg2_parser.cpp/h — Parser Bitstream MPEG-2

Parser completo ISO/IEC 13818-2:
- Sequence header, picture header, picture coding extension, slice header
- DC VLC (Table B-12/B-13), AC VLC (Table B-14 completa, 112 entries)
- Table B-15 (intra_vlc_format=1), Table B-9 CBP (64 entries)
- Macroblock decode: I, P, B frame (Table B-2/B-3/B-4)
- Motion vector decode (Table B-10 + f_code expansion)
- IQ inline, mismatch control, error recovery (skip+continue)

### 3.4 mpeg2_viewer — Viewer Standalone

Viewer BWindow per file .m2v. Decode I+P frame, YCbCr→RGB32 (BT.601).

---

## 4. File aggiunti (non presenti nell'originale)

| File | Descrizione |
|------|-------------|
| **Accelerant — Render Engine** | |
| `intel_extreme/accelerant/render.h` | Header render engine 3D per 2D |
| `intel_extreme/accelerant/render.cpp` | Infrastruttura render engine |
| **Accelerant — Media Pipeline & Compute** | |
| `intel_extreme/accelerant/media_pipeline.cpp/h` | Media pipeline: EU dispatch, IDCT, compute |
| `intel_extreme/accelerant/gpu_bo.cpp/h` | GPU buffer object allocator (GTT) |
| `intel_extreme/accelerant/gpu_ring.cpp/h` | Ring submission via kernel ioctl |
| `intel_extreme/accelerant/gpu_debug.cpp/h` | Diagnostica GPU (registri, marker) |
| `intel_extreme/accelerant/gen_ops.h` | Vtable multi-generazione |
| `intel_extreme/accelerant/gen5_ops.cpp` | Implementazione Gen5 (testata) |
| `intel_extreme/accelerant/gen6_ops.cpp` | Implementazione Gen6 (non testata) |
| `intel_extreme/accelerant/gen7_ops.cpp` | Implementazione Gen7 (non testata) |
| `intel_extreme/accelerant/idct_ref.h` | IDCT CPU reference + cosine table GPU |
| `intel_extreme/accelerant/iq_intra_ref.h` | IQ CPU reference per MPEG-2 |
| `intel_extreme/accelerant/mpeg2_parser.cpp/h` | Parser MPEG-2 bitstream |
| `intel_extreme/accelerant/commands.h` | Definizioni comandi HW (MI, 3D, MEDIA) |
| **Accelerant — EU Kernels** | |
| `intel_extreme/accelerant/kernels/idct_single.g4a` | IDCT standalone S16→S16 |
| `intel_extreme/accelerant/kernels/idct_to_u8.g4a` | IDCT + clamp → U8 |
| `intel_extreme/accelerant/kernels/iq_idct_intra.g4a` | IQ + IDCT combinato |
| `intel_extreme/accelerant/kernels/mc_forward.g4a` | MC forward (half-pel) |
| **MPEG-2 Plugin** | |
| `intel_extreme/mpeg2_plugin/mpeg2_decoder_plugin.cpp` | Plugin media_kit I+P+B |
| `intel_extreme/mpeg2_plugin/gpu_idct.cpp/h` | GPU IDCT standalone per plugin |
| `intel_extreme/mpeg2_plugin/mpeg2_viewer.cpp` | Viewer standalone .m2v |
| **Test & Tools** | |
| `intel_extreme/accelerant/tests/gpu_idct_bench.cpp` | Benchmark GPU vs CPU IDCT |
| `intel_extreme/accelerant/tests/gpu_plasma_demo.cpp` | Plasma animato via GPU IDCT |
| `intel_extreme/accelerant/tests/gpu_triangle.cpp` | 3D TRILIST + BLT demo |
| `intel_extreme/accelerant/tests/test_mc_decode.cpp` | Multi-frame I+P decoder |
| `intel_extreme/tools/gen4asm/` | Port gen4asm assembler EU |
| `tools/ring_health.sh` | Diagnostica ring buffer GPU |
| `tools/test_suite.sh` | Test suite & regression runner |
| **DRM Shim / Mesa** | |
| `intel_extreme/accelerant/libdrm_shim.cpp` | Shim DRM ioctl per Mesa crocus |
| **Documentazione** | |
| `intel_extreme/ANALISI_TECNICA_DRIVER.md` | Analisi comparativa Haiku vs Linux |
| `intel_extreme/ANALISI_RENDER_ENGINE_2D.md` | Piano render engine via shader |
| `intel_extreme/PIANO_ACCELERAZIONE_2D.md` | Piano ottimizzazione BLT |
| `intel_extreme/X_TILING_NOTE.md` | Note implementazione X-Tiling |
| `intel_extreme/X_TILING_BUG_ANALYSIS.md` | Analisi bug X-Tiling (ABI break) |
| `hda/ANALISI_HDA_ALC269.md` | Analisi codec audio ALC269 |
| `bench/bench_2d.cpp` | Benchmark 2D (7 test) |
| `VPCEB3K1E_Haiku_Driver_Report.md` | Report compatibilita hardware |
| `REPORT_RENDER_ENGINE.md` | Report render engine 3D Gen5 |
| `TODO_INTEL_GPU_HAIKU.md` | Master TODO con milestone tracking |
| `PORTING.md` | Guida: aggiungere supporto nuova generazione GPU |
| `DIFFERENZE_DRIVER.md` | Questo documento |

---

## 5. Lavoro X-Tiling (non completato)

L'X-Tiling e stato tentato e documentato ma non completato:
- Fence register a 0x03000 funziona (read/write verificato)
- Allocazione GART con allineamento 8MB funziona
- La corruzione e causata da una race condition: `program_pipe_color_modes()`
  clears il bit DSPCNTR tiled, creando una finestra in cui il display
  engine legge lineare da dati tiled
- Il fix richiede integrazione del tiled bit dentro `program_pipe_color_modes`
  basandosi su `gInfo->frame_buffer_tiled`

---

## 6. Come ripristinare i driver originali

### GPU
```bash
rm /boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant
# Il sistema caricherà il driver dal pacchetto haiku
```

### HDA
```bash
rm /boot/system/packages/hda_patched-1.0-1-x86_64.hpkg
# Rimuovere le righe hda da /boot/system/settings/packages
# Il sistema caricherà il driver dal pacchetto haiku
```
