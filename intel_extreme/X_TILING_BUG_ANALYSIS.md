# Analisi Bug X-Tiling — Causa Root

**Data:** 22 Marzo 2026

---

## Il Bug

Display corrotto con pattern a strisce orizzontali (tipico tiling) dopo
aver abilitato X-Tiling nell'accelerant. Il display era inutilizzabile,
richiedendo boot in VESA per il ripristino.

## Causa Root: ABI Break in `intel_shared_info`

La struct `intel_shared_info` (definita in `intel_extreme.h`) e
**condivisa tra il kernel driver e l'accelerant** tramite shared memory.
Il kernel driver e l'accelerant sono compilati separatamente.

Il codice X-Tiling inseriva due campi **NEL MEZZO** della struct:

```cpp
// PRIMA (kernel driver vede questo layout):
addr_t    frame_buffer;           // offset 0xNN
uint32    frame_buffer_offset;    // offset 0xNN+8
uint32    fdi_link_frequency;     // offset 0xNN+12  <-- kernel scrive qui
uint32    hraw_clock;             // offset 0xNN+16
bool      got_vbt;                // offset 0xNN+20
// ... altri 30+ campi ...

// DOPO (accelerant vede QUESTO layout):
addr_t    frame_buffer;           // offset 0xNN
uint32    frame_buffer_offset;    // offset 0xNN+8
bool      frame_buffer_tiled;    // offset 0xNN+12  <-- NUOVO, 1 byte + 3 padding
uint32    fence_register_index;  // offset 0xNN+16  <-- NUOVO
uint32    fdi_link_frequency;     // offset 0xNN+20  <-- SPOSTATO! Kernel scrive +12
uint32    hraw_clock;             // offset 0xNN+24  <-- SPOSTATO
bool      got_vbt;                // offset 0xNN+28  <-- SPOSTATO
// ... tutti gli altri campi SPOSTATI di 8 byte ...
```

### Conseguenze:
- Il kernel scrive `fdi_link_frequency` all'offset +12, ma l'accelerant
  legge quello come `frame_buffer_tiled` → **tiling attivato casualmente**
- Il kernel scrive `got_vbt = true` all'offset +20, ma l'accelerant
  legge `fdi_link_frequency` → **frequenza FDI corrotta**
- `device_type`, `pch_info`, `pll_info` tutti a offset sbagliati →
  **comportamento imprevedibile del driver**
- Il DSPCNTR tiled bit veniva settato perche `frame_buffer_tiled` leggeva
  un valore non-zero da un campo del kernel → **display corruption**

## Perche Anche il Fix del Fence Base (0x03000) Non Risolveva

Il fence base corretto (0x03000 invece di 0x100000) era giusto, ma il
problema era a monte: l'ABI break causava `frame_buffer_tiled = true`
indipendentemente dal codice tiling. Il DSPCNTR veniva impostato con il
bit tiled anche quando il framebuffer era lineare e nessun fence era
programmato.

## Soluzione Corretta

**NON modificare `intel_shared_info`**. Usare invece una di queste:

### Opzione A: Campi in `accelerant_info` (solo accelerant)
```cpp
struct accelerant_info {
    // ... campi esistenti ...
    bool    frame_buffer_tiled;
    uint32  fence_register_index;
};
```
`accelerant_info` e definita solo nell'accelerant, non condivisa col kernel.

### Opzione B: Static globals in engine.cpp
```cpp
static bool sFrameBufferTiled = false;
static uint32 sFenceRegisterIndex = 0;
```

### Opzione C: Aggiungere campi ALLA FINE di `intel_shared_info`
```cpp
struct intel_shared_info {
    // ... tutti i campi esistenti INVARIATI ...
    child_device_config device_configs[10];
    // Nuovi campi alla fine — il kernel non li accede,
    // l'accelerant li vede correttamente
    bool    frame_buffer_tiled;
    uint32  fence_register_index;
};
```
Questa opzione e sicura perche il kernel alloca la struct con una dimensione
fissa (via `sizeof`) e i nuovi campi saranno zero (dalla `memset` iniziale).
Pero richiede che il kernel allochi abbastanza memoria per la struct
estesa — potrebbe funzionare se c'e padding, potrebbe crashare se non ce n'e.

**Opzione A e la piu sicura** perche non tocca nessuna struct condivisa.

## Checklist Per il Prossimo Tentativo

- [ ] NON modificare `intel_shared_info` — usare `accelerant_info`
- [ ] Verificare che `write32(0x3000 + n*8, val)` sia accessibile
      dall'accelerant (testare con una lettura prima della scrittura)
- [ ] Programmare il fence PRIMA di settare DSPCNTR tiled
- [ ] Aggiungere TRACE con ERROR() (non TRACE()) per debug visibile
- [ ] Testare prima SOLO il fence (senza DSPCNTR tiled) per verificare
      che la scrittura del fence non causa crash
- [ ] Testare la sequenza incrementale:
      1. Solo allocazione con stride power-of-2 (senza fence, senza DSPCNTR)
      2. Aggiungere fence register (senza DSPCNTR tiled)
      3. Aggiungere DSPCNTR tiled
      4. Aggiungere BLT tiling bits
