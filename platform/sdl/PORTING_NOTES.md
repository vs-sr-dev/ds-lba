# Porting notes — engine/ + platform/sdl

Regole del port:
- `lba1-classic-community-main/` è PRISTINO: mai modificarlo. Si lavora sulle copie in `engine/`.
- Ogni modifica a un file di `engine/` va loggata qui sotto con file, motivo, una riga.
- Il codice piattaforma vive in `platform/sdl` (e in futuro `platform/nds`). Niente `#ifdef` sparsi nell'engine se evitabile.
- Simboli mancanti → stub centralizzati in `stubs.c` (audio/midi/cd) e `asm_stubs.c` (moduli ASM non ancora tradotti, futura `translate/`).
- Struct lette/scritte da file (es. T_HEADER in HQ_RESS.C): Watcom impaccava a 2, GCC padda — servono `#pragma pack` o letture campo-per-campo. Loggare ogni caso trovato.

## Modifiche a engine/ (11 edit)

| # | File | Cosa | Perché |
|---|------|------|--------|
| 1 | `LIB_SYS/LIB_SYS.H` | proto `Exists(char*)` → `Exists(UBYTE*)` | mismatch col corpo in FILES.C: GCC lo tratta come errore (conflicting types) |
| 2 | `LIB_SYS/LIB_SYS.H` | proto `HQR_GiveIndex` → `void*(LONG,LONG,void*)` | mismatch col corpo in HQR.C (era `T_HQR_BLOC*(UWORD,UWORD,void*)`) |
| 3 | `game/BUBSORT.C` | aggiunti `#include <stddef.h> <string.h> "ADELINE.H"` | il file non aveva alcun include: UBYTE/size_t/memcpy non dichiarati |
| 4 | `game/PERSO.C` | `void main(...)` → `void lba_main(...)` | SDL2main possiede `main()`/WinMain su MinGW; wrapper in `platform/sdl/main_sdl.c` |
| 5 | `LIB_SYS/HQ_RESS.C` | `#pragma pack(push,1)` attorno a `T_HEADER` | struct su disco 10 byte (Watcom); GCC paddava a 12 → header HQR letti sballati |
| 6 | `LIB_SYS/HQ_R_M.C` | idem `T_HEADER` | come sopra |
| 7 | `game/GRILLE.C` | idem `T_HEADER` | come sopra (lettura brick LBA_BRK.HQR) |
| 8 | `game/PLAYFLA.C` | pack(1) su `T_HEADER` + blocco struct FLI/FLA (`T_HEADER_FLI`, `T_HEADER_FLA`, `T_HEADER_FLA_PASSE`, `T_FLA_TYPE`, `T_FLA_SAMPLE_LIST`, ...) | tutte struct lette con `Read(handle,&s,sizeof)` dai file FLA/FLI |
| 9 | `LIB_MENU/MENUFUNC.C` | `#ifndef PORT_SDL` attorno a `Message()` | simbolo duplicato: PERSO.C definisce la sua `Message()`; col librarian Watcom questa non veniva mai estratta |
| 10 | `LIB_SVGA/MASKGPH.C` | `#ifndef PORT_SDL` attorno a `CreateMaskGph()` | duplicato di game/GRILLE.C (stessa storia) |
| 11 | `game/PERSO.C` | riattivate le 2 righe originali commentate a inizio MainLoop: `while (TimerRef == timeralign); timeralign = TimerRef;` | regolatore di frame autentico a 50 Hz — vedi sezione "Pacing"; senza, gli sprite mobili (hover-pad) volano |

Nota: le `#pragma pack` NON sono state messe sulle struct dei savegame
(`T_OBJET`, `T_EXTRA`, `T_ZONE` letti interi in GAMEMENU.C): i save prodotti da
questo port sono auto-consistenti ma NON compatibili con i .LBA DOS originali.
Da decidere più avanti (probabilmente pack(1) anche lì + confronto sizeof).

## Edit per NDS (sessione M4 bring-up — validi e verificati anche su SDL)

Tutti portabili: su x86 little-endian producono byte-identico; sul DS (ARM9,
ARMv5TE: load 16/32-bit disallineati = dati ruotati/garbage, niente trap)
sono INDISPENSABILI. Build SDL riverificata verde + smoke test ok dopo ognuno.

