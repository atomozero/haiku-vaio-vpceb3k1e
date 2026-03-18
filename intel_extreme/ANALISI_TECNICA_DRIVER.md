# Analisi Tecnica Comparativa: intel_extreme (Haiku) vs i915 (Linux)
# GPU Ironlake Mobile (0x0046) - Sony Vaio VPCEB3K1E

**Data:** 18 Marzo 2026

---

## 1. IL BUG CRITICO: LVDSPort::IsConnected() - Ports.cpp:1134-1180

### Evidenza dal syslog

```
intel_extreme: found LFP of size 1366 x 768 in BIOS VBT tables  ← SA la risoluzione!
intel_extreme: LVDS C: no EDID information found.                ← DDC/GMBUS fallisce
intel_extreme: dump_ports: No ports connected                    ← Nessuna porta!
intel_extreme: Warning: zero active displays were found!         ← Fallback VESA
```

Il registro LVDS vale `0x80308302`:
- Bit 31 (`LVDS_PORT_EN`): **SET** - il BIOS ha abilitato il pannello LVDS
- Bit 1 (`PCH_LVDS_DETECTED`): **SET** - hardware conferma la presenza del pannello

### Il codice problematico

```cpp
// Ports.cpp:1134
bool LVDSPort::IsConnected()
{
    if (gInfo->shared_info->pch_info != INTEL_PCH_NONE) {
        uint32 registerValue = read32(_PortRegister());
        if ((registerValue & PCH_LVDS_DETECTED) == 0) {
            return false;  // Questo check PASSA (bit e set)
        }
    }
    // ...cade qui...
    return HasEDID();  // ← RITORNA FALSE! GMBUS DDC fallisce!
}
```

### Perche fallisce

Per le piattaforme PCH (Ironlake+), il metodo:
1. Controlla correttamente il bit `PCH_LVDS_DETECTED` - **OK, passa**
2. Poi cade nel `return HasEDID()` finale (riga 1179) - **FALLISCE**
3. `HasEDID()` chiama la lettura DDC via GMBUS su `INTEL_I2C_IO_C` (0x5018)
4. La lettura EDID via I2C fallisce (molti pannelli LVDS non hanno EEPROM EDID)
5. Risultato: porta considerata "non connessa" nonostante sia attiva!

### Confronto con il codice pre-PCH (Gen <= 4)

Il codice per generazioni precedenti (righe 1148-1174) ha **gia** la logica corretta:
```cpp
} else if (gInfo->shared_info->device_type.Generation() <= 4) {
    if (!HasEDID()) {
        if (gInfo->shared_info->has_vesa_edid_info) {
            // Fallback a VESA EDID ← IMPLEMENTATO per Gen<=4
        } else if (gInfo->shared_info->got_vbt) {
            return true;  // Forza connesso se ha VBT ← IMPLEMENTATO per Gen<=4
        }
    }
}
```

Questa stessa logica di fallback **manca completamente** nel path PCH!

### Confronto con Linux i915

Linux `intel_lvds_init()` usa questa gerarchia:
1. OpRegion/VBT panel data
2. EDID via GMBUS/DDC
3. Stato corrente hardware (legge i timing dalla pipe attiva)
4. Fallback VBT BIOS

Linux **non richiede mai EDID** per considerare LVDS connesso. Se il registro LVDS
mostra la porta abilitata e/o il VBT contiene dati panel, il display e connesso.

### Fix proposto

```cpp
bool LVDSPort::IsConnected()
{
    if (gInfo->shared_info->pch_info != INTEL_PCH_NONE) {
        uint32 registerValue = read32(_PortRegister());
        if ((registerValue & PCH_LVDS_DETECTED) == 0) {
            TRACE("LVDS: Not detected\n");
            return false;
        }

        // Prova EDID via GMBUS
        if (HasEDID())
            return true;

        // Fallback: VESA EDID dal bootloader
        if (gInfo->shared_info->has_vesa_edid_info) {
            TRACE("LVDS: Using VESA edid info\n");
            memcpy(&fEDIDInfo, &gInfo->shared_info->vesa_edid_info,
                sizeof(edid1_info));
            if (fEDIDState != B_OK) {
                fEDIDState = B_OK;
                edid_dump(&fEDIDInfo);
            }
            return true;
        }

        // Fallback: VBT ha dati panel validi
        if (gInfo->shared_info->got_vbt) {
            TRACE("LVDS: No EDID but VBT present, force connected\n");
            return true;
        }

        // Ultimo fallback: il BIOS ha gia abilitato la porta
        if (registerValue & LVDS_PORT_EN) {
            TRACE("LVDS: No EDID/VBT but port enabled by BIOS, "
                "force connected\n");
            return true;
        }

        return false;
    }
    // ...resto del codice invariato per Gen<=4...
}
```

