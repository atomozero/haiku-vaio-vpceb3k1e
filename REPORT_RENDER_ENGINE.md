# Report: Render Engine 3D Gen5 (Ironlake) su Haiku

**Data:** 5 Aprile 2026 (aggiornato)
**Hardware:** Sony Vaio VPCEB3K1E, Intel HD Graphics 0x0046 (Ironlake Mobile)
**OS:** Haiku R1~beta5+development (hrev59506)

---

## Obiettivo

Implementare accelerazione 2D via pipeline 3D GPU (render engine) per
operazioni che il BLT engine non supporta: alpha blending, scaling,
gradienti. Il BLT engine gestisce solo fill solidi e blit rettangolari.

## Risultati

### Cosa funziona

1. **Display LVDS 1366x768** a 59.9 Hz con 4 patch all'accelerant
   (EDID fallback, dual-channel BIOS, watermark IBX, panel fitter bypass)

2. **BLT engine**: fill rettangolari, screen-to-screen blit, invert,
   fill span. Testato con rettangolo blu durante il mode set.

3. **Command parser 3D attivato**: dopo la correzione di tutti gli opcode
   dei comandi Gen5, il command parser processa comandi Type 3 (3D).
   Confermato da `IPEHR=0x7A000002` (PIPE_CONTROL) nei registri GPU.

4. **Workaround Gen5 applicati**: MI_MODE (VS_TIMER_DISPATCH),
   _3D_CHICKEN2 (WM_READ_PIPELINED), CACHE_MODE_0 (render cache flush).
   Verificati via readback registri.

5. **Ring buffer re-init**: ciclo completo disable/reset HEAD/re-enable
   matching Linux i915 `init_ring_common()`.

6. **State setup completo**: VS, SF, WM, CC state, binding table,
   surface state, vertex buffer, CC viewport. Tutti i puntatori
   verificati via dump diagnostico.

### Cosa non funziona (al 5 Aprile 2026)

1. **3D draw non produce pixel**: i comandi 3D vengono processati dal
   parser (IPEHR lo conferma, INSTDONE=0xFFFFFFFF, nessun errore GPU),
   ma nessun pixel viene scritto al framebuffer.

2. **Causa trovata: MI_PIPELINE_SELECT aveva type bits errati**.
   Il define aveva `(0x1 << 29)` che imposta type=001, ma PIPELINE_SELECT
   e un comando MI che richiede type=000 (bits[31:29]=0). Il GPU non
   riconosceva il comando e non passava mai dalla pipeline BLT alla 3D.
   Tutti i comandi 3D pipelined venivano ignorati silenziosamente.
   Fix: `CMD_PIPELINE_SELECT = (0x01 << 23)` = 0x00800000 (matching
   Linux i915 `MI_INSTR(0x01, 0)` e SNA `MI_PIPELINE_SELECT`).

