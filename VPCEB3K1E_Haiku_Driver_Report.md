# Analisi Compatibilita Driver Haiku - Sony Vaio VPCEB3K1E

**Data analisi:** 18 Marzo 2026
**Sistema:** Haiku R1~beta5+development, kernel x86_64 (build 14 Feb 2026)

---

## Configurazione Hardware Rilevata

| Componente | Dettaglio |
|---|---|
| **CPU** | Intel Core i3 M 370 @ 2.40 GHz (2 core / 4 thread) |
| **RAM** | ~3.7 GB (3940 MB totali) |
| **Chipset** | Intel HM55 (5 Series/3400) |
| **Display** | 15.5" 1366x768 |

---

## Stato Driver per Componente

### 1. CPU - FUNZIONANTE
- Tutti e 4 i thread riconosciuti e attivi
- Frequenza variabile (SpeedStep attivo): da ~1217 a ~2394 MHz
- Modulo `x86_cstates` caricato per il risparmio energetico (C-states)
- **Stato: Pieno supporto**

### 2. GPU - Intel HD Graphics (Ironlake) - FUNZIONANTE (con patch)
- Driver `intel_extreme` caricato (device 0046)
- **Risoluzione nativa: 1366x768 a 59.9 Hz** (32 bit) - CORRETTO con accelerant patchato
- Accelerant patchato installato in `/boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant`
- **Patch applicate:**
  1. `LVDSPort::IsConnected()` - Aggiunto fallback VESA EDID / VBT / BIOS per piattaforme PCH (GMBUS DDC falliva su questo pannello)
  2. `LVDSPort::SetDisplayMode()` - Preserva configurazione BIOS dual/single channel LVDS (ref: 9front fix)
  3. `Pipe::Enable()` - Aggiunto supporto watermark per PCH Ibex Peak (IBX)
  4. `LVDSPort::SetDisplayMode()` - Disabilita panel fitter a risoluzione nativa
  5. `commands.h` - Fix COMMAND_BLIT_RGBA: bit 21:20 indicano tiling su Gen5, non RGBA
  6. `engine.cpp` - Fix variabile flush non inizializzata + abilitato trace engine
- **Stato: Risoluzione nativa funzionante. Accelerazione 2D in fase di test.**

### 3. Audio - Intel HDA - FUNZIONANTE (con patch)
- Controller: Intel 5 Series/3400 HD Audio (device 3b56)
- Codec: Realtek ALC269 (subsystem Sony 104d:4600)
- Device node presente: `/dev/audio/hmulti/hda`
- Driver patchato installato via HPKG con BlockedEntries per override driver di sistema
- **Patch applicate:**
  1. Quirk Sony VAIO: VREF_GRD su pin 0x19 per eliminare crosstalk speaker
  2. Override tipo widget NID 0x23/0x24 come WT_AUDIO_SELECTOR per microfono
- Automute cuffie/speaker gia implementato nel driver base
- **Stato: Audio funzionante, microfono rilevato, VREF fix attivo (Quirks: 0x4000)**

### 4. Rete Ethernet - Marvell Yukon 88E8059 - FUNZIONANTE
- Driver `marvell_yukon` caricato
- Interfaccia `/dev/net/marvell_yukon/0` presente
- **Stato: Pieno supporto**

### 5. WiFi - Intel Centrino Wireless-N 1000 - FUNZIONANTE
- Driver `iprowifi4965` caricato
- Connesso e operativo via WPA2/802.11n(g)
- **Stato: Pieno supporto**

### 6. USB - Intel EHCI - FUNZIONANTE
- 2 controller EHCI (device 3b3c e 3b34) con hub integrati
- Dispositivi USB rilevati correttamente
- **Stato: Pieno supporto** (solo USB 2.0)

### 7. Tastiera e Touchpad (PS/2) - FUNZIONANTE
- Device node: `/dev/input/keyboard/at` e `/dev/input/mouse/ps2`
- **Stato: Pieno supporto**

### 8. Storage SATA (AHCI) - FUNZIONANTE
- Controller Intel AHCI (device 3b29) supportato
- Disco rilevato in `/dev/disk/scsi/0`
- **Stato: Pieno supporto**

### 9. Webcam - Microdia (0c45:6409) - NON FUNZIONANTE
- Rilevata via USB come "Webcam" (Video Streaming class)
- Nessun device node `/dev/video/` presente
- Haiku non ha un driver UVC (USB Video Class) funzionante per questo dispositivo
- **Progetto esterno in corso** per lo sviluppo del driver UVC
- **Stato: Non supportata (in sviluppo esterno)**