### Impatto previsto

Con questa fix:
- Il pannello LVDS sara riconosciuto come connesso
- `create_mode_list()` in mode.cpp usera il path VBT (righe 236-255) per creare la
  mode list con risoluzione 1366x768 (gia letta correttamente dal VBT)
- `intel_set_display_mode()` processera la porta LVDS (non la saltera piu)
- `LVDSPort::SetDisplayMode()` configurera PLL, FDI, transcoder e pannello

---

## 2. PROBLEMI SECONDARI IDENTIFICATI

### 2.1 Watermark solo per CPT, non per IBX - Pipes.cpp:641

```cpp
void Pipe::Enable(bool enable)
{
    if (enable) {
        // Watermark solo per CPT (Sandy Bridge PCH)
        if (gInfo->shared_info->pch_info == INTEL_PCH_CPT) {
            write32(INTEL_DISPLAY_A_PIPE_WATERMARK, 0x0783818);
        }
        // IBX (Ironlake PCH) non riceve watermark!
    }
}
```

**Problema:** I watermark controllano il buffering della pipeline di visualizzazione.
Senza watermark corretti su IBX, possibili artefatti o flickering.

**Linux:** Configura watermark specifici per ogni generazione in `ironlake_update_wm()`.

**Priorita:** Media (funzionamento base non compromesso)

### 2.2 Dual/Single Channel basato solo su P2 divisor - Ports.cpp:1313

```cpp
// LVDSPort::SetDisplayMode()
if (divisors.p2 == 5 || divisors.p2 == 7) {
    lvds |= LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP;  // dual
} else {
    lvds &= ~(LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP); // single
}
```

**Problema:** Il 9front driver fix (di Michael Forney) ha dimostrato che sovrascrivere
la configurazione dual-channel del BIOS causa schermo nero su alcuni laptop. Il BIOS
potrebbe configurare dual-channel per ragioni specifiche dell'hardware.

**Fix proposto:** Preservare l'impostazione BIOS per dual/single channel leggendo
il valore corrente del registro prima di modificarlo:
```cpp
// Preserva la configurazione dual/single channel del BIOS
uint32 bios_lvds = read32(_PortRegister());
lvds = (lvds & ~(LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP))
     | (bios_lvds & (LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP));
```

**Priorita:** Alta (potenziale schermo nero)

### 2.3 Transcoder mapping mancante per IBX - Ports.cpp:1303

```cpp
if (gInfo->shared_info->pch_info == INTEL_PCH_CPT) {
    lvds &= ~PORT_TRANS_SEL_MASK;
    if (fPipe->Index() == INTEL_PIPE_A)
        lvds |= PORT_TRANS_A_SEL_CPT;
    else
        lvds |= PORT_TRANS_B_SEL_CPT;
}
// Per IBX: NESSUN mapping transcoder esplicito
```

**Problema:** Su IBX il mapping transcoder usa il bit `DISPLAY_MONITOR_PIPE_B` nel
registro LVDS (bit 30). Il codice attuale non lo imposta esplicitamente per IBX.
Questo funziona solo perche il valore BIOS viene preservato dal `read32()` iniziale.

**Priorita:** Bassa (funziona finche si usa la stessa pipe del BIOS)

### 2.4 Generation check per panel power - Ports.cpp:1223

```cpp
if (gInfo->shared_info->device_type.Generation() != 4) {
    // Power off Panel
    write32(panelControl, ...);
}
```

**Nota:** Gen 5 (Ironlake) esegue il power off/on. Questo e corretto secondo
la documentazione Intel. Non e un bug, ma vale la pena annotare.

---

## 3. ANALISI PLL - Confronto Haiku vs Linux

### Limiti PLL Ironlake LVDS Single-Channel (120MHz ref)

Il commento in pll.cpp riga 42 dice: *"we use the values of N+2, M1+2 and M2+2"*

| Param | Haiku (con offset +2) | Haiku (valore reale) | Linux i915 |
|-------|----------------------|---------------------|------------|
| N min | 3 | 1 | 1 |
| N max | 5 | 3 | 3 |
| M1 min | 14 | 12 | 12 |
| M1 max | 24 | 22 | 22 |
| M2 min | 7 | 5 | 5 |
| M2 max | 11 | 9 | 9 |

**Risultato: I valori PLL sono CORRETTI.** L'offset +2 e documentato e applicato
correttamente. Non e questa la causa del problema di risoluzione.

### SSC (Spread Spectrum Clock)

