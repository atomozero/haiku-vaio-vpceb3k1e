# Intel Gen5-Gen8 Hardware Wiki

Knowledge base empirica raccolta durante lo sviluppo del driver Haiku.
Queste informazioni NON sono nei PRM Intel — sono state scoperte
tramite test su hardware reale (Sony Vaio VPCEB3K1E, Gen5 Ironlake).

---

## 1. Regole del Ring Buffer

### 1.1 MAI resettare il ring dopo il boot
Il kernel driver inizializza il ring al boot (`setup_ring_buffer`).
Disable + re-enable del ring (write 0 to RING_CTL, then re-enable)
uccide il Command Streamer **permanentemente**. HEAD resta a 0x0
e non avanza mai piu. Solo un reboot ripristina il CS.

**Workaround**: Usare `gpu_ring_init()` che legge il TAIL hardware
e sincronizza la posizione senza toccare RING_CTL.

### 1.2 MMIO writes ignorate da userspace
Tutte le scritture MMIO da userspace (via `clone_area` o `/dev/misc/poke`
BAR0 0xF0000000) sono **silenziosamente ignorate**. Il kernel mappa i
registri con `B_KERNEL_WRITE_AREA` senza `B_WRITE_AREA`.

Verificato: scritto 0xDEADBEEF a scratch register, letto 0x0.
RING_BUFFER_HEAD bloccato a 0x5134, non resettabile.

**Workaround**: Tutte le scritture MMIO devono passare attraverso
kernel ioctl (`INTEL_RING_WRITE_TAIL`, `INTEL_RING_INIT_3D`).
La ring memory (graphics_memory) E' scrivibile da userspace.

### 1.3 MI_LOAD_REGISTER_IMM (LRI) non funziona su Gen5
LRI (opcode 0x22) nel ring hanga il CS dopo 2 DW su Ironlake.
I registri masked (MI_MODE, _3D_CHICKEN2, CACHE_MODE_0) si corrompono
con scritture raw via LRI.

**Workaround**: Usare `INTEL_RING_INIT_3D` kernel ioctl — il kernel
scrive i workaround registers via MMIO diretto.

### 1.4 Ring wrap-around
Il ring e' circolare (tipicamente 64KB). Quando `pos + needed > size`,
bisogna:
1. Aspettare che HEAD superi la nostra posizione (GPU ha consumato)
2. Riempire di MI_NOOP fino alla fine del ring
3. Resettare pos a 0
4. NON usare RING_RESET!

### 1.5 Coesistenza con app_server
Il ring e' condiviso tra accelerant (app_server) e tool/plugin userspace.
Due strategie:
- **Lock-based**: `acquire_lock(&ring->lock)` — serializza accesso
  ma richiede tracking della posizione condiviso
- **Stateless** (preferito): `gpu_ring` mantiene posizione locale,
  sincronizza con HW TAIL. Nessun lock necessario perche' le scritture
  a ring memory sono atomiche a livello di DWORD.

---

## 2. Media Pipeline (EU Kernel Dispatch)

### 2.1 Preamble a 10 comandi
Sequenza obbligatoria prima dei MEDIA_OBJECT:
```
1. MI_FLUSH
2. 3DSTATE_DEPTH_BUFFER (null)
3. PIPELINE_SELECT MEDIA
4. STATE_BASE_ADDRESS (8 DW Ironlake, 10 DW SNB+)
5. URB_FENCE (VFE_REALLOC + CS_REALLOC)
6. MEDIA_STATE_POINTERS (VFE state + IDRT)
7. CS_URB_STATE (entry_size=16, num_entries=1)
8. CONSTANT_BUFFER (CURBE data pointer + size)
9. N x MEDIA_OBJECT (inline data per thread)
10. MI_FLUSH (finale)
```

### 2.2 Max 48 EU thread per submission
VFE num_urb_entries non puo superare 48 su Gen5. Il 49esimo dispatch
causa IS stall (Instruction Stream stall) permanente.