### 10. Bluetooth - BCM2070 (Foxconn) - PARZIALE
- Adattatore rilevato via USB: "Foxconn T77H114 BCM2070"
- Moduli kernel `btCoreData` e `hci` caricati
- Device nodes Bluetooth presenti (`/dev/bluetooth/h2..h5`)
- Lo stack Bluetooth di Haiku e storicamente incompleto
- **Progetto esterno in corso** per il completamento dello stack BT
- **Stato: Rilevato, funzionalita limitate/sperimentali (in sviluppo esterno)**

### 11. Lettore Schede - Ricoh SDHCI + Memory Stick - PARZIALE
- 2x Ricoh SD Host Controller (device e822) rilevati
- 1x Ricoh Memory Stick Host Controller (device e230) rilevato
- Moduli `sdhci` e `mmc` caricati nel kernel
- **Stato: Driver caricato, funzionalita potenzialmente limitata**

### 12. Gestione Energia (ACPI) - PARZIALE
- `acpi_battery`: monitoraggio batteria presente
- `acpi_thermal`: monitoraggio temperatura presente
- `acpi_button`: pulsante power/lid presente
- Manca: sospensione/ibernazione (non supportata in Haiku)
- **Stato: Monitoraggio base funzionante. Nessun suspend/hibernate.**

### 13. Intel MEI (Management Engine) - NON SUPPORTATO
- Device 3b64 rilevato ma nessun driver caricato
- Non critico per il funzionamento quotidiano
- **Stato: Non supportato (irrilevante)**

### 14. SMBus - NON SUPPORTATO
- Intel SMBus Controller (device 3b30) rilevato
- Usato per sensori hardware (temperature, ventole)
- **Stato: Non supportato**

---

## Tabella Riepilogativa

| Componente | Stato | Note |
|---|---|---|
| CPU (i3 M370) | **Funzionante** | 4 thread, SpeedStep attivo |
| GPU (Intel HD) | **Funzionante** | 1366x768 nativo, 6 patch applicate |
| Audio (HDA) | **Funzionante** | ALC269 con VREF fix + mic fix |
| Ethernet (Marvell) | **Funzionante** | |
| WiFi (Intel N1000) | **Funzionante** | Connesso e operativo |
| USB 2.0 | **Funzionante** | |
| Tastiera/Touchpad | **Funzionante** | |
| SATA/HDD | **Funzionante** | |
| Webcam | **Non funzionante** | Progetto esterno in corso |
| Bluetooth | **Parziale** | Progetto esterno in corso |
| Card Reader | **Parziale** | Driver caricato, da verificare |
| Power Mgmt | **Parziale** | No suspend/hibernate |
| Intel MEI | **Non supportato** | Non critico |
| SMBus | **Non supportato** | Non critico |

---

## Piano di Implementazione (dal piu facile al piu difficile)

### Livello 1 - Configurazione / Fix rapidi

**1. Risoluzione GPU (1366x768)**
- **Difficolta:** Bassa
- **Descrizione:** La risoluzione e bloccata a 1024x768 nonostante il driver `intel_extreme` sia caricato. Potrebbe bastare forzare la modalita corretta via `screenmode` o aggiungere un EDID override. Verificare se il pannello LVDS e correttamente rilevato e se una configurazione manuale dei modeline risolve il problema.
- **Azione:** Testare `screenmode --set 1366x768x32` o verificare i log del driver con `syslog` per capire perche la risoluzione nativa non viene offerta.

**2. Verifica funzionamento Card Reader (SDHCI)**
- **Difficolta:** Bassa
- **Descrizione:** I driver `sdhci` e `mmc` sono gia caricati. Inserire una scheda SD e verificare se viene montata automaticamente. Se il controller Ricoh richiede quirk specifici, potrebbero servire piccole patch.
- **Azione:** Test con scheda SD fisica, analisi syslog per eventuali errori di inizializzazione.

### Livello 2 - Miglioramenti a driver esistenti

**3. Audio - Configurazione output/mixer**
- **Difficolta:** Bassa-Media
- **Descrizione:** Il driver HDA e caricato e funzionante, ma il codec specifico del VPCEB3K1E (probabilmente Realtek ALC275) potrebbe richiedere pin configuration specifiche per gestire correttamente il routing tra altoparlanti interni, cuffie e microfono.
- **Azione:** Verificare il codec HDA in uso, testare switching cuffie/speaker, configurare il mixer.

**4. ACPI Battery - Monitoraggio avanzato**
- **Difficolta:** Media
- **Descrizione:** Il driver `acpi_battery` e caricato. Verificare che i dati di carica/scarica siano letti correttamente. Eventuali miglioramenti per notifiche di batteria bassa o integrazione con la Deskbar.
- **Azione:** Leggere `/dev/power/acpi_battery` e verificare la correttezza dei dati riportati.

### Livello 3 - Sviluppo driver / integrazione progetti esterni

