# Analisi Render Engine per Accelerazione 2D via Shader
# Intel Ironlake Gen5 (device 0x0046) - Sony Vaio VPCEB3K1E

**Data:** 21 Marzo 2026
**Prerequisito:** Lavoro di accelerazione BLT completato (HWS sync, ottimizzazioni)

---

## 1. STATO ATTUALE: BLT Engine

### Cosa abbiamo implementato
L'accelerazione 2D attuale usa il **BLT engine** (blitter) tramite il ring buffer:

```
app_server --> accelerant hook --> QueueCommands --> Ring Buffer --> BLT Engine
                                                        |
                                                MI_STORE_DWORD_INDEX
                                                        |
                                                   HWS Page (sync)
```

**Operazioni supportate:**
| Operazione | Comando BLT | Performance vs Sistema |
|---|---|---|
| Color Fill | XY_COLOR_BLT | +6% |
| Screen Copy | XY_SRC_COPY_BLT | +7% |
| Invert | XY_COLOR_BLT (ROP 0x55) | +18% |
| Full Clear | XY_COLOR_BLT | +24% |
| Span Fill | XY_SCANLINE_BLIT | (non testato) |

**Limitazioni del BLT engine:**
- Nessun alpha blending (trasparenze)
- Nessuno scaling/rotazione
- Nessuna conversione formato pixel
- Nessun antialiasing
- Nessun gradiente
- Solo operazioni raster (ROP) fisse

Queste limitazioni significano che **tutte le operazioni di compositing**
(trasparenze finestre, ombre, testo antialiased, immagini scalate) vengono
fatte dalla CPU in software, anche se la GPU ha l'hardware per farle.

---

## 2. RENDER ENGINE: COSA PUO FARE

L'Ironlake Gen5 ha una **pipeline 3D completa** con 12 Execution Units (EU)
programmabili. Linux la usa per il 2D tramite il backend SNA di xf86-video-intel.

### 2.1 Hardware disponibile su Gen5

| Componente | Quantita | Funzione |
|---|---|---|
| Execution Units (EU) | 12 | Shader programmabili (vertex + fragment) |
| Thread Dispatcher | 1 | Gestisce fino a 60 thread HW simultanei |
| Sampler | 2 | Lettura texture con filtro bilineare |
| Data Port | 1 | Scrittura framebuffer |
| URB (Unified Return Buffer) | 64KB | Dati temporanei shader |
| L1 Texture Cache | 16KB per sampler | Cache texture |
| L2 Cache | 256KB | Cache condivisa |

### 2.2 Operazioni 2D possibili via Render Engine

| Operazione | BLT | Render | Vantaggio Render |
|---|---|---|---|
| Color fill | Si | Si | Nessuno (BLT e sufficiente) |
| Screen copy | Si | Si | Nessuno per copie semplici |
| **Alpha composite** | **No** | **Si** | Trasparenze finestre, ombre |
| **Scaling bilineare** | **No** | **Si** | Ridimensionamento immagini fluido |
| **Rotazione/trasformazione** | **No** | **Si** | Effetti di rotazione |
| **Gradients (lineari/radiali)** | **No** | **Si** | Sfondi, barre progresso |
| **Conversione formato** | Limitata | **Si** | YUV→RGB per video |
| **Text rendering AA** | **No** | **Si** | Testo subpixel nitido |
| **Porter-Duff blend** | **No** | **Si** | Tutti i 12 modi di compositing |
| **Pattern fill** | Limitata | **Si** | Pattern arbitrari |

### 2.3 Come Linux SNA lo implementa (gen5_render.c)

La pipeline 3D viene configurata con la maggior parte degli stadi **disabilitati**.
Solo il **WM (Windower/Masker = fragment shader)** e il **CC (Color Calculator)**
sono attivi per il compositing 2D:

```
Vertex Input (coordinate rettangolo)
    |
    v
VS (Vertex Shader) -- passthrough minimo
    |
    v
[GS disabilitato] [Clipper minimo]
    |
    v
WM (Fragment Shader) -- CUORE: esegue il compositing
    |                    Legge source texture via Sampler
    |                    Applica operazione blend
    |                    Scrive risultato
    v
CC (Color Calculator) -- modo blend Porter-Duff
    |
    v
Framebuffer Output
```

Il tipo di primitiva usato e **RECTLIST** (3 vertici = 1 rettangolo):
- Vertice 0: top-left (x0, y0, u0, v0)
- Vertice 1: bottom-left (x0, y1, u0, v1)
- Vertice 2: bottom-right (x1, y1, u1, v1)
La GPU inferisce il quarto vertice automaticamente.

---

## 3. ARCHITETTURA PROPOSTA PER HAIKU

### 3.1 Integrazione con il lavoro BLT esistente

Il render engine NON sostituisce il BLT engine — lo **complementa**:

