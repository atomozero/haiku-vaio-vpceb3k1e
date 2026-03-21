# Piano di Accelerazione 2D - Intel Ironlake Gen5
# Sony Vaio VPCEB3K1E (device 0x0046)

**Data:** 21 Marzo 2026
**Baseline benchmark (driver sistema):** FillRect ~66K/s, CopyBits ~9.2K/s, FullClear ~65K/s
**Baseline benchmark (driver patchato):** FillRect ~60K/s, CopyBits ~9.5K/s, FullClear ~52K/s
**Obiettivo:** Superare il driver di sistema su tutti i test, stabilita assoluta

---

## Architettura Attuale

```
CPU (app_server) --> Lock ring --> Scrivi comando MMIO --> Aggiorna TAIL --> Unlock
                                       |
                                       v
                              Ring Buffer (64KB)
                                       |
                                       v
                              GPU BLT Engine --> Framebuffer Lineare
```

**Problemi:**
1. Ogni comando = lock + polling MakeSpace + write DWORD-by-DWORD + unlock
2. Framebuffer lineare = accesso cache inefficiente per rettangoli
3. Sync tramite polling HEAD register = busy-wait su ogni wait_engine_idle

---

## Architettura Obiettivo

```
CPU (app_server) --> Scrivi in Batch Buffer (WC) --> MI_BATCH_BUFFER_START nel ring
                           |                                    |
                           v                                    v
                    Batch Buffer (GTT, 64KB)            Ring Buffer (2MB)
                           |                                    |
                           v                                    v
                    GPU BLT Engine --> Framebuffer X-Tiled + Fence Register
                           |
                           v
                    MI_STORE_DWORD_INDEX --> HWS Page (sequence number)
                           |
                           v
                    CPU legge seq# dalla HWS (no polling HEAD)
```

---

## FASE 1: Batch Buffer System

### Obiettivo
Eliminare l'overhead per-comando del ring buffer. Scrivere i comandi BLT
in un buffer GTT (veloce, write-combining) e sottometterli con un singolo
`MI_BATCH_BUFFER_START` nel ring.

### Prerequisiti
- Allocare un buffer GTT per il batch (64KB)
- Definire MI_BATCH_BUFFER_START (0x31 << 23 = 0x62000000) e MI_BATCH_BUFFER_END (0x0A << 23 = 0x14000000)

### File da modificare

**intel_extreme.h** - Aggiungere define:
```cpp
#define MI_BATCH_BUFFER_START    (0x31 << 23)
#define MI_BATCH_BUFFER_END      (0x0A << 23)
#define MI_STORE_DWORD_INDEX     (0x21 << 23)
#define MI_NOOP                  0x00000000
```

**accelerant.h** - Aggiungere struttura batch_buffer:
```cpp
struct batch_buffer {
    addr_t      base;           // indirizzo virtuale (WC mapped)
    uint32      offset;         // offset GTT per la GPU
    uint32      size;           // dimensione totale (64KB)
    uint32      position;       // posizione di scrittura corrente
};
```
Aggiungere `batch_buffer primary_batch;` in `intel_shared_info`.

**engine.cpp** - Nuova classe BatchCommands:
```cpp
class BatchCommands {
public:
    BatchCommands(batch_buffer &batch, ring_buffer &ring);
    ~BatchCommands();  // Scrive MI_BATCH_BUFFER_END, poi MI_BATCH_BUFFER_START nel ring

    void Put(struct command &command, size_t size);
    void Write(uint32 data);

private:
    batch_buffer &fBatch;
    ring_buffer  &fRing;
    uint32       fStartPosition;
};
```

Il distruttore:
1. Scrive `MI_BATCH_BUFFER_END` nel batch
2. Allinea a 8 byte con MI_NOOP
3. Acquisisce il lock del ring
4. Scrive `MI_BATCH_BUFFER_START` + indirizzo batch nel ring
5. Aggiorna TAIL del ring
6. Rilascia il lock
7. Resetta la posizione del batch

### Funzioni BLT aggiornate
Le funzioni `intel_fill_rectangle`, `intel_screen_to_screen_blit`, etc.
useranno `BatchCommands` invece di `QueueCommands`:
```cpp
void intel_fill_rectangle(engine_token* token, uint32 color,
    fill_rect_params* params, uint32 count)
{
    BatchCommands batch(gInfo->shared_info->primary_batch,
                        gInfo->shared_info->primary_ring_buffer);

    xy_color_blit_command blit(false);
    blit.color = color;

    for (uint32 i = 0; i < count; i++) {
        blit.dest_left = params[i].left;
        blit.dest_top = params[i].top;
        blit.dest_right = params[i].right + 1;
        blit.dest_bottom = params[i].bottom + 1;
        batch.Put(blit, sizeof(blit));
    }
}
// ~BatchCommands() auto-submits via MI_BATCH_BUFFER_START
```

