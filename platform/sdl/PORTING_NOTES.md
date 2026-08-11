# Porting notes — engine/ + platform/sdl

Rules of the port:
- `lba1-classic-community-main/` is PRISTINE: never modify it. Work on the copies in `engine/`.
- Every change to a file in `engine/` must be logged below with the file, the reason, and one line.
- Platform code lives in `platform/sdl` (and, later, `platform/nds`). No `#ifdef`s scattered through the engine if it can be avoided.
- Missing symbols → stubs centralised in `stubs.c` (audio/midi/cd) and `asm_stubs.c` (ASM modules not yet translated, future `translate/`).
- Structs read from or written to files (e.g. T_HEADER in HQ_RESS.C): Watcom packed to 2, GCC pads — they need `#pragma pack` or field-by-field reads. Log every case found.

## Changes to engine/ (11 edits)

| # | File | What | Why |
|---|------|------|--------|
| 1 | `LIB_SYS/LIB_SYS.H` | prototype `Exists(char*)` → `Exists(UBYTE*)` | mismatch with the body in FILES.C: GCC treats it as an error (conflicting types) |
| 2 | `LIB_SYS/LIB_SYS.H` | prototype `HQR_GiveIndex` → `void*(LONG,LONG,void*)` | mismatch with the body in HQR.C (it was `T_HQR_BLOC*(UWORD,UWORD,void*)`) |
| 3 | `game/BUBSORT.C` | added `#include <stddef.h> <string.h> "ADELINE.H"` | the file had no includes at all: UBYTE/size_t/memcpy undeclared |
| 4 | `game/PERSO.C` | `void main(...)` → `void lba_main(...)` | SDL2main owns `main()`/WinMain on MinGW; wrapper in `platform/sdl/main_sdl.c` |
| 5 | `LIB_SYS/HQ_RESS.C` | `#pragma pack(push,1)` around `T_HEADER` | the on-disk struct is 10 bytes (Watcom); GCC padded it to 12 → HQR headers read wrong |
| 6 | `LIB_SYS/HQ_R_M.C` | same for `T_HEADER` | as above |
| 7 | `game/GRILLE.C` | same for `T_HEADER` | as above (reading bricks from LBA_BRK.HQR) |
| 8 | `game/PLAYFLA.C` | pack(1) on `T_HEADER` plus the FLI/FLA struct block (`T_HEADER_FLI`, `T_HEADER_FLA`, `T_HEADER_FLA_PASSE`, `T_FLA_TYPE`, `T_FLA_SAMPLE_LIST`, ...) | all of them are read with `Read(handle,&s,sizeof)` from the FLA/FLI files |
| 9 | `LIB_MENU/MENUFUNC.C` | `#ifndef PORT_SDL` around `Message()` | duplicate symbol: PERSO.C defines its own `Message()`; with the Watcom librarian this one was never extracted |
| 10 | `LIB_SVGA/MASKGPH.C` | `#ifndef PORT_SDL` around `CreateMaskGph()` | duplicate of game/GRILLE.C (same story) |
| 11 | `game/PERSO.C` | re-enabled the 2 original commented-out lines at the top of MainLoop: `while (TimerRef == timeralign); timeralign = TimerRef;` | the authentic 50 Hz frame regulator — see the "Pacing" section; without it, moving sprites (the hover-pad) fly |

Note: the `#pragma pack`s were NOT applied to the savegame structs
(`T_OBJET`, `T_EXTRA`, `T_ZONE`, read whole in GAMEMENU.C): the saves this port
produces are self-consistent but NOT compatible with the original DOS .LBA
files. To be decided later (probably pack(1) there too, plus a sizeof
comparison).

## NDS edits (M4 bring-up session — valid and verified on SDL too)

All portable: on little-endian x86 they produce byte-identical results; on the
DS (ARM9, ARMv5TE: unaligned 16/32-bit loads return rotated/garbage data, with
no trap) they are INDISPENSABLE. SDL build re-verified green + smoke test OK
after each one.