```
app_server richiesta 2D
    |
    +--- Operazione semplice (fill, copy, invert)?
    |       |
    |       v
    |    BLT Engine (codice attuale, veloce)
    |    [QueueCommands + HWS sync]
    |
    +--- Operazione complessa (alpha blend, scale, gradient)?
            |
            v
         Render Engine (nuovo codice)
         [BatchCommands + shader pre-compilati]
```

Il **BatchCommands** che abbiamo gia implementato (Fase 1, attualmente
disabilitato) diventa utile qui: i comandi 3D state sono molti piu DWORD
dei comandi BLT (30-50 DWORD per setup + 3 DWORD per primitiva), quindi
il batch buffer ammortizza meglio l'overhead.

### 3.2 Stato 3D necessario (una tantum per frame)

Per inizializzare la pipeline 3D per operazioni 2D, servono questi comandi
(emessi una volta, poi riusati per multiple primitive):

```
1. PIPELINE_SELECT               -- Seleziona pipeline 3D (1 DWORD)
2. STATE_BASE_ADDRESS             -- Indirizzi base per state heap (4+ DWORD)
3. 3DSTATE_PIPELINED_POINTERS     -- Puntatori a VS/WM/CC state (3 DWORD)
4. 3DSTATE_BINDING_TABLE_PTRS     -- Puntatore a binding table (2 DWORD)
5. 3DSTATE_VERTEX_ELEMENTS        -- Formato vertici (3+ DWORD)
6. 3DSTATE_VERTEX_BUFFERS         -- Buffer vertici (5 DWORD)
```

Poi per ogni rettangolo:
```
7. 3DPRIMITIVE                    -- Draw RECTLIST (3 DWORD)
```

**Totale setup:** ~25-30 DWORD (una volta)
**Per rettangolo:** ~3 DWORD + 3 vertici nel vertex buffer

### 3.3 Strutture state in memoria GPU

Servono allocazioni GPU per:

| Struttura | Dimensione | Contenuto |
|---|---|---|
| VS State | 32B | Vertex shader state (passthrough) |
| WM State | 64B | Fragment shader state + kernel pointer |
| CC State | 64B | Color calculator (blend mode) |
| Binding Table | 16B | Puntatori a surface state (src, dst) |
| Surface State | 32B x 2 | Descrizione source e destination surface |
| WM Kernel | ~256B | Programma shader compilato |
| Vertex Buffer | 4KB | Coordinate rettangoli (riusabile) |
| **Totale** | **~5KB** | Allocato una volta all'init |

### 3.4 Shader pre-compilati

SNA usa shader pre-compilati (codice EU in binario). Per le operazioni 2D
di base servono pochi kernel:

**Kernel 1: Source Copy (formato conversion)**
```
// Pseudocodice EU shader
sample(src_surface, texcoord)
write(dst_surface, position, sampled_color)
```

**Kernel 2: Alpha Composite (Porter-Duff SRC_OVER)**
```
sample(src_surface, texcoord) -> src_color
sample(dst_surface, position) -> dst_color  // solo se serve
result = src_color + dst_color * (1 - src_alpha)
write(dst_surface, position, result)
```

**Kernel 3: Solid Color Fill (con alpha)**
```
// Colore passato come costante
result = blend(constant_color, dst_color, blend_mode)
write(dst_surface, position, result)
```

**Kernel 4: Linear Gradient**
```
t = dot(position, gradient_direction) / gradient_length
color = lerp(color0, color1, clamp(t, 0, 1))
write(dst_surface, position, color)
```

I kernel EU Gen5 sono in formato binario specifico Intel. Si possono:
1. Estrarre da SNA (sono embedded come array uint32[])
2. Scrivere a mano in assembly EU (documentato nel PRM)
3. Compilare con l'assemblatore EU di Mesa (intel_eu_emit)

---

## 4. PIANO DI IMPLEMENTAZIONE

### Fase R1: Infrastruttura (2-3 settimane)

**Obiettivo:** Allocare strutture GPU, emettere comandi state, disegnare
un singolo rettangolo colorato tramite render engine.

1. Allocare 5KB di memoria GPU per state structures
2. Costruire VS/WM/CC state in memoria
3. Scrivere un kernel EU minimo (solid color fill)
4. Emettere sequenza di comandi 3D nel batch buffer
5. Disegnare un rettangolo di test
6. Verificare che il rendering e corretto

**File da creare:**
- `render.cpp` — gestione pipeline 3D e state
- `render.h` — strutture state e definizioni comandi 3D
- `gen5_shader.h` — kernel EU pre-compilati (array binari)

**File da modificare:**
- `hooks.cpp` — aggiungere hook per overlay/composite
- `accelerant.cpp` — init/uninit render state
- `commands.h` — aggiungere comandi 3DSTATE