| # | File | Cosa | Perché |
|---|------|------|--------|
| 12 | `LIB_MENU/MENUFUNC.C` | guardia `#ifndef PORT_SDL` → `#if !defined(PORT_SDL) && !defined(PORT_NDS)` | stesso duplicato di `Message()` sulla build NDS |
| 13 | `LIB_SVGA/MASKGPH.C` | idem | duplicato `CreateMaskGph()` |
| 14 | `game/GAMEMENU.C:1204` | `CopyBlockMCGA(...,0xA0000)` → `...Phys` sotto `PORT_SDL\|\|PORT_NDS` | 0xA0000 è VGA fisica DOS; gli shim presentano `Phys` (fix del rischio già censito) |
| 15 | `LIB_SYS/EXPAND.C` | token LZ `*(UWORD*)esi` → composizione a byte | `esi` è spesso dispari: su ARM leggeva garbage → TUTTE le immagini decompresse corrotte |
| 16 | `LIB_SYS/HQ_MEM.C` | `HQM_Alloc`/`HQM_Shrink_Last`: size arrotondata a 4 | i blocchi HQM erano impacchettati a byte → BufMap/TabBlock/scene a indirizzi dispari |
| 17 | `LIB_SYS/HQ_RESS.C` | `HQR_Get`/`HQR_GetSample`: entry del pool arrotondate a 4 | anim/body letti a WORD dal pool: entry a offset dispari rompevano anche translate/ |
| 18 | `game/PLAYFLA.C` | `InitFla`: return FALSE se `OpenRead` fallisce | senza .FLA, `fread(NULL)` è crash su newlib (msvcrt ritornava 0) |
| 19 | `game/PERSO.C` | `BufText` 25000→+500, `BufOrder` 1024→+500 | **bug latente anche su PC**: Load_HQR scrive fino a SizeFile+500 (margine LZS in-place); la order entry più grossa di TEXT.HQR è 536 byte → 536+500 > 1024 = heap corruption (su DS distruggeva una FILE struct) |
| 20 | `game/GAMEMENU.C` | `GetMenuMultiTextAux`: `*dst=0` se il testo non esiste da nessuna parte | stringa stack non inizializzata passata a Font/SizeFont |
| 21 | `game/DEFINES.H` | macro `LE_R16/LE_RU16/LE_R32/LE_W16/LE_W32` | accessori little-endian a byte per i dati script/scene impacchettati |
| 22 | `game/GERELIFE.C` | tutti i `*(WORD*)PtrPrg` → `LE_R16` (30 siti) | lo script life è byte-packed: offset dispari costanti |
| 23 | `game/GERETRAK.C` | `*(WORD/ULONG*)ptrtrack(±n)` → `LE_R16/R32/W16/W32` (23 siti, incl. le WRITE dei timer nel track) | idem per lo script track (auto-modificante!) |
| 24 | `game/FICHE.C` | `*(WORD/UWORD*)ptrc` → `LE_R16/RU16/W16` (11 siti, incl. il GET_WORD macro e la write hqrbody) | anim-action data byte-packed |
| 25 | `game/DISKFUNC.C` | `GET_WORD` → `LE_R16`; + riallineamento coda scena (se `PtrSce` dispari prima delle zone: memmove di 1 byte) | lo stream scena ha blocchi track/life a lunghezza arbitraria; `T_ZONE`/`T_TRACK` sono letti come struct e vogliono allineamento 2 |
| 26 | `game/GRILLE.C` | `LoadUsedBrick` (entry blocchi a `ptb+5`, read+write) e `AffBrickBlock` `numbrick` → `LE_*` | gli offset dei blocchi .BLL sono 3+4n → entry UWORD a offset dispari |
| 27 | `game/GRILLE_A.C` | `DecompColonne`/`MixteColonne`: `*(UWORD*)pts` → `LE_RU16` | RLE colonne byte-granulare: length garbage → write selvagge oltre BufCube (heap trash) |
| 28 | `LIB_3D/P_ANIM.C` | i 4 helper `read_word/read_dword/write_word/write_dword` → accesso a byte | record anim byte-packed (il file aveva già gli helper centralizzati) |
| 29 | `game/PERSO.C` | MainLoop, dopo `MyKey = Key`: sotto `#ifdef PORT_NDS`, se `PORT_InjectKey != 0` → `MyKey = PORT_InjectKey; PORT_InjectKey = 0` | UI touch DS (nds_ui.c): iniezione one-shot di scan code (K_1/K_2 = balle/sabre) consumata in un punto solo — mai persa da frame lunghi, mai doppia. Su SDL non compila (guardia) |
| 30 | `game/PERSO.C` | MainLoop, accanto al blocco K_F5-K_F8: sotto `#ifdef PORT_NDS`, `PORT_TouchComportement` (0..3, -1=idle) → `SetComportement()` diretto con le stesse guardie Body/MOVE_MANUAL | UI touch DS: cambio behaviour senza il flash di MenuComportement né AffScene(TRUE) — stessa chiamata dell'opcode SET_COMPORTEMENT di GERELIFE.C |
| 31 | `game/PERSO.C` | MainLoop, blocco inventario: sotto `#ifdef PORT_NDS`, `PORT_TouchInvAction` (-1=idle) entra nella condizione F_SHIFT e, se settato, salta `Inventory()` assegnando direttamente `InventoryAction`; consumo incondizionato a fine blocco | UI touch DS: holomap (action 0) e meca-pinguino (action 14) passano dallo STESSO switch della selezione inventario reale, con le stesse guardie |
| 32 | `game/MSG_CUST.C` | `GetCustomizedMultiText`: `return result ? result : "";` → `... : calloc(1, sizeof(char))` | **bug latente anche su PC**: il contratto documentato è che il CHIAMANTE fa `free()` del risultato (GetMenuMultiTextAux e InfoWallCollisionDamage lo fanno) — `free("")` su un letterale .rodata corrompe lo heap. msvcrt ignorava il puntatore estraneo; newlib sul DS legge i byte ASCII della rodata come chunk header (guru: pc=`_free_r`, addr=0x726F0A25 = "%\nor") → data abort al menu pausa: i testi 950/951 "Load/Save game" aggiunti dalla community NON esistono in TEXT.HQR e ctxt.csv non è shippato, quindi il fallback girava per ogni voce di menu mancante |
| 33 | `game/OBJECT.C` (3 siti MOVE_RANDOM), `game/PERSO.C` (spawn meca-pinguino) | i 4 `*(ULONG *)(&ptrobj->Info)` → `LE_W32`/`LE_R32` su `&ptrobj->Info` | timer a 32 bit punnato su Info/Info1 (WORD adiacenti). `offsetof(T_OBJET,Info)` ≡ **2 (mod 4)** (dopo PtrLife puntatore + OffsetLife WORD — verificato con static assert in `platform/nds/nds_ui_glue.c`): su ARMv5 la **STR ignora i 2 bit bassi** ⇒ la write a 32 bit atterrava su `{OffsetLife, Info}` **corrompendo OffsetLife** (lo script life riparte da un offset selvaggio → comportamenti erratici/freeze), e la LDR tornava ruotata di 16. Colpisce OGNI attore MOVE_RANDOM (guardie esterne, animali) e il pinguino: primo sospettato dei freeze non deterministici visti su hardware fuori dalla Citadella. Su x86 il byte layout è identico (little-endian): nessun cambio di comportamento PC |