| # | File | What | Why |
|---|------|------|--------|
| 12 | `LIB_MENU/MENUFUNC.C` | guard `#ifndef PORT_SDL` → `#if !defined(PORT_SDL) && !defined(PORT_NDS)` | the same `Message()` duplicate on the NDS build |
| 13 | `LIB_SVGA/MASKGPH.C` | same | `CreateMaskGph()` duplicate |
| 14 | `game/GAMEMENU.C:1204` | `CopyBlockMCGA(...,0xA0000)` → `...Phys` under `PORT_SDL\|\|PORT_NDS` | 0xA0000 is the physical DOS VGA address; the shims present `Phys` (fixes a risk already catalogued) |
| 15 | `LIB_SYS/EXPAND.C` | LZ token `*(UWORD*)esi` → byte-wise composition | `esi` is often odd: on ARM it read garbage → ALL decompressed images corrupted |
| 16 | `LIB_SYS/HQ_MEM.C` | `HQM_Alloc`/`HQM_Shrink_Last`: size rounded to 4 | HQM blocks were packed to the byte → BufMap/TabBlock/scenes at odd addresses |
| 17 | `LIB_SYS/HQ_RESS.C` | `HQR_Get`/`HQR_GetSample`: pool entries rounded to 4 | anims/bodies are read as WORDs from the pool: entries at odd offsets broke translate/ too |
| 18 | `game/PLAYFLA.C` | `InitFla`: return FALSE if `OpenRead` fails | without a .FLA, `fread(NULL)` is a crash on newlib (msvcrt returned 0) |
| 19 | `game/PERSO.C` | `BufText` 25000→+500, `BufOrder` 1024→+500 | **latent on PC too**: Load_HQR writes up to SizeFile+500 (the in-place LZS margin); the largest order entry in TEXT.HQR is 536 bytes → 536+500 > 1024 = heap corruption (on the DS it destroyed a FILE struct) |
| 20 | `game/GAMEMENU.C` | `GetMenuMultiTextAux`: `*dst=0` if the text exists nowhere | an uninitialised stack string was passed to Font/SizeFont |
| 21 | `game/DEFINES.H` | macros `LE_R16/LE_RU16/LE_R32/LE_W16/LE_W32` | byte-wise little-endian accessors for the packed script/scene data |
| 22 | `game/GERELIFE.C` | every `*(WORD*)PtrPrg` → `LE_R16` (30 sites) | the life script is byte-packed: odd offsets are constant |
| 23 | `game/GERETRAK.C` | `*(WORD/ULONG*)ptrtrack(±n)` → `LE_R16/R32/W16/W32` (23 sites, including the timer WRITES into the track) | same for the track script (which is self-modifying!) |
| 24 | `game/FICHE.C` | `*(WORD/UWORD*)ptrc` → `LE_R16/RU16/W16` (11 sites, including the GET_WORD macro and the hqrbody write) | byte-packed anim-action data |
| 25 | `game/DISKFUNC.C` | `GET_WORD` → `LE_R16`; plus scene-tail realignment (if `PtrSce` is odd before the zones: a 1-byte memmove) | the scene stream has track/life blocks of arbitrary length; `T_ZONE`/`T_TRACK` are read as structs and want 2-alignment |
| 26 | `game/GRILLE.C` | `LoadUsedBrick` (block entries at `ptb+5`, read and write) and `AffBrickBlock`'s `numbrick` → `LE_*` | the .BLL block offsets are 3+4n → UWORD entries at odd offsets |
| 27 | `game/GRILLE_A.C` | `DecompColonne`/`MixteColonne`: `*(UWORD*)pts` → `LE_RU16` | byte-granular column RLE: a garbage length → wild writes past BufCube (heap trash) |
| 28 | `LIB_3D/P_ANIM.C` | the 4 helpers `read_word/read_dword/write_word/write_dword` → byte access | byte-packed anim records (the file already had centralised helpers) |
| 29 | `game/PERSO.C` | MainLoop, after `MyKey = Key`: under `#ifdef PORT_NDS`, if `PORT_InjectKey != 0` → `MyKey = PORT_InjectKey; PORT_InjectKey = 0` | DS touch UI (nds_ui.c): one-shot scan-code injection (K_1/K_2 = ball/sabre) consumed at a single point — never lost to a long frame, never doubled. Not compiled on SDL (guarded) |
| 30 | `game/PERSO.C` | MainLoop, next to the K_F5-K_F8 block: under `#ifdef PORT_NDS`, `PORT_TouchComportement` (0..3, -1=idle) → `SetComportement()` directly, with the same Body/MOVE_MANUAL guards | DS touch UI: a behaviour change without the MenuComportement flash or AffScene(TRUE) — the same call as GERELIFE.C's SET_COMPORTEMENT opcode |
| 31 | `game/PERSO.C` | MainLoop, inventory block: under `#ifdef PORT_NDS`, `PORT_TouchInvAction` (-1=idle) enters the F_SHIFT condition and, if set, skips `Inventory()` by assigning `InventoryAction` directly; consumed unconditionally at the end of the block | DS touch UI: the holomap (action 0) and the mecha penguin (action 14) go through the SAME switch as a real inventory selection, with the same guards |
| 32 | `game/MSG_CUST.C` | `GetCustomizedMultiText`: `return result ? result : "";` → `... : calloc(1, sizeof(char))` | **latent on PC too**: the documented contract is that the CALLER `free()`s the result (GetMenuMultiTextAux and InfoWallCollisionDamage do) — `free("")` on a .rodata literal corrupts the heap. msvcrt ignored the foreign pointer; newlib on the DS reads the rodata's ASCII bytes as a chunk header (guru: pc=`_free_r`, addr=0x726F0A25 = "%\nor") → a data abort in the pause menu: texts 950/951 "Load/Save game", added by the community, do NOT exist in TEXT.HQR and ctxt.csv is not shipped, so the fallback ran for every missing menu entry |
| 33 | `game/OBJECT.C` (3 MOVE_RANDOM sites), `game/PERSO.C` (mecha penguin spawn) | the 4 `*(ULONG *)(&ptrobj->Info)` → `LE_W32`/`LE_R32` on `&ptrobj->Info` | a 32-bit timer punned over Info/Info1 (adjacent WORDs). `offsetof(T_OBJET,Info)` ≡ **2 (mod 4)** (after the PtrLife pointer plus the OffsetLife WORD — verified with a static assert in `platform/nds/nds_ui_glue.c`): on ARMv5 the **STR ignores the low two bits**, so the 32-bit write landed on `{OffsetLife, Info}`, **corrupting OffsetLife** (the life script restarts from a wild offset → erratic behaviour/freezes), and the LDR came back rotated by 16. It hits EVERY MOVE_RANDOM actor (the outdoor guards, the animals) and the penguin: prime suspect for the non-deterministic freezes seen on hardware outside the Citadel. On x86 the byte layout is identical (little-endian): no behaviour change on PC |

