# Prompt: Intel GPU Acceleration Benchmark & Server su Haiku

Questo prompt contiene tutto il contesto necessario per continuare
il lavoro di accelerazione GPU Intel Gen5 (Ironlake) su Haiku OS.
Usalo per riprendere il lavoro in una nuova sessione.

---

## Contesto hardware e software

- **Laptop**: Sony Vaio VPCEB3K1E
- **CPU**: Intel Core i3 M370 (Arrandale, 2 core / 4 thread)
- **GPU**: Intel HD Graphics device 0x0046 (Ironlake Mobile, Gen5)
- **PCH**: Intel Ibex Peak (IBX) device 0x3b09
- **Display**: 15.5" LVDS 1366x768
- **OS**: Haiku R1~beta5+development (hrev59506)
- **Directory progetto**: `/boot/home/Desktop/Sony Vaio VPCEB3K1E/`

## Stato attuale del progetto

### Cosa funziona

1. **Display LVDS 1366x768** stabile con 4 patch all'accelerant
   intel_extreme (EDID fallback, dual-channel BIOS, watermark IBX,
   panel fitter bypass).

2. **BLT engine 2D**: fill rettangolari, screen-to-screen blit,
   invert, fill span - funzionano quando chiamati direttamente.

3. **Server GPU prototype** (`intel_gfx_server/IntelGfxServer`):
   - Apre `/dev/graphics/intel_extreme_000200` da processo separato
   - Mappa shared_info e registri MMIO via clone_area()
   - Legge registri GPU (RING_CTL, MI_MODE, INSTDONE)
   - Acquisisce ring buffer lock cross-process (benaphore condiviso)
   - Accede alla GTT aperture (lettura/scrittura GPU memory)
   - NON serve kernel driver separato - usa l'intel_extreme esistente

4. **Opcode 3D Gen5 corretti**: tutti i comandi 3D verificati con
   macro GEN5_3D(pipeline, opcode, subopcode) da SNA.

5. **Workaround Gen5 applicati**: MI_MODE VS_TIMER_DISPATCH,
   _3D_CHICKEN2 WM_READ_PIPELINED, CACHE_MODE_0 render cache.

### Cosa NON funziona

1. **3DPRIMITIVE blocca la GPU**: il draw 3D con RECTLIST causa un
   hang hardware - il command streamer si ferma al MI_FLUSH successivo
   e non processa piu' nessun comando. Causa: SF kernel (7 istruzioni
   dal vaapi-driver) non produce output valido per il WM dispatch.
   Il ring buffer rimane bloccato fino al riavvio.

2. **app_server non usa accelerazione 2D**: il codice e' stato
   rimosso in hrev59543 (dicembre 2024). Tutto il rendering e'
   software via AGG + memcpy al framebuffer.

3. **Mesa solo software**: nessun driver DRM per Intel su Haiku.
   Solo llvmpipe (software OpenGL 4.5).

### Scoperta critica recente

Il ring dump ha mostrato che il GPU si blocca al `MI_FLUSH` dopo
`3DPRIMITIVE`:
```
[0x00f0] 0x7b000c04  → 3DPRIMITIVE (RECTLIST)
[0x0108] 0x7a000002  → PIPE_CONTROL
[0x0118] 0x02000000  → MI_FLUSH ← GPU HUNG HERE
```
MI_FLUSH attende che la pipeline 3D dreni, ma 3DPRIMITIVE ha
bloccato la pipeline internamente. Tutti i comandi successivi
(MI_STORE_DATA_IMM, BLT) non vengono mai eseguiti.

**Il file `/boot/home/Desktop/render_test` e' stato rimosso** per
evitare che il draw 3D rotto venga eseguito al boot.

## Task immediati (Fase 1 - test server GPU)

Dopo il riavvio (senza render_test), verificare che il server GPU
funziona correttamente:

```sh
cd "/boot/home/Desktop/Sony Vaio VPCEB3K1E/intel_gfx_server"
make
./IntelGfxServer
```

I 4 test devono passare:
1. Register access (lettura RING_CTL, MI_MODE)
2. Cross-process ring lock (acquire/release benaphore)
3. GPU command execution (MI_STORE_DATA_IMM a scratch GTT 0x10000)
4. BLT fill da processo server (rettangolo giallo al framebuffer)

## Roadmap completa

Vedi `TODO_INTEL_GPU_HAIKU.md` per la roadmap dettagliata (6 fasi,
~5 mesi). L'architettura segue il modello X547/RadeonGfx:

