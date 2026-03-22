# Differenze rispetto ai driver originali Haiku
# Sony Vaio VPCEB3K1E — 22 Marzo 2026

Questo documento elenca tutte le modifiche apportate ai driver
`intel_extreme` (GPU) e `hda` (audio) rispetto alla versione
originale inclusa in Haiku R1~beta5 (hrev59506).

---

## 1. GPU — intel_extreme accelerant

Il driver accelerant originale e in:
`/boot/system/add-ons/accelerants/intel_extreme.accelerant` (194179 byte)

Il nostro driver patchato e in:
`/boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant`

### 1.1 Ports.cpp — LVDSPort::IsConnected()

**Problema:** Su piattaforme PCH (Ironlake+), la lettura EDID via GMBUS
DDC fallisce su molti pannelli LVDS. Il driver considera la porta "non
connessa" e cade in fallback VESA a 1024x768.

**Fix:** Aggiunta catena di fallback dopo il fallimento di HasEDID():
1. Prova VESA EDID dal bootloader (`has_vesa_edid_info`)
2. Fallback a dati VBT (`got_vbt`) — contiene 1366x768 dal BIOS
3. Ultimo fallback: porta abilitata dal BIOS (bit `LVDS_PORT_EN`)

La stessa logica esisteva gia per Gen<=4 ma mancava nel path PCH.

### 1.2 Ports.cpp — LVDSPort::SetDisplayMode()

**Problema:** La configurazione dual/single channel LVDS veniva derivata
dal divisore P2 del PLL, potenzialmente sovrascrivendo la configurazione
BIOS corretta e causando schermo nero.

**Fix:** Preserva la configurazione BIOS leggendo il bit `LVDS_CLKB_POWER_UP`
dal registro LVDS corrente. Riferimento: fix di Michael Forney per il
driver igfx di 9front.

### 1.3 Ports.cpp — LVDSPort::SetDisplayMode()

**Problema:** Il panel fitter veniva abilitato incondizionatamente, anche
quando la risoluzione richiesta corrispondeva a quella nativa del pannello.
Con 1366 pixel (non divisibile uniformemente), il panel fitter a 1:1
introduceva artefatti visivi.

**Fix:** Disabilitare il panel fitter quando `needsScaling` e false
(risoluzione nativa).

### 1.4 Pipes.cpp — Pipe::Enable()

**Problema:** I watermark del display venivano configurati solo per
PCH Cougar Point (CPT/Sandy Bridge), non per Ibex Peak (IBX/Ironlake).

**Fix:** Aggiunto `INTEL_PCH_IBX` alla condizione watermark.

### 1.5 engine.cpp — QueueCommands (HWS Sync)

**Problema:** `intel_wait_engine_idle()` usava polling MMIO del registro
HEAD del ring buffer (~100-500ns per lettura), causando latenza eccessiva
durante la sincronizzazione GPU.

**Fix:** Implementato sync tramite Hardware Status Page:
- Ogni 8 comandi, emette `MI_STORE_DWORD_INDEX` che scrive un sequence
  number nella HWS page
- `intel_wait_engine_idle()` legge la HWS page (memoria cached, ~1-2ns)
  e verifica se il sequence number ha raggiunto il target
- Fallback automatico a polling HEAD se la HWS non e disponibile

### 1.6 engine.cpp — Ottimizzazioni BLT

- Costruzione comandi BLT fuori dal loop (opcode/mode/stride costanti)
- Fast path `memcpy` in `QueueCommands::Put()` per comandi senza wrap
- Fix variabile `flush` non inizializzata nel distruttore
- Fix `intel_fill_span`: aggiunta chiamata `queue.Put()` mancante nel loop

### 1.7 engine.cpp — Batch Buffer (infrastruttura, disabilitato)

Implementata classe `BatchCommands` e funzioni `init_batch_buffer()`/
`uninit_batch_buffer()` per sottomissione comandi via `MI_BATCH_BUFFER_START`.
Attualmente disabilitata nelle funzioni BLT perche l'overhead per
operazioni singole (count=1) supera il beneficio.

### 1.8 render.cpp/render.h — Render Engine (infrastruttura)

Implementata infrastruttura per rendering 2D via pipeline 3D Gen5:
- Allocazione stato GPU (VS/WM/CC, binding table, surface state, kernel EU)
- Funzione `render_fill_rect()` proof-of-concept
- Inizializzata al boot, pronta per future operazioni di compositing

### 1.9 hooks.cpp — Fill Span abilitato

Il hook `B_FILL_SPAN` era disabilitato (`return NULL`). Riabilitato
ora che il bug nel loop di `intel_fill_span` e stato corretto.

### 1.10 hooks.cpp — Overlay abilitato per Ironlake

`INTEL_GROUP_ILK` rimosso dalla blacklist overlay. L'overlay hardware
legacy (MI_OVERLAY_FLIP) e supportato su Gen3-Gen5. I registri overlay
sono allocati correttamente dal kernel driver.

### 1.11 intel_extreme.h — Nuove definizioni

- `MI_STORE_DWORD_INDEX`, `MI_BATCH_BUFFER_START`, `MI_BATCH_BUFFER_END`
- `HWS_SYNC_SEQUENCE_INDEX`
- `DISPLAY_CONTROL_TILED` (bit 10 di DSPCNTR)
- `INTEL_FENCE_BASE_965` (0x03000), `INTEL_FENCE_BASE_GEN6` (0x100000)
- Costanti fence register (FENCE_REG_VALID, FENCE_REG_PITCH_SHIFT)