### Vantaggi
- Scrittura batch in memoria WC = ~10x piu veloce di MMIO per-DWORD
- Un solo lock/unlock del ring per N comandi (invece di 1 per batch)
- Nessun polling MakeSpace per ogni comando singolo
- Il ring buffer viene usato solo per MI_BATCH_BUFFER_START (2 DWORD)

### Test
```
Benchmark PRIMA (QueueCommands):     FillRect ~60K/s
Benchmark DOPO  (BatchCommands):     FillRect target >100K/s
```

### Rollback
Mantenere `QueueCommands` come fallback. Flag `use_batch_buffer` in shared_info
per switch runtime.

---

## FASE 2: Framebuffer X-Tiled

### Obiettivo
Convertire il framebuffer da lineare a X-Tiled per migliorare la cache
locality del BLT engine e ridurre la banda di memoria del display engine.

### Cos'e X-Tiling
```
Lineare:                         X-Tiled:
Riga 0: pixel 0,1,2,3,...       Tile 0 (4KB):
Riga 1: pixel 0,1,2,3,...         Riga 0: 128 pixel (512B)
Riga 2: pixel 0,1,2,3,...         Riga 1: 128 pixel (512B)
...                                ...
                                   Riga 7: 128 pixel (512B)
                                 Tile 1 (4KB):
                                   Riga 0: pixel 128-255
                                   ...
```
Pixel adiacenti verticalmente sono nello stesso tile (4KB) = stessa
cache line della GPU. Per operazioni rettangolari questo e molto meglio.

### Prerequisiti
- Fence register libero (Gen5 ne ha 16)
- Stride deve essere potenza di 2 per tiling
- Allineamento: base del framebuffer allineata alla dimensione della regione tiled

### Problema dello stride
1366 pixel * 4 byte = 5464, allineato a 64 = 5504 byte.
Per X-tiling lo stride deve essere potenza di 2: **8192 byte** (2048 pixel).
Questo significa allocare piu memoria per il framebuffer (8192 * 768 = 6.3MB
invece di 5504 * 768 = 4.2MB), ma la GPU ha 256MB+ di apertura.

### File da modificare

**Kernel driver (intel_extreme.cpp)** - Allocare framebuffer tiled:
- Calcolare stride come potenza di 2
- Allocare framebuffer con allineamento necessario
- Programmare un fence register:
```cpp
// FENCE register format per Gen5 (I915_FENCE):
// Bit 0: Valid
// Bit 1: X-Tile (0) / Y-Tile (1) -- per noi 0
// Bit 11:4: Pitch (in tile widths, 128B increments per X-tile)
// Bit 31:12: Start address (4KB aligned)
// Bit 43:32: End address (4KB aligned) -- in secondo DWORD per 64-bit

uint32 fence_pitch_val = (stride / 128) - 1; // stride in 128B units
uint64 fence_val = start_addr | (fence_pitch_val << 2) | FENCE_VALID;
// Scrivere in FENCE_REG_SANDYBRIDGE_0 + n*8 per Gen5
```

**Accelerant (mode.cpp)** - Aggiornare set_display_mode:
- Impostare `DISPLAY_CONTROL_TILED` (bit 10) in DSPCNTR
- Usare stride tiled nel registro DSPSTRIDE

**Accelerant (commands.h)** - Aggiungere flag tiling nei comandi BLT:
```cpp
// Nel costruttore xy_command, se il framebuffer e tiled:
if (gInfo->shared_info->frame_buffer_tiled) {
    // XY_COLOR_BLT: bit 11 = dst tiling
    opcode |= (1 << 11);
    // Lo stride nei comandi BLT per tiled e in DWORD, non byte
    dest_bytes_per_row = gInfo->shared_info->bytes_per_row >> 2;
}
```

**Accelerant (commands.h)** - Per XY_SRC_COPY_BLT con tiling:
```cpp
// bit 15 del DWORD 0 = src tiling
if (gInfo->shared_info->frame_buffer_tiled) {
    opcode |= (1 << 15);  // source tiled
    source_bytes_per_row = dest_bytes_per_row;  // gia in DWORD
}
```

### Rischi
- **ALTO**: Stride potenza di 2 richiede modifica del kernel driver
  (allocazione framebuffer diversa)
