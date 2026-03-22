# X-Tiling: Note sull'implementazione fallita

**Data:** 22 Marzo 2026
**Stato:** DISABILITATO — richiede supporto kernel driver

## Cosa è stato tentato

### Modifiche all'accelerant (commit 47d0317, poi revertito):
1. **intel_extreme.h**: Aggiunto `frame_buffer_tiled`, `fence_register_index` in shared_info
2. **intel_extreme.h**: Definizioni fence register (Gen5: 0x03000, Gen6+: 0x100000)
3. **intel_extreme.h**: `DISPLAY_CONTROL_TILED` (bit 10 di DSPCNTR)
4. **mode.cpp**: Allocazione FB con stride potenza di 2 (8192 per 1366x32bpp)
5. **mode.cpp**: Programmazione fence register con sequenza Linux i965
6. **Pipes.cpp**: DSPCNTR bit tiled in `program_pipe_color_modes()`
7. **commands.h**: BLT tiling bits (bit 11 dst, bit 15 src) e stride diviso 4
8. **memory.cpp**: Overload `intel_allocate_memory()` con alignment
9. **accelerant.cpp**: Pulizia fence al shutdown

## Bug trovato e corretto

Il fence register base era **SBAGLIATO**:
- Usavamo: `0x100000` (Gen6+ Sandy Bridge)
- Corretto per Gen5: `0x03000` (formato i965)

Questo è stato corretto ma il display restava corrotto.

## Perchè non funziona

Il pattern di corruzione (strisce orizzontali) indica che:
1. Il DSPCNTR dice "tiled" → il display engine legge in modalità tiled
2. Ma il framebuffer in memoria è lineare O il fence non copre l'area corretta

### Cause probabili:
- **ABI mismatch**: L'aggiunta di campi a `intel_shared_info` potrebbe
  spostare i campi successivi, causando corruzione nelle strutture condivise
  tra kernel e accelerant
- **Alignment GART**: L'allocazione con alignment a 8MB potrebbe fallire
  silenziosamente nel GART, producendo un buffer non allineato e un fence
  che non copre l'area corretta
- **Fence non accessibile**: Anche se il registro 0x03000 è mappato
  nell'accelerant, il fence potrebbe richiedere configurazione kernel-side

### Soluzione corretta:
Il fence register dovrebbe essere programmato dal **kernel driver**
(`intel_extreme.cpp`) dove:
1. Si conosce l'indirizzo fisico del framebuffer
2. Si controlla l'allocazione GART con l'allineamento corretto
3. Non c'è rischio di ABI mismatch

## Formato fence register Gen5 (riferimento Linux)

```
Offset: FENCE_REG_965_LO(i) = 0x03000 + i*8

Low DWORD [31:0]:
  [31:12] = Start offset (GTT, 4K aligned)
  [11:2]  = Pitch: (stride/128 - 1) << 2
  [1]     = Tile walk: 0=X, 1=Y
  [0]     = Valid

High DWORD [63:32]:
  [31:12] = End offset (start + size - 4096, 4K aligned)

Sequenza scrittura (Linux i965_write_fence_reg):
  1. write32(lo, 0)     // invalidate
  2. read32(lo)          // posting read
  3. write32(hi, end)    // end address
  4. write32(lo, val)    // start+pitch+valid
  5. read32(lo)          // posting read
```

## Valori per Sony Vaio 1366x768 32bpp

```
Stride tiled:     8192 bytes (potenza di 2)
FB size:          8192 * 768 = 6,291,456 bytes
Fence size (po2): 8,388,608 bytes (8MB)
Pitch value:      (8192/128) - 1 = 63 (0x3F)
```

## File di riferimento Linux

- `drivers/gpu/drm/i915/gt/intel_ggtt_fencing.c`: i965_write_fence_reg()
- `drivers/gpu/drm/i915/i915_reg.h`: FENCE_REG_965_LO, I965_FENCE_PITCH_SHIFT