Known residues NOT yet touched (not hit on the boot→first scene path):
HOLOMAP.C (already a stub); MESSAGE.C reads BufText at even offsets from an
aligned base (fine). The Info/Info1 pun (a former residue) was closed by #33.

## Build infrastructure (platform/sdl)

- `watcom_compat.h`: force-included (`-include`) ahead of engine files only.
  It kills `cdecl/__far/__near/__loadds` and provides `min/max` (Watcom's
  stdlib.h had them).
- `compat/LIB386/LIB_SYS/SYS_{FILESYSTEM,TIME}.H`: wrappers for the
  `"../LIB386/LIB_SYS/..."` includes in the game/ sources (the original tree
  layout); resolved through `-Icompat/inc` (GCC concatenates the relative path
  onto the -I directories).
- Makefile: the engine is compiled with `-std=gnu89` plus
  `-Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion
  -Wno-incompatible-pointer-types -Wno-pointer-sign -Wno-discarded-qualifiers`
  (GCC ≥ 14 promotes them to errors), `-fsigned-char -fcommon -pipe`; extra
  `-I`s for the bare includes (`ADELINE.H`, `LIB_CD.H`, `lib_menu.h`…).
  `TMP/TEMP` are exported towards `build/` (some shells pass TMP=C:\WINDOWS →
  collect2/as fail).
- `translate/*.c` (the C translations of the LIB_SVGA ASM modules) are wired
  into the link with a dedicated rule (`$(OBJDIR)/translate/%.o`), compiled with
  clean CFLAGS. No signature mismatch found against C_EXTERN.H/LIB_SVGA.H; no
  edits to translate/.

## platform/sdl — the shim's files

- `main_sdl.c` — the SDL2 entry point, calls `lba_main`. stdout unbuffered.
- `sdl_video.c` — replaces PLATFORM/DOS/LIB_SVGA (INITSVGA.ASM, INITMODE.C,
  S_PAL.ASM, S_PHYS.ASM, S_DLL.C): `Log`/`Phys`/`Screen_X/Y`/`TabOffLine`/clip;
  `Palette/PalMulti/PalOne` (faithful 6-bit DAC quantisation: `>>2` then
  re-expanded), `Flip/CopyBlockPhys[Clip]/Vsync`; `InitGraphSvga/Mcga`,
  `InitMcgaMode` (320x200 for the FLAs), `SimpleInitSvga`, `SvgaInitDLL`→TRUE.
  A dedicated **video thread** (SDL_CreateThread) pumps the events, updates
  keyboard/mouse/timer and presents `Phys` plus the palette at ~60 Hz: the
  engine busy-waits on `Key/Joy/Fire/TimerRef`, so input and time have to
  advance asynchronously (on the DS the VBlank IRQ will do it).
