# Analisi Driver HDA - Realtek ALC269 su Sony Vaio VPCEB3K1E

**Data:** 18 Marzo 2026
**Codec:** Realtek ALC269 (Vendor 0x10ec, Product 0x0269)
**Subsystem:** Sony 0x104d:0x4600
**Revision:** 1.0.0.4
**Controller:** Intel 5 Series/3400 HD Audio (0x8086:0x3b56)
**Quirks attuali Haiku:** 0x0700 (HDA_QUIRK_IVREF globale)

---

## 1. STATO ATTUALE

### Cosa funziona
- Riproduzione audio analogica (speaker interni)
- DAC widget 2 e 3 correttamente associati ai pin di output
- Stream playback: 192kHz/24bit, 2 canali

### Cosa NON funziona
- **Registrazione (microfono):** l'input tree dell'ALC269 ORA si costruisce
  correttamente (fix selector, commit 7bd9965). Il `build input tree failed`
  ancora presente nel syslog appartiene al codec **HDMI Intel** (function group
  senza microfono), non all'ALC269. Il bug residuo era nel binding dello stream
  di cattura: entrambi gli ADC (7 e 8) venivano legati allo stesso stream tag e
  canale 0, collidendo sul link seriale HDA. Vedi sezione 4. FIXATO.
- **Jack detection cuffie:** nessun automute speaker quando si inseriscono cuffie
- **HDMI audio:** `Failed to setup new audio function group` (secondo function group)
- **VREF pin 0x19:** configurato erroneamente (VREF_80 invece di VREF_GRD)

---

## 2. MAPPA WIDGET DEL CODEC ALC269

### DAC (Digital to Analog Converter)
| NID | Tipo | Formato | Note |
|-----|------|---------|------|
| 2 | Audio Output | 16/20/24bit, 44-192kHz | DAC primario (speaker) |
| 3 | Audio Output | 16/20/24bit, 44-192kHz | DAC secondario (cuffie) |
| 6 | Audio Output | Digital, 32-192kHz | SPDIF |
| 16 (0x10) | Audio Output | Digital, 32-192kHz | SPDIF/HDMI |

### ADC (Analog to Digital Converter)
| NID | Tipo | Formato | Connessioni |
|-----|------|---------|-------------|
| 7 | Audio Input | 16/20/24bit, 44-96kHz | Input da NID 36 (selector 0x24) |
| 8 | Audio Input | 16/20/24bit, 44-96kHz | Input da NID 35 (selector 0x23) |

### Mixer
| NID | Ingressi | Funzione |
|-----|----------|----------|
| 11 (0x0b) | 24, 25, 26, 27, 29 | Mixer analogico (tutti i pin input) |
| 12 (0x0c) | DAC 2, Mixer 11 | Mix per speaker |
| 13 (0x0d) | DAC 3, Mixer 11 | Mix per cuffie |
| 14 (0x0e) | Mixer 12, 13 | Selettore master |

### Pin Complex (connettori fisici)
| NID | Config BIOS | Tipo | Device | Associazione |
|-----|------------|------|--------|-------------|
| 17 (0x11) | - | Output | N/C (disabilitato) | 15 |
| 18 (0x12) | Fixed | Input | **Mic interno (digitale)** | 2 |
| 20 (0x14) | Fixed | Output | **Speaker interni** | 1 |
| 21 (0x15) | Jack | Output | **Cuffie (jack 3.5mm)** | 1, jack detect |
| 22 (0x16) | - | Output | N/C (disabilitato) | 15 |
| 24 (0x18) | Jack | I/O | **Mic esterno (jack 3.5mm)** | 3, jack detect |
| 25 (0x19) | - | I/O | **N/C ma VREF attivo!** | 15 |
| 26 (0x1a) | - | I/O | N/C (disabilitato) | 15 |
| 27 (0x1b) | - | I/O | N/C (disabilitato) | 15 |
| 29 (0x1d) | Fixed | Input | ATAPI interno (CD audio) | 0 |
| 30 (0x1e) | - | Output | Digital out | 15 |

### Selettori Input (per ADC)
| NID | Funzione |
|-----|----------|
| 35 (0x23) | Input Source Selector per ADC 8 |
| 36 (0x24) | Input Source Selector per ADC 7 |

