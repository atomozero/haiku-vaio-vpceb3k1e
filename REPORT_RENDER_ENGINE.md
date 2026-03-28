# Report: Render Engine 3D Gen5 (Ironlake) su Haiku

**Data:** 28 Marzo 2026
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

### Cosa non funziona

1. **3D draw non produce pixel**: i comandi 3D vengono processati dal
   parser (IPEHR lo conferma, INSTDONE=0xFFFFFFFF, nessun errore GPU),
   ma nessun pixel viene scritto al framebuffer.

2. **Causa probabile**: il SF kernel (Setup/Rasterizer) non genera gli
   span di dispatch per il WM (fragment shader). Il kernel SF dal
   vaapi-driver (7 istruzioni: math_inv + delta computation + URB_WRITE)
   potrebbe non corrispondere al vertex format usato, oppure il WM kernel
   (6 istruzioni: MOV immediati + FB_WRITE SIMD8) ha errori di encoding.

3. **PIPE_CONTROL Write Immediate**: non scrive dati in memoria. Potrebbe
   richiedere flag aggiuntivi su Gen5 o un pipeline state completamente
   valido per funzionare.

## Bug trovati e corretti

### Opcode dei comandi 3D (critico)

Quasi tutti gli opcode dei comandi 3D avevano encoding errato. Il campo
SubOpcode (bits[23:16]) era posizionato nel campo Opcode (bits[26:24]),
causando l'invio di comandi non riconosciuti che il parser scartava
silenziosamente.

Introdotto il macro `GEN5_3D(pipeline, opcode, subopcode)` allineato
a xf86-video-intel SNA:

| Comando | Prima (sbagliato) | Dopo (corretto) |
|---|---|---|
| PIPELINE_SELECT | `0x61040000` | `0x69040000` |
| VERTEX_BUFFERS | `0x68000000` | `0x68080000` |
| VERTEX_ELEMENTS | `0x69000000` | `0x68090000` |
| 3DPRIMITIVE | `0x78000000` | `0x7B000000` |
| BINDING_TABLE_PTRS | `0x69000001` (2 DW) | `0x69010000` (6 DW) |
| URB_FENCE | `0x02800000` (MI Type 0!) | `0x60050000` (3D Type 3) |
| CS_URB_STATE | `0x00800000` (MI Type 0!) | `0x61000000` (3D Type 3) |

### STATE_BASE_ADDRESS opcode

Il define `CMD_STATE_BASE_ADDRESS` aveva valore `0x69000006` che decodifica
come `3DSTATE_DRAWING_RECTANGLE` con lunghezza sbagliata, non come
`STATE_BASE_ADDRESS` (`0x61010006`). Risultato: le basi degli indirizzi
non venivano mai impostate e un DRAWING_RECTANGLE malformato con clip
rect (1,0)-(1,0) veniva emesso, clippando tutto.

### WM state bit positions

- `thread_dispatch_enable` era al bit 29 (DW5) - corretto al bit 19
- `max_threads` era in DW4 - corretto in DW5 bits[31:25]
- `binding_table_entry_count` deve essere 0 su Ironlake (requisito HW)
- `dispatch_grf_start_reg` corretto da 1 a 3 (matching SNA)
- `grf_reg_count` corretto da 0 a 2

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

## Lavoro futuro (se Haiku aggiunge DRM o modifica app_server)

1. **SF kernel**: il kernel SF del vaapi-driver potrebbe necessitare
   adattamento al vertex format (2 float position-only senza UV) o
   sostituzione con un kernel che produca URB output minimo valido.

2. **WM kernel SIMD16**: SNA usa SIMD16, non SIMD8. Potrebbe essere
   necessario per il corretto dispatch dei thread WM su Ironlake.

3. **Texture sampling approach**: SNA usa un kernel WM che campiona
   una texture 1x1 con il colore solido, invece di MOV immediati.
   Richiede sampler state e source surface state aggiuntivi.

4. **DRM layer**: per Mesa hardware serve un layer DRM/KMS su Haiku.
   X547 ha fatto lavoro preliminare per NVIDIA (nvidia-haiku) ma
   con architettura completamente diversa (ioctl + GSP firmware).

## Statistiche

- **36 commit** nel repository
- **3897 righe aggiunte**, 86 rimosse (diff da main)
- **4 patch display** funzionanti e testate
- **1 render engine** infrastruttura completa ma non ancora funzionale
- **~15 riavvii** per test e debug della pipeline 3D