Residui noti NON ancora toccati (non colpiti nel percorso boot→prima scena):
HOLOMAP.C (già stub); MESSAGE.C legge BufText con offset pari su base
allineata (ok). Il pun Info/Info1 (ex residuo) è stato chiuso con la #33.

## Infrastruttura di build (platform/sdl)

- `watcom_compat.h`: force-included (`-include`) davanti ai soli file engine.
  Uccide `cdecl/__far/__near/__loadds` e fornisce `min/max` (Watcom stdlib.h li aveva).
- `compat/LIB386/LIB_SYS/SYS_{FILESYSTEM,TIME}.H`: wrapper per gli include
  `"../LIB386/LIB_SYS/..."` dei sorgenti game/ (layout del tree originale);
  risolti via `-Icompat/inc` (GCC concatena il path relativo alle dir -I).
- Makefile: engine compilato con `-std=gnu89` + `-Wno-implicit-function-declaration
  -Wno-implicit-int -Wno-int-conversion -Wno-incompatible-pointer-types
  -Wno-pointer-sign -Wno-discarded-qualifiers` (GCC≥14 li promuove a errori),
  `-fsigned-char -fcommon -pipe`; `-I` extra per gli include bare
  (`ADELINE.H`, `LIB_CD.H`, `lib_menu.h`...). `TMP/TEMP` esportati verso `build/`
  (alcune shell passano TMP=C:\WINDOWS → collect2/as falliscono).
- `translate/*.c` (traduzioni C dei moduli ASM LIB_SVGA) cablate nel link con
  regola dedicata (`$(OBJDIR)/translate/%.o`), compilate con CFLAGS puliti.
  Nessun mismatch di firma trovato vs C_EXTERN.H/LIB_SVGA.H; nessun edit a translate/.

## platform/sdl — file dello shim