### 1.12 memory.cpp — Allocazione con alignment

Aggiunto overload `intel_allocate_memory(size, alignment, flags, base)`
che propaga il parametro di allineamento al kernel GART allocator.

### 1.13 accelerant.h — Stato tiling

Aggiunti campi `frame_buffer_tiled` e `fence_register_index` alla
struct `accelerant_info` (privata dell'accelerant, NON in `intel_shared_info`
per evitare ABI break con il kernel driver).

### Prestazioni GPU vs driver originale

| Test | Originale | Patchato | Differenza |
|------|-----------|----------|------------|
| FillRect | 57,294/s | 67,316/s | **+18%** |
| CopyBits | 9,614/s | 9,648/s | = |
| InvertRect | 56,715/s | 61,943/s | **+9%** |
| FullClear | 70,827/s | 71,412/s | +1% |
| SyncLatency | 22.4 us | 19.2 us | **+14%** |
| SmallRect 16x16 | 52,093/s | 48,902/s | -6% |
| LargeRect | 176 MB/s | 162 MB/s | -8% |

---

## 2. Audio — HDA kernel driver

Il driver originale e in:
`/boot/system/add-ons/kernel/drivers/bin/hda` (dal pacchetto haiku)

Il nostro driver patchato e installato via HPKG:
`/boot/system/packages/hda_patched-1.0-1-x86_64.hpkg`

Con `BlockedEntries` in `/boot/system/settings/packages` per
sovrascrivere il driver di sistema.

### 2.1 hda_codec.cpp — Quirk Sony Vaio ALC269 (VREF fix)

**Problema:** Il quirk globale `HDA_QUIRK_IVREF` imposta VREF_80 su
tutti i pin con capacita VREF. Il pin 0x19 dell'ALC269 e logicamente
disabilitato ma elettricamente connesso al percorso audio. VREF_80
causa crosstalk/distorsione sugli speaker interni.

**Fix:** Aggiunto quirk specifico per Sony (vendor 0x104d) con ALC269:
```cpp
{ SONY_VENDORID, HDA_ALL, REALTEK_VENDORID, 0x0269,
    HDA_QUIRK_SONY_VAIO, HDA_QUIRK_IVREF },
```
Dopo il tree build, invia `SET_PIN_WIDGET_CONTROL(0x19, VREF_GRD)`
per impostare il pin a VREF Ground. Equivalente al fix Linux
`ALC269_FIXUP_SONY_VAIO` in `patch_realtek.c`.

### 2.2 hda_codec.cpp — Override tipo widget per selettori input ALC269

**Problema:** I widget NID 0x23 e 0x24 (selettori input per ADC) sono
riportati dall'hardware come "Vendor Defined" (tipo 15). La funzione
`hda_widget_find_input_path()` li ignora nel `default: return false`,
causando `build input tree failed` e microfono non funzionante.

**Fix:** Aggiunto caso nel `switch(codec_id)` per ALC269:
```cpp
case 0x10ec0269:
    if (nodeID == 0x23 || nodeID == 0x24)
        widget.type = WT_AUDIO_SELECTOR;
    break;
```

### Risultato HDA

- Quirks: 0x4000 (HDA_QUIRK_SONY_VAIO, IVREF rimosso)
- Mic interno (NID 18) e mic esterno (NID 24) rilevati
- Automute cuffie/speaker gia presente nel driver base
- Audio funzionante senza distorsione

---

## 3. File aggiunti (non presenti nell'originale)

| File | Descrizione |
|------|-------------|
| `intel_extreme/accelerant/render.h` | Header render engine 3D per 2D |
| `intel_extreme/accelerant/render.cpp` | Infrastruttura render engine |
| `intel_extreme/ANALISI_TECNICA_DRIVER.md` | Analisi comparativa Haiku vs Linux |
| `intel_extreme/ANALISI_RENDER_ENGINE_2D.md` | Piano render engine via shader |
| `intel_extreme/PIANO_ACCELERAZIONE_2D.md` | Piano ottimizzazione BLT |
| `intel_extreme/X_TILING_NOTE.md` | Note implementazione X-Tiling |
| `intel_extreme/X_TILING_BUG_ANALYSIS.md` | Analisi bug X-Tiling (ABI break) |
| `hda/ANALISI_HDA_ALC269.md` | Analisi codec audio ALC269 |
| `bench/bench_2d.cpp` | Benchmark 2D (7 test) |
| `VPCEB3K1E_Haiku_Driver_Report.md` | Report compatibilita hardware |
| `DIFFERENZE_DRIVER.md` | Questo documento |

---

## 4. Lavoro X-Tiling (non completato)

L'X-Tiling e stato tentato e documentato ma non completato:
- Fence register a 0x03000 funziona (read/write verificato)
- Allocazione GART con allineamento 8MB funziona
- La corruzione e causata da una race condition: `program_pipe_color_modes()`
  clears il bit DSPCNTR tiled, creando una finestra in cui il display
  engine legge lineare da dati tiled
- Il fix richiede integrazione del tiled bit dentro `program_pipe_color_modes`
  basandosi su `gInfo->frame_buffer_tiled`

---

## 5. Come ripristinare i driver originali

### GPU
```bash
rm /boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant
# Il sistema caricherà il driver dal pacchetto haiku
```

### HDA
```bash
rm /boot/system/packages/hda_patched-1.0-1-x86_64.hpkg
# Rimuovere le righe hda da /boot/system/settings/packages
# Il sistema caricherà il driver dal pacchetto haiku
```