---

## 3. BUG CRITICO: VREF SU PIN 0x19 (NID 25)

### Il problema
In Linux, il fix fondamentale per TUTTI i Sony Vaio con ALC269 e:

```c
// sound/pci/hda/patch_realtek.c
[ALC269_FIXUP_SONY_VAIO] = {
    .type = HDA_FIXUP_VERBS,
    .v.verbs = (const struct hda_verb[]) {
        { 0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREFGRD },
        { }
    }
},
```

Il pin 0x19 (NID 25) e elettricamente connesso al percorso audio anche se
logicamente "disabilitato" (config 0x411111f0). Se il suo VREF e impostato
a 80% (come fa Haiku con il quirk globale IVREF), si verifica una perdita
di segnale/crosstalk che causa distorsione o silenziamento degli speaker.

### Stato in Haiku
Il quirk globale alle righe 82-83 di `hda_codec.cpp`:
```cpp
{ HDA_ALL, HDA_ALL, HDA_ALL, HDA_ALL, HDA_QUIRK_IVREF, 0 },
```
Applica `HDA_QUIRK_IVREF` (50+80+100) a TUTTI i codec. Questo causa
`hda_widget_prepare_pin_ctrl()` (riga 695) a impostare VREF_80 sul pin 0x19,
che e ESATTAMENTE il problema che Linux corregge.

### Fix necessario
Aggiungere un quirk Sony che rimuova IVREF dal pin 0x19, oppure (piu semplice)
aggiungere il supporto per verb di inizializzazione specifici per codec.

---

## 4. BUG: MICROFONO NON FUNZIONANTE (collisione ADC su stream di cattura)

### Aggiornamento diagnosi (dal syslog reale)
L'input tree dell'ALC269 si costruisce correttamente dopo il fix selector:
```
build input tree
  look at input widget 7 -> selector 36 -> input pin 24 (mic esterno)
  look at input widget 8 -> selector 35 -> input pin 18 (mic interno)
build tree!
```
Il `build input tree failed` residuo nel syslog e del codec HDMI Intel
(function group senza ingressi), non dell'ALC269.

### Il bug vero
Il record stream lega ENTRAMBI gli ADC (7 e 8) allo stesso stream tag e
canale 0 (`hda_stream_start`, `channelNum += 2` commentato). Due convertitori
di input sullo stesso (tag, canale) collidono sul link seriale HDA ->
cattura corrotta/muta. Inoltre l'ADC 7 (usato per primo) instrada solo il mic
esterno (selector 36 non contiene il pin 18): il mic interno non veniva mai
catturato.

### Fix applicato
In `hda_audio_group_get_widgets`, per gli stream di record si usa un SINGOLO
convertitore, preferendo l'ADC il cui percorso di input attivo termina su un
mic Fixed (interno). Cosi il mic interno registra di default; il MUX del mixer
(selector 35: ingressi 24 25 26 27 29 18 11) permette di passare al jack.

### Il problema (storico)
Il syslog mostrava:
```
hda: build input tree
hda: build input tree failed
```

### Analisi del percorso
Il percorso input corretto sarebbe:
```
Pin 18 (mic interno) --> Selector 36 (0x24) --> ADC 7
Pin 24 (mic esterno) --> Selector 36 (0x24) --> ADC 7
```

La funzione `hda_audio_group_build_input_tree()` (riga 1081) cerca:
1. Widget di tipo `WT_AUDIO_INPUT` (ADC 7, 8)
2. Per ogni ADC, guarda i suoi input: ADC 7 ha input `36`
3. Chiama `hda_widget_find_input_path()` sul widget 36

Il widget 36 (NID 0x24) e un selector che connette i vari pin mic
all'ADC. Ma nel dump del syslog, i widget 35 e 36 NON vengono stampati
(il dump si ferma al 34). Possibili cause:

1. **Widget type non riconosciuto:** se il widget 36 e parsato come
   "Vendor defined" (tipo sconosciuto), `hda_widget_find_input_path()`
   non lo attraversa (il `switch` al tipo default restituisce `false`)