- `sdl_sys.c` — replaces PLATFORM/DOS/LIB_SYS: a 50 Hz timer derived from
  SDL_GetTicks (`TimerRef/TimerSystem/NbFramePerSecond` with the same logic as
  TIMER_A.C's IRQ0 handler); a keyboard with the exact semantics of KEYBOARD.C
  (Joy = arrows, bits 1/2/4/8; Fire = space/ret/ctrl/alt/del/shift;
  FuncKey = F1..F12; `Key` = positional set-1 scan code; `GetAscii` with an
  int 16h-style ring buffer); mouse (Click/Mouse_X/Y/GetMouseDep);
  `DosMalloc`→calloc; `ultoa`; `Touch`→_utime; `SYS_Find*`→_findfirst;
  `SYS_ComputeTime` with the same packing as DOS TIME.C (`cpttime + cptdate`).
- `stubs.c` — the absent drivers, all "success but silent": the Watcom DLL
  loader (`DLL_load/FILE_read`→NULL), CD-ROM (`InitCDR`→FALSE ⇒ the HD/GOG
  path; `DriveCDR = -1` like the DOS driver with no CD, so MESSAGE.C does not
  try to open `A:\LBA\VOX\...` for the found-object voices), MIDI AIL32
  (Init*→TRUE but `Midi_Driver_Enable=FALSE`; `InitMidiTimer` redirects to
  `InitTimer` so the tick is not lost).
  The `*AskVars` return an empty identifier list (`{""}`) ⇒ ADELINE.C's loop
  exits immediately.
- `sdl_audio.c` — a real Wave (samples) and Mixer (volumes) driver: SFX and
  voices play. See the "Audio (sdl_audio.c)" section.
- `asm_stubs.c` — reduced to the **TEXTURE.ASM** stubs alone (the holomap):
  `AsmTexturedTriangleNoClip`, `FillTextPolyNoClip`, `TabText`, `LYmin/LYmax`.
  P_TRIGO/S_POLY/P_OB_ISO and all their data (`X0/Y0/Z0`, `Xp/Yp`, cameras,
  matrices, `TabPoly/TypePoly/NbPolyPoints`, `Ymin/Ymax/TabVertic*/TabCoul*`,
  `ScreenX/Ymin/max`, `List_*`) now live in `translate/p_trigo.c`,
  `translate/s_poly.c`, `translate/p_ob_iso.c` (see TRANSLATION_NOTES.md).
  **Overtaken since**: TEXTURE.ASM was translated too (`translate/texture.c`),
  and the file's own header now reads "Remaining: NONE — the ASM translation
  campaign is complete (16/16)".
- Test instruments in the shim (added with the 3D session):
  - `sdl_video.c`: 24bpp BMP screenshots from the post-palette ARGB buffer.
    **F12** = manual shot; env `LBA_SHOT_MS=<ms>` = periodic shot. Files
    `shot_NNN.bmp` in the working directory.
  - `sdl_sys.c`: `PORT_AutoInput()` — env `LBA_AUTOENTER=<sec>` pulses Return
    (Fire bit 2 + the ascii ring) every ~350 ms for N seconds: a
    non-interactive smoke test up to the first scene.
  - `sdl_audio.c`: a `[wave] play ...` log for every WavePlay; env
    `LBA_WAVDUMP=<file.wav>` writes the first 20 s of the mix to disk.

## Audio (sdl_audio.c)

Replaces the pair of DOS DLLs `W_SB16.DLL` (LIB_SAMP: WAVE.C + WAVE_A.ASM, a
software 8→16 bit stereo mixer) and `M_SB16.DLL` (LIB_MIX: MIXER_A.ASM, the
SB16 chip's volume registers). All the semantics below were verified against
the pristine DOS source (`LIB386/PLATFORM/DOS/LIB_SAMP/WAVE_A.ASM`,
`WAVE.INC`, `LIB_MIX/MIXER_A.ASM`) and against the testdata HQRs — this is the
specification for the ARM7.

### Sample format (verified on the GOG data)

- **SAMPLES.HQR** = 244 HQR entries (method 0 = store or 1 = LZSS), each entry
  a **Creative VOC** file: a 26-byte header (`"Creative Voice File"`, u16 at
  +0x14 = header size), then a SINGLE type-1 block (sound data): `u8 type=1,
  u24 blocksize, u8 sr, u8 pack=0`, then **8-bit unsigned mono** PCM.
  `rate = 1000000/(256-sr)`; data length = `blocksize - 2`.
  Real frequencies in testdata: 4098–17544 Hz (mode: 11111 Hz, sr=166, ×135
  entries; 16129 Hz, sr=194, ×36; 8000 Hz ×16). All ≤ 22 kHz.
- **VOX** (`VOX\EN_000..010.VOX`, `EN_GAM.VOX` etc.) = the same archive format
  as HQR (a u32 offset table, entries of `u32 size, u32 sizeLZSS, u16 method`),
  read by hand in MESSAGE.C (`InitFileNar` loads the offset table into
  BufMemoSeek, `PlaySpeakVoc` decompresses the entry into the 256 KB
  `BufSpeak`). Each entry is a VOC identical to the above EXCEPT that **the
  first byte ('C', 0x43) is patched by the game**: `0` = last part, `1..n` =
  another VOC entry follows (read in sequence: MESSAGE.C's
  `FlagNextVoc = *BufSpeak`, and TestSpk chains the parts).
- **The double use of byte 0**: the DOS driver re-reads it as an interpolation
  flag (`if (byte0+1 < 10) INTERPOL=on`), so VOICES (byte0=0/1) are linearly
  interpolated and SFX (0x43) are not. Reproduced in sdl_audio.c.
- The pointer passed to `WavePlay` points at the start of the VOC (header
  included); the driver skips the header using the u16 at +0x14. There is no
  external length information: the VOC block's blocksize is the authority.

### Mixer semantics (from WAVE_A.ASM, the SB16 build = 16-bit stereo)

- **Channels**: the DOS list has 50 slots but `SHIFT_SAMPLE=3` ⇒ headroom for 8
  channels at full volume (8 × ±16384 = exactly ±32768, with no clamp: it
  wraps!). sdl_audio uses **16 channels** (= the ARM7's hardware channels) with
  an int32 accumulator plus a clamp.
- **No priority, no stealing**: a full list ⇒ WavePlay returns 0 and the sound
  is lost. The only DOS exception: in the 8-bit drivers (not SB16) the voice
  handle 0x1234 did a StopSample() of everything. Not on SB16 ⇒ not reproduced.
- **Handle**: a u16 from the game = the sample's HQR index, or `0x1234`
  (SPEAK_SAMPLE, the dialogue voice — MESSAGE.C calls WaveStopOne(0x1234)
  before each new part). The handle is NOT unique: `WaveStopOne(h)` and
  `WaveInList(h)` operate on ALL channels carrying that handle. The
  `longhandle` returned is unique (`seq<<16|handle` in our case), for
  WaveChangeVolume/WaveGiveInfo0.
- **Pitchbend**: 4096 = 1.0; `rate_eff = (voc_rate * (pb<<4)) >> 16` with
  rounding (≈ rate*pb/4096). The game uses a fixed 0x1000, or 0x800+Rnd(0x800)
  (tracks), or 0x1000±Rnd (footsteps/ambiance/fire). The DOS INCR is
  rate_eff/PlayRate in 16.16 (capped at 0xFFFF: never downsampling with
  PlayRate 22000).
- **Repeat**: the number of playbacks; **0 = 65536** (the `dec word`
  underflowing) ≈ an infinite loop (used by GERETRAK for continuous sounds). At
  the loop the cursor restarts from 0 (FRACT cleared).
- **Per-channel volumes**: VolLeft/VolRight arrive as 0..128
  (Balance/RegleTrois); the VOICE uses **512/512** (MESSAGE.C) = 4× louder than
  the SFX maximum. The DOS per-channel contribution is `s8 * (vol >> 2)`,
  summed into the 16-bit buffer.
  Panning: AMBIANCE.C's `GiveBalance` projects the world position onto the
  screen (Xp 0..640 ⇒ `Balance()`: volright=SinTab[bal*2]*vol>>14,
  volleft=SinTab[bal*2+512]*vol>>14 — a quarter sine, constant power; off
  screen it scales with RegleTrois32 up to ±320px and ±240px past the edges).
  The driver receives the volumes ALREADY panned: the ARM7 has nothing to
  compute, just a left/right volume per channel (0..127 in hardware: use
  vol>>2).
- **Master volume**: the SB16 hardware mixer. The engine reads the defaults with
  `MixerGetVolume` (return 255), overwrites them with the LBA.CFG values
  (`WaveVolume: 163`, `MasterVolume: 195`…) and calls `MixerChangeVolume`
  (-1 = do not change). We apply `wave/255 * master/255` to the final mix
  (on the DS: the master register plus a multiplication in the channel volume).
  `MixerGetInfo` → all 1s (the five sliders in the volume menu, as on SB16).
- **WaveMove** (careful, this is not a simple memmove!): HQ_RESS.C compacts the
  samples buffer WHILE samples are playing; the driver has to move the bytes AND
  re-base the active channels' pointers by `dest-src` (DOS's ShiftSamples).
  Without the fixup: pops and garbage whenever the HQR evicts. **A porting
  trap** (fixed 2026-07-13): DOS re-based everything with `START >= src` — safe
  only because BufSpeak (the voice) lived in low DOS memory, always BELOW the
  samples heap. With a flat heap (PC/DS) BufSpeak can sit ABOVE src ⇒ re-base
  ONLY the pointers inside the moved range `[src, src+Size)`, otherwise the
  voice is shifted at random.
- **Samples longer than 64K samples** (a bug found on the intro, fixed
  2026-07-13 — symptoms: the narration restarting in a loop from the beginning
  AND the typewriter text never shown, because `WaveInList` never said
  "finished" and the caller never exited or advanced): the VOX narration parts
  EXCEED 65535 samples (EN intro: 206681, 145985, 162705 samples). A 16.16
  position cursor in 32 bits wraps at index 65536 and rewinds the sample to
  zero without ever reaching `length`. What is needed is a 32-bit whole sample
  index plus a separate fractional accumulator (DOS: a byte pointer plus a
  separate FRACT word — never a single 16.16). On the DS the ARM7's length
  registers are enough (22 bits of words) and the duration/tick mirror is
  unaffected, but any position or end-of-sample counter must be kept on 32-bit
  counts.
- **WavePause/WaveContinue**: a global pause (menus, video mode changes); DOS
  also did a micro-fade on the buffer, but for us and the DS silence/stopping
  the channels is enough.
- **Follow/"son"** (queueing one sample at the end of another): it exists in the
  DOS driver but the game ALWAYS passes Follow=0 ⇒ not implemented (log it if
  it ever shows up).
- **Reference DOS output**: PlayRate from LBA.CFG's `WaveRate: 22000`, a
  1024-sample half-buffer (~46 ms). SDL: a 22000 Hz S16 stereo device with a
  1024-sample callback.

### Who plays what (observed / from the code)

- Dialogue voices: `MESSAGE.C` → VOX (file 0=SYS, 1=CRE, 2=GAM, 3..14=000..011
  per island; the entry index is the text's position in BufOrder). Handle
  0x1234.
- Positional 3D SFX: `HQ_3D_MixSample` (FICHE.C for actions/animations with a
  sample in the anim stream, EXTRA.C: 11 = magic ball bounce, 86, 97, OBJECT.C
  11, GERETRAK.C for samples from a track script). Footsteps: `(code>>4)+126`
  and `(code&15)+126+15` (BASE_STEP_SOUND=126 ⇒ entries 126..156 = footsteps on
  different materials, with random pitch ±).
- UI/fixed SFX: GAMEMENU 41 (item found?), 34/34+92 (menu plasma), 37
  (breath/fire with random pitch), PERSO.C 37.
- Ambiance: AMBIANCE.C's `GereAmbiance`: 4 samples per scene
  (`SampleAmbiance[]` from DISKFUNC/the scene), vol 110/110, pitch
  0x1000±Rnd(decal), every SecondMin+Rnd(SecondEcart) seconds.
- FLA: PLAYFLA.C plays samples synchronised with the movie (not in testdata).

### LBA.CFG

**No change required**: `WaveDriver: W_SB16.DLL` (any value other than
"NoWave" is fine: our WaveInitDLL accepts anything), `WaveRate: 22000` (read
through WaveAskVars — a real AskVars protocol: we only ask for "WaveRate"; if
it is missing from the CFG the engine calls exit(1), as it did on DOS),
`MixerDriver: NoMixer` MUST stay (MixerInitDLL is real engine code that wants a
Watcom DLL; our MixerChangeVolume/MixerGetVolume are called anyway and handle
the volumes). `LanguageCD: English` plus the `VOX\EN_*.VOX` files present ⇒
FlagSpeak active (the voices only start if `Wave_Driver_Enable` is TRUE,
MESSAGE.C InitLanguage). `FlagKeepVoice: ON` — do NOT set it to OFF: with OFF
the game DELETES the `VOX\*.VOX` files on exit (ClearVoiceFile: it was the hard
disk cache of the files copied from the CD).

### Verification instruments

- Every `WavePlay` is logged to stdout: `[wave] play #idx (h=hhhh) freq pb rep
  vol L/R len [interp]`; a natural end = `[wave] end #idx`, an explicit stop =
  `[wave] stop #idx xN` — with those three you can see the multi-part voice
  chain (play→end→play of the next part) and any anomalous loop.
- env `LBA_WAVDUMP=file.wav` → a dump of the first 20 s of the mix (WAV s16
  stereo). Smoke test 2026-07-12: the cell voice (h=1234, 11111 Hz, 206k
  samples) + door/footsteps (#66, #126/#141 with varying pan and pitch) +
  ambiance #34; peak 3880, no clipping, 30% non-zero samples.
  Smoke test 2026-07-13 (the >64K cursor fix): the full narrated intro,
  play(206681)→end→play(145985)→end→play(162705)→end, with the typewriter text
  visible in the screenshots (LBA_SHOT_MS) over the planet Twinsun.

### Code shared with the DS

The VOC parsing and the pitchbend computation live in
**`platform/audio_common.h`** (header-only, to be included after port.h), used
by both sdl_audio.c and `platform/nds/nds_audio.c`. The DS backend is
documented in `platform/nds/PORTING_NOTES_NDS.md`, "## ARM7/calico audio".

### Notes for the ARM7 (DS)

- The ARM7's 16 hardware channels do the mixing: the only part to port is the
  `SND_Channel` layer (channel state) plus SND_ParseVoc; `SND_MixInto`
  disappears. Mapping: `SCHANNEL_TIMER = -0x1000000/rate_eff`, 8-bit PCM format
  (the VOC data is 8-bit UNSIGNED, the DS hardware wants SIGNED ⇒ XOR 0x80 when
  loading into shared RAM/VRAM, or convert at HQR load time), hardware volume
  0..127 = `vol_engine >> 2` (clamping the 512 voice to 127… careful: the voice
  has to stay ~4× the SFX, so `vol>>2` without a clamp is not an option; better:
  the voice channel at vol 127 and SFX at vol>>2 (max 32), which is exactly the
  DOS ratio).
- A DS hardware loop is an infinite loop: for a finite Repeat>1 you need an
  ARM7 IRQ/timer counting the repetitions (or you accept the infinite loop plus
  a stop from the ARM9 — the game almost only uses Repeat=1 and Repeat=0=
  infinite, Repeat>1 being rare: GERELIFE had a commented-out
  `HQ_MixSample(83,...,2,...)`; GERETRAK's BigSampleRepeat can be >1).
- The ARM9→ARM7 commands needed: play (which channel? no: the ARM7 allocates
  and returns a longhandle), stop_handle (all the channels with that handle),
  stop_long, stop_all, pause/continue, change_volume(longhandle),
  in_list(handle) → this one needs either a synchronous reply or a state mirror
  in shared RAM (WaveInList is polled frequently in the dialogues: better to
  mirror a channel+handle bitmask that the ARM9 can read).
- WaveMove/re-basing: if the ARM7 plays directly from the HQR buffer in main
  RAM (possible: main RAM is visible to the ARM7), the pointer fixup is still
  necessary and delicate (the channel is playing while the bytes move — on the
  DS it is better to COPY the sample into a dedicated audio pool at WavePlay
  time and ignore WaveMove, or to stop the channels pointing into the moved
  range).
- The voices: BufSpeak is a fixed 256 KB buffer ⇒ on the DS (4 MB) it needs to
  be resized (the largest VOX entry in EN_*: ~172 KB in EN_000; the other
  islands still to be checked) or streamed.

## testdata (smoke test)

- `<repo>\testdata` = a copy of `<lba1-install>\Speedrun\Windows`
  (*.HQR, LBA.DAT/DOT, SETUP.LST, VOX\) plus lba1sdl.exe and SDL2.dll.
- `LBA.CFG`: **must be kept with CRLF line endings** — the DEF_FILE.C parser
  only terminates strings on CR(13); with an LF-only file `ReadThisString`
  overruns `DefString[256]` and corrupts `DefHandle` (that is how the first
  crash was found). Two content changes: `MixerDriver: NoMixer` (MixerInitDLL
  is real engine code that requires a genuine Watcom DLL; with any other value
  it calls exit(1)) and the added key `WindowsFilenameSaving: OFF` (absent from
  the GOG CFG; PERSO.C does `strcpy(string, Def_ReadString(...))` with no NULL
  check → crash).

## Smoke test status (current build, 2026-07-12)

`LBA_AUTOENTER=30 LBA_SHOT_MS=2500` (wd=testdata): logo → menu → new game →
first scene. **The 3D models work**: Twinsen in his cell (striped suit), the
change of clothes at the crate, the two doctors (including the "microphone"
animation with the cable drawn as lines), the sweeping clone, the robot — all
with correct gouraud/flat shading and a plausible painter's sort. No crash in
~55 s of automatic gameplay. **Audio active** (audio session, same day): the
cell dialogue voice plus footstep/door/ambiance SFX, verified through the
`[wave]` log and the WAV dump (see the Audio section). Only the music is
missing (MIDI stub).

## Stubs / open symbols (relevant)

- `translate/` complete for LIB_SVGA (12 blitters) **plus P_TRIGO + S_POLY +
  P_OB_ISO** (the full 3D pipeline: actors are visible).
- Only **TEXTURE.ASM** is missing (the texture mapper: the holomap needs it;
  stubbed in asm_stubs.c). CPYMASK.ASM is already C, in game/.
  **Overtaken since**: TEXTURE.ASM is translated (`translate/texture.c`) and
  the campaign is complete, 16/16 — asm_stubs.c is empty.
- Audio: **Wave + Mixer implemented** (sdl_audio.c: SFX + VOX voices). The MIDI
  (music: it will arrive as transcoded tracks) and CD stubs remain.
  **Still true on PC**: the music exists only on the DS build
  (`platform/nds/nds_music.c`); the SDL shim is still silent.

## Pacing / timing (gold-plated documentation for the DS target)

The authentic DOS mechanism, verified against the source (the "absurdly fast
hoverpad" ticket):

- **A single clock**: a 50 Hz tick (`TimerRef`, IRQ0 reprogrammed with the
  divisor 23864 = 1193180/50, TIMER_A.C). Everything time-based samples
  `TimerRef`: the dialogues (MESSAGE.C:942 literally does
  `while (SaveTimerForCar == TimerRef);` for the letter typing), the
  interpolations (`T_REAL_VALUE`/`GetRealValue`), the fades, `TimerPause`,
  the ambiance.
- **FLA**: PLAYFLA.C:509-518 is ALREADY self-regulating: for each frame it
  waits for `TimerRef - cadence >= 50 / ImageCadence` (ImageCadence from the
  .FLA header, default 12 fps). The shim has nothing to do; the movies were
  not the cause (and there are no .FLA files in testdata).
- **The game's MainLoop**: on DOS it had NO active limiter — the brake was the
  VGA hardware (~15-25 fps at 640x480). The only valve: `if (NbFramePerSecond >
  500) Vsync();` (never effective below 500 fps). But the developers had
  written — and commented out — the real regulator:
  `while (TimerRef == timeralign); timeralign = TimerRef;` = at most one logic
  iteration per tick (50 fps).
- **The cause of the bug**: moving sprites (DoAnim, the SPRITE_3D branch with
  `SRot`) compute their displacement with `GetRealValue` (time-based, correct),
  BUT if a frame lasts less than one tick the delta is 0 and the fallback
  `/* tanpis machine trop speed */ n = ±1` fires → **1 unit per FRAME** → a
  speed proportional to the frame rate (at hundreds of fps the hover-pad
  flies). Everything else (dialogues, walking, triggers) is TimerRef-based,
  which is why it all looked right.
- **The fix (edit #11)**: the developers' original regulator was re-enabled.
  The loop runs at ≤50 iterations/s, the delta is always ≥1 tick, and sprite
  speed is scaled by SRot again. The logic ticks stay at 50 Hz: no slowdown of
  dialogues or gameplay.
- **For the DS**: the same scheme — a hardware timer at **exactly** 50 Hz
  incrementing `TimerRef` (plus TIMER_A.C's WaitNbTicks/NbFramePerSecond
  logic); the MainLoop's busy-wait becomes a wait on the IRQ (swiIntrWait). Do
  NOT use the VBlank (~59.83 Hz) as the game clock. CAREFUL: `RestoreTimer()`
  (P_ANIM.C) REWINDS `TimerRef` backwards after menus — the platform timer must
  only ever do a relative `TimerRef++` (never assign an absolute derived from
  wall-clock time), exactly as `PORT_TimerCatchUp` already does in sdl_sys.c.

## Known risks for the DS target

*(Written before the DS bring-up. Two of the four have since been settled —
noted in place.)*

- **Memory**: Log+Phys+Screen (640x480) = ~900 KB of framebuffer alone, plus
  400 KB of HQM and the sprite/sample/anim buffers (min ~350 KB). On the DS
  (4 MB) the 320x200/256x192 mode and aggressive cuts will be needed.
  `Malloc(-1)` returns 0 (query unsupported) ⇒ the engine already uses the
  minimums.
  **Settled**: it fits with no cuts. The engine keeps its 640x480 buffers and
  the DS decimates 2× on presentation (see PORTING_NOTES_NDS.md, "RAM"). And
  `Malloc(-1)` returning 0 was not a convenience but a bug: it made every pool
  fall to its floor, and a compacting cache corrupts pointers callers hold.
  The DS sizes the pools explicitly from the measured archives.
- `game/GAMEMENU.C:1204` calls `CopyBlockMCGA(..., 0xA0000)` with a hardcoded
  VGA address: if that path (the FLA zoom) runs on PC/DS it crashes. To be
  shimmed when the FLAs are ported.
  **Settled**: shimmed to `Phys` by edit #14 above.
- Savegames: whole structs are written (GCC padding ≠ Watcom) — incompatible
  with DOS saves, but self-consistent.
- The DEF_FILE parser: it requires CRs in the config files (see above) — mind
  how the files on the DS's SD card get generated.
- Endianness is fine (the DS is little-endian); `-fsigned-char` and `-fcommon`
  are mandatory on ARM too. The translate/ modules avoid unaligned accesses
  (see TRANSLATION_NOTES.md); the HQR readers read packed structs with `fread`
  into local buffers → no unaligned access noticed so far.
  *(The bring-up found plenty of unaligned accesses elsewhere — the whole #15-28
  campaign above.)*
- The 50 Hz timer: on the DS use an IRQ timer at exactly 50 Hz (not the ~59.8 Hz
  VBlank).

## Commands

```powershell
$env:PATH = 'C:\msys64\mingw32\bin;C:\msys64\usr\bin;' + $env:PATH
make -C <repo>\platform\sdl -j8
copy <repo>\platform\sdl\lba1sdl.exe <repo>\testdata\
cd <repo>\testdata; .\lba1sdl.exe
```