**5. Bluetooth - Integrazione progetto esterno**
- **Difficolta:** Media-Alta
- **Descrizione:** Lo stack BT di Haiku ha le basi (`btCoreData`, `hci`) e i device nodes esistono. Il chip BCM2070 e riconosciuto. Un progetto esterno sta lavorando al completamento dello stack. L'integrazione richiede: completamento del transport layer USB-HCI, implementazione dei profili BT minimi (almeno HID e file transfer), e testing con il chip BCM2070 specifico.
- **Dipendenze:** Progetto esterno per lo stack BT.
- **Azione:** Monitorare il progetto esterno, testare le build intermedie con l'hardware BCM2070.

**6. Webcam - Integrazione driver UVC**
- **Difficolta:** Media-Alta
- **Descrizione:** La webcam Microdia (0c45:6409) e un dispositivo USB Video Class standard. Un progetto esterno sta sviluppando un driver UVC per Haiku. L'integrazione richiede: framework di cattura video (`/dev/video/`), implementazione del protocollo UVC, gestione dei formati di compressione (MJPEG/YUV).
- **Dipendenze:** Progetto esterno per driver UVC, framework media_kit video.
- **Azione:** Monitorare il progetto esterno, testare con questa webcam specifica quando disponibile.

**7. GPU - Accelerazione 2D/3D e mode-setting migliorato**
- **Difficolta:** Alta
- **Descrizione:** Il driver `intel_extreme` per la generazione Ironlake (Gen5) ha supporto base ma incompleto. Miglioramenti possibili: corretta gestione LVDS/eDP per il pannello nativo, accelerazione 2D via BLT engine, eventuale supporto OpenGL via Mesa/Gallium (molto complesso).
- **Azione:** Contribuire al driver `intel_extreme` upstream di Haiku per migliorare il supporto Ironlake.

### Livello 4 - Funzionalita di sistema complesse

**8. SMBus - Monitoraggio sensori hardware**
- **Difficolta:** Alta
- **Descrizione:** Il controller Intel SMBus (device 3b30) permetterebbe di leggere sensori di temperatura, velocita ventole e voltage. Richiede l'implementazione di un driver SMBus per il chipset Intel 5 Series e un framework di hwmon.
- **Azione:** Implementazione driver i2c/smbus, interfaccia sensori.

**9. Suspend/Hibernate (ACPI S3/S4)**
- **Difficolta:** Molto Alta
- **Descrizione:** Haiku non supporta la sospensione/ibernazione. Si tratta di una limitazione a livello di sistema operativo che richiede: salvataggio/ripristino dello stato di tutti i driver, gestione ACPI S3/S4, reinizializzazione hardware al resume. E un progetto a livello di kernel/OS, non specifico per questo laptop.
- **Azione:** Contribuzione upstream al progetto Haiku. Non risolvibile a livello di singola macchina.

---

## Patch sviluppate (18 Marzo 2026)

Sono state sviluppate 3 patch per risolvere il punto 1 (risoluzione GPU):

### Patch 1 - `Ports.cpp:LVDSPort::IsConnected()` [CRITICA]
Bug: il path PCH (Ironlake+) cadeva in `return HasEDID()` senza fallback.
Fix: aggiunta catena di fallback VESA EDID -> VBT -> BIOS port enabled.
Questo e il fix principale che risolve il pannello "non connesso".

### Patch 2 - `Ports.cpp:LVDSPort::SetDisplayMode()` [PREVENTIVA]
Bug: dual/single channel LVDS veniva forzato in base al divisore P2.
Fix: preserva la configurazione BIOS leggendo il registro LVDS corrente.
Previene potenziali schermi neri (ref: 9front igfx fix).

### Patch 3 - `Pipes.cpp:Pipe::Enable()` [MIGLIORATIVA]
Bug: watermark impostati solo per CPT (Sandy Bridge PCH), non per IBX.
Fix: aggiunto INTEL_PCH_IBX alla condizione watermark.
Migliora stabilita display su Ironlake.

**Stato:** In attesa di compilazione e test.
**Dettagli:** Vedi `ANALISI_TECNICA_DRIVER.md` per l'analisi completa.

---

## Note BT

Nota: durante i test e stato osservato un kernel panic nello stack Bluetooth
(`bt_a2dp_source_test` - deadlock spinlock in btCoreData). Questo e un problema
separato dal driver GPU, relativo allo stack BT sperimentale.

---

## Note Finali

Il Sony Vaio VPCEB3K1E risulta **ben supportato per un uso quotidiano** su Haiku. Le funzionalita core (CPU, rete, audio, disco, input) sono tutte operative. I punti critici immediati da affrontare sono:

1. **Priorita alta:** Risolvere la risoluzione GPU a 1366x768 - **PATCH PRONTE**
2. **Priorita media:** Verificare card reader e configurazione audio avanzata
3. **Priorita bassa:** Attendere/contribuire ai progetti esterni per webcam e bluetooth
4. **Lungo termine:** Accelerazione GPU (predisposizioni identificate), suspend/hibernate