```cpp
// pll.cpp:476-479
if (gInfo->shared_info->pch_info == INTEL_PCH_IBX) {
    hasCK505 = false;
    wantsSSC = hasCK505;  // = false
}
```

Per IBX, SSC e **gia disabilitato**. Linux ha un quirk esplicito per Sony Vaio
(device 0x0046, vendor 0x104d, subsystem 0x9076) che forza SSC off. Il comportamento
di Haiku e gia allineato.

---

## 4. PREDISPOSIZIONI PER PUNTO 7 (Accelerazione GPU)

### 4.1 Stato attuale dell'infrastruttura

| Componente | Stato | File |
|---|---|---|
| Register mapping (CPU/PCH split) | Implementato | intel_extreme.cpp |
| VBT/OpRegion parsing | Implementato | bios.cpp |
| FDI Link Training (ILK-specific) | Implementato | FlexibleDisplayInterface.cpp |
| Panel Fitter (CPU-side) | Implementato | PanelFitter.cpp |
| PLL computation (ILK limits) | Implementato | pll.cpp |
| DPLL programming (PCH DPLLs) | Implementato | Pipes.cpp |
| Mode setting sequenza base | Implementato | Ports.cpp, Pipes.cpp |
| Comandi BLT 2D | Definiti (header) | intel_extreme.h:1536-1547 |
| Ring Buffer | Infrastruttura base | intel_extreme.h:1522-1526 |
| Overlay (video) | Strutture definite | intel_extreme.h:1754-1919 |

### 4.2 Cosa serve per accelerazione 2D (BLT engine)

L'header `intel_extreme.h` definisce gia i comandi:
```cpp
#define XY_COMMAND_SOURCE_BLIT       0x54c00006
#define XY_COMMAND_COLOR_BLIT        0x54000004
#define XY_COMMAND_SETUP_MONO_PATTERN 0x44400007
#define XY_COMMAND_SCANLINE_BLIT     0x49400001
#define COMMAND_COLOR_BLIT           0x50000003
```

Per implementare accelerazione 2D servono:
1. **Ring Buffer Manager** - Inizializzazione e gestione del command ring buffer
   della GPU. La struttura `hardware_status` e gia definita.
2. **Accelerant hooks** - Implementare `intel_fill_rectangle()`, `intel_blit()`,
   `intel_screen_to_screen_blit()` nell'accelerant.
3. **Fence registers** - Per tiling della memoria (gia supportato parzialmente).
4. **GTT (Graphics Translation Table)** - Per mappare la memoria grafica.

**Difficolta:** Media. L'infrastruttura e gia abbozzata, servono le implementazioni
concrete delle funzioni di blit.

### 4.3 Cosa serve per accelerazione 3D (OpenGL/Mesa)

Per Ironlake (Gen5) la pipeline 3D e EU (Execution Units) based:
1. **Mesa Gallium driver** - Portatare il driver `i915g` di Mesa (copre fino a Gen5)
2. **GEM/GTT memory management** - Gestione oggetti buffer GPU
3. **Batch buffer submission** - Invio comandi shader alla GPU
4. **Shader compiler** - Gen5 ha un set limitato di istruzioni shader

**Difficolta:** Molto alta. Richiede un port completo di Mesa e integrazione con
il sistema grafico di Haiku. E un progetto a livello di OS, non specifico per
questo laptop.

### 4.4 Lavoro preparatorio consigliato (fattibile ora)

Le seguenti predisposizioni possono essere implementate gia in questa fase,
migliorando le performance e preparando il terreno per il punto 7:

1. **Fix del bug IsConnected()** (questo documento, sezione 1)
   - Prerequisito per tutto il resto: senza display funzionante non si testa nulla

2. **Ring Buffer initialization**
   - Allocare e inizializzare il command ring buffer della GPU
   - Scrivere le funzioni base di submit/wait
   - Questo e il fondamento sia per 2D che per 3D

3. **GTT setup**
   - Configurare la Graphics Translation Table per Gen5
   - Mappare le regioni di memoria grafica
   - Prerequisito per qualsiasi operazione accelerata

4. **Watermark corretti per IBX**
   - Migliorare la stabilita del display
   - Necessario per evitare corruzione quando si attivano operazioni asincrone

5. **BLT engine test**
   - Implementare un semplice color fill via BLT
   - Validare che il ring buffer e GTT funzionino
   - Primo passo verso le hook di accelerazione 2D

---

## 5. RIEPILOGO AZIONI

### Azione immediata (Fix risoluzione 1366x768)

