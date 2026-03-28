# TODO: Accelerazione GPU Intel Gen5 su Haiku (modello X547)

**Obiettivo:** OpenGL hardware-accelerated su Intel Ironlake (Gen5) via
Mesa crocus, seguendo l'architettura di X547 (RadeonGfx/libdrm2/accelerant2).

**Hardware target:** Intel HD Graphics 0x0046 (Ironlake Mobile, Gen5)
**Riferimento architetturale:** X547/RadeonGfx + X547/libdrm2 + X547/accelerant2

---

## Fase 0: Preparazione e studio (1-2 settimane)

### 0.1 Studio dell'architettura X547
- [x] Clonare e studiare X547/RadeonGfx (server GPU, ring buffer, memory manager)
- [x] Clonare e studiare X547/libdrm2 (shim DRM → accelerant2)
- [x] Clonare e studiare X547/accelerant2 (API COM-like, vtable C/C++)
- [x] Clonare e studiare X547/VideoStreams (buffer passing producer/consumer)
- [x] Analizzare build system RadeonGfx: usa meson + subprojects
      (SADomains, Locks, ThreadLink) + libaccelerant + libdrm + VideoStreams.
      Kernel driver usa Makefile standard Haiku.  Non compilato per
      dipendenze mancanti, ma struttura chiara dal sorgente.

### 0.2 Studio del driver Intel i915 Linux
- [x] Studiare il winsys crocus (crocus_bufmgr.c, crocus_batch.c, crocus_fence.c)
- [x] Documentare tutti i DRM_IOCTL_I915_* usati da crocus
      → 30 ioctl totali, 8 essenziali (Tier 1), 11 raccomandati (Tier 2)
      → Analisi completa in intel_extreme/CROCUS_DRM_IOCTL_ANALYSIS.md
- [x] Studiare GEM object management in i915 (create, mmap, execbuffer)
- [x] Studiare la gestione GTT globale Gen5 (vs PPGTT Gen6+)
- [x] Studiare il batch buffer submission (execbuffer2)
      → Formato: exec_object2[] + relocation_entry[] + execbuffer2 header
      → Gen5: no context (rsvd1=0), no PPGTT, global GTT offsets
      → Batch structure: commands + MI_STORE_DWORD_INDEX + MI_BATCH_BUFFER_END

### 0.3 Documentazione hardware Gen5
- [x] Documentare il register map GTT (PGTBL_CTL 0x2020)
- [x] Documentare il ring buffer RCS (TAIL 0x2030, HEAD 0x2034, START 0x2038, CTL 0x203C)
- [x] Documentare il formato batch buffer e relocation
      → Gen5: call-style nesting (1 level), no chain
      → Relocation: global GTT offsets, presumed_offset caching
- [x] Localizzare Intel Ironlake PRM (disponibili online):
      Vol 1 Part 1: Graphics Core
      Vol 1 Part 2: MMIO, Ring Buffer, Commands
      Vol 1 Part 3: Memory Interface, Render Engine, 2D BLT
      Vol 4 Part 2: URB, Message Gateway
      Mirror: https://kiwitree.net/~lina/intel-gfx-docs/prm/ilk/
      Ufficiale: https://www.intel.com/content/www/us/en/docs/graphics-for-linux/developer-reference/1-0/intel-core-processor-2010.html
      GitHub: https://github.com/Igalia/intel-osrc-gfx-prm

---

## Fase 1: Accesso GPU dal server userspace (1 settimana)

**Decisione architetturale:** NON serve un kernel driver separato.
Il kernel driver `intel_extreme` esistente fornisce gia' tutto:
MMIO registers, GTT aperture, ring buffer, HWS page.  Il server GPU
accede al device esistente `/dev/graphics/intel_extreme_*` via ioctl
`INTEL_GET_PRIVATE_DATA` e `clone_area()`, come fa l'accelerant.

### 1.1 Struttura progetto IntelGfx
- [ ] Creare directory `intel_gfx_server/`
- [ ] Struttura base: BApplication con thread di ascolto IPC
- [ ] Aprire `/dev/graphics/intel_extreme_*` come device fd
- [ ] Ottenere shared_info via `ioctl(INTEL_GET_PRIVATE_DATA)`
- [ ] Clonare aree: shared_info, registers, graphics_memory