### 2.3 Submission singola obbligatoria
Dopo MEDIA_OBJECT + MI_FLUSH, una seconda submission media nel ring
causa IS stall. Tutto (preamble + N dispatch + flush) deve andare
in una singola gpu_ring_advance().

### 2.4 Completion tracking via marker
NON usare HEAD polling per completion (fragile con ring condiviso).
Usare MI_STORE_DATA_IMM che scrive un tag in un buffer GTT:
```
DW0: 0x10400002 (MI_STORE_DATA_IMM, GGTT, 4 DW)
DW1: 0
DW2: marker_bo GTT offset
DW3: tag value (es. 0xBEEF0042)
```
CPU legge il marker address fino a trovare il tag.

### 2.5 CURBE layout per IDCT
- curbe_read_len = 20 (20 GRF di 32 byte = 640 byte)
- g1-g4: padding (128 byte)
- g5-g20: cosine table IDCT (512 byte, kIdctTableGpu)
- CONSTANT_BUFFER: 10 unita da 64 byte = 640 byte

### 2.6 Inline data position
Con `const_urb_entry_read_len = N`, i dati inline di MEDIA_OBJECT
arrivano al kernel EU a `g(1 + N)`, NON a g1. Il CURBE occupa g1..gN.

---

## 3. EU Kernel Assembly (gen4asm)

### 3.1 Subregister .N e' in BYTE
Senza flag `-a`, gen4asm interpreta `.N` come offset in byte:
```
g90.0<1>UW   → byte 0  = UW element 0
g90.8<1>UW   → byte 8  = UW element 4 (NON element 8!)
g90.16<1>UW  → byte 16 = UW element 8
```

### 3.2 {compr} widening UB->UW rotto
`mov (16) g70<1>UW g1.0<16,16,1>UB {compr}` scrive SOLO byte 0-15
di g70. Byte 16-31 restano stale (dati del thread precedente).

**Fix**: 2 × SIMD8 espliciti:
```asm
mov (8) g70.0<1>UW  g1.0<8,8,1>UB  { align1 };
mov (8) g70.16<1>UW g1.8<8,8,1>UB  { align1 };
```

### 3.3 {compr} su D destination INCREMENTA registro
`mul (16) g80<1>D ... {compr}` scrive correttamente: 8 D in g80 +
8 D in g81. Il {compr} incrementa perche D riempie tutto il GRF.

### 3.4 Send: header da GRF, payload da MRF
Su Gen4/5, send legge l'header dal registro GRF sorgente e il payload
da MRF (m1, m2, ...). Il GRF sorgente NON e' usato per payload.

### 3.5 MEDIA_BLOCK_READ: msg_type=2
Il PRM DevILK mostra msg_type=4 (3-bit), ma gen4asm usa encoding
a 2 bit dove msg_type=2 e' corretto. msg_type=4 fallisce silenziosamente.

### 3.6 OWord Block R/W: SURFTYPE_BUFFER obbligatorio
SURFTYPE_2D causa drop silenzioso per operazioni OWord Block.
Sempre SURFTYPE_BUFFER (type 4).

---

## 4. MPEG-2 Parser

### 4.1 DC level shift nel dc_pred
Il valore iniziale dc_pred `2^(7 + intra_dc_precision)` = 128 per
dc_prec=0 include gia il +128 level shift. NON aggiungere +128
dopo IDCT. Il kernel EU NON deve avere l'add +128.

### 4.2 EOB dopo blocco pieno (64 coefficienti)
Quando tutti i 64 coefficienti sono codificati (idx arriva a 63),
il bitstream contiene ANCORA un marcatore EOB (`10`, 2 bit).
DEVE essere consumato. Altrimenti: drift di 2 bit per blocco,
corruzione della slice intera. Causa #1 di "copertura 75%".

### 4.3 Error recovery: skip+continue, MAI resync
Quando `mpeg2_decode_intra_macroblock()` fallisce, saltare il MB
e continuare (`mbx++; continue;`). NON chiamare
`mpeg2_resync_to_next_slice()` — perde tutti i MB rimanenti.
Fix: copertura da 76-82% a 100%.