- **MEDIO**: Fence register deve essere configurato correttamente o
  si ottiene corruzione totale dello schermo
- **BASSO**: Applicazioni che accedono al framebuffer via CPU vedranno
  pattern tiled senza fence -- servono fence per accesso CPU lineare

### Test
```
Benchmark PRIMA (lineare):     FullClear ~65K/s, ~164 MB/s
Benchmark DOPO  (X-Tiled):    FullClear target >100K/s, >250 MB/s
```

### Rollback
Flag `frame_buffer_tiled` in shared_info. Se false, tutto funziona
come prima (lineare).

---

## FASE 3: Hardware Status Page Sync

### Obiettivo
Eliminare il busy-wait polling del ring HEAD register in
`intel_wait_engine_idle()`. Usare sequence number nella HWS page.

### Meccanismo
1. Dopo ogni batch di comandi, inserire `MI_STORE_DWORD_INDEX`:
   - Scrive un sequence number incrementale in una posizione nota della HWS page
2. In `intel_wait_engine_idle()`:
   - Leggere il sequence number dalla HWS page (cached, velocissimo)
   - Se sequence_number >= last_submitted, la GPU ha finito
   - Nessun polling MMIO del registro HEAD

### File da modificare

**accelerant.h** - Aggiungere contatore:
```cpp
// In intel_shared_info:
vint32      last_submitted_seq;    // ultimo seq# inviato alla GPU
// La HWS page e gia allocata: status_page / physical_status_page
// Usiamo status_page->store[0] per il nostro sequence number
```

**engine.cpp** - Modificare BatchCommands::~BatchCommands():
```cpp
// Prima di MI_BATCH_BUFFER_END, aggiungere:
uint32 seq = atomic_add(&gInfo->shared_info->last_submitted_seq, 1) + 1;
Write(MI_STORE_DWORD_INDEX);
Write(0);  // offset 0 nella HWS page (store[0])
Write(seq);
Write(MI_BATCH_BUFFER_END);
```

**engine.cpp** - Modificare intel_wait_engine_idle():
```cpp
void intel_wait_engine_idle()
{
    hardware_status* hws = (hardware_status*)gInfo->shared_info->status_page;
    uint32 target = gInfo->shared_info->last_submitted_seq;
    bigtime_t start = system_time();

    while (hws->store[0] < target) {
        if (system_time() > start + 1000000LL) {
            ERROR("GPU timeout waiting for seq %d (current %d)\n",
                target, hws->store[0]);
            break;
        }
        spin(1);  // micro-wait, molto piu leggero del polling MMIO
    }
}
```

### Vantaggi
- Lettura HWS page = lettura memoria cached (1-2 ns)
- Lettura HEAD register = MMIO uncached (~100-500 ns)
- Riduzione latenza sync di 50-100x

### Test
```
Benchmark sync-heavy (molti Sync() calls):
PRIMA (polling HEAD):    latenza ~500us per sync
DOPO  (HWS seq#):       latenza target <50us per sync
```

### Rollback
Se hws->store[0] non viene aggiornato dalla GPU, fallback al polling
HEAD tradizionale.

---

## FASE BONUS: Ring Buffer 2MB

### Obiettivo
Aumentare il ring buffer da 64KB (16 pagine) a 2MB (512 pagine) per
ridurre la frequenza di stalli ring-full.

### Modifica
Nel kernel driver, cambiare l'allocazione:
```cpp
// DA: 16 pagine (64KB)
// A:  512 pagine (2MB) -- massimo supportato da Gen5
```

Il registro RING_BUFFER_CONTROL bits 20:12 supporta fino a 0x1FF = 511
pagine = ~2MB.

### Rischio: BASSO
Il ring buffer piu grande riduce solo la probabilita di stallo.
Non cambia la logica di funzionamento.

---

## Ordine di Implementazione

| Fase | Descrizione | Rischio | Impatto | Dipendenze |
|------|-------------|---------|---------|------------|
| **BONUS** | Ring Buffer 2MB | Basso | Basso | Nessuna |
| **3** | HWS Sync | Basso | Medio | Nessuna |
| **1** | Batch Buffer | Medio | **Alto** | Fase 3 (opzionale) |
| **2** | X-Tiling | Alto | **Alto** | Modifica kernel |

Ordine consigliato: BONUS → 3 → 1 → 2

Le Fasi BONUS e 3 sono indipendenti, a basso rischio, e migliorano
l'infrastruttura. La Fase 1 e il cambio piu impattante per le prestazioni.
La Fase 2 richiede modifiche al kernel driver e va affrontata per ultima.

---

## Protocollo di Test