### 1.2 Verifica accesso hardware
- [ ] Leggere registri MMIO (device ID, GTT size, ring status)
- [ ] Verificare accesso GTT aperture (leggere/scrivere pixel framebuffer)
- [ ] Verificare che il ring buffer lock funzioni cross-process
- [ ] Test: scrivere un pixel al framebuffer via MI_STORE_DATA_IMM
      dal server (non dall'accelerant)

### 1.3 Gestione conflitti con accelerant
- [ ] L'accelerant (in app_server) e il server GPU condividono:
  - Ring buffer RCS (serializzato via benaphore in shared_info)
  - GTT aperture (allocazioni separate, no overlap)
  - Registri MMIO (letture concorrenti ok, scritture serializzate)
- [ ] Definire protocollo di allocazione GTT (server alloca dalla fine,
      accelerant dall'inizio, o usa allocatore condiviso)
- [ ] Test: BLT fill da accelerant + MI_STORE da server, verificare
      che non si corrompano

---

## Fase 2: Server GPU `IntelGfx` (4-8 settimane)

Server userspace BApplication che gestisce il GPU.  Piu' semplice di
RadeonGfx perche' Gen5 ha GTT globale e un solo ring.

### 2.1 Struttura base
- [ ] BApplication con loop messaggi
- [ ] Apertura device `/dev/graphics/intel_gfx/0`
- [ ] Mapping dei registri MMIO dal shared_info
- [ ] Shutdown pulito con rilascio risorse

### 2.2 GTT Manager
- [ ] Leggere GTT size dal registro PGTBL_CTL (0x2020)
- [ ] Allocatore di GTT entries (bitmap o free list)
- [ ] Funzione: alloca N pagine contigue nel GTT
- [ ] Funzione: mappa pagine fisiche → GTT entries
- [ ] Funzione: libera GTT entries
- [ ] NOTA: Gen5 usa GTT globale, non per-process PPGTT

### 2.3 GEM Object Manager
- [ ] Struttura `IntelBufferObject`: size, GTT offset, CPU mapping, handle
- [ ] `gem_create()`: alloca area Haiku + GTT entries
- [ ] `gem_mmap()`: mappa area in address space del client
- [ ] `gem_close()`: libera area + GTT entries
- [ ] Handle table per-client (modello RadeonGfx TeamState)
- [ ] Reference counting per oggetti condivisi

### 2.4 Ring Buffer Manager
- [ ] Inizializzazione ring RCS (disable → reset HEAD → enable)
- [ ] Workaround Gen5 (MI_MODE, _3D_CHICKEN2, CACHE_MODE_0)
- [ ] Funzione: scrivi comandi al ring (gestione wrap-around)
- [ ] Funzione: aggiorna TAIL per submit
- [ ] Funzione: attendi completamento (poll HEAD o HWS sequence)
- [ ] MI_STORE_DWORD_INDEX per sequence number in HWS page

### 2.5 Batch Buffer Execution
- [ ] `execbuffer()`: ricevi batch buffer dal client
  - Validare handle degli oggetti referenziati
  - Applicare relocations (patch indirizzi GTT nel batch)
  - Emettere MI_BATCH_BUFFER_START nel ring
  - Registrare fence/sequence number
- [ ] Supporto per lista di buffer objects (exec_object2)
- [ ] Gestione delle dipendenze (wait su fence precedenti)

### 2.6 Fencing e Sync
- [ ] Sequence number via HWS page
- [ ] `wait_rendering()`: attendi che un batch completi
- [ ] Sync objects (create, destroy, wait, signal) per accelerant2
- [ ] Timeout con fallback a poll HEAD==TAIL

### 2.7 IPC Client-Server
- [ ] Protocollo messaggi (modello RadeonGfx PortLink)
- [ ] Operazioni: gem_create, gem_close, gem_mmap, execbuffer, wait
- [ ] Serializzazione/deserializzazione parametri
- [ ] Gestione errori e cleanup alla disconnessione del client

### 2.8 Test standalone
- [ ] Test: alloca buffer, scrivi dati, verifica via CPU
- [ ] Test: submit batch con MI_STORE_DATA_IMM, verifica write
- [ ] Test: submit batch con BLT fill, verifica framebuffer
- [ ] Test: submit batch con comandi 3D (pipeline state + primitive)

---

## Fase 3: Interfaccia accelerant2 (2-3 settimane)

### 3.1 Implementare AccelerantIntel
- [ ] Struttura add-on accelerant2 (dlopen, instantiate_accelerant)
- [ ] Implementare `AccelerantBase`: QueryInterface, reference counting
- [ ] Implementare `AccelerantDrm` ("drm/v1"):
  - `DrmMmap()` - mappa buffer in address space client
  - `DrmGemClose()` - chiudi handle
  - `DrmPrimeHandleToFd()` / `DrmPrimeFdToHandle()` - export/import
  - `DrmSyncobjCreate/Destroy/Wait/Signal()` - sync objects
- [ ] Implementare `AccelerantI915` ("i915/v1") (interfaccia custom):
  - `I915GemCreate()` - crea buffer object
  - `I915GemMmap()` - mappa in CPU
  - `I915Execbuffer()` - submit batch buffer
  - `I915GemWait()` - attendi completamento
  - `I915GetParam()` - query capabilities (chipset, GTT size, etc.)

### 3.2 IPC verso IntelGfx server
- [ ] Ogni metodo AccelerantI915 serializza e invia al server
- [ ] Ricevi risposta con risultato e handle
- [ ] Cache locale degli handle per performance

### 3.3 Integrazione con AccelerantRoster
- [ ] Registrare l'add-on in `/boot/system/non-packaged/add-ons/accelerants/`
- [ ] Device signature matching per `/dev/graphics/intel_gfx/*`
- [ ] Test: client carica accelerant2, esegue gem_create + execbuffer

---

## Fase 4: libdrm2 Intel shim (2-3 settimane)

### 4.1 Implementare libdrm_intel
- [ ] Nuovo subdirectory `intel/` in libdrm2
- [ ] `drm_intel_bufmgr_gem_init()` - inizializza, connetti ad accelerant2
- [ ] Struttura `drm_intel_bo`:
  - `drm_intel_bo_alloc()` → AccelerantI915::I915GemCreate()
  - `drm_intel_bo_map()` → AccelerantI915::I915GemMmap()
  - `drm_intel_bo_unmap()`
  - `drm_intel_bo_subdata()` - scrivi dati nel buffer
  - `drm_intel_bo_exec()` → AccelerantI915::I915Execbuffer()
  - `drm_intel_bo_wait_rendering()`
  - `drm_intel_bo_unreference()`
- [ ] Relocations: `drm_intel_bo_emit_reloc()`
- [ ] Context management (opzionale per Gen5)

### 4.2 Alternativa: intercettare DRM ioctl i915
- [ ] Mappare `DRM_IOCTL_I915_GEM_CREATE` → I915GemCreate()
- [ ] Mappare `DRM_IOCTL_I915_GEM_EXECBUFFER2` → I915Execbuffer()
- [ ] Mappare `DRM_IOCTL_I915_GEM_MMAP` → I915GemMmap()
- [ ] Mappare `DRM_IOCTL_I915_GEM_WAIT` → I915GemWait()
- [ ] Mappare `DRM_IOCTL_I915_GETPARAM` → I915GetParam()
- [ ] Totale: ~15-20 ioctl da mappare per crocus

### 4.3 Test con programma standalone
- [ ] Scrivere test C che usa libdrm_intel per allocare e eseguire batch
- [ ] Verificare che la catena libdrm2 → accelerant2 → server funzioni

---

## Fase 5: Mesa crocus winsys (2-4 settimane)

### 5.1 Adattare il winsys crocus per Haiku
- [ ] Copiare `src/gallium/winsys/crocus/drm/` nel fork Mesa
- [ ] Sostituire le chiamate `drmIoctl(DRM_IOCTL_I915_*)` con libdrm_intel
- [ ] Adattare `crocus_drm_winsys.c` per usare le API Haiku
  (area_id per mapping, thread per sync)
- [ ] Compilare crocus come driver Gallium per Haiku

### 5.2 State tracker e frontend
- [ ] Verificare che il Gallium state tracker OpenGL compili per Haiku
- [ ] Collegare al BGLView di Haiku (o al EGL driver)
- [ ] Primo test: glxgears o equivalente Haiku (GLTeapot)

### 5.3 Integrazione display
- [ ] Connettere il framebuffer 3D al display tramite VideoStreams
  oppure tramite swap-buffer diretto al framebuffer dell'accelerant
- [ ] Test: finestra OpenGL con rendering hardware

### 5.4 Ottimizzazione e debug
- [ ] Verificare rendering corretto con glmark2 o piglit
- [ ] Profile performance vs llvmpipe
- [ ] Fix bug di rendering specifici Gen5

---

## Fase 6: Integrazione sistema (1-2 settimane)

### 6.1 Coesistenza con intel_extreme
- [ ] intel_extreme: continua a gestire display (mode setting, DPMS)
- [ ] intel_gfx: gestisce 3D (batch submit, GEM, GTT)
- [ ] Condivisione del framebuffer tra i due driver
- [ ] Strategia per evitare conflitti sul ring buffer

### 6.2 Packaging
- [ ] HPKG per kernel driver intel_gfx
- [ ] HPKG per server IntelGfx
- [ ] HPKG per accelerant2 add-on
- [ ] HPKG per libdrm2 con supporto Intel
- [ ] HPKG per Mesa con crocus

### 6.3 Documentazione
- [ ] README con istruzioni di installazione
- [ ] Architettura e design document
- [ ] Lista GPU supportate (Gen4-Gen7 con crocus)
- [ ] Known issues e limitazioni

---

## Dipendenze esterne

| Dipendenza | Stato | Azione |
|---|---|---|
| X547/accelerant2 API headers | Prototipo 2023 | Forkare, adattare per Intel |
| X547/libdrm2 framework | AMDGPU-only | Estendere con modulo Intel |
| Mesa 22+ con crocus | Upstream | Forkare, adattare winsys |
| Intel Gen5 PRM | Pubblico | Scaricare da 01.org |
| Haiku kernel headers | hrev59506 | Disponibile nel sistema |

## Semplificazioni Gen5 rispetto a RadeonGfx

| Aspetto | RadeonGfx (AMD) | IntelGfx (Gen5) |
|---|---|---|
| Address space | Per-client PPGTT + VMID | GTT globale (piu' semplice) |
| Ring buffers | GFX + SDMA + compute | Solo RCS (uno) |
| Firmware GPU | Si (MC, RLC, CP firmware) | No (tutto in hardware) |
| Page tables | 2-level per client | Nessuna (GTT flat) |
| Command format | PM4 packets | MI + Type 2/3 commands |
| Memory domains | VRAM + GTT | Solo GTT (UMA) |

## Rischi e mitigazioni

| Rischio | Impatto | Mitigazione |
|---|---|---|
| Gen5 troppo vecchio per la community | Basso interesse | Crocus supporta Gen4-7, il lavoro e' estendibile |
| Conflitto intel_extreme/intel_gfx | Instabilita' | Separare chiaramente display vs 3D |
| Complessita' Mesa winsys | Blocco prolungato | Iniziare con test standalone, poi Mesa |
| X547 cambia API accelerant2 | Incompatibilita' | Forkare accelerant2, sync periodico |
| Hardware non disponibile per test | Rallentamento | Il Sony Vaio VPCEB3K1E e' il test bed |

---

## Timeline stimata

```
Mese 1:  Fase 0 (studio) + Fase 1 (kernel driver)
Mese 2:  Fase 2 (server GPU - GTT, ring buffer, GEM)
Mese 3:  Fase 2 (server GPU - execbuffer, fencing, IPC) + Fase 3 (accelerant2)
Mese 4:  Fase 4 (libdrm2 Intel) + Fase 5 (Mesa crocus)
Mese 5:  Fase 5 (integrazione, debug) + Fase 6 (packaging)
```

**Primo milestone visibile:** fine Mese 2 - batch buffer execution con
BLT fill verificato via server GPU (senza Mesa).

**Secondo milestone:** fine Mese 4 - GLTeapot hardware-rendered su Ironlake.