- `main_sdl.c` — entry SDL2, chiama `lba_main`. stdout non bufferizzato.
- `sdl_video.c` — rimpiazza PLATFORM/DOS/LIB_SVGA (INITSVGA.ASM, INITMODE.C,
  S_PAL.ASM, S_PHYS.ASM, S_DLL.C): `Log`/`Phys`/`Screen_X/Y`/`TabOffLine`/clip;
  `Palette/PalMulti/PalOne` (quantizzazione DAC 6 bit fedele: `>>2` poi re-espansa),
  `Flip/CopyBlockPhys[Clip]/Vsync`; `InitGraphSvga/Mcga`, `InitMcgaMode` (320x200
  per i FLA), `SimpleInitSvga`, `SvgaInitDLL`→TRUE. Un **thread video** dedicato
  (SDL_CreateThread) pompa gli eventi, aggiorna tastiera/mouse/timer e presenta
  `Phys`+palette a ~60 Hz: l'engine fa busy-wait su `Key/Joy/Fire/TimerRef`,
  quindi input e tempo devono avanzare in modo asincrono (sul DS lo farà l'IRQ VBlank).
- `sdl_sys.c` — rimpiazza PLATFORM/DOS/LIB_SYS: timer 50 Hz derivato da
  SDL_GetTicks (`TimerRef/TimerSystem/NbFramePerSecond` con la stessa logica del
  handler IRQ0 di TIMER_A.C); tastiera con la semantica esatta di KEYBOARD.C
  (Joy=frecce bit 1/2/4/8, Fire=space/ret/ctrl/alt/del/shift, FuncKey=F1..F12,
  `Key`=scan code set-1 posizionale, `GetAscii` con ring buffer stile int 16h);
  mouse (Click/Mouse_X/Y/GetMouseDep); `DosMalloc`→calloc; `ultoa`;
  `Touch`→_utime; `SYS_Find*`→_findfirst; `SYS_ComputeTime` con lo stesso packing
  di TIME.C DOS (`cpttime + cptdate`).
- `stubs.c` — driver assenti, tutto "successo ma muto": loader DLL Watcom
  (`DLL_load/FILE_read`→NULL), CD-ROM (`InitCDR`→FALSE ⇒ percorso HD/GOG;
  `DriveCDR = -1` come il driver DOS senza CD, così MESSAGE.C non prova
  ad aprire `A:\LBA\VOX\...` per le voci degli oggetti trovati),
  MIDI AIL32 (Init*→TRUE ma `Midi_Driver_Enable=FALSE`; `InitMidiTimer` reindirizza
  a `InitTimer` per non perdere il tick).
  Le `*AskVars` ritornano lista identificatori vuota (`{""}`) ⇒ il loop di
  ADELINE.C esce subito.
- `sdl_audio.c` — driver Wave (campioni) + Mixer (volumi) reali: SFX e voci
  suonano. Vedi sezione "Audio (sdl_audio.c)".
- `asm_stubs.c` — ridotto ai soli stub di **TEXTURE.ASM** (holomap):
  `AsmTexturedTriangleNoClip`, `FillTextPolyNoClip`, `TabText`, `LYmin/LYmax`.
  P_TRIGO/S_POLY/P_OB_ISO e tutti i loro dati (`X0/Y0/Z0`, `Xp/Yp`, camere,
  matrici, `TabPoly/TypePoly/NbPolyPoints`, `Ymin/Ymax/TabVertic*/TabCoul*`,
  `ScreenX/Ymin/max`, `List_*`) ora vivono in `translate/p_trigo.c`,
  `translate/s_poly.c`, `translate/p_ob_iso.c` (vedi TRANSLATION_NOTES.md).
- Strumenti di test nel shim (aggiunti con la sessione 3D):
  - `sdl_video.c`: screenshot BMP 24bpp dal buffer ARGB post-palette.
    **F12** = scatto manuale; env `LBA_SHOT_MS=<ms>` = scatto periodico.
    File `shot_NNN.bmp` nella working dir.
  - `sdl_sys.c`: `PORT_AutoInput()` — env `LBA_AUTOENTER=<sec>` pulsa Return
    (Fire bit 2 + ring ascii) ogni ~350ms per N secondi: smoke test non
    interattivo fino alla prima scena.
  - `sdl_audio.c`: log `[wave] play ...` per ogni WavePlay; env
    `LBA_WAVDUMP=<file.wav>` scrive i primi 20 s del mix su disco.

## Audio (sdl_audio.c)

Rimpiazza la coppia di DLL DOS `W_SB16.DLL` (LIB_SAMP: WAVE.C + WAVE_A.ASM,
mixer software 8→16 bit stereo) e `M_SB16.DLL` (LIB_MIX: MIXER_A.ASM, registri
volume del chip SB16). Tutta la semantica sotto è verificata sul sorgente DOS
pristino (`LIB386/PLATFORM/DOS/LIB_SAMP/WAVE_A.ASM`, `WAVE.INC`,
`LIB_MIX/MIXER_A.ASM`) e sugli HQR di testdata — è la specifica per l'ARM7.

### Formato campioni (verificato sui dati GOG)

- **SAMPLES.HQR** = 244 entry HQR (metodo 0=store o 1=LZSS), ogni entry è un
  file **Creative VOC**: header 26 byte (`"Creative Voice File"`, u16 a +0x14
  = dimensione header), poi UN solo blocco tipo 1 (sound data): `u8 type=1,
  u24 blocksize, u8 sr, u8 pack=0`, quindi PCM **8-bit unsigned mono**.
  `rate = 1000000/(256-sr)`; lunghezza dati = `blocksize - 2`.
  Frequenze reali in testdata: 4098–17544 Hz (moda: 11111 Hz sr=166 ×135
  entry, 16129 Hz sr=194 ×36, 8000 Hz ×16). Tutte ≤ 22 kHz.
- **VOX** (`VOX\EN_000..010.VOX`, `EN_GAM.VOX` ecc.) = stesso formato archivio
  HQR (tabella offset u32, entry `u32 size, u32 sizeLZSS, u16 method`), letto
  a mano da MESSAGE.C (`InitFileNar` carica la tabella offset in BufMemoSeek,
  `PlaySpeakVoc` decomprime l'entry in `BufSpeak` 256 KB). Ogni entry è un VOC
  identico ai precedenti MA **il primo byte ('C', 0x43) è patchato dal game**:
  `0` = ultima parte, `1..n` = segue un'altra entry VOC (letta in sequenza:
  MESSAGE.C `FlagNextVoc = *BufSpeak`, TestSpk incatena le parti).
- **Doppio uso del byte 0**: il driver DOS lo rilegge come flag interpolazione
  (`if (byte0+1 < 10) INTERPOL=on`): quindi le VOCI (byte0=0/1) vengono
  interpolate linearmente, gli SFX (0x43) no. Riprodotto in sdl_audio.c.
- Il puntatore passato a `WavePlay` punta all'inizio del VOC (header incluso);
  il driver salta l'header via il u16 a +0x14. Nessuna informazione di
  lunghezza esterna: fa fede il blocksize del blocco VOC.

### Semantica mixer (da WAVE_A.ASM, build SB16 = 16bit stereo)

- **Canali**: lista DOS di 50 slot ma `SHIFT_SAMPLE=3` ⇒ headroom per 8 canali
  a volume pieno (8 × ±16384 = ±32768 esatti, senza clamp: wrap!). sdl_audio
  usa **16 canali** (= canali hardware ARM7) con accumulo int32 + clamp.
- **Nessuna priorità/steal**: lista piena ⇒ WavePlay ritorna 0 e il suono è
  perso. Unica eccezione DOS: nei driver 8-bit (non SB16) l'handle voce
  0x1234 faceva StopSample() di tutto. Su SB16 no ⇒ non riprodotto.
- **Handle**: u16 dal game = indice HQR del sample, o `0x1234` (SPEAK_SAMPLE,
  voce dialogo — MESSAGE.C fa WaveStopOne(0x1234) prima di ogni nuova parte).
  Handle NON univoco: `WaveStopOne(h)`/`WaveInList(h)` operano su TUTTI i
  canali con quell'handle. Il `longhandle` di ritorno è univoco
  (`seq<<16|handle` nel nostro caso) per WaveChangeVolume/WaveGiveInfo0.
- **Pitchbend**: 4096 = 1.0; `rate_eff = (voc_rate * (pb<<4)) >> 16` con
  arrotondamento (≈ rate*pb/4096). Il game usa 0x1000 fisso, 0x800+Rnd(0x800)
  (tracce), 0x1000±Rnd (passi/ambiance/fuoco). INCR DOS = rate_eff/PlayRate
  in 16.16 (cap a 0xFFFF: mai downsampling con PlayRate 22000).
- **Repeat**: numero di riproduzioni; **0 = 65536** (underflow del `dec word`)
  ≈ loop infinito (usato da GERETRAK per suoni continui). Al loop il cursore
  riparte da 0 (FRACT azzerato).
- **Volumi per canale**: VolLeft/VolRight ricevuti 0..128 (Balance/RegleTrois);
  la VOCE usa **512/512** (MESSAGE.C) = 4× più forte del massimo SFX.
  Contributo DOS per canale: `s8 * (vol >> 2)` sommato nel buffer 16-bit.
  Pan: AMBIANCE.C `GiveBalance` proietta la posizione mondo sullo schermo
  (Xp 0..640 ⇒ `Balance()`: volright=SinTab[bal*2]*vol>>14,
  volleft=SinTab[bal*2+512]*vol>>14 — quarto di sinusoide, potenza costante;
  fuori schermo scala con RegleTrois32 fino a ±320px e ±240px oltre i bordi).
  Il driver riceve i volumi GIÀ pannati: l'ARM7 non deve calcolare nulla,
  basta vol sinistro/destro per canale (0..127 hw: usare vol>>2).
- **Volume master**: mixer hardware SB16. L'engine legge i default con
  `MixerGetVolume` (ritornare 255), li sovrascrive coi valori LBA.CFG
  (`WaveVolume: 163`, `MasterVolume: 195`...) e chiama `MixerChangeVolume`
  (-1 = non cambiare). Applichiamo `wave/255 * master/255` al mix finale
  (sul DS: registro master + moltiplicazione nel volume canale).
  `MixerGetInfo` → tutti 1 (le 5 slider nel menu volumi, come SB16).
- **WaveMove** (attenzione, non è un semplice memmove!): HQ_RESS.C compatta
  il buffer samples MENTRE i sample suonano; il driver deve spostare i byte E
  ricalibrare i puntatori dei canali attivi di `dest-src` (ShiftSamples DOS).
  Senza fixup: pop/garbage quando l'HQR evicta. **Trappola port** (fix
  2026-07-13): DOS ricalibrava tutto ciò che aveva `START >= src` — sicuro
  solo perché BufSpeak (voce) stava in memoria DOS bassa, sempre SOTTO lo
  heap samples. Con uno heap piatto (PC/DS) BufSpeak può stare SOPRA src ⇒
  ricalibrare SOLO i puntatori dentro il range spostato `[src, src+Size)`,
  altrimenti la voce viene shiftata a caso.