### Benchmark suite
Usare `bench/bench_2d` con 3 run per ogni configurazione.
Aggiungere test specifici per sync latency.

### Matrice di test

| Test | Cosa misura | Impattato da |
|------|-------------|-------------|
| FillRect (5000 random) | Throughput fill + overhead comando | Fase 1 |
| CopyBits (2000 random) | Throughput blit + overhead comando | Fase 1 |
| InvertRect (5000) | Throughput invert + overhead | Fase 1 |
| FullClear (500 fullscreen) | Bandwidth pura memoria GPU | Fase 2 |
| SyncLatency (1000 sync) | Overhead sincronizzazione | Fase 3 |
| SmallRects (10000 16x16) | Overhead per-comando (domina) | Fase 1 |
| LargeRects (100 800x600) | Bandwidth pura (domina) | Fase 2 |

### Nuovi test da aggiungere a bench_2d.cpp

```cpp
// Test 5: Sync latency
start = system_time();
for (int i = 0; i < 1000; i++) {
    SetHighColor(i % 256, 0, 0);
    FillRect(BRect(0, 0, 10, 10));
    Sync();
}
elapsed = system_time() - start;
printf("SyncLatency: 1000 syncs in %lld us (%.1f us/sync)\n",
    elapsed, (double)elapsed / 1000.0);

// Test 6: Small rects (overhead dominated)
start = system_time();
for (int i = 0; i < 10000; i++) {
    FillRect(BRect(rand()%780, rand()%580, rand()%780+16, rand()%580+16));
}
Sync();
elapsed = system_time() - start;
printf("SmallRects: 10000 rects in %lld us (%.1f rects/sec)\n",
    elapsed, 10000.0 * 1000000.0 / elapsed);

// Test 7: Large rects (bandwidth dominated)
start = system_time();
for (int i = 0; i < 100; i++) {
    SetHighColor(rand()%256, rand()%256, rand()%256);
    FillRect(bounds);
}
Sync();
elapsed = system_time() - start;
printf("LargeRects: 100 fills in %lld us (%.1f MB/s)\n",
    elapsed, 100.0 * WINDOW_W * WINDOW_H * 4.0 / elapsed);
```

### Criteri di successo

| Metrica | Baseline (sistema) | Target |
|---------|-------------------|--------|
| FillRect | ~66K/s | >100K/s (+50%) |
| CopyBits | ~9.2K/s | >15K/s (+63%) |
| FullClear | ~65K/s (164 MB/s) | >100K/s (250+ MB/s) |
| SyncLatency | TBD | <50 us/sync |
| SmallRects | TBD | >150K/s |
| Stabilita | Nessun crash | Zero crash in 24h |

---

## Rischi e Mitigazioni

### GPU Hang
**Causa:** Comandi malformati, batch buffer corrotto, tiling errato
**Mitigazione:** Mantenere sempre backup dell'accelerant funzionante.
Aggiungere timeout e reset del ring in caso di stallo.
```bash
# Rollback emergenza:
cp intel_extreme.accelerant.backup \
   /boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant
shutdown -r
```

### Corruzione display
**Causa:** Fence register errato, stride tiled sbagliato, DSPCNTR non aggiornato
**Mitigazione:** Implementare X-Tiling come ultima fase. Testare prima
con superficie offscreen tiled prima di convertire il framebuffer.

### Regressione prestazioni
**Causa:** Overhead batch buffer per operazioni singole piccole
**Mitigazione:** Soglia minima: usare batch solo quando count > 4.
Per operazioni singole, mantenere path diretto ring buffer.

---

## Riferimenti

### Documentazione Intel
- Intel Open Source HD Graphics PRM Volume 1 (Gen5/Ironlake)
- BSpec: BLT Engine Command Streamer
- Intel GPU Commands: MI_BATCH_BUFFER_START, MI_STORE_DWORD_INDEX

### Codice Linux
- `drivers/gpu/drm/i915/gt/intel_gpu_commands.h` -- opcode commands
- `drivers/gpu/drm/i915/gt/intel_ggtt_fencing.c` -- fence registers
- `xf86-video-intel/src/sna/sna_blt.c` -- BLT 2D acceleration
- `xf86-video-intel/src/sna/kgem.c` -- batch buffer management

### Codice Haiku
- `intel_extreme/accelerant/engine.cpp` -- implementazione attuale
- `intel_extreme/accelerant/commands.h` -- strutture comandi BLT
- `intel_extreme/accelerant/accelerant.h` -- strutture condivise
- `headers/private/graphics/intel_extreme/intel_extreme.h` -- registri HW