| # | Cosa | File | Righe | Difficolta |
|---|------|------|-------|-----------|
| 1 | Fix `IsConnected()` fallback EDID/VBT | Ports.cpp | 1134-1180 | Bassa |
| 2 | Preservare dual-channel BIOS | Ports.cpp | 1292-1319 | Bassa |
| 3 | Aggiungere watermark IBX | Pipes.cpp | 640-646 | Media |

### Predisposizioni per accelerazione (Punto 7)

| # | Cosa | Difficolta | Dipendenze |
|---|------|-----------|------------|
| 4 | Ring Buffer init | Media | Fix #1 funzionante |
| 5 | GTT setup Gen5 | Media | Fix #1 funzionante |
| 6 | BLT color fill test | Media | #4, #5 |
| 7 | Accelerant 2D hooks | Media-Alta | #4, #5, #6 |
| 8 | Mesa/Gallium port | Molto Alta | Tutto il sopra |

### Test consigliati dopo il fix

1. Verificare che `screenmode --list` mostri 1366x768
2. Verificare che il display si accenda alla risoluzione nativa
3. Testare cambio risoluzione tramite Screen preferences
4. Verificare funzionamento FDI link training nei log
5. Testare output HDMI/VGA esterno (se disponibile)

---

## 6. PATCH IMPLEMENTATE (18 Marzo 2026)

### Patch 1: LVDSPort::IsConnected() - Ports.cpp
**Righe modificate:** 1148-1179 (32 righe aggiunte)

Aggiunta catena di fallback per piattaforme PCH (Ironlake+) quando la lettura
EDID via GMBUS/DDC fallisce:
1. Prova EDID via GMBUS (comportamento originale)
2. Fallback a VESA EDID dal bootloader
3. Fallback a VBT (che contiene 1366x768 dal BIOS)
4. Ultimo fallback: porta abilitata dal BIOS (bit LVDS_PORT_EN)

La stessa logica esisteva gia per Gen<=4 ma mancava nel path PCH.

### Patch 2: LVDSPort::SetDisplayMode() - Ports.cpp
**Righe modificate:** 1343-1351 (commento e condizione cambiati)

Sostituita la determinazione dual/single channel basata sul divisore P2
con la preservazione della configurazione BIOS. Il registro LVDS viene
letto per verificare se `LVDS_CLKB_POWER_UP` e impostato dal BIOS.
Riferimento: fix di Michael Forney per il driver igfx di 9front.

### Patch 3: Pipe::Enable() - Pipes.cpp
**Riga modificata:** 641-642

Aggiunto `INTEL_PCH_IBX` alla condizione per i watermark di display.
Precedentemente solo `INTEL_PCH_CPT` (Sandy Bridge PCH) riceveva watermark.
Ora anche Ibex Peak (Ironlake PCH) viene configurato correttamente.

### Stato: TESTATO E FUNZIONANTE (18 Marzo 2026)

L'accelerant patchato e stato compilato e installato con successo:
- **Compilazione:** `make` con GCC 13.3 su Haiku (Makefile custom creato per build standalone)
- **Installazione:** `/boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant`
- **Risultato dopo riavvio:** Display a **1366x768 32 bit 59.9 Hz** (risoluzione nativa)

**Evidenza dal syslog dopo il fix:**
```
LVDS: No EDID, but force enabled as we have a VBT          ← Patch 1 attiva
Hardware mode will actually be 1366x768 at 59Hz             ← Risoluzione nativa!
LVDS: single channel (preserving BIOS setting)              ← Patch 2 attiva
Port configuration completed successfully!                  ← Tutto OK
```

**Note:**
- Il pannello non ha EEPROM EDID (GMBUS DDC fallisce), la modalita VBT e quella corretta
- Il panel fitter e attivo (PCH_PANEL_FITTER_CONTROL = 0x80800000)
- FDI link training completato con successo (4 lane, 18bit colordepth, 270MHz ref)
- PLL: p=28 (p1=2, p2=14), n=5, m=84 (m1=15, m2=9) — pixel clock 72MHz

---

## 7. REGISTRI CHIAVE PER DEBUG

Per diagnostica futura, questi registri sono utili da leggere:

```
LVDS Port Register:        0xe1180 (PCH_LVDS)
PCH Panel Status:          0xc7200
PCH Panel Control:         0xc7204
PCH DPLL A:                0xc6014
PCH DPLL B:                0xc6018
FDI TX A Control:          0x60100
FDI RX A Control:          0xf000c
FDI RX A IIR:              0xf0014
PCH DREF Control:          0xc6200
Pipe A Config:             0x70008
Transcoder A HTOTAL:       0xe0000
Transcoder A VTOTAL:       0xe000c
Panel Fitter A Control:    0x68080
Panel Fitter A Window Size: 0x68074
```