- **Lunghezza campioni > 64K campioni** (bug trovato sull'intro, fix
  2026-07-13 — sintomi: narrazione che riparte in loop dall'inizio E testo
  typewriter mai mostrato, perché `WaveInList` non diceva mai "finito" e il
  chiamante non usciva/avanzava): le parti VOX delle narrazioni SUPERANO i
  65535 campioni (intro EN: 206681, 145985, 162705 campioni). Un cursore di
  posizione 16.16 in 32 bit wrappa a indice 65536 e riavvolge il sample da
  zero senza mai raggiungere `length`. Serve indice campione intero a 32 bit
  + accumulatore frazionario a parte (DOS: puntatore byte + word FRACT
  separata — mai un 16.16 unico). Su DS i registri lunghezza ARM7 bastano
  (22 bit di word) e il mirror a durata/tick non è affetto, ma qualunque
  contatore di posizione/fine-sample va tenuto su conteggi 32 bit.
- **WavePause/WaveContinue**: pausa globale (menu/cambio modo video); DOS
  faceva anche un micro-fade sul buffer, noi/DS: basta silenzio/stop canali.
- **Follow/"son"** (accodamento di un sample alla fine di un altro): esiste
  nel driver DOS ma il game passa SEMPRE Follow=0 ⇒ non implementato (log se
  mai comparisse).
