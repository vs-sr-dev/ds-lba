# PORTING_NOTES_NDS — LBA1 su Nintendo DS (M4 bring-up)

Stato a fine sessione: **lba1ds.nds boota su melonDS e arriva alla PRIMA SCENA
DI GIOCO** — logo Adeline → bumper EA → menu principale (navigabile) → New
Game → pagine di intro (testo typewriter su immagini Twinsun) → cella di
Twinsen renderizzata (brick iso + modello 3D) — stabile per minuti. Dopo la
sessione performance (vedi "## Performance"): **50 fps (cap tick-lock) nel
gameplay incrementale** con ~6.5 ms per attore 3D; il full-redraw della
griglia (scroll camera / cambio cubo) resta un hitch da ~235 ms a botta.
**Salvataggi PERSISTENTI su SD** (sessione savegame: libfat + routing in
nds_sys.c, vedi "## Savegame su fat:/"): i save vanno in `fat:/lba1/save/`.
L'holomap ora esiste (translate/texture.c, sessione parallela).
**Audio: SFX + voci VOX funzionanti** (sessione audio, vedi
"## Audio ARM7/calico"). **Musica d'area + jingle** in streaming dalla SD
(vedi "## Musica") — scritta, compilata, **ancora da collaudare a orecchio**.

Gli edit a engine/ fatti in questa sessione sono loggati in
`platform/sdl/PORTING_NOTES.md`, sezione "Edit per NDS" (#12-28): il grosso è
la campagna **accessi disallineati** (ARMv5TE non trappa: un load 16/32-bit a
indirizzo dispari ritorna dati ruotati — su x86 tutto funzionava). Build SDL
riverificata verde + smoke test PC ok dopo gli edit.

## Comandi

```powershell
# build (Docker devkitARM, immagine calico-based)
docker run --rm -v ${PWD}:/work -w /work/platform/nds devkitpro/devkitarm:latest make -j8

# run
<melonDS>\melonDS.exe <repo>\platform\nds\lba1ds.nds

# build SDL di controllo (obbligatoria dopo ogni edit a engine/)
$env:PATH = 'C:\msys64\mingw32\bin;C:\msys64\usr\bin;' + $env:PATH
make -C <repo>\platform\sdl -j8
```

Smoke test non interattivo (harness): `autoenter` = N secondi di pulse Return
ogni ~0.7s (equivalente di LBA_AUTOENTER dello shim SDL); `autopause` = N → a
N secondi dal boot tiene premuto START ~0.4s (repro headless menu pausa);
`autowalk` = N secondi di UP tenuto; `autosave` = N → a N secondi esegue la
sequenza scriptata menu pausa → Save game → nome "DS" (4 impulsi Esc iniziali:
un dialogo idle può mangiarne fino a 2); `autoload` = N → pulsa Esc (skip
loghi) fino a N secondi, poi main menu → Continue → primo save della lista.
**I file harness NON stanno più in nitrofiles/**: la ROM di default è pulita
per costruzione (il Makefile ERRORE se ne trova uno in nitrofiles/); per una
ROM strumentata usare `make diag DIAG_AUTOENTER=60 DIAG_AUTOSAVE=85` →
`lba1ds_diag.nds` (stage in build/nitrofs_diag, vedi Makefile).

## Toolchain: ATTENZIONE, libnds nuova (calico)

`devkitpro/devkitarm:latest` è la libnds 2.x basata su **calico**:
- compile: servono `-D__NDS__ -DARM9 -I$(CALICO)/include` oltre a libnds;
- link: `-specs=$(DEVKITPRO)/calico/share/ds9.specs`, librerie
  `-lfilesystem -lfat -lnds9 -lcalico_ds9 -lm`;
- ndstool: serve l'ARM7 esplicito `-7 $(CALICO)/bin/ds7_maine.elf`
  (niente più default7 integrato);
- API vecchie (timerStart, irqSet, consoleDemoInit, bgInit...) esistono ancora.
- `pmMainLoop()`/`swiWaitForVBlank()` funzionano come da m2-hqrview.

Il Makefile replica le convenzioni SDL (gnu89 + watcom_compat force-included,
`-x c`, `-fsigned-char -fcommon`, `-DCDROM -DPORT_NDS`, compat/inc) e in più fa
**dependency tracking `-MMD`** (il Makefile SDL non lo fa: qui, cambiando
watcom_compat.h, mezz'ora è andata in una build mista inconsistente).

## Architettura platform/nds

- `main_nds.c` — defaultExceptionHandler (guru meditation con pc/registri al
  posto dello schermo bianco: indispensabile), consoleDemoInit (console DS sul
  sotto), nitroFSInit + `chdir("nitro:/")`, hook autoenter, chiama `lba_main`.
- `nds_video.c` — video path di m2-hqrview: main engine MODE_5_2D, BG3
  rotoscale bitmap 8bpp 512x256 in VRAM A.
  - SVGA (gioco CD 640x480): `Flip`/`CopyBlockPhys[Clip]` decimano Log 2× al
    volo (sample, non average) → 320x240 → scala hw a 256x192. Scritture VRAM
    SOLO a u32 (VRAM ignora i byte write).
  - MCGA (320x200: FLA/SceZoom): `Phys` è un buffer RAM di 64000 byte (il
    codice engine `MCGA.C` ci fa memcpy dentro); il VBlank IRQ lo presenta
    ogni frame. `CopyBlockPhysMCGA` (edit #14) scrive in `Phys`.
  - Palette: quantizzazione DAC 6-bit fedele (>>2, re-espansa) poi >>3 →
    BGR555 hardware, applicata subito ⇒ i fade funzionano senza re-blit.
  - `Vsync()` = swiWaitForVBlank.
- `nds_sys.c` — timer hardware 2 a **50.005 Hz** (bus/256/2618, MAI il VBlank
  a 59.83) che incrementa TimerRef/TimerSystem con la logica esatta di
  TIMER_A.C (solo ++ relativi: RestoreTimer riavvolge TimerRef). Input su
  VBlank IRQ (scanKeys): dpad→Joy(1/2/4/8), B→Fire1(space), A→Fire2(return),
  **L→Fire4(ctrl)+Key=0x1D** (= CTRL DOS: tieni L per il pannello behaviour
  classico, scegli col dpad), SELECT→Fire8(alt), **X/Y/R→behaviour diretto
  Normal/Sporty/Aggressive** via PORT_TouchComportement (Discreto: touch o
  pannello L), START→Key=1(Esc); GetAscii ring buffer stile int16h
  (A=0x1C0D, B=0x3920, START=0x011B). NB: la mappa X/Y/L/R→FuncKey F1-F4
  del bring-up era codice morto (l'engine non legge mai FuncKey).
  Mouse: stub. DosMalloc→calloc contabilizzato.
- `stubs.c` / `asm_stubs.c` — COPIE degli stub SDL (non condivisi). Dalla
  sessione audio stubs.c copre solo MIDI+CD+DLL: Wave e Mixer sono reali in
  `nds_audio.c`.
- `watcom_compat.h` (variante NDS) — oltre a uccidere le keyword Watcom:
  `stricmp/strcmpi/strnicmp→strcasecmp` (dichiarati a mano: <strings.h> di
  newlib esporta `index()` che collide con le variabili `index` di GIF.C/PCX.C
  → si include <string.h> e poi `#define index lba1_index`), `_MAX_*`, e i
  **wrapper stdio/heap** (sotto).
- `compat_nds/` — `process.h` (spawnl→-1) e `direct.h` (→unistd.h) che newlib
  non ha; `compat/` = copia dei wrapper `../LIB386/...` dello shim SDL.

### Wrapper stdio (msvcrt-emulation) — nds_sys.c

L'engine passa FILE* NULL/stantii a fseek/fread in vari punti (su DOS/msvcrt
la libc valida i parametri e fallisce garbata; newlib fa data abort). La
variante NDS di watcom_compat.h rimappa nei soli TU engine:
`fopen/fclose/fread/fwrite/fseek/ftell` → `NDS_*` con un registro dei FILE*
aperti (16 slot): handle non registrato ⇒ log `[FIO]` + errore soft.
`NDS_fopen` inoltre normalizza i path: salta `.\`, `\`→`/`, **lowercase**
(i file in nitrofiles/ sono tutti lowercase), cwd = nitro:/. Le write su
nitroFS falliscono ⇒ `Save()`/`Def_WriteString` ritornano FALSE, non fatale
(savegame: vedi Aperti).

### Heap strumentato — nds_sys.c

`malloc/calloc/realloc/free` engine → wrapper con header 16B + canary di coda
4B + lista dei blocchi vivi: ogni alloc logga `[MEM]` se ≥32KB (con totale e
spare) e fa `PORT_HeapCheck()` (verifica tutti i canary → `[HEAP] OVERFLOW
after N-byte block` al momento del danno). **`NDS_realloc` con shrink è
IN-PLACE obbligatoriamente**: `Mshrink`/`LoadMalloc_HQR` ignorano il valore di
ritorno (un realloc che sposta lascia PtrPal/LbaFont dangling — successo così).
I wrapper valgono solo per i TU engine: FILE structs e allocazioni newlib
restano fuori (per questo il canary non aveva beccato l'overflow di BufOrder
— trovato invece ragionando sui margini LZS, edit #19).

## Bug-hunt della sessione (per il prossimo che tocca un target nuovo)

1. **Schermo bianco totale** = crash prima/nella console init → mettere SEMPRE
   `defaultExceptionHandler()` come prima riga: la guru meditation con pc/addr
   + addr2line sul .elf risolve in un minuto. Primo crash: `fopen(NULL)` da
   `FileSize(getenv("ADELINE"))`.
2. **Immagini garbage con logica giusta** = `Expand()` leggeva i token LZ a
   UWORD da offset dispari (#15). Se succede di nuovo su altro target: è
   SEMPRE l'allineamento.
3. **Heap corruption deterministica** (FILE struct piena di byte ASCII):
   `Load_HQR` scrive fino a `SizeFile+500` nel buffer destinazione (il
   compresso viene copiato in coda e decompresso in-place). OGNI buffer dest
   deve avere +500 (#19). `Screen` ce l'ha ("+ decomp marge"), BufOrder no.
4. **Scena garbage + write selvagge**: DecompColonne (#27) — length RLE letta
   disallineata → scrive oltre BufCube.
5. Makefile senza `-MMD` + edit agli header force-included = build mista
   silenziosamente incoerente. Ora c'è il dependency tracking.

## RAM — breakdown e verdetto 4MB

Binario ARM9 (thumb, -O2, gc-sections): text 287K + data 13K + bss 114K =
**414K**. Allocazioni engine a regime (log [MEM] alla prima scena):

| Blocco | KB |
|---|---|
| Phys (MCGA backbuffer) | 62 |
| Log 640x480 | 300 |
| Screen 640x480+500 | 300 |
| BufSpeak (DosMalloc) | 256 |
| BufCube 64x25x64x2 | 200 |
| BufferBrick | 353 |
| HQM (pool scene/grid/mask) | 391 |
| HQR sprites (SpriteMem min) | 49 |
| HQR anims (AnimMem min) | 98 |
| BufText/InvObj/minori | ~80 |
| **Totale heap** | **~2090** |

Spare a regime: **~1.5MB** (fake_heap_end − sbrk). Verdetto: **il gioco sta in
un DS liscio da 4MB senza tagli e senza DSi mode**, con margine per l'audio
(SampleMem min 200K se/quando Wave_Driver_Enable diventa vero) e per il
buffer holomap. `Malloc(-1)`→0 fa già usare all'engine i minimi (SpriteMem
50K/SampleMem 200K/AnimMem 100K). Niente VOX/FLA nel nitroFS (il .nds è 9.6MB
di soli HQR core).

## Performance (sessione ottimizzazione — misure su melonDS full-speed)

### Strumenti (restano nel repo, attivi)

- **Profiler per fasi**: `nds_prof.c` intercetta con `ld --wrap` (vedi `WRAPS`
  nel Makefile — zero edit all'engine) AffScene/Cls/CopyScreen/ClsBoxes/
  AffGrille/SetInterAnimObjet2/AffObjetIso/DrawOverBrick[3]; il present è
  strumentato direttamente in `PresentRect640`. Ogni 50 frame stampa sul
  console 7 righe `P0..P6`: μs medi/frame per fase (`lg` logica+attesa tick,
  `cb` ClsBoxes, `cl` Cls, `cp` CopyScreen, `gr` AffGrille, `an` anim interp,
  `ob` AffObjetIso, `ov` overbrick, `pr` present, `ot` resto AffScene,
  `sf`/`si` = AffScene totale medio nei frame full/incrementali, `n`/`q` =
  frame/frame-full nella finestra, `o` = attori/frame, `fps` =
  NbFramePerSecond, `stk` = margine minimo stack DTCM in byte).
- **Timebase**: `tickGetCount()` di calico (SYSTEM_CLOCK/64 = 1.909 μs/tick),
  avviata con `tickInit()` in main_nds.c. Sanity check: `P0 w` ≈ 20000 μs a
  50 fps.
- **`make PROFFULL=1`**: ROM diagnostica con AffScene forzata al full-redraw
  ogni frame (caso "redraw pieno"/scroll camera sostenuto). Non shippare.
- **Smoke input**: oltre a `nitrofiles/autoenter`, `nitrofiles/autowalk` =
  secondi di UP tenuto (con svolte periodiche) dopo la fine dell'autoenter.
  ATTENZIONE: in gioco Return apre l'HoloMap (ora funzionante, texture.c) —
  l'autoenter NON deve durare oltre l'ingresso in scena (60 s ok).

### FIX timer (bug latente, non solo profiling)

`timerStart(2, …)` di libnds programma l'HW TM2, ma **calico possiede TM2
(system tick, `tickGetCount`) e TM3 (tick task) sull'ARM9**: il tick di gioco
a 50 Hz lo stompava dal bring-up (tickGetCount congelato, tick task calico a
rischio). Spostato su **TIMER 0** (`nds_sys.c`); TM1 resta libero. Su ARM9
gli unici timer liberi per l'app sono TM0/TM1.

### Profiling PRIMA (prima scena, cella recintata, 1 attore visibile, μs/frame)

| fase | μs | note |
|---|---|---|
| lg (logica+attesa tick) | 9000-9900 | al cap 50 fps include il busy-wait |
| cb ClsBoxes | 430-500 | |
| an SetInterAnimObjet2 | ~95 | |
| **ob AffObjetIso** | **8600-9100** | **collo di bottiglia: ~9 ms PER ATTORE** |
| ov DrawOverBrick | 200-1000 | |
| pr present | 116-140 | dirty box |
| ot resto AffScene | ~290 | |
| **si AffScene tot (incr.)** | **10000-11000** | |

Con ~4-5 attori a schermo + full redraw = i 12-13 fps osservati al bring-up.
Il present NON era il collo (130 μs nei frame incrementali).

### Mosse applicate (delta misurato, stessa scena)

1. **ITCM+ARM32 per il codice caldo** (macro `PORT_FASTCODE` in
   `translate/port_fast.h`, vuote su PC): AffObjetIso e statici (p_ob_iso),
   RotList/TransRotList/RotMatIndex2/Rot/proiezioni (p_trigo), ComputePoly_A/
   EdgeGauche/EdgeDroite/ClipPolyEdge/ComputeSphere_A (s_poly), tutti i
   filler SVGAPoly* + FillVertic_A (s_fillv), AffGraph (graph_a), Line_A
   (s_line), PresentRect640 (nds_video). `CPYMASK.C` promosso INTERO in ITCM
   senza edit: oggetto rinominato `CPYMASK.itcm.o` (+ `-marm`) — ds9.ld mette
   in ITCM il .text degli oggetti `*.itcm.*`. ITCM: 20112/32736 byte.
2. **DTCM** (`PORT_FASTBSS`/`PORT_FASTDATA`): List_Point (3K), TabVerticG/D +
   TabCoulG/D (3.8K), TabMat (1.1K), matrici LMatrice* e scalari caldi
   (compteur/lAlpha…/X0…/Xp/Yp/XCentre/YCentre). **.dtcm.bss = 8.2K**.
3. **Present**: pack loop a letture u32 (2 load per parola invece di 8 byte
   load) + ITCM/ARM. pr 130→110 μs (dirty box); pesa nei frame full.

   → **ob 8.9 → 6.9-7.0 ms (−23%), si 10.5 → 8.1 ms (−23%)** su melonDS.
   NB: melonDS non emula le cache al 100%: il guadagno ITCM/DTCM su hardware
   reale dovrebbe essere maggiore, i rapporti tra fasi restano affidabili.

4. **-O3 sui translate caldi: REVERTITO** — nessun guadagno misurabile
   (±1%) col codice caldo già in ITCM; costava 1.1KB di ITCM.
5. **GRILLE.C in ITCM: REVERTITO** — gr 252.7→251.1 ms nel test full-redraw:
   il costo di AffGrille non è il walk del cubo 64×25×64 ma i blit RLE di
   AffGraph (già in ITCM). Non vale 3.6KB di ITCM.
6. **memcpy/memset override ITCM/ARM (`nds_fastmem.c`)** — AffGraph fa una
   memcpy/memset newlib per OGNI run RLE (centinaia di migliaia di copie da
   2-20 byte in un full redraw): definizione locale con fast-path per n
   piccoli + blocchi u32×8, compilata `-ffreestanding -fno-builtin` (altrimenti
   GCC ri-pattern-matcha i loop in call a se stessa). Misurato sul full
   redraw forzato: **gr 252.7→207.0 ms (−18%), Cls 5.2→4.0 ms,
   CopyScreen 8.3→7.6 ms, AffScene full 283→235 ms**; anche ob scende
   6.9→6.4 ms. Vince su tutto (fread incluse), nessun --wrap: l'oggetto
   utente batte il membro d'archivio libc.

### DTCM: ATTENZIONE allo stack (guru meditation)

Lo stack del thread principale di calico sta in CIMA alla DTCM e cresce verso
il basso dentro quel che `.dtcm.bss` lascia libero (budget totale 16000 byte).
Il primo tentativo (15KB di dati in DTCM) ha lasciato ~1KB di stack → data
abort dentro calico (mutex/ntrcardRomRead) al boot. Il probe `P6 stk` (paint
0x5A in main_nds.c) misura il margine minimo reale: **con 8.2KB di dati in
DTCM il margine osservato è ~2.7KB** (uso stack max ~5.1KB attraverso
boot/nitroFS/menu/intro/scena). Non scendere sotto ~1.5KB di margine.

### Profiling DOPO

Frame incrementale (gameplay normale, stessa scena, μs/frame):

| fase | prima | dopo |
|---|---|---|
| ob AffObjetIso (1 attore) | 8600-9100 | **6300-6600** |
| cb ClsBoxes | 430-500 | 300-500 |
| ov DrawOverBrick | 200-1000 | 190-1100 |
| pr present (dirty box) | 116-140 | 103-117 |
| an / ot | 95 / 290 | 95 / 285 |
| **si AffScene tot** | **10000-11000** | **~7100-8500** (−23%) |
| fps | 50 (cap) | 50 (cap) |

Frame FULL-REDRAW (misura `make PROFFULL=1`, forzato ogni frame — equivale
allo scroll camera; μs/frame):

| fase | prima* | dopo |
|---|---|---|
| cl Cls | 5161 | 4014 |
| cp CopyScreen | 8315 | 7597 |
| **gr AffGrille** | **252660** | **206984** |
| pr present full | 8622 | 8621 |
| ob + ov | ~8100 | ~7300 |
| **sf AffScene full tot** | **283116** | **~234800 (−17%)** |
| fps (full forzato ogni frame) | 3 | 4 |

\* "prima" della colonna full = build già con ITCM/DTCM round A (il PROFFULL
è nato dopo); il baseline assoluto era ancora più lento (present a byte,
AffGraph thumb in main RAM).

### Stato finale / margini residui

- **Gameplay normale (frame incrementali): 50 fps** (cap tick-lock) con
  ~11-12 ms di margine per frame — regge 3-4 attori a schermo prima di
  scendere sotto i 50; a 5+ attori ~25-30 fps stimati (ob ≈ 6.5 ms/attore).
- **Il redraw pieno resta il muro: ~235 ms** (era ~283+ nel round A, di più
  al bring-up). NON è sostenuto in gioco: il recenter camera di PERSO.C è
  UN frame full ogni attraversamento di bordo schermo (poi si torna
  incrementali) → hitch percettibile a ogni scroll, non slideshow costante.
- Margini futuri sul full redraw (in ordine di resa attesa):
  1. **AffGrille incrementale/dirty**: nel caso scroll il 90% dei brick resta
     identico — shift del Log + repaint della sola striscia nuova
     eliminerebbe quasi tutto il costo (grosso intervento engine, va
     progettato: ListBrickColon/DrawOverBrick dipendono dal repaint totale).
  2. **Inner loop di AffGraph**: le run RLE sono 2-20 byte — inline della
     copia (niente call memcpy) dentro graph_a.c darebbe un altro taglio
     stimato 20-40% di gr (richiede di toccare translate/graph_a.c oltre le
     macro: da concordare, il file è condiviso col PC).
  3. Cls+CopyScreen (11.6 ms): evitabili solo cambiando la strategia di
     restore (Screen è il "pulito" per ClsBoxes) — poco succo.
- melonDS non emula cache/TCM al 100%: su hardware reale i rapporti possono
  spostarsi (ITCM/DTCM dovrebbero rendere di più, la main RAM di meno).
  `NbFramePerSecond` resta la metrica finale da verificare su console vera.

## Savegame su fat:/ (sessione savegame)

**I salvataggi sono persistenti sulla SD** (flashcart via DLDI auto-patchato;
melonDS via immagine SD/folder-sync). nitroFS resta read-only per gli asset.

### Come funziona (tutto in platform/, 0 edit engine)

- `main_nds.c`: `PORT_FatInit()` (nds_sys.c) PRIMA di nitroFSInit —
  `fatInitDefault()` + `mkdir fat:/lba1` + `mkdir fat:/lba1/save`. Se
  fallisce (es. emulatore senza DLDI): log `fat: NO SD (DLDI) - saves
  DISABLED` e il gioco gira come prima (Save() fallisce garbato in-engine,
  "Error Writing Saved Game" nel menu).
- `nds_sys.c` — routing per **basename** (dopo la normalizzazione lowercase):
  - `*.lba` → `fat:/lba1/save/<nome>` — sono SOLO i savegame (S*.LBA random
    da `SaveGameWithName`, AUTOSAVE.LBA; gli asset sono .hqr/.cfg/.lst).
    Vale per fopen (read+write), quindi Load/Save/FileSize/Exists/Copy.
  - `lba.cfg` → in scrittura sempre su fat; in lettura la copia fat vince
    SOLO se esiste (primo boot: si legge quella nitroFS). Così i volumi
    salvati da "save settings" persistono.
  - `__tempo.def` (lo scratch `c://__tempo.def` di DEF_FILE.C) → su fat.
  - `SYS_FindFirst("*.LBA")` (PlayerGameList/FindPlayerFile, enumerazione
    slot del file-selector) → opendir di `fat:/lba1/save` invece della cwd.
  - `remove()` → `NDS_remove` (nuovo wrapper in watcom_compat.h): stessa
    normalizzazione+routing (menu "détruire une sauvegarde", __tempo.def).
  - Ogni open/remove ruotato logga `[SAV] ...` sul console.
- **AUTOSAVE**: l'engine salva `AUTOSAVE.LBA` da solo a OGNI cambio cubo
  (InitCube: `NewCube != oldcube && !DisableAutoSave`) — verificato live su
  melonDS alla prima scena. Il save manuale è nel **menu pausa** (START →
  Save game → "create new saved game" → nome con la griglia lettere →
  Return); al riavvio: main menu → **Continue saved game** → lista slot
  (AUTOSAVE + salvataggi con nome).
- I save sono nel formato di QUESTO port (struct GCC senza pack, vedi nota
  savegame in PORTING_NOTES.md): NON compatibili coi .LBA DOS, ma stabili
  tra build finché T_OBJET & co. non cambiano layout.

### Setup melonDS (config usata e verificata)

`<melonDS>\melonDS.toml`, sezione `[DLDI]`:

```toml
[DLDI]
Enable = true
ImagePath = "<repo>\\platform\\nds\\build\\melonds_sd.img"
ImageSize = 256
ReadOnly = false
FolderSync = true
FolderPath = "<repo>\\sd_root"
```

melonDS sincronizza `sd_root/` → immagine al boot e riscrive i file
modificati in `sd_root/` alla CHIUSURA (i save compaiono in
`sd_root/LBA1/save/*.lba` dopo aver chiuso l'emulatore). NB: melonDS
inserisce anche la ROM caricata nella root dell'immagine (per argv) e la
sincronizza in sd_root — file da ignorare/cancellare.
Su flashcart: i save finiscono in `SD:/lba1/save/` (la dir viene creata al
primo boot).

### Test end-to-end (harness diag, vedi "Smoke test")

1. `make diag DIAG_AUTOENTER=60 DIAG_AUTOSAVE=85` → run: a ~85s lo script
   apre il menu pausa e salva col nome "DS" → `[SAV] fopen(wb)
   fat:/lba1/save/s####.lba` sul console, file in sd_root dopo la chiusura.
2. `make diag DIAG_AUTOENTER= DIAG_AUTOLOAD=45` → run: skip loghi a colpi
   di Esc, al menu → Continue saved game → carica il primo slot → la scena
   riparte dal punto salvato.

## Freeze dossier

L'utente riporta piccoli freeze non deterministici su hardware reale (non
guru: FREEZE). Stato indiziario:

1. **Type-pun Info/Info1 (edit #33) — FIXATO, primo sospettato.**
   `*(ULONG *)(&ptrobj->Info)` in OBJECT.C (MOVE_RANDOM ×3) e PERSO.C
   (spawn pinguino): `offsetof(T_OBJET,Info)` ≡ 2 (mod 4) — su ARMv5 la
   STR ignora i 2 bit bassi ⇒ la write a 32 bit del timer atterrava su
   `{OffsetLife, Info}` **corrompendo OffsetLife** dell'attore: al DoLife
   successivo lo script riparte da un offset selvaggio (opcode garbage →
   loop/stati incoerenti = freeze senza exception, oppure comportamenti
   erratici). Colpiva OGNI attore MOVE_RANDOM — le guardie/animali fuori
   dalla Citadella, esattamente dove l'utente sta giocando ora. Static
   assert di guardia in nds_ui_glue.c. Grep sistematico di tutto engine/:
   nessun altro type-pun `*(TYPE *)(&...)` residuo (GRILLE/MESSAGE leggono
   da basi allineate con offset pari — ok).
2. **Finestra WaveMove sulle voci (aperto, non toccato)**: le voci suonano
   direttamente da BufSpeak (nessuna copia nel pool); se un nuovo dialogo
   interrompe una voce in corso l'ARM7 legge il buffer mentre viene
   riscritto — artefatto audio ("crac"), presente anche su DOS, NON è un
   freeze del gioco. Restare in ascolto; eventuale fix = copiare anche le
   voci nel pool con budget maggiorato (~180K in più).
3. **Busy-wait su condizione che non arriva** (es. `while(!WavePause())`,
   attese su WaveInList): i mirror di stato canali sono calcolati a tick
   ARM9 (niente PXI nelle query) — nessun caso noto di stallo, ma se si
   ripresenta un freeze il probe è: console sotto (SELECT tenuto 1s),
   guardare se le righe P0-P6 del profiler continuano a scorrere (ISR viva,
   MainLoop bloccato) e se ci sono `[wave]` pendenti.

Durante i test di questa sessione (4 run melonDS da ~2 min, boot→scena→
menu pausa→save→load): nessun freeze osservato.

## Audio ARM7/calico (nds_audio.c — sessione audio)

**SFX + voci VOX funzionano.** Wave + Mixer reali in `nds_audio.c` (gli stub
sono stati rimossi da stubs.c); semantica DOS come da spec in
`platform/sdl/PORTING_NOTES.md` "Audio (sdl_audio.c)". Il parse VOC +
pitchbend è condiviso col PC in **`platform/audio_common.h`** (header-only,
incluso da sdl_audio.c e nds_audio.c dopo port.h; build SDL riverificata
verde + smoke LBA_WAVDUMP: voce cella 11111 Hz, peak 4317, 50% campioni
non-zero).

### Architettura

- **Mixing in hardware**: ogni sample engine → 1 canale hw dei 16, pilotato
  dall'ARM9 con l'API sound di calico (`soundPreparePcm/soundStop/
  soundChSetVolume/...`, `<calico/nds/arm9/sound.h>`): i comandi viaggiano
  via PXI verso il server audio dentro l'ARM7 stock di calico
  (`ds7_maine.elf`) — **zero codice ARM7 custom**.
- `InitWave`: `soundInit()` + `soundPowerOn()` + mixer config; nessun timer
  hw toccato (TM0 gioco e TM2/TM3 calico intatti), nessun IRQ.
- **Niente PXI nelle query**: WaveInList è pollato fitto dai dialoghi, quindi
  lo stato canali è specchiato in una tabella ARM9 (`wch[16]`) e la fine di
  ogni sample è CALCOLATA: `ticks = samples * hwtimer / 32` (esatto:
  TICK_FREQ = SYSTEM_CLOCK/64, SOUND_CLOCK = SYSTEM_CLOCK/2) + margine 4 ms.
  Repeat=1 → SoundMode_OneShot (si ferma da solo in hw); Repeat=0 →
  SoundMode_Repeat infinito (fermato da WaveStop*); Repeat>1 (raro,
  BigSampleRepeat di GERETRAK) → loop hw + **watchdog thread calico**
  (prio MAIN-1, stack 1.5KB, sleep 50 ms) che fa `soundStop` allo scadere.
  Il mutex `wmutex` serializza tabella + chiamate sound tra main e watchdog.
- **Volumi**: engine passa L/R già pannati (0..128 SFX, 512/512 voce) →
  vol hw 11-bit = (L+R)*2 (voce satura a 2047 = esattamente il rapporto 4×
  voce/SFX del DOS), pan = R*127/(L+R). Master = WaveVolume*MasterVolume
  del CFG sul volume mixer hw 7-bit (`soundSetMixerVolume`). WavePause =
  master a 0 (i canali continuano muti: la contabilità a tick resta
  coerente), WaveContinue lo ripristina.

### Memoria sample (l'ARM7 legge la main RAM: byte fermi + cache flush)

- **SFX**: copiati XOR 0x80 (VOC unsigned → hw signed) in un **pool LRU
  keyed sull'handle HQR** (budget 320 KB, 64 entry, malloc per entry,
  pad a parola con silenzio, `armDCacheFlush` dopo il fill). Così
  **WaveMove è un memmove puro**: l'hw non legge mai lo heap engine,
  niente fixup dei puntatori. Le entry con un canale attivo non vengono
  mai evictate. Budget usato a regime: pochi 10-100 KB (i sample sono
  piccoli); heap spare resta > 1 MB (il pool + SampleMem 200 KB
  dell'engine stanno nel margine da ~1.5 MB).
- **Voci (handle 0x1234)**: play DIRETTO da BufSpeak con XOR in place +
  flush (niente copia da 172 KB). Sicuro perché (MESSAGE.C): PlaySpeakVoc
  ricarica BufSpeak fresco prima di OGNI WavePlay, TestSpk carica la parte
  successiva solo quando WaveInList dice finito, BufSpeak è un blocco
  DosMalloc fisso mai toccato da WaveMove. Unica finestra: un nuovo dialogo
  che interrompe una voce in corso sovrascrive il buffer mentre l'hw legge
  — stesso artefatto "crac HP" del driver DOS originale (commentato in
  MESSAGE.C), non regressione.

### VOX / CFG

- In nitrofiles ci sono SOLO `vox/en_gam.vox` (1.8 MB) + `vox/en_000.vox`
  (6 MB) = voci inglesi della prima isola; .nds totale ~17.8 MB. Il CFG
  nitroFS ha già `LanguageCD: English`, `WaveDriver: W_SB16.DLL`,
  `WaveRate: 22000` (letto per parità protocollo ma ignorato: ogni canale
  hw ha il suo timer), `MixerDriver: NoMixer`, `FlagKeepVoice: ON`.
  Il **BYOA finale (libfat)** porterà tutte le lingue/isole.
- I nomi file nitroFS sono lowercase (NDS_fopen normalizza il path).

### Limiti noti

- Interpolazione lineare del driver DOS sulle voci: l'hw DS non interpola
  (suono leggermente più "raw", non udibile a 11 kHz sullo speaker DS).
- Fino a 3 sample di coda troncati sulle voci (len hw in parole da 4);
  gli SFX sono paddati con silenzio, nessun click.
- WaveSaveState/RestoreState: stop senza resume posizionale (nessun
  chiamante nel game — verificato).
- **Intro: il testo parte qualche secondo dopo la voce** (osservato su
  melonDS): non è il driver — la voce parte a fine `Speak()` e il
  typewriter può iniziare solo dopo il load LZSS dell'immagine di pagina
  da nitroFS (secondi su DS, istantaneo su PC). Stesso ordine di chiamate
  del PC/DOS.
- Musica: vedi "## Musica" qui sotto (fatta, streaming da SD).

## Musica (nds_music.c — sessioni musica)

**Tutta la musica del gioco vive in UN solo spazio di indici**, il numero di
traccia CD `N`, e l'engine ci arriva da due API diverse:

| chiamata engine | catena | indice |
|---|---|---|
| `PlayCdTrack(num)` | → `PlayTrackCDR(num+1)` | `N = num+1` |
| `PlayMidiFile(num)` | → `PlayMidi(HQR_Get(HQR_Midi,num))` | `N = num+1` |

`PlayMusic()` (AMBIANCE.C:384) smista: con `CDEnable` e `num` in 1..9 va sul
CD, altrimenti sul MIDI; più una manciata di `PlayMidiFile()` diretti
(GAMEMENU 203/2555/2613/3191, PERSO 321/463, PLAYFLA 309 = "fla flute").

### Sorgente asset: Common/Midi, non solo Common/Music

GOG shippa **entrambe le metà** dello spazio di indici già renderizzate in
audio: `Common/Midi/LBA1-NN.mp3`, N = 01..33 (le 02..10 sono le stesse
registrazioni di `Common/Music/Track_NN.mp3`). Quindi basta quella cartella.

L'identità **`LBA1-NN` = entry XMI `NN-1`** non è indovinata, è inchiodata da
un fingerprint di identità byte-a-byte: in MIDI_MI/MIDI_SB.HQR le entry
{8, 9, 32} sono identiche fra loro ed è l'**unico** gruppo di 3 duplicati;
fra gli mp3 lo sono {LBA1-09, -10, -33}, anch'esso unico. Offset +1, confermato.

Transcodifica: `tools/make_music_nds.sh` → per ogni traccia **due** WAV mono
IMA-ADPCM a 22050 Hz (`musNN_l.wav` + `musNN_r.wav`), perché il player usa due
canali hw separati per L e R. ~52 MB in totale, staging in
`platform/nds/music_sd/`, destinazione **SD `fat:/lba1/music/`** (BYOA come i
save). Niente estrazione ISO.

**Perché ADPCM e non MP3**: l'hw audio del DS decodifica solo PCM8/PCM16/
IMA-ADPCM/PSG. L'MP3 richiederebbe un decoder software (Helix) su una CPU che
è già il collo di bottiglia (12-13 fps in redraw pieno). Nota: l'ADPCM
hardware del DS NON è usabile per lo streaming — ha un solo header di stato a
inizio stream e non lo ricarica ai loop point, quindi non si può ripartire a
metà; si decodifica in PCM16 nel thread di refill (costo trascurabile).

### Player

Canali calico **14 = L, 15 = R**, riservati (nds_audio.c alloca SFX/voci solo
0..13 via `NDS_SFX_CHANNELS=14`). Ognuno cicla un doppio buffer PCM16 in
`SoundMode_Repeat`; un thread a 30 ms segue la testina via tick
(`elapsed*32/timer` — sound timer e tick condividono la base 33 MHz, zero
drift) e ridecodifica la metà appena liberata + `armDCacheFlush` (l'ARM7 fa
DMA dalla main RAM, non dalla nostra D$). Metà = 8192 frame ≈ 0.37 s.

**One-shot, come il DOS**: il CD suonava la traccia una volta e poi silenzio
finché il gioco non la ri-triggerava (l'engine lo sa via `EndMusicCD`/
`GetMusicCD`). Serve comunque per i jingle, perché l'engine polla
`IsMidiPlaying()`. `MUS_LOOP 1` per tornare al loop seamless.

### Jingle senza toccare l'engine

`PlayMidi()` riceve il blob XMI, non il numero — ma **tutti e tre** i call site
(AMBIANCE.C:424, AMBIANCE.C:485, PLAYFLA.C:310) fanno `NumXmi = num`
immediatamente prima. Quindi la nostra `PlayMidi()` legge quel globale: **zero
edit all'engine**. `Midi_Driver_Enable = 1` in stubs.c è l'interruttore
(ADELINE.C non lo assegna mai — in DOS lo esportava A32MT32.DLL); di
conseguenza PERSO.C:1485 carica `HQR_Midi` e **`midi_mi.hqr` deve stare nel
nitroFS** (LBA.CFG dice `MidiType: Midi` → `MidiFM=0` → midi_mi, non midi_sb),
perché `PlayMidiFile()` lo dereferenzia — i byte XMI li ignoriamo.

Fade (`FadeMidiDown/Up`) asincroni sullo stesso thread, `soundChSetVolume`.

**Entry 0 e 18 sono XMI vuoti** (242 byte ≈ 48 s di nulla) in entrambi gli
HQR: in DOS lì non c'era musica, mentre il set mp3 del remaster ci mette
tracce vere. Skippate (`MUS_IS_SILENT`, indici N = 1 e 19) per fedeltà.

### Lock (importante)

Tre lock, ordine fisso e mai invertito:
`mmutex` (decoder + FILE del player) → `PORT_SndLock` (calico/PXI, condiviso
con nds_audio.c) → `PORT_IoLock` (card).

`PORT_IoLock` (nds_sys.c) è nato con questa sessione: il thread musica legge
la SD **mentre** il main thread legge il nitroFS, e su flashcart è lo stesso
dispositivo — né libfat né il layer nitroFS serializzano, due letture
interlacciate corrompono (una scrittura di savegame è il caso peggiore). Ora
ogni operazione su file di entrambi i thread lo prende, per una singola
chiamata libc alla volta, così una lettura SD lenta non blocca un frame per
più di un blocco. Coperti: `NDS_fopen/fclose/fread/fwrite/fseek/ftell/remove`
e l'enumerazione directory (`SYS_FindFirst/Next/Close`, attiva nel menu save
mentre la musica suona).

## Multilingua (VOX su SD + selettore lingua)

**Il testo è GRATIS**: `text.hqr` che già spediamo contiene **tutte e 5 le
lingue** (EN/FR/DE/SP/IT). Indicizzazione `Language*MAX_TEXT_LANG*2 + file*2`
con `MAX_TEXT_LANG=14` → 28 entry per lingua, 141 totali = 5×28+1. Verificato
decomprimendo: l'italiano c'è ed è corretto. Nota di formato: il blob
`BufText` **comincia con la propria tabella di offset** (la prima WORD dice
dove inizia il testo) — non è testo dal byte 0.

**Le voci no**: `MESSAGE.C` costruisce il nome come `VOX\` +
`ListLanguage[LanguageCD]` + `ListFileText[file]` + `.VOX` → **12 banchi per
lingua** (`GAM`, `000`..`010`), ~33 MB l'uno. Nel nitroFS stanno solo
`en_gam`+`en_000` (era il limite noto "voci solo nella prima zona"): il resto
arriva da **SD `fat:/lba1/vox/`**, staging in `platform/nds/sd_files/lba1/vox`
via `tools/make_vox_nds.sh`. GOG spedisce **solo EN/FR/DE** (SP e IT uscirono
sottotitolate) → 36 file, 99 MB.

`RouteSdPath()` (ex `RouteSavePath`) instrada `*.vox` sulla SD **solo se il
file c'è davvero**, altrimenti lascia il path al nitroFS: una cart senza SD
continua a parlare nella zona iniziale. Stesso routing per l'enumerazione
(`SYS_FindFirst("VOX\*.VOX")` di `InitVoiceFile`). Tetto: `MAX_FILE_VOICE 42`
e ne enumeriamo 36 — ci sta, ma è stretto se si aggiungono lingue.

### Due trappole trovate spostando i VOX su supporto scrivibile

1. **`ClearVoiceFile()` CANCELLA i banchi** quando `FlagKeepVoice` è off.
   Innocuo finché stavano nel nitroFS read-only, ora sarebbero 99 MB di
   roba dell'utente. `LBA.CFG` spedisce `FlagKeepVoice: ON`, ma `NDS_remove`
   ora **rifiuta comunque** qualunque `*.vox` (ritorna successo: l'engine fa
   solo bookkeeping).
2. **`InitFileNar()` aveva un loop infinito**, e i dev lo sapevano — il
   commento originale dice `// Sans Filet`. `while (!offset) Read(...)` gira
   per sempre su un file troncato (Read torna 0 e `offset` resta 0): copia
   su SD interrotta = hang. Aggiunta uscita su `wr != 4`, più un bound su
   `offset > 2048` perché il blob va dritto in `BufMemoSeek`, che è una
   SmartMalloc fissa da 2048 (PERSO.C:1558) — stessa famiglia
   dell'overflow di BufOrder. Edit PORT-gated in MESSAGE.C.

### Selettore lingua (nds_ui.c)

LBA1 **non ha nessuna opzione lingua in-game**: `InitLanguage()` legge
LBA.CFG una volta al boot e basta. Il selettore è stato costruito: occupa il
touch screen **quando il gameplay non è attivo** (`!PlayStable`), dove gli 8
pulsanti sarebbero comunque tutti disabilitati — zero spazio rubato al
pannello behaviour. Griglia 3+2, footer `TEXT xx / VOICE xx`.

Due impostazioni indipendenti, come nella release originale: `Language`
(sottotitoli, tutte e 5) e `LanguageCD` (parlato); SP e IT ricadono
automaticamente sulla voce inglese (`LangVoice[]`).

Applicare il cambio è **una sola chiamata**: azzerare la guardia di cache
`LastFileInit` e rientrare in `InitDial(cur)` — ricarica il testo per
`Language` e, via `InitSpeak()`, riapre il banco per `LanguageCD`.

**MA `InitDial` fa HQR load e I/O su card, e `nds_ui.c` gira nell'ISR di
VBlank.** Quindi il tap posta solo su una mailbox (`LangMail`), drenata da
`PORT_PumpLang()` dal **main thread**: chiamata da `Vsync`/`Flip`/
`CopyBlockPhys` in nds_video.c (i punti di redraw che l'engine attraversa sia
in gameplay sia nei menu), con guardia di rientranza.

La scelta si ricorda in `fat:/lba1/save/lang.txt` (file nostro, non LBA.CFG —
il CFG ha il vincolo CRLF di DEF_FILE.C) e viene riapplicata al boot appena
`LastFileInit >= 0`, cioè strettamente dopo che `InitLanguage()` ha letto il
config: così lo sovrascriviamo invece di corrergli contro.

Limite noto: la UI touch compare solo dopo il primo gameplay
(`UiAutoShown`), quindi al primissimo avvio la lingua si cambia entrando in
partita e mettendo in pausa. Dal boot successivo `lang.txt` fa il resto.

## UI secondo schermo (nds_ui.c + nds_ui_glue.c — sessione touch UI)

**Fatta e verificata su melonDS** (test manuale dell'utente: tutti i pulsanti
funzionano; gating reale dei flag inventario attivo — a inizio partita
ball/sabre/holomap/pinguino sono spenti finché non vengono ottenuti).

### Layout (BG2 sub, bitmap 8bpp 256x192)

```
┌──────────────────────────────────────────────┐
│ ♥ [██████ vita ██████]   ⛁ kashes   ✤ n/m    │  strip di stato y 0..45
│ ✦ [█ magia (se tunica+lv>0) █] ⚿ chiavi FPSnn│  (redraw solo se cambia)
├──────────────────────────────────────────────┤
│ [NORMAL] [SPORTY] [AGGRO] [SNEAK]            │  behaviour y 50..111
│ [BALL]   [SABER]  [HOLOMAP] [PENGUIN]        │  azioni    y 116..177
├──────────────────────────────────────────────┤
│    L: CTRL PANEL - HOLD SELECT: CONSOLE      │  hint y 184
└──────────────────────────────────────────────┘
```
Pulsanti 59x62 (4 colonne x 63px, comodi per il pollice), icona 16x16
disegnata 2x + label 5x7; tutto font/icone = tabelle C in nds_ui.c (BYOA,
zero asset esterni). Stati: spento (faccia scura, glifi dim, touch ignorato),
attivo (bevel+testo oro: il behaviour corrente e l'arma corrente), premuto
(faccia chiara). Palette UI = entry SUB 1..31 (0 e 240..255 restano alla
console). Stile LBA: pannelli blu scurissimi, accenti oro.

### Video sub / convivenza con la console

`PORT_UI_Init` (main_nds.c, dopo nitroFSInit): `videoSetModeSub(MODE_5_2D |
DISPLAY_BG0_ACTIVE)` — la console libnds resta su BG0 (map 22/tile 3, 44-56K
di VRAM C), la UI è BG2 ExRot bitmap a map base 4 (64K..128K, nessuna
sovrapposizione). ATTENZIONE trovata sul campo: a parità di priorità BG0
vince su BG2 → il testo console "sanguinava" sopra la UI; serve
`REG_BG0CNT_SUB |= 3` (console a priorità 3, UI a 0).
Disegno: shadow buffer 48K in bss (byte write liberi) + blit dei soli rect
sporchi in u32 (VRAM ignora i byte write). Redraw SOLO su cambiamento di
stato/valore; il caso peggiore (8 pulsanti che flippano) è pochi KB di
scritture nella VBlank ISR — la stessa ISR fa già 64K/frame nel present MCGA.

- Boot: console visibile (log come prima). Alla PRIMA scena di gioco la UI
  appare da sola (`PORT_UI_SceneSeen`).
- **SELECT tenuto 1 s = toggle console/UI** (solo il bit BG2 di
  DISPCNT_SUB: deterministico). La console continua a stampare anche mentre
  è coperta.

### Touch → azione (mappa e meccanismo di iniezione)

La VBlank ISR legge `touchRead()` e NON chiama mai l'engine: posta in 3
mailbox one-shot consumate in UN punto del MainLoop di PERSO.C (edit
#29-31, `#ifdef PORT_NDS`). **Percorso touch indurito** (bug "icone che
lampeggiano + slowdown", sessione stessa): un tap è accettato solo con
`KEY_TOUCH` held **e** `touchRead()==true` **e** coordinate in range per 2
VBlank consecutivi (un campione spurio singolo non può mai sparare), UNA
azione per contatto (ri-arma dopo 2 frame puliti di rilascio), cooldown
250 ms tra azioni, i tap no-op sul behaviour corrente vengono scartati
(SetComportement ricarica il body anche a valore uguale = churn), e il
touch parte **DISARMATO al boot**: serve un rilascio pulito prima del primo
tap accettato — melonDS può riportare un pennino-fantasma "premuto da
sempre" con coordinate rumorose finché non arriva il primo click reale
(sintomo osservato: fila behaviour che lampeggia da sola e si "guarisce"
dopo il primo tocco vero). In più
il predicato gameplay è **debounced** (10 VBlank stabili prima di flippare
enabled/disabled: l'engine brakketta SaveTimer/RestoreTimer dentro il
frame e la ISR può campionare a metà bracket) e la finestra di recency di
AffScene è 1.5 s (i full redraw da recenter distano fino a ~0.7 s sotto
autoenter — sotto la soglia vecchia di 0.8 s il gate flappava).

| Pulsante | Meccanismo | Percorso engine |
|---|---|---|
| Normal/Sporty/Aggro/Sneak | `PORT_TouchComportement=0..3` | `SetComportement()` diretto (= opcode SET_COMPORTEMENT di GERELIFE) — niente flash del menu behaviour, niente AffScene(TRUE) |
| Ball / Saber | `PORT_InjectKey=K_1/K_2` (MyKey per UNA iterazione) | gli handler tastiera K_1/K_2 già presenti (equip + anim degaine), gating engine sui flag |
| Holomap | `PORT_TouchInvAction=0` | lo switch(InventoryAction) esistente, senza aprire l'inventario |
| Penguin | `PORT_TouchInvAction=14` | idem (spawn del pinguino = STESSO codice della selezione da inventario) |

Perché non FuncKey: **l'engine non legge mai FuncKey** (verificato: solo la
dichiarazione in LIB_SYS.H) — la mappa X/Y/L/R→FK_F1..4 del bring-up era
morta; i behaviour da tastiera in questo sorgente sono K_F5..K_F8 via `Key`
(con flash di MenuComportement). Mailbox > iniezione a frame: consumo
esattamente-una-volta anche durante un full redraw da 235 ms.
Mailbox non consumate scadono dopo ~200 ms (MAIL_TTL) — un tap postato
mentre si apriva un dialogo non spara mai in ritardo.

### Gating (pulsanti attivi SOLO in gameplay interattivo)

`GameplayActive()` = `CmptMemoTimerRef == 0` (profondità SaveTimer: >0 in
ogni menu/inventario/holomap/opzioni) AND `!FlagCredits` AND AffScene
eseguita < 40 tick fa (stampigliata da `__wrap_AffScene` in nds_prof.c:
falsa in main menu/intro/FLA/holomap) AND `PORT_UI_PersoManual()` (glue:
`Body != -1 && Move == MOVE_MANUAL`, falsa nelle cutscene). In più, per
oggetto, lo stesso predicato di `Inventory()`: `ListFlagGame[item]==1 &&
ListFlagGame[FLAG_CONSIGNE]==0`; il pinguino richiede anche
`NumPingouin > 0` (oggetto valido nel cubo — default -1). Le stesse guardie
sono comunque replicate ENGINE-side negli hook (#30/#31): doppia cintura.

- `nds_ui_glue.c` è l'unico file platform compilato con ENGINE_CFLAGS (vere
  intestazioni engine) per leggere `ListObjet[0].LifePoint/Body/Move` senza
  specchiare la struct T_OBJET; tutto il resto sono extern di WORD/UBYTE
  (Comportement, Weapon, MagicPoint/Level, NbGoldPieces, NbLittleKeys,
  NbFourLeafClover/NbCloverBox, NumPingouin, ListFlagGame,
  CmptMemoTimerRef, FlagCredits).

### Contatori live (strip di stato)

Vita = `ListObjet[0].LifePoint` /50 (barra verde); magia = `MagicPoint` su
max `MagicLevel*20`, visibile solo con `FLAG_TUNIQUE` e livello>0 (larghezza
frame proporzionale al livello, come DrawInfoMenu); kashes `NbGoldPieces`,
chiavi `NbLittleKeys`, quadrifogli `NbFourLeafClover/NbCloverBox`.
Campionati ogni VBlank, ridisegnati solo se cambiati (memcmp su struct).

### Controlli pad (remap sessione touch UI)

| Tasto DS | Funzione |
|---|---|
| dpad | movimento (Joy) |
| B / A | azione (space) / valida-recentra (return) |
| **L** | **CTRL DOS**: tieni premuto = pannello behaviour classico (MenuComportement), scegli col dpad |
| X / Y / R | behaviour diretto Normal / Sporty / Aggressive (stessa mailbox del touch, niente flash menu); Discreto = touch o pannello L |
| SELECT | alt; tenuto 1 s = toggle console/UI sul sub |
| START | Esc (menu pausa / skip) |

Legenda sulla UI touch: tag "X"/"Y"/"R" nell'angolo dei pulsanti behaviour,
hint "L: CTRL PANEL - HOLD SELECT: CONSOLE" in basso.

### Crash menu pausa (fix edit #32, stessa sessione)

START in gioco → guru `data abort` in `_free_r` con addr/r7 ASCII ("%\nor").
Causa: `GetCustomizedMultiText` (MSG_CUST.C, codice community) ritornava il
letterale `""` quando un testo manca sia in TEXT.HQR che nel ctxt.csv (non
shippato) e i chiamanti fanno `free()` del risultato per contratto →
`free("")` su .rodata: msvcrt lo ignorava, newlib cammina i byte ASCII come
chunk header e abortisce. Trigger: i testi **950/951 "Load/Save game"**
(aggiunti dalla community al menu pausa) non esistono in TEXT.HQR. Fix:
ritorna `calloc(1,1)` liberabile (edit #32). Repro headless:
`nitrofiles/autopause`.

### Verifica / regressione

- melonDS: boot pulito, prima scena a **50 fps** invariati (P5 fps50 negli
  screenshot col profiler attivo), heap invariato (la UI usa solo 48K di
  bss + 0 heap), stack DTCM non toccato (nessun dato UI in DTCM, ISR con
  locals piccoli e niente printf).
- Touch verificato a mano dall'utente: **tutti i pulsanti funzionano**
  (behaviour switch istantaneo, ball/saber, holomap, contatori live). Il
  gating è stato provato col hook temporaneo `nitro:/uitest` (forzava i 3
  flag), POI RIMOSSO: la build finale gata sui flag reali.
- Build SDL riverificata verde dopo gli edit #29-31 (guardie PORT_NDS).

### Restano (polish futuri)

- Icone "vere" renderizzate a runtime da INVOBJ.HQR (v1 = glifi C
  disegnati a mano; AGGRO/SPORTY sono i più naive).
- Pinguino non provato dal vivo (serve una partita avanzata con
  FLAG_MECA_PINGOUIN: il percorso è lo stesso switch dell'holomap, provato).
- Eventuale feedback audio sul tap (SFX 41?) e pulsante protopack/clover.

## Aperti / TODO

- **Savegame**: FATTO su fat:/ (vedi "## Savegame su fat:/"). Restano: BYOA
  degli asset da SD (oggi solo i save; i VOX extra potrebbero venire letti
  da fat:/lba1/ con la stessa tecnica di routing).
- **Audio**: FATTO (SFX + voci, vedi "## Audio ARM7/calico"; musica d'area +
  jingle, vedi "## Musica"; VOX completi + selettore lingua, vedi "##
  Multilingua"). **Da collaudare**: musica e multilingua non sono ancora
  mai stati provati, né su melonDS né su hardware.
- **Holomap**: TEXTURE.ASM ancora stub (come SDL).
- **Unaligned residui**: il pun Info/Info1 è FIXATO (edit #33, vedi "##
  Freeze dossier"); resta HOLOMAP.C da riguardare quando si farà l'holomap.
- **MCGA path** (FLA/SceZoom): implementato (present-from-Phys nel VBlank) ma
  mai esercitato (niente .FLA shippati).
- **UI touch / seconda schermata**: per ora la console di debug; niente touch.
- `GetMouseDep`/mouse = zeri: i menu funzionano da tastiera/pad.
- FICHE.C aveva un `GET_WORD` suo: convertito, ma il file "fiche" (scheda
  personaggio, K_F?) non è stato esercitato a fondo.
- Il guard-heap e i log [MEM]/[FIO] sono ATTIVI anche nella build finale:
  costano poco e sono oro per il debug; togliere `PORT_HeapCheck` per la
  build "release" quando si farà l'ottimizzazione.
