# Piano X-Tiling via Kernel Driver

**Data:** 23 Marzo 2026

## File da modificare

### 1. Header condiviso (kernel + accelerant)
**File:** `headers/private/graphics/intel_extreme/intel_extreme.h`
**Copia locale:** `intel_extreme/accelerant/intel_extreme.h`

**Modifica:** Aggiungere campi ALLA FINE di `intel_shared_info`:
```cpp
    child_device_config device_configs[10];

    // X-Tiling support (added at END to preserve ABI)
    bool            frame_buffer_tiled;
    uint32          fence_register_index;
    uint32          tiled_bytes_per_row;  // stride potenza di 2
};
```

**Sicurezza ABI:** I campi sono alla FINE della struct. Il kernel alloca
`ROUND_TO_PAGE_SIZE(sizeof(intel_shared_info)) + 3 * B_PAGE_SIZE` (riga 614),
quindi c'e abbondante spazio. I campi saranno zero di default (area azzerata).

### 2. Kernel driver
**File:** `src/add-ons/kernel/drivers/graphics/intel_extreme/intel_extreme.cpp`

**Modifica 1 (riga ~731, dopo allocazione ring buffer):**
Aggiungere allocazione framebuffer tiled e programmazione fence.

```cpp
// Dopo il ring buffer e prima di init_overlay_registers:

// X-Tiling: allocate tiled framebuffer for Ironlake
if (info.device_type.InGroup(INTEL_GROUP_ILK)) {
    // Calcolare stride potenza di 2 per 1366x768x32
    uint32 linearStride = 1366 * 4;  // verra' ricalcolato dall'accelerant
    uint32 tiledStride = 512;
    while (tiledStride < linearStride)
        tiledStride <<= 1;

    uint32 fbSize = tiledStride * 768;  // sara' sovrascritto dall'accelerant
    uint32 fenceSize = 1;
    while (fenceSize < fbSize)
        fenceSize <<= 1;

    // Non allocare il FB qui — l'accelerant lo fa in intel_set_display_mode.
    // Programmare solo il fence QUANDO l'accelerant setta frame_buffer_tiled.

    // Ma possiamo pre-programmare il fence register qui per testare.
    // Il fence verra' aggiornato dall'accelerant quando alloca il FB.

    info.shared_info->frame_buffer_tiled = false;
    info.shared_info->fence_register_index = 0;
    info.shared_info->tiled_bytes_per_row = 0;
}
```

**Modifica 2 (opzione migliore):**
Non programmare il fence nel kernel — lasciare che l'accelerant lo faccia
come prima, MA usare `shared_info->frame_buffer_tiled` (che ora e alla FINE
della struct, senza ABI break) per comunicare lo stato tra kernel e accelerant.

### 3. Accelerant — mode.cpp
**Modifica:** Usare `sharedInfo.frame_buffer_tiled` invece di `gInfo->frame_buffer_tiled`.

Ora che il campo e alla fine di shared_info (nessun ABI break), possiamo:
1. Settare `sharedInfo.frame_buffer_tiled = true` dopo il fence
2. Tutti i path (program_pipe_color_modes, BLT, etc.) lo leggono da shared_info
3. Il kernel puo anche leggerlo se serve

### 4. Accelerant — Pipes.cpp
**Modifica:** `program_pipe_color_modes` legge `sharedInfo.frame_buffer_tiled`
e setta/clear DSPCNTR bit 10 atomicamente con il color mode.

```cpp
} else {
    uint32 tiledBit = gInfo->shared_info->frame_buffer_tiled
        ? DISPLAY_CONTROL_TILED : 0;
    write32(INTEL_DISPLAY_A_CONTROL, (read32(INTEL_DISPLAY_A_CONTROL)
        & ~(DISPLAY_CONTROL_COLOR_MASK | DISPLAY_CONTROL_GAMMA
            | DISPLAY_CONTROL_TILED))
        | colorMode | tiledBit);
    // ...
}
```

Questo RISOLVE la race condition: il tiled bit non e mai clearato senza
essere ri-settato, perche la decisione e presa nello STESSO write32.

### 5. Accelerant — commands.h
**Modifica:** BLT tiling bits basati su `gInfo->shared_info->frame_buffer_tiled`.

---

## Sequenza di operazioni al mode set

```
intel_set_display_mode():
  1. Invalidare fence se precedente FB era tiled
  2. Liberare vecchio framebuffer
  3. Calcolare stride po2 (8192)
  4. Allocare FB con allineamento po2
  5. Programmare fence a 0x03000
  6. sharedInfo.frame_buffer_tiled = true
  7. memset(FB, 0) ← attraverso fence, dati tiled
  8. Port configuration (LVDS, FDI, etc.)
  9. program_pipe_color_modes() ← legge frame_buffer_tiled, setta DSPCNTR tiled
  10. DSPSTRIDE = 8192
  11. set_frame_buffer_base() ← arm surface con DSPCNTR gia tiled
```

Il punto chiave: al passo 9, `program_pipe_color_modes` setta il tiled bit
INSIEME al color mode nella stessa write32. Non c'e mai un momento in cui
DSPCNTR ha tiled=0 mentre il fence e attivo.

---

## Test incrementali

### Test 1: Solo aggiunta campi a shared_info (nessun cambio comportamento)
- Aggiungere campi alla FINE di shared_info
- Ricompilare kernel con jam + accelerant con make
- Installare entrambi
- Verificare: display funziona normalmente, campi sono 0/false

### Test 2: Stride po2 + allocazione allineata (senza fence)
- Modificare mode.cpp per stride 8192 e allocazione allineata
- sharedInfo.frame_buffer_tiled = false
- Verificare: display lineare con stride piu largo

### Test 3: Fence + DSPCNTR (il test critico)
- Programmare fence in mode.cpp
- sharedInfo.frame_buffer_tiled = true
- program_pipe_color_modes legge il flag e setta DSPCNTR
- Verificare: display tiled funzionante

### Test 4: BLT tiling
- commands.h legge sharedInfo.frame_buffer_tiled
- Stride diviso 4, bit 11/15 nell'opcode
- Benchmark per misurare miglioramento

---

## Compilazione

### Kernel driver
```bash
cd /boot/home/Desktop/haiku-build/generated
# Copiare intel_extreme.h modificato nel source tree
cp /boot/home/Desktop/Sony\ Vaio\ VPCEB3K1E/intel_extreme/accelerant/intel_extreme.h \
   ../headers/private/graphics/intel_extreme/intel_extreme.h
jam -q hda  # oppure jam -q intel_extreme se esiste il target
```

### Accelerant
```bash
cd /boot/home/Desktop/Sony\ Vaio\ VPCEB3K1E/intel_extreme/accelerant
make clean && make && make install
```

### Installazione kernel driver
Via HPKG con BlockedEntries (come per HDA).

---

## Rollback
```bash
# GPU accelerant:
cp intel_extreme.accelerant.pre_tiling \
   /boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant

# Kernel driver (se modificato):
# Rimuovere il pacchetto HPKG e le BlockedEntries
```

---

## Rischi

| Rischio | Probabilita | Mitigazione |
|---------|------------|-------------|
| ABI break shared_info | Bassissima | Campi alla FINE, area sovradimensionata |
| Fence timing | Bassa | DSPCNTR tiled settato atomicamente in program_pipe_color_modes |
| DPMS resetta DSPCNTR | Media | Verificare set_display_power_mode path |
| BLT stride sbagliato | Bassa | Flag controlla il comportamento |