- **Output DOS di riferimento**: PlayRate da LBA.CFG `WaveRate: 22000`,
  half-buffer 1024 sample (~46 ms). SDL: device 22000 Hz S16 stereo,
  callback da 1024 sample.

### Chi suona cosa (mappa osservata / dal codice)

- Voci dialoghi: `MESSAGE.C` → VOX (file 0=SYS,1=CRE,2=GAM,3..14=000..011 per
  isola; indice entry = posizione del testo in BufOrder). Handle 0x1234.
- SFX 3D posizionali: `HQ_3D_MixSample` (FICHE.C azioni/animazioni con sample
  nel flusso anim, EXTRA.C: 11=magic ball bounce, 86, 97, OBJECT.C 11,
  GERETRAK.C sample da script track). Passi: `(code>>4)+126` e
  `(code&15)+126+15` (BASE_STEP_SOUND=126 ⇒ entry 126..156 = passi su
  materiali diversi, pitch random ±).
- SFX UI/fissi: GAMEMENU 41 (item trovato?), 34/34+92 (plasma menu), 37
  (respiro/fuoco con pitch random), PERSO.C 37.
- Ambiance: AMBIANCE.C `GereAmbiance`: 4 sample per scena (`SampleAmbiance[]`
  da DISKFUNC/scene), vol 110/110, pitch 0x1000±Rnd(decal), ogni
  SecondMin+Rnd(SecondEcart) secondi.
- FLA: PLAYFLA.C suona sample sincronizzati col filmato (non in testdata).

### LBA.CFG

**Nessuna modifica necessaria**: `WaveDriver: W_SB16.DLL` (qualsiasi valore ≠
"NoWave" va bene: WaveInitDLL nostro accetta tutto), `WaveRate: 22000` (letto
via WaveAskVars — protocollo AskVars reale: chiediamo solo "WaveRate"; se
manca dal CFG l'engine fa exit(1), com'era su DOS), `MixerDriver: NoMixer`
DEVE restare (MixerInitDLL è codice engine reale che vuole una DLL Watcom;
i nostri MixerChangeVolume/MixerGetVolume vengono comunque chiamati e
gestiscono i volumi). `LanguageCD: English` + `VOX\EN_*.VOX` presenti ⇒
FlagSpeak attivo (le voci partono solo se `Wave_Driver_Enable` è TRUE,
MESSAGE.C InitLanguage). `FlagKeepVoice: ON` — NON metterlo OFF: con OFF il
game CANCELLA i file VOX\*.VOX all'uscita (ClearVoiceFile: era la cache HD
dei file copiati dal CD).

### Strumenti di verifica

- Ogni `WavePlay` è loggato su stdout: `[wave] play #idx (h=hhhh) freq pb rep
  vol L/R len [interp]`; fine naturale = `[wave] end #idx`, stop esplicito =
  `[wave] stop #idx xN` — con questi tre si vede la catena voce multi-parte
  (play→end→play della parte successiva) e ogni loop anomalo.