3. **PIPE_CONTROL Write Immediate**: non scriveva il marker 0xDEADBEEF
   perche la pipeline 3D non era attiva (conseguenza del bug #2).

**Stato dopo la fix PIPELINE_SELECT**: da verificare al prossimo riavvio.

## Bug trovati e corretti

### MI_PIPELINE_SELECT type bits (critico — root cause)

`CMD_PIPELINE_SELECT` aveva `(0x1 << 29) | (0x01 << 23)` = `0x20800000`,
impostando type=001 (bits[31:29]). Ma PIPELINE_SELECT e un comando MI
che richiede type=000. Senza questo comando riconosciuto, la GPU restava
in modalita BLT e tutti i comandi 3D pipelined venivano ignorati.

Corretto a `(0x01 << 23)` = `0x00800000` (matching Linux i915 e SNA).

### Opcode dei comandi 3D

Quasi tutti gli opcode dei comandi 3D avevano encoding errato. Il campo
SubOpcode (bits[23:16]) era posizionato nel campo Opcode (bits[26:24]),
causando l'invio di comandi non riconosciuti che il parser scartava
silenziosamente.

Introdotto il macro `GEN5_3D(pipeline, opcode, subopcode)` allineato
a xf86-video-intel SNA:

Tabella completa verificata contro Mesa gen5.xml, brw_defines.h e
Linux i915 intel_gpu_commands.h:

| Comando | Sbagliato | Corretto | Errore |
|---|---|---|---|
| PIPELINE_SELECT | `0x00800000` (MI!) | `0x69040000` (3D SubType=1) | Non e MI su Gen5 |
| PIPELINED_POINTERS | `0x68000005` (SubType=1) | `0x78000005` (SubType=3) | SubType errato |
| DRAWING_RECTANGLE | `0x69000002` (SubType=1) | `0x79000002` (SubType=3) | SubType errato |
| BINDING_TABLE_PTRS | `0x69010000` (SubType=1) | `0x78010000` (SubType=3) | SubType+Opcode |
| VERTEX_BUFFERS | `0x68080000` (SubType=1) | `0x78080000` (SubType=3) | SubType errato |
| VERTEX_ELEMENTS | `0x68090000` (SubType=1) | `0x78090000` (SubType=3) | SubType errato |
| URB_FENCE | `0x60050000` (SubOp=5) | `0x60000000` (SubOp=0) | SubOpcode errato |
| CS_URB_STATE | `0x61000000` (Op=1,SubOp=0) | `0x60010000` (Op=0,SubOp=1) | Op/SubOp invertiti |

Comandi gia corretti: STATE_BASE_ADDRESS (`0x61010006`), 3DPRIMITIVE
(`0x7B000000`), PIPE_CONTROL (`0x7A000002`), PRIM_RECTLIST (`0x0F`).

### STATE_BASE_ADDRESS opcode

Il define `CMD_STATE_BASE_ADDRESS` aveva valore `0x69000006` che decodifica
come `3DSTATE_DRAWING_RECTANGLE` con lunghezza sbagliata, non come
`STATE_BASE_ADDRESS` (`0x61010006`). Risultato: le basi degli indirizzi
non venivano mai impostate e un DRAWING_RECTANGLE malformato con clip
rect (1,0)-(1,0) veniva emesso, clippando tutto.

### SF state bitfield positions (7 campi sbagliati)

Tutti i campi dello SF state (DW3, DW4, DW6) avevano shift errati
rispetto a `brw_structs.h` (`brw_sf_unit_state`):
- DW3: `dispatch_grf_start_reg` bits[3:0] non [4:1],
  `urb_entry_read_offset` bits[9:4] non [10:5],
  `urb_entry_read_length` bits[16:11] non [18:12]
- DW4: `nr_urb_entries` bits[17:11] non [16:9],
  `urb_entry_allocation_size` bits[23:19] non [22:18]
- DW6: `dest_org_vbias` bits[12:9] non [25:22],
  `dest_org_hbias` bits[16:13] non [19:16]

### WM state bit positions

- `thread_dispatch_enable` era al bit 29 (DW5) - corretto al bit 19
- `max_threads` era in DW4 - corretto in DW5 bits[31:25]
- `binding_table_entry_count` deve essere 0 su Ironlake (requisito HW)
- `dispatch_grf_start_reg` corretto da 1 a 3 (matching SNA)
- `grf_reg_count` corretto da 0 a 2

### RECTLIST vertex order

Ordine vertici era (left,bottom), (left,top), (right,bottom) — due vertici
con lo stesso X. SNA usa (right,bottom), (left,bottom), (left,top) e il
rasterizer inferisce il quarto vertice (right,top).

### SEND instruction msg_reg_nr

Le istruzioni EU SEND (SF URB_WRITE e WM FB_WRITE) avevano `msg_reg_nr=0`
(bits[27:24] di DW0) quando doveva essere 1. Il GPU leggeva i dati
dal MRF m0 (non inizializzato) invece che da m1 (dove scriviamo header
e colori).

## Architettura implementata

### File modificati

- **render.h**: definizioni comandi 3D, surface state, vertex element,
  WM dispatch mode, strutture stato render engine
- **render.cpp**: init render engine, SF/WM kernel binari, state setup,
  fill rettangolo via pipeline 3D, color patching, diagnostica GPU
- **engine.cpp**: toggle render engine via file trigger, debug tracing
- **mode.cpp**: test visivi (CPU fill, BLT fill, 3D fill), diagnostica
  registri GPU, readback pixel, output su render_diag.txt

### Pipeline 3D (sequenza comandi)

```
MI_FLUSH (serializzazione BLT→3D)
MI_LOAD_REGISTER_IMM × 3 (workaround Gen5)
PIPELINE_SELECT = 3D
STATE_BASE_ADDRESS (basi a 0, puntatori assoluti GTT)
PIPELINED_POINTERS (VS, GS=off, CLIP=off, SF, WM, CC)
URB_FENCE (VS: 256 entries, SF: 64 entries)
CS_URB_STATE
DRAWING_RECTANGLE (clip rect = framebuffer)
BINDING_TABLE_POINTERS (VS=0, GS=0, CLIP=0, SF=0, WM=binding table)
VERTEX_BUFFERS (3 vertici × 2 float, pitch=8)
VERTEX_ELEMENTS (R32G32_FLOAT + Z=0 + W=1.0)
3DPRIMITIVE (RECTLIST, 3 vertici)
PIPE_CONTROL (marker diagnostico)
MI_FLUSH (serializzazione 3D→BLT)
```

### EU Kernel binari

**SF kernel** (7 istruzioni, da intel-vaapi-driver): computa delta
attributi (dA/dx, dA/dy) via MATH_INV e scrive al URB con msg_length=4
e SWIZZLE_TRANSPOSE.

**WM kernel** (6 istruzioni, custom): carica colore RGBA come float
immediati (patchati per ogni draw), copia header thread a m1, invia
FB_WRITE SIMD8 con EOT. Il colore viene convertito da BGRA uint32 a
4 float e scritto direttamente nelle istruzioni MOV del kernel.

### State block layout (1024 byte in GPU memory)

```
0x000  VS state (64 B)
0x040  WM state (64 B)
0x080  CC state (64 B)
0x0C0  CC viewport (64 B)
0x100  Binding table (64 B)
0x140  Surface state dst (32 B)
0x160  Surface state src (32 B)
0x180  SF state (64 B)
0x1C0  SF kernel (128 B, 112 usati)
0x240  WM kernel (128 B, 96 usati)
0x300  Vertex buffer (256 B)
```

## Limiti strutturali di Haiku

### app_server non usa accelerazione 2D hardware

Dal 2013 il flag `USE_ACCELERATION` era hardcoded a 0. A dicembre 2024
(commit `03f77fd7d9db`, incluso in hrev59506) tutto il codice di
accelerazione 2D hardware e stato rimosso da `AccelerantHWInterface.cpp`
(467 righe eliminate).

Motivazioni ufficiali:
- Conflitto con double buffering (flickering)
- Alpha blending richiede lettura da buffer CPU (VRAM lenta in lettura)
- CPU moderne sufficienti per memset/memcpy
- Accelerazione moderna via OpenGL/Vulkan

### Mesa solo software

Haiku non ha un framework DRM/KMS. Mesa funziona solo con llvmpipe
(software rendering via LLVM). Non esistono driver hardware Mesa per
Intel (i965, iris, crocus) su Haiku. OpenGL 4.5 disponibile ma
interamente su CPU.

### Nessun path per accelerazione GPU

```
app_server → Painter (AGG, software) → back buffer (CPU malloc)
           → memcpy → framebuffer

Mesa → llvmpipe (software) → nessun accesso GPU hardware
```

Non esiste attualmente un percorso funzionante per usare il GPU Intel
per il rendering 2D o 3D su Haiku.

## Ecosistema GPU di Haiku: l'approccio X547

X547 (X512) e' l'unico sviluppatore che ha portato accelerazione GPU
hardware su Haiku, con un'architettura che evita completamente DRM/KMS:

```
Mesa (radeonsi/radv/nvk)
    |
libdrm2 (shim: traduce ioctl DRM → vtable accelerant2)
    |
accelerant2 (API COM-like con QueryInterface, vtable C/C++)
    |
GPU Server (RadeonGfx / nvidia-haiku, userspace BApplication)
    |
Kernel driver (minimale: PCI, MMIO, shared_info)
```

### Componenti chiave

**libdrm2** (github.com/X547/libdrm2):
Reimplementazione di libdrm che intercetta le chiamate DRM e le instrada
all'accelerant2.  Modalita' server: su `amdgpu_device_initialize()` carica
l'add-on accelerant2, ottiene i vtable `AccelerantDrm` e `AccelerantAmdgpu`,
e delega tutte le operazioni (buffer alloc, VA mapping, command submit,
sync objects) al server GPU.  Implementato: GEM buffer create/close/map/
export/import, virtual address mapping, command submission, sync objects.
Non implementato: KMS/mode-setting (gestito separatamente via VideoStreams).

**accelerant2** (github.com/X547/accelerant2):
API accelerant di nuova generazione con pattern COM-like.  Interfacce:
- `AccelerantDrm` ("drm/v1"): mmap, buffer handle, sync objects completi
- `AccelerantAmdgpu` ("amdgpu/v1"): info, buffer alloc/map, command submit
- `AccelerantDisplay`: VideoStreams consumer per CRTC, cursore
Supporta sia vtable C che classi virtuali C++.

**RadeonGfx** (github.com/X547/RadeonGfx):
Server GPU completo per AMD GCN 1.0 (Cape Verde).  Architettura:
- Kernel driver minimale (PCI, MMIO, shared_info)
- Server userspace BApplication: firmware, MC, ring buffer, command submit
- IPC client-server via PortLink/ThreadLink (stile app_server)
- Ring buffer hardware diretto (scrive a GPU memory-mapped)
- Memory manager: VRAM, GTT, page table 2-level per client
- Isolamento per-client: handle table, AddressSpace, VM pool

**VideoStreams** (github.com/X547/VideoStreams):
Equivalente di GBM + wl_buffer per Haiku.  Producer/Consumer con SwapChain.
Buffer reference via `area_id` (CPU) o `fd` + fence (GPU, equivalente
DMA-BUF).  Compositing multi-surface con dirty region tracking.

### Stato attuale dei driver GPU su Haiku

| GPU | Driver | 3D | Vulkan | Stato |
|---|---|---|---|---|
| NVIDIA Turing+ | nvidia-haiku (X547) | Zink OpenGL | NVK | v0.0.2, funzionante |
| AMD GCN 1.0 | RadeonGfx (X547) | radeonsi | RADV | Sperimentale |
| Intel Gen2-Gen12 | intel_extreme (Haiku) | No | No | Solo display |
| Qualsiasi | Mesa llvmpipe | Software | Lavapipe | Funzionante |

### Roadmap: Intel Gen5 nell'ecosistema X547

**Cosa servirebbe per portare Intel Gen5 nello stack X547:**

1. **Kernel driver** (`intel_gfx`): minimale - PCI, MMIO mapping,
   shared_info.  Basabile sull'attuale `intel_extreme` kernel driver
   gia' funzionante.

2. **Server GPU** (`IntelGfx`): gestione GTT (globale su Gen5, non
   per-process come AMDGPU), ring buffer RCS, batch buffer submission,
   fencing via HWS page.  Piu' semplice di RadeonGfx perche' Gen5 ha
   GTT globale e un solo ring.

3. **Interfaccia accelerant2**: `AccelerantIntel` con operazioni
   GEM-like (buffer create/map, execbuffer submit, wait).

4. **libdrm2 Intel shim**: `libdrm_intel` o adattamento del winsys
   `crocus` (Gen4-7 Gallium driver) per chiamare AccelerantIntel
   invece di `DRM_IOCTL_I915_*`.

5. **Mesa winsys crocus**: adattare `src/gallium/winsys/crocus/drm/`
   per usare il libdrm2 shim al posto dei DRM ioctl Linux.

**Semplificazioni rispetto a RadeonGfx:**
- Gen5 ha GTT globale (niente page table per-client)
- Un solo ring buffer (RCS) vs. multipli (GFX, SDMA, etc.)
- Nessun firmware GPU da caricare
- Hardware ben documentato (PRM Intel pubblici)

**Complessita' stimate:**
- Kernel driver: 1-2 settimane (adattamento intel_extreme esistente)
- Server GPU: 4-8 settimane (GTT, ring buffer, batch submit, fencing)
- libdrm2 + winsys: 2-4 settimane
- Integrazione Mesa crocus: 2-4 settimane
- **Totale: 2-4 mesi per uno sviluppatore esperto**

### Blocchi tecnici

1. **Nessuna astrazione Intel in libdrm2**: tutto e' AMDGPU-specifico
   (ioctl numbers, strutture, semantica).  Serve un layer Intel parallelo.

2. **Mesa crocus** chiama `DRM_IOCTL_I915_GEM_CREATE`,
   `DRM_IOCTL_I915_GEM_EXECBUFFER2`, etc. - completamente diversi
   da AMDGPU.  Serve un winsys adapter dedicato.

3. **Gen5 e' vecchio**: crocus lo supporta ma e' il target piu' basso.
   La community potrebbe non essere interessata a mantenerlo.

4. **X547 non ha hardware Intel**: nessuno sta lavorando su Intel
   nell'ecosistema GPU Haiku.

## Lavoro futuro sul render engine 3D

### Priorita 1: verificare fix PIPELINE_SELECT

La fix `CMD_PIPELINE_SELECT = (0x01 << 23)` dovrebbe risolvere il
problema principale.  Dopo il riavvio, verificare:
- PIPE_CONTROL marker = 0xDEADBEEF (3D pipeline attiva)
- Rettangolo rosso visibile nel test a (270,50)-(370,150)

### Se il 3D produce pixel ma con artefatti

1. **SF kernel**: il kernel SF dal vaapi-driver potrebbe non corrispondere
   al vertex format (2 float position-only senza UV).  Alternativa:
   usare l'approccio SNA con texture 1x1 + kernel WM con sampling.

2. **WM kernel SIMD16**: SNA usa SIMD16, non SIMD8.  Potrebbe essere
   necessario per il dispatch corretto dei thread WM su Ironlake.

3. **Validazione EU encoding**: le istruzioni EU Gen5 sono derivate
   dal bitfield layout di brw_eu.h.  Senza un assembler EU o un
   reference binary verificato, l'encoding potrebbe avere errori
   sottili nei campi src/dst dei MOV e SEND.

## Statistiche

- **37 commit** nel repository
- **~4100 righe aggiunte**, 86 rimosse
- **4 patch display** funzionanti e testate (LVDS 1366x768 stabile)
- **1 render engine** con pipeline 3D attiva (comandi processati)
  ma draw non ancora funzionale (SF/WM kernel)
- **~15 riavvii** per test e debug della pipeline 3D
- **Bug opcode critico** trovato e corretto (tutti i comandi Gen5)

## Riferimenti

- xf86-video-intel SNA gen5_render.c (reference per opcode e state setup)
- intel-vaapi-driver exa_sf.g4b.gen5 (SF kernel binary)
- Linux i915 init_render_ring() (workaround Gen5)
- X547/libdrm2, X547/accelerant2, X547/RadeonGfx (architettura GPU Haiku)
- Haiku AccelerantHWInterface.cpp commit 03f77fd7d9db (rimozione 2D HW accel)
- Intel Gen5 (Ironlake) PRM Vol 1-4