**Dipendenza dal lavoro BLT:**
- Usa il BatchCommands gia implementato (riabilitarlo per render)
- Usa il HWS sync per sincronizzazione
- Usa init_batch_buffer() per allocazione memoria GPU

### Fase R2: Compositing (2-3 settimane)

**Obiettivo:** Implementare alpha blending per compositing finestre.

1. Aggiungere kernel EU per SRC_OVER blend
2. Implementare surface state per source texture
3. Gestire formati pixel multipli (B_RGBA32, B_RGB32)
4. Hook B_COMPOSITE nell'accelerant
5. Testare con finestre trasparenti

**Impatto visivo:** Trasparenze fluide, ombre sotto le finestre.

### Fase R3: Scaling e Transform (1-2 settimane)

**Obiettivo:** Scaling bilineare per ridimensionamento immagini.

1. Configurare il sampler per filtro bilineare
2. Implementare trasformazione coordinate texture
3. Hook per scaled blit
4. Testare con ridimensionamento immagini in ShowImage

**Impatto visivo:** Ridimensionamento immagini senza pixel visibili.

### Fase R4: Gradients e Text (2-3 settimane)

**Obiettivo:** Gradients lineari/radiali e miglioramento text rendering.

1. Kernel EU per gradiente lineare
2. Kernel EU per gradiente radiale
3. Integrazione con il text rendering di app_server
4. Subpixel antialiasing via shader

**Impatto visivo:** UI piu moderna, testo nitido.

---

## 5. COMANDI 3D GEN5 NECESSARI

### 5.1 Opcode dei comandi (da intel_gpu_commands.h Linux)

```cpp
// Pipeline setup
#define CMD_PIPELINE_SELECT          (0x6104)  // 3D vs Media
#define CMD_STATE_BASE_ADDRESS       (0x6101)  // Base addresses
#define CMD_PIPELINED_POINTERS       (0x6100)  // VS/WM/CC state ptrs

// Binding tables
#define CMD_BINDING_TABLE_PTRS       (0x6801)  // Surface binding

// Vertex setup
#define CMD_VERTEX_BUFFERS           (0x6808)  // VB base/size/stride
#define CMD_VERTEX_ELEMENTS          (0x6809)  // Vertex format

// Drawing
#define CMD_3DPRIMITIVE              (0x7b00)  // Draw call
#define PRIM_RECTLIST                3         // Rectangle list primitive

// Primitive format
// DWORD 0: opcode
// DWORD 1: vertex count, start vertex, instance count
// DWORD 2: start instance, base vertex
```

### 5.2 Registri Surface State Gen5

```cpp
struct gen5_surface_state {
    uint32 dw0;  // surface type, format, tiling
    uint32 dw1;  // base address (GTT offset)
    uint32 dw2;  // width, height
    uint32 dw3;  // pitch, depth
    uint32 dw4;  // multisampling (0 per 2D)
    uint32 dw5;  // reserved
};

// Surface types
#define SURFACE_2D    1
// Surface formats (BGRA per Haiku)
#define FORMAT_B8G8R8A8_UNORM  0x0C0
#define FORMAT_B8G8R8X8_UNORM  0x0C8
```

### 5.3 Registri WM State Gen5

```cpp
struct gen5_wm_state {
    uint32 dw0;  // kernel pointer 0
    uint32 dw1;  // single program flow, binding table count
    uint32 dw2;  // scratch space
    uint32 dw3;  // URB entry size, thread count
    uint32 dw4;  // max threads, stats
    uint32 dw5;  // dispatch mode (16-pixel wide), kernel pointer 1
    uint32 dw6;  // kernel pointer 2
    uint32 dw7;  // reserved
    uint32 dw8;  // kernel start pointer (main entry)
};
```

---

## 6. CONFRONTO PRESTAZIONI ATTESE

### Operazioni che il Render Engine migliora

| Operazione | CPU Software | BLT Engine | Render Engine |
|---|---|---|---|
| Alpha composite 800x600 | ~5 ms | N/A | ~0.5 ms |
| Scale 1920→800 bilineare | ~15 ms | N/A | ~1 ms |
| Gradiente lineare 800x600 | ~3 ms | N/A | ~0.3 ms |
| Glyph composite (testo) | ~0.1 ms/glyph | N/A | ~0.01 ms/glyph |

**Stima impatto:** Il compositing del desktop Haiku usa molto alpha blending
(menu, tooltip, notifiche). Passando da CPU a GPU si riduce il carico CPU
del 30-50% durante operazioni UI intensive.

### Operazioni dove il BLT resta migliore

| Operazione | BLT | Render | Motivo |
|---|---|---|---|
| Fill rettangolo solido | 60K/s | ~40K/s | BLT ha meno setup overhead |
| Screen copy | 9.5K/s | ~7K/s | BLT ha DMA path ottimizzato |
| Invert | 66K/s | N/A | Solo BLT supporta ROP |