2. **Widget fuori range:** se `widget_count` non include i widget 35-36,
   `hda_audio_group_get_widget()` restituisce NULL

3. **Pin gia usato:** se il pin 24 (mic esterno) e gia marcato come
   `WIDGET_FLAG_OUTPUT_PATH` (perche ha capability I/O), il check a
   riga 972-973 lo esclude dal path input

### Fix necessario
Verificare il parsing dei widget 35-36. Se sono selettori mal-parsati,
forzare il tipo corretto. Altrimenti, verificare che i flag dei pin I/O
non blocchino il path input.

---

## 5. BUG: HDMI AUDIO FUNCTION GROUP FALLITO

### Il problema
```
hda: hda_audio_group_get_widgets failed for playback stream
hda: hda_audio_group_get_widgets failed for record stream
hda: Failed to setup new audio function group (No such device)!
```

### Analisi
Il controller Intel 5 Series ha DUE function group:
1. **FG 1:** Codec analogico Realtek ALC269 (funziona parzialmente)
2. **FG 2:** Codec digitale Intel HDMI (fallisce)

Il secondo function group contiene i widget HDMI/DP (NID 4, 5, 6 nel
secondo FG) che sono pin complex digitali. Il tree builder non riesce
a costruire ne il path playback ne quello record perche:
- I widget sono "Digital" con Content Protection (HDCP)
- Il tree builder cerca widget audio standard (DAC/mixer) che non esistono
  nella topologia HDMI (l'HDMI codec usa un path diretto pin→output)

### Priorita
Bassa. L'HDMI audio non e essenziale. Il messaggio di errore non impatta
il funzionamento dell'audio analogico. Linux gestisce l'Intel HDMI con un
codec separato (`snd-hda-codec-hdmi`).

---

## 6. HEADPHONE AUTOMUTE - GIA IMPLEMENTATO

### Analisi approfondita
Contrariamente all'analisi iniziale, il driver HDA di Haiku **ha gia**
l'automute implementato:

- `hda_audio_group_switch_init()` (riga 1212): abilita unsolicited responses
  su tutti i pin complex con presence detect
- `hda_codec_switch_handler()` (riga 1282): thread kernel che riceve gli
  eventi unsolicited via semaforo
- `hda_audio_group_check_sense()` (riga 1233): legge il presence detect
  del pin HP (PIN_DEV_HEAD_PHONE_OUT) e muta/smuta i pin speaker

Il pin 21 (HP, config 0x0221101f, device=PIN_DEV_HEAD_PHONE_OUT) e il pin
20 (Speaker, config 0x90170110, device=PIN_DEV_SPEAKER) hanno i device type
corretti per essere gestiti dall'automute esistente.

### Stato: NESSUN FIX NECESSARIO
L'automute dovrebbe funzionare correttamente una volta applicati i fix
VREF e input tree. Da verificare con test pratico.

---

## 7. RIEPILOGO PIN CONFIG BIOS vs LINUX

| Pin NID (Dec) | Pin NID (Hex) | Config BIOS | Tipo | Linux Override |
|-------|-------|-------------|------|---------------|
| 17 | 0x11 | 0x411111f0 | Disabilitato | Nessuno |
| 18 | 0x12 | 0x90a60920 | Mic interno | Nessuno |
| 20 | 0x14 | 0x90170110 | Speaker | Nessuno |
| 21 | 0x15 | 0x0221101f | Cuffie HP | Nessuno |
| 24 | 0x18 | 0x02a15830 | Mic esterno | Nessuno |
| 25 | 0x19 | 0x411111f0 | Disabilitato | **VREF_GRD** |
| 26 | 0x1a | 0x411111f0 | Disabilitato | Nessuno |
| 27 | 0x1b | 0x411111f0 | Disabilitato | Nessuno |
| 29 | 0x1d | 0x4015812d | BIOS interno | Nessuno |
| 30 | 0x1e | 0x411111f0 | Disabilitato | Nessuno |

Linux NON sovrascrive le pin config BIOS - invia solo il verb VREF_GRD
al pin 0x19. Le config BIOS del VPCEB3K1E sono corrette.

---

## 8. PIANO DI FIX

### Fix 1: VREF Sony Vaio (Critico) - Difficolta: Bassa
Aggiungere nel quirk table `kCodecQuirks[]` in `hda_codec.cpp`:
```cpp
{ 0x104d, HDA_ALL, REALTEK_VENDORID, 0x0269,
    0, HDA_QUIRK_IVREF },  // Sony Vaio ALC269: rimuovi IVREF globale
```
E dopo il setup del quirk, inviare il verb specifico:
```cpp
// SET_PIN_WIDGET_CONTROL(0x19, PIN_VREFGRD)
// NID 25 (0x19), verb 0x707, valore 0x01
```

### Fix 2: Input tree (Medio) - Difficolta: Bassa - IMPLEMENTATO
Forzato tipo WT_AUDIO_SELECTOR per NID 0x23 e 0x24 dell'ALC269.
Questi widget sono riportati dall'hardware come "vendor defined" ma
funzionano come selettori input per il routing mic → ADC.

### Fix 3: Headphone automute - NON NECESSARIO
L'automute e gia implementato nel driver HDA di Haiku tramite:
- `hda_audio_group_switch_init()` per abilitare unsolicited responses
- `hda_codec_switch_handler()` thread per ricevere eventi
- `hda_audio_group_check_sense()` per toggle mute speaker/HP

### Fix 4: HDMI audio (Opzionale) - Difficolta: Alta
Gestire il secondo function group Intel HDMI. Richiede supporto
specifico per codec HDMI (path diretto, content protection, ELD).
Non prioritario per questo laptop.

---

## 9. CONFRONTO QUIRK SYSTEM: HAIKU vs LINUX

| Caratteristica | Haiku | Linux |
|---|---|---|
| Tipo quirk | GPIO + VREF globali | Verb, Pin config, Funzioni custom |
| Granularita | Per codec/vendor | Per subsystem ID specifico |
| Pin config override | No | Si (completo) |
| Verb di init | No | Si (array di verb) |
| Jack detection | Si (base) | Si (unsolicited events) |
| Automute | Si (base) | Si (generico + per-codec) |
| Input switching | No | Si (automatico) |

Il sistema di quirk di Haiku e piu semplice di quello di Linux ma
funzionale. Per l'ALC269 i fix applicati (VREF + selector type) sono
sufficienti per il funzionamento base. Fix avanzati richiederebbero:
1. Pin VREF control per-pin (non globale) - AGGIRATO con quirk dedicato
2. Input source switching automatico (mic interno/esterno)

---

## 10. REGISTRI E VERB CHIAVE PER DEBUG

```
# Leggere pin config (GET_CONFIG_DEFAULT)
Verb: MAKE_VERB(codec_addr, NID, 0xf1c, 0)  // byte 0
Verb: MAKE_VERB(codec_addr, NID, 0xf1d, 0)  // byte 1
Verb: MAKE_VERB(codec_addr, NID, 0xf1e, 0)  // byte 2
Verb: MAKE_VERB(codec_addr, NID, 0xf1f, 0)  // byte 3

# Leggere pin widget control
Verb: MAKE_VERB(codec_addr, NID, 0xf07, 0)

# Impostare VREF_GRD su pin 0x19
Verb: MAKE_VERB(codec_addr, 0x19, 0x707, 0x01)

# Leggere presence detect
Verb: MAKE_VERB(codec_addr, NID, 0xf09, 0)  // bit 31 = jack present

# Codec addresses
ALC269 analog: codec_addr = 0
Intel HDMI:    codec_addr = 3 (tipicamente)
```

---

## 11. FILE SORGENTI DRIVER HDA HAIKU

Scaricati in `hda/`:
- `hda_codec.cpp` - Parsing codec, widget tree, quirk table (FILE PRINCIPALE)
- `hda_codec_defs.h` - Definizioni verb, tipi widget, pin capabilities
- `driver.h` - Strutture dati driver
- `hda_controller.cpp` - Init controller, DMA, interrupt
- `hda_multi_audio.cpp` - Interfaccia mixer/audio multi-channel

Repository: `https://github.com/haiku/haiku/tree/master/src/add-ons/kernel/drivers/audio/hda`