- env `LBA_WAVDUMP=file.wav` → dump dei primi 20 s del mix (WAV s16 stereo).
  Smoke test 2026-07-12: voce cella (h=1234, 11111 Hz, 206k campioni) +
  porta/passi (#66, #126/#141 pan e pitch variabili) + ambiance #34;
  peak 3880, zero clipping, 30% campioni non-zero.
  Smoke test 2026-07-13 (fix cursore >64K): intro narrata completa,
  play(206681)→end→play(145985)→end→play(162705)→end, testo typewriter
  visibile negli screenshot (LBA_SHOT_MS) sopra il pianeta Twinsun.

### Codice condiviso col DS

Il parse VOC + il calcolo pitchbend vivono in **`platform/audio_common.h`**
(header-only, da includere dopo port.h), usato sia da sdl_audio.c che da
`platform/nds/nds_audio.c`. Il backend DS è documentato in
`platform/nds/PORTING_NOTES_NDS.md`, "## Audio ARM7/calico".

### Note per l'ARM7 (DS)

- I 16 canali hw ARM7 fanno il mixing: la parte da portare è solo il layer
  `SND_Channel` (stato canale) + SND_ParseVoc; `SND_MixInto` sparisce.
  Mappatura: `SCHANNEL_TIMER = -0x1000000/rate_eff`, formato 8-bit PCM
  (i dati VOC sono 8-bit UNSIGNED, l'hw DS vuole SIGNED ⇒ XOR 0x80 al
  caricamento in RAM condivisa/VRAM oppure conversione al load HQR),
  vol hw 0..127 = `vol_engine >> 2` (clamp per la voce 512→127... attenzione:
  la voce deve restare ~4× gli SFX: usare vol>>2 SENZA clamp non si può,
  meglio: canale voce a vol 127 e SFX a vol>>2 (max 32), che è esattamente
  il rapporto DOS).
- Loop hw DS = loop infinito: per Repeat>1 finito serve IRQ/timer ARM7 che
  conta le ripetizioni (o si accetta il loop infinito + stop dall'ARM9 —
  il game usa quasi solo Repeat=1 e Repeat=0=infinito, Repeat>1 è raro:
  GERELIFE aveva un `HQ_MixSample(83,...,2,...)` commentato; BigSampleRepeat
  di GERETRAK può essere >1).
- Comandi ARM9→ARM7 necessari: play(canale? no: alloca l'ARM7, ritorna
  longhandle), stop_handle(tutti i canali con handle), stop_long, stop_all,
  pause/continue, change_volume(longhandle), in_list(handle) → serve risposta
  sincrona o mirror di stato in RAM condivisa (WaveInList è polling frequente
  nei dialoghi: meglio specchiare una bitmask canali+handle leggibile
  dall'ARM9).
- WaveMove/rebase: se l'ARM7 riproduce direttamente dal buffer HQR in main
  RAM (possibile: main RAM è visibile all'ARM7), il fixup puntatori resta
  necessario ed è delicato (il canale suona mentre i byte si spostano —
  su DS conviene COPIARE il sample in un pool audio dedicato al WavePlay
  e ignorare WaveMove, oppure stoppare i canali che puntano nel range mosso).
- Le voci: BufSpeak è un buffer fisso da 256 KB ⇒ su DS (4 MB) va
  ridimensionato (l'entry VOX più grossa di EN_*: ~172 KB in EN_000; da
  verificare le altre isole) o streammato.

## testdata (smoke test)

- `<repo>\testdata` = copia di `<lba1-install>\Speedrun\Windows`
  (*.HQR, LBA.DAT/DOT, SETUP.LST, VOX\) + lba1sdl.exe + SDL2.dll.
- `LBA.CFG`: **va tenuto con line ending CRLF** — il parser DEF_FILE.C termina le
  stringhe solo su CR(13); con file LF-only `ReadThisString` sfonda `DefString[256]`
  e corrompe `DefHandle` (trovato così il primo crash). Due modifiche al contenuto:
  `MixerDriver: NoMixer` (MixerInitDLL è codice engine reale che richiede una DLL
  Watcom vera; con qualunque altro valore fa exit(1)) e aggiunta la chiave
  `WindowsFilenameSaving: OFF` (assente nel CFG GOG; PERSO.C fa
  `strcpy(string, Def_ReadString(...))` senza check NULL → crash).

## Stato smoke test (build corrente, 2026-07-12)

`LBA_AUTOENTER=30 LBA_SHOT_MS=2500` (wd=testdata): logo → menu → nuova partita
→ prima scena. **I modelli 3D funzionano**: Twinsen in cella (tuta a righe),
cambio vestiti alla cassa, i due dottori (incl. animazione "microfono" con il
cavo disegnato a linee), il clone che spazza, robot — tutti con gouraud/flat
shading corretto e sorting painter's plausibile. Nessun crash in ~55s di
gameplay automatico. **Audio attivo** (sessione audio, stesso giorno): voce
del dialogo in cella + SFX passi/porta/ambiance, verificati via log
`[wave]` e dump WAV (vedi sezione Audio). Manca solo la musica (MIDI stub).

## Stub / simboli aperti (rilevanti)

- `translate/` completa per LIB_SVGA (12 blitter) **+ P_TRIGO + S_POLY +
  P_OB_ISO** (pipeline 3D completa: attori visibili).
- Manca solo **TEXTURE.ASM** (texture mapper: serve all'holomap; stub in
  asm_stubs.c). CPYMASK.ASM è già C nel game/.
- Audio: **Wave + Mixer implementati** (sdl_audio.c: SFX + voci VOX).
  Restano stub MIDI (musica: arriverà come tracce transcodificate) e CD.

## Pacing / timing (documentazione d'oro per il target DS)

Meccanismo DOS autentico, verificato sul sorgente (ticket "hoverpad straveloce"):

- **Clock unico**: tick a 50 Hz (`TimerRef`, IRQ0 riprogrammato con divisore
  23864 = 1193180/50, TIMER_A.C). Tutto ciò che è "a tempo" campiona `TimerRef`:
  dialoghi (MESSAGE.C:942 fa proprio `while (SaveTimerForCar == TimerRef);` per
  la battitura lettere), interpolazioni (`T_REAL_VALUE`/`GetRealValue`), fades,
  `TimerPause`, ambiance.
- **FLA**: PLAYFLA.C:509-518 è GIÀ auto-regolato: per ogni frame aspetta
  `TimerRef - cadence >= 50 / ImageCadence` (ImageCadence dal header .FLA,
  default 12 fps). Lo shim non deve fare nulla; i filmati non erano la causa
  (e in testdata non ci sono .FLA).
- **MainLoop di gioco**: su DOS NON aveva un limitatore attivo — il freno era
  l'hardware VGA (~15-25 fps a 640x480). Unica valvola: `if (NbFramePerSecond >
  500) Vsync();` (mai efficace sotto i 500 fps). I devs però avevano scritto —
  e commentato — il regolatore vero: `while (TimerRef == timeralign);
  timeralign = TimerRef;` = massimo un'iterazione logica per tick (50 fps).
- **Causa del bug**: gli sprite mobili (DoAnim, ramo SPRITE_3D con `SRot`)
  calcolano lo spostamento con `GetRealValue` (time-based, corretto), MA se il
  frame dura meno di 1 tick il delta è 0 e scatta il fallback
  `/* tanpis machine trop speed */ n = ±1` → **1 unità per FRAME** → velocità
  proporzionale agli fps (a centinaia di fps l'hover-pad vola). Tutto il resto
  (dialoghi, camminata, trigger) è TimerRef-based, per questo sembrava giusto.
- **Fix (edit #11)**: riattivato il regolatore originale dei devs. Loop ≤50
  iterazioni/s, delta sempre ≥1 tick, velocità sprite di nuovo scalata su SRot.
  I tick logici restano 50 Hz: nessun rallentamento di dialoghi/gameplay.
- **Per il DS**: identico schema — timer hardware a 50 Hz **esatti** che
  incrementa `TimerRef` (+ logica WaitNbTicks/NbFramePerSecond di TIMER_A.C);
  il busy-wait del MainLoop diventa attesa dell'IRQ (swiIntrWait). NON usare il
  VBlank (~59.83 Hz) come clock di gioco. ATTENZIONE: `RestoreTimer()`
  (P_ANIM.C) RIAVVOLGE `TimerRef` all'indietro dopo i menu — il timer di
  piattaforma deve solo fare `TimerRef++` relativo (mai assegnare un assoluto
  derivato dal wall-clock), come già fa `PORT_TimerCatchUp` in sdl_sys.c.

## Rischi noti per il target DS

- **Memoria**: Log+Phys+Screen (640x480) = ~900 KB già solo di framebuffer,
  più HQM 400 KB e buffer sprite/sample/anim (min ~350 KB). Su DS (4 MB) servirà
  la modalità 320x200/256x192 e tagli aggressivi. `Malloc(-1)` ritorna 0 (query
  non supportata) ⇒ l'engine usa già i minimi.
- `game/GAMEMENU.C:1204` chiama `CopyBlockMCGA(..., 0xA0000)` con indirizzo VGA
  hardcoded: se quel path (zoom FLA) viene eseguito su PC/DS crasha. Da shimmare
  quando si porteranno i FLA.
- Savegame: struct scritte intere (padding GCC ≠ Watcom) — incompatibili coi
  save DOS, ma auto-consistenti.
- Parser DEF_FILE: richiede CR nei file di config (vedi sopra) — attenzione a
  come verranno generati i file sulla SD del DS.
- Endianness OK (DS little-endian), `-fsigned-char` e `-fcommon` obbligatori
  anche su ARM. Le traduzioni translate/ evitano accessi disallineati (vedi
  TRANSLATION_NOTES.md); i lettori HQR leggono struct packed con `fread` su
  buffer locali → nessun accesso disallineato notato finora.
- Timer 50 Hz: su DS usare un timer IRQ a 50 Hz esatti (non il VBlank a ~59.8).

## Comandi

```powershell
$env:PATH = 'C:\msys64\mingw32\bin;C:\msys64\usr\bin;' + $env:PATH
make -C <repo>\platform\sdl -j8
copy <repo>\platform\sdl\lba1sdl.exe <repo>\testdata\
cd <repo>\testdata; .\lba1sdl.exe
```