Ecco perche i due engine devono **coesistere**: BLT per operazioni semplici,
Render per operazioni complesse.

---

## 7. SFIDE TECNICHE

### 7.1 Context switching BLT ↔ Render

Su Gen5 il ring buffer e condiviso tra BLT e Render. Quando si passa da
comandi BLT a comandi 3D serve un `MI_FLUSH` per evitare conflitti:

```
[comandi BLT]
MI_FLUSH          <-- flush pipeline BLT
PIPELINE_SELECT   <-- switch a 3D
[comandi 3D state setup]
3DPRIMITIVE       <-- draw
MI_FLUSH          <-- flush pipeline 3D
[comandi BLT]     <-- torna a BLT
```

Il context switch ha un costo (~5-10 us). Per minimizzarlo, le operazioni
render dovrebbero essere raggruppate (batch).

### 7.2 Shader EU Gen5 — formato binario

I kernel EU sono in formato binario proprietario Intel. Ogni istruzione e
128 bit (16 byte). Il set di istruzioni include:

- `send` — invia messaggio a unita funzionale (sampler, data port)
- `mov` — copia registro
- `add/mul/mad` — aritmetica
- `sel` — selezione condizionale
- `cmp` — confronto
- `jmp/if/else/endif` — controllo flusso

Per un kernel SRC_OVER minimo servono ~10-15 istruzioni (~200 byte).
I kernel possono essere estratti da SNA (file `brw_wm_kernels.h`) o
scritti con l'assembler EU.

### 7.3 Haiku app_server — hook accelerant

Haiku non ha un hook accelerant standard per compositing. L'app_server
usa `ServerBitmap::HandleComposite()` che e tutto software. Per
accelerare serve:

1. Aggiungere un hook `B_COMPOSITE` all'interfaccia accelerant
2. Modificare app_server per usarlo quando disponibile
3. Fallback software per hardware non supportato

Questo richiede modifiche all'app_server (codice upstream Haiku), non
solo all'accelerant. E il punto piu critico dell'integrazione.

### 7.4 Alternativa: overlay per video

Un approccio piu semplice per il video: usare l'**hardware overlay** del
display engine (attualmente disabilitato per Ironlake in hooks.cpp riga 131).

L'overlay non richiede la pipeline 3D — e un piano hardware separato
che fa scaling YUV→RGB e compositing con il framebuffer. Per il video
playback questo sarebbe sufficiente e molto piu semplice da implementare.

---

## 8. ROADMAP INTEGRATA

```
COMPLETATO (Marzo 2026):
  [x] BLT Engine funzionante
  [x] HWS page sync (+18-24% vs sistema)
  [x] Ottimizzazioni (memcpy, costruzione fuori loop)
  [x] Batch buffer infrastruttura (pronta, disabilitata)
  [x] Benchmark suite (7 test)

PROSSIMO (richiede solo accelerant):
  [ ] Fase R1: Infrastruttura render (state, primo rettangolo)
  [ ] Video overlay hardware (riabilitare per Ironlake)
  [ ] Fase R2: Alpha compositing via shader

FUTURO (richiede modifiche app_server/kernel):
  [ ] Hook B_COMPOSITE in app_server
  [ ] Framebuffer X-Tiled (modifica kernel driver)
  [ ] Fase R3-R4: Scaling, gradients, text rendering
  [ ] Ring buffer 2MB (modifica kernel driver)

LUNGO TERMINE (progetto OS-level):
  [ ] Mesa/Gallium port (DRM equivalente per Haiku)
  [ ] OpenGL 2.1 via i915g driver
```

---

## 9. CONCLUSIONE

L'accelerazione 2D via Render Engine e **fattibile** sull'hardware Ironlake
Gen5, ma richiede:

1. **Conoscenza dell'ISA degli Execution Units** — documentata nel PRM Intel
2. **Tempo di sviluppo significativo** — 2-3 mesi per le Fasi R1-R2
3. **Modifiche all'app_server di Haiku** — per il hook di compositing
4. **Testing accurato** — la pipeline 3D ha molti piu stati del BLT

Il lavoro BLT gia fatto fornisce le **fondamenta**:
- Il batch buffer e l'infrastruttura ideale per i comandi 3D (molti DWORD)
- Il HWS sync funziona anche per comandi 3D
- L'allocazione memoria GPU (intel_allocate_memory) e gia funzionante
- Il framework di benchmark permette di misurare i miglioramenti

Il passo piu utile e immediato (senza modificare app_server) sarebbe
**riabilitare l'hardware overlay per Ironlake** per il video playback,
e poi procedere con la Fase R1 (primo rettangolo via render engine) come
proof of concept.