```
Mesa crocus (Gallium Gen4-7)
    |
libdrm2 Intel shim (ioctl DRM → vtable accelerant2)
    |
AccelerantIntel (interfaccia COM-like con QueryInterface)
    |
IntelGfx server (userspace BApplication, IPC client-server)
    |
intel_extreme kernel driver (esistente, PCI + MMIO + GTT)
```

## File chiave nel repository

```
intel_extreme/accelerant/
  render.cpp          - pipeline 3D (state setup, EU kernels, comandi)
  render.h            - opcode Gen5 con macro GEN5_3D()
  engine.cpp          - ring buffer, BLT, batch buffer, sync
  commands.h          - strutture comandi BLT (xy_color_blit, etc.)
  mode.cpp            - mode setting + test visivi
  accelerant.cpp      - init accelerant, render_init()
  Ports.cpp           - patch LVDS (4 fix)
  Pipes.cpp           - watermark IBX

intel_gfx_server/
  IntelGfxServer.cpp  - server GPU prototype (4 test)
  Makefile            - build standalone
  mem_test.cpp        - test accesso memoria GTT
  ring_dump.cpp       - dump ring buffer per debug
  check_marker.cpp    - verifica marker PIPE_CONTROL
  ring_reset.cpp      - reset ring buffer

bench/
  bench_2d.cpp        - benchmark 2D (BView drawing)

REPORT_RENDER_ENGINE.md - report tecnico completo
TODO_INTEL_GPU_HAIKU.md - roadmap 6 fasi con checklist
CROCUS_DRM_IOCTL_ANALYSIS.md - 30 ioctl DRM mappati per crocus
DIFFERENZE_DRIVER.md   - lista patch vs stock Haiku
```

## Informazioni tecniche per il benchmark

### Ring buffer
- Registro base: 0x2030 (RCS)
- Size: 64KB (0x10000)
- Lock: benaphore in shared_info (cross-process)
- GPU memory: 256 MB GTT aperture a 0xFFFFFFFF_A0000000
- Framebuffer: GTT offset 0x21000, 1366x768x32bpp, stride 5504

### Comandi funzionanti dal ring buffer
- MI_STORE_DATA_IMM (0x10400002) - scrive DWORD a indirizzo GGTT
- MI_FLUSH (0x02000000) - serializza pipeline
- XY_COLOR_BLT (0x54200004) - fill rettangolare 32bpp
- XY_SRC_COPY_BLT - blit screen-to-screen

### Comandi 3D (opcode corretti ma draw non funziona)
- PIPELINE_SELECT: 0x69040000
- STATE_BASE_ADDRESS: 0x61010006
- PIPELINED_POINTERS: 0x68000005
- 3DPRIMITIVE: 0x7B000000 ← CAUSA GPU HANG con SF kernel attuale
- PIPE_CONTROL: 0x7A000002

### Benchmark utili
- **CPU fill**: memset su framebuffer, misura bandwidth CPU→VRAM
- **BLT fill**: XY_COLOR_BLT via ring, misura throughput GPU BLT
- **BLT blit**: XY_SRC_COPY_BLT, misura scroll/window move
- **MI_STORE_DATA_IMM**: latenza singolo pixel write via GPU
- **Confronto**: CPU memcpy vs BLT vs MI_STORE per diverse dimensioni

### Pattern di benchmark
```cpp
// Apri device e mappa shared_info (come IntelGfxServer.cpp)
// Poi:

bigtime_t start = system_time();
for (int i = 0; i < iterations; i++) {
    // Submit BLT fill via ring buffer
    acquire_lock(&ring.lock);
    // ... write XY_COLOR_BLT commands ...
    gpu_write32(ring.register_base, ring.position);
    release_lock(&ring.lock);
}
// Wait for GPU idle
// ...
bigtime_t elapsed = system_time() - start;
double mpixels_sec = (double)(width * height * iterations) / elapsed;
```

## Note importanti

- Il ring buffer e' condiviso con app_server. Usare SEMPRE il lock.
- Non sottomettere 3DPRIMITIVE - blocca il GPU (SF kernel rotto).
- HEAD register su Gen5 si aggiorna pigramente - non usare HEAD==TAIL
  come test di completamento. Usare MI_STORE_DATA_IMM a scratch memory.
- Il file `render_test` in Desktop ABILITA il draw 3D rotto. NON crearlo.
- I diagnostic clang (lock.h not found, etc.) sono falsi positivi -
  GCC compila correttamente con gli header privati Haiku.