### 4.4 Table B-14 VLC codes 12-bit
I codici VLC 12-bit nella Table B-14 NON sono sequenziali.
L'ordine e' per frequenza statistica. Usare la mappatura corretta
da ISO 13818-2 o da ffmpeg `ff_mpeg1_vlc_table`.

### 4.5 CBP Table B-9 completa
Servono tutte le 64 entry. Una tabella incompleta causa corruzione
silenziosa nei P-frame inter MB.

### 4.6 quantiser_scale per-slice e per-MB
Deve essere calcolato da `q_scale_code` usando
`mpeg2_compute_quantiser_scale()`. Aggiornato per ogni slice header
e per ogni MB con flag quant.

---

## 5. Display Engine (Gen5 Ironlake)

### 5.1 LVDS EDID fallback
Su PCH (Ironlake+), GMBUS DDC fallisce su molti pannelli LVDS.
Catena di fallback:
1. GMBUS EDID (standard)
2. VESA EDID dal bootloader
3. Dati VBT dal BIOS
4. Porta abilitata dal BIOS (bit LVDS_PORT_EN)

### 5.2 Dual/single channel LVDS
NON derivare da P2 del PLL. Preservare la config BIOS leggendo
il bit `LVDS_CLKB_POWER_UP` dal registro LVDS corrente.

### 5.3 Panel fitter
Disabilitare quando la risoluzione richiesta == nativa del pannello.
1366 pixel (non divisibile) + panel fitter 1:1 = artefatti.

### 5.4 Watermark IBX
Il codice originale configura watermark solo per CPT (Sandy Bridge).
Aggiungere `INTEL_PCH_IBX` alla condizione.

---

## 6. Mesa / OpenGL Integration

### 6.1 glapi dispatch bridge
Mesa 25 usa `_mesa_glapi`, il sistema `_glapi`. Trovare libglapi.so
GIA' caricata via `get_next_image_info`, NON `load_add_on`.

### 6.2 ISL format table corruzione
La sparse array C99 della ISL table viene corrotta dal linker.
170/918 entry valide. Entry B8G8R8X8_UNORM ha bpb=0 → crash.
Fix: binary patch o compilare ISL con -O0.

### 6.3 SET_TILING via intel_ioctl
crocus_bufmgr.c usa `ioctl()` raw bypassando il DRM shim.
Cambiare in `intel_ioctl()` per passare attraverso la traduzione.

### 6.4 Batch #3 hang (APERTO)
EXECBUF2 #1 (state setup): OK. #2 (glClear): OK.
#3 (readback): HANG su 3DSTATE_GLOBAL_DEPTH_OFFSET_CLAMP.
Differenza: DW2 = 0x00c7012b (batch #2) vs 0x00000000 (batch #3).
Serve debug DW-per-DW del batch #3 vs #2.

---

## 7. Kernel Driver

### 7.1 Ioctl per TAIL write
`INTEL_RING_WRITE_TAIL`: unico modo per scrivere TAIL da userspace.
Il kernel riceve tail_value e scrive il registro MMIO.

### 7.2 Ioctl per workaround 3D
`INTEL_RING_INIT_3D`: scrive MI_MODE (0x209C), _3D_CHICKEN2 (0x208C),
CACHE_MODE_0 (0x2120) via MMIO diretto nel kernel.

### 7.3 Build kernel driver
ABI differenze tra build tree e kernel runtime (es. `_mutex_unlock`
vs `mutex_unlock`) richiedono mutex compatibility shim. Compilare
con `-fPIC`. Blacklistare driver di sistema via
`/boot/system/settings/packages`.

### 7.4 RING_RESET — NON USARE
`INTEL_RING_RESET` funziona UNA volta dopo il boot. La seconda
volta il CS muore permanentemente. Il kernel lo chiama al boot
in `setup_ring_buffer()` — da quel momento, mai piu.
