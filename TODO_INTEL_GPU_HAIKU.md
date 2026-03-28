# TODO: Accelerazione GPU Intel Gen5 su Haiku (modello X547)

**Obiettivo:** OpenGL hardware-accelerated su Intel Ironlake (Gen5) via
Mesa crocus, seguendo l'architettura di X547 (RadeonGfx/libdrm2/accelerant2).

**Hardware target:** Intel HD Graphics 0x0046 (Ironlake Mobile, Gen5)
**Riferimento architetturale:** X547/RadeonGfx + X547/libdrm2 + X547/accelerant2

---

## Fase 0: Preparazione e studio (1-2 settimane)

### 0.1 Studio dell'architettura X547
- [ ] Clonare e studiare X547/RadeonGfx (server GPU, ring buffer, memory manager)
- [ ] Clonare e studiare X547/libdrm2 (shim DRM → accelerant2)
- [ ] Clonare e studiare X547/accelerant2 (API COM-like, vtable C/C++)
- [ ] Clonare e studiare X547/VideoStreams (buffer passing producer/consumer)
- [ ] Compilare RadeonGfx su Haiku per capire il build system

### 0.2 Studio del driver Intel i915 Linux
- [ ] Studiare il winsys crocus (src/gallium/winsys/crocus/drm/)
- [ ] Documentare tutti i DRM_IOCTL_I915_* usati da crocus
- [ ] Studiare GEM object management in i915 (create, mmap, execbuffer)
- [ ] Studiare la gestione GTT globale Gen5 (vs PPGTT Gen6+)
- [ ] Studiare il batch buffer submission (execbuffer2)

### 0.3 Documentazione hardware Gen5
- [ ] Scaricare Intel Open Source PRM Vol 1-4 per Ironlake
- [ ] Documentare il register map GTT (PGTBL_CTL, PTE format)
- [ ] Documentare il ring buffer RCS (TAIL, HEAD, START, CTL)
- [ ] Documentare il formato batch buffer e relocation

---

## Fase 1: Kernel driver `intel_gfx` (1-2 settimane)

Il kernel driver e' minimale (modello RadeonGfx): PCI setup, MMIO
mapping, shared_info.  Basato sull'attuale intel_extreme.

### 1.1 Fork del kernel driver
- [ ] Copiare intel_extreme kernel driver come `intel_gfx`
- [ ] Rimuovere il codice display (gestito dall'accelerant esistente)
- [ ] Mantenere: PCI probe, BAR mapping, interrupt setup

### 1.2 shared_info per GPU server
- [ ] Definire `intel_gfx_shared_info` con:
  - Indirizzo base registri MMIO
  - GTT base address e dimensione
  - Aperture base address e dimensione
  - Ring buffer offset e dimensione
  - Status page offset
  - Device ID e stepping
- [ ] Creare area condivisa kernel ↔ userspace

### 1.3 IOCTL minimali
- [ ] `INTEL_GFX_GET_SHARED_INFO` - ottieni shared_info
- [ ] `INTEL_GFX_ALLOC_GTT` - alloca pagine GTT (o gestire in userspace)
- [ ] `INTEL_GFX_MAP_GTT` - mappa pagine fisiche nel GTT

### 1.4 Build e test
- [ ] Makefile per kernel driver (_KERNEL_MODE, -nostdlib)
- [ ] Verificare che il driver si carica senza conflitto con intel_extreme
- [ ] Strategia di coesistenza: intel_extreme per display, intel_gfx per 3D

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
