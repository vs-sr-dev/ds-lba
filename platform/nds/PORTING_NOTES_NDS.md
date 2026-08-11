# PORTING_NOTES_NDS — LBA1 on the Nintendo DS

## Current state (2026-08-11)

Most of this file is a set of session logs, kept in the order they were
written: each records how a subsystem was built and what was measured at the
time. That makes it useful and slightly dangerous — a measurement from bring-up
is not a description of the ROM you build today. Where a later session overtook
a section, it now says so in place.

The short version:

- The game has been **played through to the end** (build 10, on melonDS). Real
  hardware — a 3DS in DS mode — runs the same code, verified through the
  opening hours.
- Everything the old TODO list called a stub is done: holomap, touch UI, audio,
  music, savegames, multi-language. That section has been rewritten to say what
  is genuinely left.
- The bugs found between bring-up and that playthrough — and what they
  generalise to for anyone moving 1994 code onto an ARM — are in
  [`../../DEVLOG.md`](../../DEVLOG.md).

## M4 bring-up snapshot

State at the end of the session: **lba1ds.nds boots on melonDS and reaches the
FIRST GAMEPLAY SCENE** — Adeline logo → EA bumper → main menu (navigable) → New
Game → intro pages (typewriter text over Twinsun images) → Twinsen's cell
rendered (iso bricks + 3D model) — stable for minutes. After the performance
session (see "## Performance"): **50 fps (tick-lock cap) in incremental
gameplay** at ~6.5 ms per 3D actor; the full grid redraw (camera scroll / cube
change) remains a hitch of ~235 ms a time.
**Savegames PERSIST on SD** (savegame session: libfat + routing in nds_sys.c,
see "## Savegames on fat:/"): saves go to `fat:/lba1/save/`.
The holomap now exists (translate/texture.c, parallel session).
**Audio: SFX + VOX voices working** (audio session, see
"## ARM7/calico audio"). **Area music + jingles** streamed from the SD
(see "## Music") — written, compiled, **still to be checked by ear**.

The engine/ edits made in this session are logged in
`platform/sdl/PORTING_NOTES.md`, section "Edit per NDS" (#12-28): most of it is
the **unaligned access** campaign (ARMv5TE does not trap: a 16/32-bit load at an
odd address returns rotated data — on x86 everything worked). SDL build
re-verified green + PC smoke test OK after the edits.

## Commands

```powershell
# build (Docker devkitARM, calico-based image)
docker run --rm -v ${PWD}:/work -w /work/platform/nds devkitpro/devkitarm:latest make -j8

# run
<melonDS>\melonDS.exe <repo>\platform\nds\lba1ds.nds

# control SDL build (mandatory after every edit to engine/)
$env:PATH = 'C:\msys64\mingw32\bin;C:\msys64\usr\bin;' + $env:PATH
make -C <repo>\platform\sdl -j8
```

Non-interactive smoke test (harness): `autoenter` = N seconds of Return pulses
every ~0.7 s (the equivalent of the SDL shim's LBA_AUTOENTER); `autopause` = N →
N seconds after boot, holds START for ~0.4 s (headless pause-menu repro);
`autowalk` = N seconds of UP held; `autosave` = N → at N seconds runs the
scripted sequence pause menu → Save game → name "DS" (4 initial Esc pulses: an
idle dialogue can eat up to 2 of them); `autoload` = N → pulses Esc (skipping
the logos) until N seconds, then main menu → Continue → first save in the list.
**The harness files are no longer in nitrofiles/**: the default ROM is clean by
construction (the Makefile ERRORS OUT if it finds one in nitrofiles/); for an
instrumented ROM use `make diag DIAG_AUTOENTER=60 DIAG_AUTOSAVE=85` →
`lba1ds_diag.nds` (staged in build/nitrofs_diag, see the Makefile).

## Toolchain: CAREFUL, this is the new libnds (calico)

`devkitpro/devkitarm:latest` is libnds 2.x, based on **calico**:
- compile: you need `-D__NDS__ -DARM9 -I$(CALICO)/include` on top of libnds;
- link: `-specs=$(DEVKITPRO)/calico/share/ds9.specs`, libraries
  `-lfilesystem -lfat -lnds9 -lcalico_ds9 -lm`;
- ndstool: the ARM7 must be given explicitly, `-7 $(CALICO)/bin/ds7_maine.elf`
  (there is no built-in default7 any more);
- the old APIs (timerStart, irqSet, consoleDemoInit, bgInit…) still exist.
- `pmMainLoop()`/`swiWaitForVBlank()` work as in m2-hqrview.

The Makefile mirrors the SDL conventions (gnu89 + force-included
watcom_compat, `-x c`, `-fsigned-char -fcommon`, `-DCDROM -DPORT_NDS`,
compat/inc) and additionally does **`-MMD` dependency tracking** (the SDL
Makefile does not: here, changing watcom_compat.h once cost half an hour to an
inconsistent mixed build).

## platform/nds architecture

- `main_nds.c` — defaultExceptionHandler (a guru meditation with pc/registers
  instead of a white screen: indispensable), consoleDemoInit (the DS console on
  the bottom screen), nitroFSInit + `chdir("nitro:/")`, the autoenter hook,
  then calls `lba_main`.
- `nds_video.c` — the video path from m2-hqrview: main engine MODE_5_2D, BG3
  rotoscale bitmap, 8bpp 512x256 in VRAM A.
  - SVGA (the 640x480 CD game): `Flip`/`CopyBlockPhys[Clip]` decimate Log 2× on
    the fly (sampling, not averaging) → 320x240 → hardware scale to 256x192.
    VRAM writes ONLY as u32 (VRAM ignores byte writes).
  - MCGA (320x200: FLA/SceZoom): `Phys` is a 64000-byte RAM buffer (the engine
    code in `MCGA.C` memcpys into it); the VBlank IRQ presents it every frame.
    `CopyBlockPhysMCGA` (edit #14) writes into `Phys`.
  - Palette: faithful 6-bit DAC quantisation (>>2, re-expanded) then >>3 →
    hardware BGR555, applied immediately ⇒ fades work without a re-blit.
  - `Vsync()` = swiWaitForVBlank.
- `nds_sys.c` — hardware timer 2 at **50.005 Hz** (bus/256/2618, NEVER the
  VBlank at 59.83) incrementing TimerRef/TimerSystem with the exact logic of
  TIMER_A.C (relative ++ only: RestoreTimer rewinds TimerRef). Input on the
  VBlank IRQ (scanKeys): dpad→Joy(1/2/4/8), B→Fire1(space), A→Fire2(return),
  **L→Fire4(ctrl)+Key=0x1D** (= the DOS CTRL: hold L for the classic behaviour
  panel, choose with the dpad), SELECT→Fire8(alt), **X/Y/R→direct behaviour
  Normal/Sporty/Aggressive** through PORT_TouchComportement (Discreet: touch or
  the L panel), START→Key=1(Esc); GetAscii is an int16h-style ring buffer
  (A=0x1C0D, B=0x3920, START=0x011B). NB: the bring-up's X/Y/L/R→FuncKey F1-F4
  map was dead code (the engine never reads FuncKey).
  Mouse: stub. DosMalloc→accounted calloc.
- `stubs.c` / `asm_stubs.c` — COPIES of the SDL stubs (not shared). Since the
  audio session, stubs.c covers only MIDI+CD+DLL: Wave and Mixer are real, in
  `nds_audio.c`.
- `watcom_compat.h` (NDS variant) — besides killing the Watcom keywords:
  `stricmp/strcmpi/strnicmp→strcasecmp` (declared by hand: newlib's
  <strings.h> exports `index()`, which collides with the `index` variables in
  GIF.C/PCX.C → include <string.h> and then `#define index lba1_index`),
  `_MAX_*`, and the **stdio/heap wrappers** (below).
- `compat_nds/` — `process.h` (spawnl→-1) and `direct.h` (→unistd.h), which
  newlib lacks; `compat/` = a copy of the SDL shim's `../LIB386/...` wrappers.

### stdio wrappers (msvcrt emulation) — nds_sys.c

The engine passes NULL/stale FILE*s to fseek/fread in several places (on
DOS/msvcrt the libc validates its parameters and fails gracefully; newlib data
aborts). The NDS variant of watcom_compat.h remaps, in engine TUs only:
`fopen/fclose/fread/fwrite/fseek/ftell` → `NDS_*` with a registry of open
FILE*s (16 slots): an unregistered handle ⇒ a `[FIO]` log plus a soft error.
`NDS_fopen` also normalises paths: it skips `.\`, turns `\` into `/`, and
**lowercases** (everything in nitrofiles/ is lowercase); cwd = nitro:/. Writes
to nitroFS fail ⇒ `Save()`/`Def_WriteString` return FALSE, which is not fatal
(savegames: see Open items).

### Instrumented heap — nds_sys.c

Engine `malloc/calloc/realloc/free` → wrappers with a 16-byte header, a 4-byte
tail canary and a list of live blocks: every allocation logs `[MEM]` if ≥32 KB
(with the total and the spare) and runs `PORT_HeapCheck()` (verifying every
canary → `[HEAP] OVERFLOW after N-byte block` at the moment of the damage).
**`NDS_realloc` with a shrink MUST be IN-PLACE**: `Mshrink`/`LoadMalloc_HQR`
ignore the return value (a realloc that moves leaves PtrPal/LbaFont dangling —
which is exactly what happened). The wrappers apply to engine TUs only: FILE
structs and newlib's own allocations stay outside (which is why the canary
never caught the BufOrder overflow — that was found by reasoning about the LZS
margins instead, edit #19).

## This session's bug hunt (for whoever tackles a new target next)

1. **A completely white screen** = a crash before or inside the console init →
   ALWAYS put `defaultExceptionHandler()` on the first line: the guru meditation
   with pc/addr plus addr2line on the .elf solves it in a minute. First crash:
   `fopen(NULL)` from `FileSize(getenv("ADELINE"))`.
2. **Garbage images with correct logic** = `Expand()` was reading its LZ tokens
   as UWORDs from odd offsets (#15). If this happens again on another target:
   it is ALWAYS alignment.
3. **Deterministic heap corruption** (a FILE struct full of ASCII bytes):
   `Load_HQR` writes up to `SizeFile+500` into the destination buffer (the
   compressed data is copied to the tail and decompressed in place). EVERY
   destination buffer needs that +500 (#19). `Screen` has it ("+ decomp
   marge"), BufOrder did not.
4. **Garbage scene + wild writes**: DecompColonne (#27) — an RLE length read
   unaligned → writes past BufCube.
5. A Makefile without `-MMD` plus edits to force-included headers = a silently
   inconsistent mixed build. Dependency tracking is in place now.

## RAM — breakdown and the 4 MB verdict

ARM9 binary (thumb, -O2, gc-sections): text 287K + data 13K + bss 114K =
**414K**. Steady-state engine allocations ([MEM] log at the first scene):

| Block | KB |
|---|---|
| Phys (MCGA backbuffer) | 62 |
| Log 640x480 | 300 |
| Screen 640x480+500 | 300 |
| BufSpeak (DosMalloc) | 256 |
| BufCube 64x25x64x2 | 200 |
| BufferBrick | 353 |
| HQM (scene/grid/mask pool) | 391 |
| HQR sprites (SpriteMem min) | 49 |
| HQR anims (AnimMem min) | 98 |
| BufText/InvObj/minor | ~80 |
| **Heap total** | **~2090** |

Steady-state spare at bring-up: **~1.5 MB** (fake_heap_end − sbrk). The verdict
still holds: **the game fits a plain 4 MB DS with no cuts and no DSi mode**.

**Overtaken since (build 4).** The two rows above marked "min" were the symptom
of a bug, not a budget. `Malloc(-1)` is a stub that returns 0, so PERSO.C sized
every pool as a fraction of zero and all of them fell to their clamped floors —
and because eviction *compacts* the pool, any pointer a caller was still
holding started pointing at other data. That single stub produced missing 3D
objects, lost animations, text advancing one letter per keypress and gurus in
three unrelated functions; the full story is in
[`../../DEVLOG.md`](../../DEVLOG.md), §5.

The pools are now sized from the measured archives, under `#ifdef PORT_NDS`:
`NDS_ANIM_MEM` 460000 (431 KB of data, 517 entries), `NDS_SPRITE_MEM` 300000
(286 KB, 119 entries), InventoryObj 64000 (57 KB) — roughly 650 KB more heap,
in exchange for zero eviction. Samples keep the floor: 3.4 MB will never be
resident on a 4 MB machine, and the SFX have their own LRU pool in nds_audio.c.
The shipping ROM also carries two voice banks, so the .nds is 17.8 MB rather
than 9.6 MB, and the measured heap after init is now `use=3071K top=380K` —
a far tighter margin than the 1.5 MB above, and the number to watch before
adding anything else.

## Performance (optimisation session — measured on melonDS at full speed)

### Instruments (they stay in the repo, active)

- **Per-phase profiler**: `nds_prof.c` intercepts, via `ld --wrap` (see `WRAPS`
  in the Makefile — zero engine edits), AffScene/Cls/CopyScreen/ClsBoxes/
  AffGrille/SetInterAnimObjet2/AffObjetIso/DrawOverBrick[3]; the present is
  instrumented directly in `PresentRect640`. Every 50 frames it prints seven
  `P0..P6` lines to the console: mean μs/frame per phase (`lg` logic + tick
  wait, `cb` ClsBoxes, `cl` Cls, `cp` CopyScreen, `gr` AffGrille, `an` anim
  interpolation, `ob` AffObjetIso, `ov` overbrick, `pr` present, `ot` the rest
  of AffScene, `sf`/`si` = mean total AffScene over full/incremental frames,
  `n`/`q` = frames/full frames in the window, `o` = actors/frame, `fps` =
  NbFramePerSecond, `stk` = minimum DTCM stack headroom in bytes).
- **Timebase**: calico's `tickGetCount()` (SYSTEM_CLOCK/64 = 1.909 μs/tick),
  started with `tickInit()` in main_nds.c. Sanity check: `P0 w` ≈ 20000 μs at
  50 fps.
- **`make PROFFULL=1`**: a diagnostic ROM with AffScene forced to a full redraw
  every frame (the "full redraw" / sustained camera scroll case). Do not ship.
- **Input smoke test**: besides `nitrofiles/autoenter`, `nitrofiles/autowalk` =
  seconds of UP held (with periodic turns) after the autoenter ends. CAREFUL:
  in game, Return opens the HoloMap (which now works, texture.c) — the
  autoenter must NOT outlast the entry into the scene (60 s is fine).

### Timer FIX (a latent bug, not just a profiling one)

libnds's `timerStart(2, …)` programs hardware TM2, but **calico owns TM2
(the system tick, `tickGetCount`) and TM3 (the tick task) on the ARM9**: the
50 Hz game tick had been stomping it since bring-up (tickGetCount frozen,
calico's tick task at risk). Moved to **TIMER 0** (`nds_sys.c`); TM1 stays
free. On the ARM9 the only timers free for the application are TM0/TM1.

### Profiling BEFORE (first scene, fenced cell, 1 visible actor, μs/frame)

| phase | μs | note |
|---|---|---|
| lg (logic + tick wait) | 9000-9900 | at the 50 fps cap this includes the busy-wait |
| cb ClsBoxes | 430-500 | |
| an SetInterAnimObjet2 | ~95 | |
| **ob AffObjetIso** | **8600-9100** | **the bottleneck: ~9 ms PER ACTOR** |
| ov DrawOverBrick | 200-1000 | |
| pr present | 116-140 | dirty box |
| ot rest of AffScene | ~290 | |
| **si AffScene total (incr.)** | **10000-11000** | |

With ~4-5 actors on screen plus a full redraw, that is the 12-13 fps observed
at bring-up. The present was NOT the bottleneck (130 μs on incremental frames).

### Moves applied (measured delta, same scene)

1. **ITCM+ARM32 for the hot code** (the `PORT_FASTCODE` macros in
   `translate/port_fast.h`, empty on PC): AffObjetIso and the statics
   (p_ob_iso), RotList/TransRotList/RotMatIndex2/Rot/the projections (p_trigo),
   ComputePoly_A/EdgeGauche/EdgeDroite/ClipPolyEdge/ComputeSphere_A (s_poly),
   all the SVGAPoly* fillers + FillVertic_A (s_fillv), AffGraph (graph_a),
   Line_A (s_line), PresentRect640 (nds_video). `CPYMASK.C` was promoted WHOLE
   into ITCM with no edits: the object is renamed `CPYMASK.itcm.o` (plus
   `-marm`) — ds9.ld puts the .text of `*.itcm.*` objects into ITCM. ITCM:
   20112/32736 bytes.
2. **DTCM** (`PORT_FASTBSS`/`PORT_FASTDATA`): List_Point (3K), TabVerticG/D +
   TabCoulG/D (3.8K), TabMat (1.1K), the LMatrice* matrices and the hot scalars
   (compteur/lAlpha…/X0…/Xp/Yp/XCentre/YCentre). **.dtcm.bss = 8.2K**.
3. **Present**: the pack loop switched to u32 reads (2 loads per word instead
   of 8 byte loads) plus ITCM/ARM. pr 130→110 μs (dirty box); it weighs on full
   frames.

   → **ob 8.9 → 6.9-7.0 ms (−23%), si 10.5 → 8.1 ms (−23%)** on melonDS.
   NB: melonDS does not emulate the caches fully; the ITCM/DTCM gain on real
   hardware should be larger, while the ratios between phases stay reliable.

4. **-O3 on the hot translate files: REVERTED** — no measurable gain (±1%)
   with the hot code already in ITCM, and it cost 1.1 KB of ITCM.
5. **GRILLE.C in ITCM: REVERTED** — gr 252.7→251.1 ms in the full-redraw test:
   the cost of AffGrille is not the 64×25×64 cube walk but AffGraph's RLE blits
   (already in ITCM). Not worth 3.6 KB of ITCM.
6. **memcpy/memset overridden in ITCM/ARM (`nds_fastmem.c`)** — AffGraph makes
   one newlib memcpy/memset per RLE run (hundreds of thousands of 2-20 byte
   copies in a full redraw): a local definition with a fast path for small n
   plus u32×8 blocks, compiled `-ffreestanding -fno-builtin` (otherwise GCC
   pattern-matches the loops back into calls to itself). Measured on the forced
   full redraw: **gr 252.7→207.0 ms (−18%), Cls 5.2→4.0 ms, CopyScreen
   8.3→7.6 ms, AffScene full 283→235 ms**; ob drops 6.9→6.4 ms as well. It wins
   everywhere (fread included), with no --wrap: the user object beats the libc
   archive member.

### DTCM: MIND THE STACK (guru meditation)

The stack of calico's main thread lives at the TOP of DTCM and grows downwards
into whatever `.dtcm.bss` leaves free (total budget 16000 bytes). The first
attempt (15 KB of data in DTCM) left ~1 KB of stack → a data abort inside
calico (mutex/ntrcardRomRead) at boot. The `P6 stk` probe (painting 0x5A in
main_nds.c) measures the real minimum headroom: **with 8.2 KB of data in DTCM
the observed headroom is ~2.7 KB** (max stack usage ~5.1 KB across
boot/nitroFS/menu/intro/scene). Do not go below ~1.5 KB of headroom.

### Profiling AFTER

Incremental frame (normal gameplay, same scene, μs/frame):

| phase | before | after |
|---|---|---|
| ob AffObjetIso (1 actor) | 8600-9100 | **6300-6600** |
| cb ClsBoxes | 430-500 | 300-500 |
| ov DrawOverBrick | 200-1000 | 190-1100 |
| pr present (dirty box) | 116-140 | 103-117 |
| an / ot | 95 / 290 | 95 / 285 |
| **si AffScene total** | **10000-11000** | **~7100-8500** (−23%) |
| fps | 50 (cap) | 50 (cap) |

FULL-REDRAW frame (measured with `make PROFFULL=1`, forced every frame — the
equivalent of a camera scroll; μs/frame):

| phase | before* | after |
|---|---|---|
| cl Cls | 5161 | 4014 |
| cp CopyScreen | 8315 | 7597 |
| **gr AffGrille** | **252660** | **206984** |
| pr present full | 8622 | 8621 |
| ob + ov | ~8100 | ~7300 |
| **sf AffScene full total** | **283116** | **~234800 (−17%)** |
| fps (full forced every frame) | 3 | 4 |

\* the "before" of the full column is already a build with ITCM/DTCM round A
(PROFFULL came later); the absolute baseline was slower still (byte-wise
present, AffGraph in thumb in main RAM).

### Final state / remaining headroom

- **Normal gameplay (incremental frames): 50 fps** (tick-lock cap) with
  ~11-12 ms of headroom per frame — it holds 3-4 actors on screen before
  dropping below 50; at 5+ actors, an estimated ~25-30 fps (ob ≈ 6.5 ms per
  actor).
- **The full redraw is still the wall: ~235 ms** (it was ~283+ in round A, and
  more at bring-up). It is NOT sustained in game: PERSO.C's camera recentre is
  ONE full frame per screen-edge crossing (then it goes back to incremental) →
  a perceptible hitch at every scroll, not a constant slideshow.
- Future headroom on the full redraw (in expected order of return):
  1. **Incremental/dirty AffGrille**: in the scroll case 90% of the bricks stay
     identical — shifting Log and repainting only the new strip would remove
     almost all of the cost (a large engine change, to be designed:
     ListBrickColon/DrawOverBrick depend on the total repaint).
  2. **AffGraph's inner loop**: the RLE runs are 2-20 bytes — inlining the copy
     (no memcpy call) inside graph_a.c would cut an estimated further 20-40% of
     gr (it means touching translate/graph_a.c beyond the macros: to be agreed,
     since the file is shared with the PC).
  3. Cls+CopyScreen (11.6 ms): avoidable only by changing the restore strategy
     (Screen is the "clean" copy for ClsBoxes) — little juice.
- melonDS does not emulate caches/TCM fully: on real hardware the ratios may
  shift (ITCM/DTCM should pay off more, main RAM less). `NbFramePerSecond`
  remains the final metric, to be verified on a real console.

## Savegames on fat:/ (savegame session)

**Savegames persist on the SD** (flashcart through auto-patched DLDI; melonDS
through an SD image / folder sync). The nitroFS stays read-only, for assets.

### How it works (all in platform/, 0 engine edits)

- `main_nds.c`: `PORT_FatInit()` (nds_sys.c) BEFORE nitroFSInit —
  `fatInitDefault()` + `mkdir fat:/lba1` + `mkdir fat:/lba1/save`. If it fails
  (e.g. an emulator with no DLDI): it logs `fat: NO SD (DLDI) - saves DISABLED`
  and the game runs as before (Save() fails gracefully in-engine, "Error
  Writing Saved Game" in the menu).
- `nds_sys.c` — routing by **basename** (after the lowercase normalisation):
  - `*.lba` → `fat:/lba1/save/<name>` — these are ONLY savegames (the random
    S*.LBA from `SaveGameWithName`, AUTOSAVE.LBA; assets are .hqr/.cfg/.lst).
    It applies to fopen (read and write), hence to Load/Save/FileSize/Exists/
    Copy.
  - `lba.cfg` → always on fat when writing; when reading, the fat copy wins
    ONLY if it exists (first boot: the nitroFS one is read). That way the
    volumes saved by "save settings" persist.
  - `__tempo.def` (DEF_FILE.C's `c://__tempo.def` scratch file) → on fat.
  - `SYS_FindFirst("*.LBA")` (PlayerGameList/FindPlayerFile, the file
    selector's slot enumeration) → opendir on `fat:/lba1/save` instead of the
    cwd.
  - `remove()` → `NDS_remove` (a new wrapper in watcom_compat.h): the same
    normalisation and routing (the "détruire une sauvegarde" menu,
    __tempo.def).
  - Every routed open/remove logs `[SAV] ...` to the console.
- **AUTOSAVE**: the engine saves `AUTOSAVE.LBA` by itself at EVERY cube change
  (InitCube: `NewCube != oldcube && !DisableAutoSave`) — verified live on
  melonDS at the first scene. The manual save is in the **pause menu** (START →
  Save game → "create new saved game" → a name from the letter grid → Return);
  on restart: main menu → **Continue saved game** → the slot list (AUTOSAVE
  plus named saves).
- The saves are in THIS port's format (GCC structs without packing, see the
  savegame note in PORTING_NOTES.md): NOT compatible with DOS .LBA files, but
  stable between builds as long as T_OBJET and friends do not change layout.

### melonDS setup (the configuration used and verified)

`<melonDS>\melonDS.toml`, section `[DLDI]`:

```toml
[DLDI]
Enable = true
ImagePath = "<repo>\\platform\\nds\\build\\melonds_sd.img"
ImageSize = 256
ReadOnly = false
FolderSync = true
FolderPath = "<repo>\\sd_root"
```

melonDS syncs `sd_root/` → image at boot and writes the modified files back
into `sd_root/` when it CLOSES (saves appear in `sd_root/LBA1/save/*.lba` after
you close the emulator). NB: melonDS also inserts the loaded ROM into the root
of the image (for argv) and syncs it into sd_root — a file to ignore or delete.
On a flashcart the saves end up in `SD:/lba1/save/` (the directory is created
on first boot).

### End-to-end test (the diag harness, see "Smoke test")

1. `make diag DIAG_AUTOENTER=60 DIAG_AUTOSAVE=85` → run: at ~85 s the script
   opens the pause menu and saves under the name "DS" → `[SAV] fopen(wb)
   fat:/lba1/save/s####.lba` on the console, the file in sd_root after closing.
2. `make diag DIAG_AUTOENTER= DIAG_AUTOLOAD=45` → run: skip the logos with Esc
   pulses, at the menu → Continue saved game → load the first slot → the scene
   restarts from the saved point.

## Freeze dossier

The user reports small non-deterministic freezes on real hardware (not gurus:
FREEZES). Circumstantial state:

1. **The Info/Info1 type-pun (edit #33) — FIXED, prime suspect.**
   `*(ULONG *)(&ptrobj->Info)` in OBJECT.C (MOVE_RANDOM ×3) and PERSO.C
   (penguin spawn): `offsetof(T_OBJET,Info)` ≡ 2 (mod 4) — on ARMv5 the STR
   ignores the low two bits, so the 32-bit write of the timer landed on
   `{OffsetLife, Info}`, **corrupting the actor's OffsetLife**: at the next
   DoLife the script restarts from a wild offset (garbage opcodes →
   loops/incoherent states = a freeze with no exception, or erratic behaviour).
   It hit EVERY MOVE_RANDOM actor — the guards and animals outside the Citadel,
   exactly where the user is playing now. A static assert guards it in
   nds_ui_glue.c. Systematic grep of all of engine/: no other `*(TYPE *)(&...)`
   type-pun left (GRILLE/MESSAGE read from aligned bases at even offsets — fine).
2. **The WaveMove window on voices (open, untouched)**: voices play directly
   from BufSpeak (no copy into the pool); if a new dialogue interrupts a voice
   in progress, the ARM7 reads the buffer while it is being rewritten — an
   audio artefact (a "crack"), present on DOS too, NOT a game freeze. Keep
   listening; the fix, if needed, is to copy the voices into the pool as well,
   with a larger budget (~180K more).
3. **A busy-wait on a condition that never arrives** (e.g. `while(!WavePause())`,
   waits on WaveInList): the channel state mirrors are computed on ARM9 ticks
   (no PXI in the queries) — no known stall case, but if a freeze reappears the
   probe is: bring up the console (hold SELECT for 1 s), see whether the
   profiler's P0-P6 lines are still scrolling (ISR alive, MainLoop stuck) and
   whether there are pending `[wave]`s.

During this session's tests (4 melonDS runs of ~2 min: boot→scene→pause
menu→save→load): no freeze observed.

## ARM7/calico audio (nds_audio.c — audio session)

**SFX + VOX voices work.** Wave + Mixer are real, in `nds_audio.c` (the stubs
were removed from stubs.c); DOS semantics as specified in
`platform/sdl/PORTING_NOTES.md`, "Audio (sdl_audio.c)". The VOC parsing and the
pitchbend are shared with the PC in **`platform/audio_common.h`** (header-only,
included by sdl_audio.c and nds_audio.c after port.h; SDL build re-verified
green + LBA_WAVDUMP smoke test: cell voice at 11111 Hz, peak 4317, 50% non-zero
samples).

### Architecture

- **Mixing in hardware**: each engine sample → 1 of the 16 hardware channels,
  driven from the ARM9 with calico's sound API (`soundPreparePcm/soundStop/
  soundChSetVolume/...`, `<calico/nds/arm9/sound.h>`): the commands travel over
  PXI to the audio server inside calico's stock ARM7 (`ds7_maine.elf`) —
  **zero custom ARM7 code**.
- `InitWave`: `soundInit()` + `soundPowerOn()` + mixer config; no hardware
  timer touched (the game's TM0 and calico's TM2/TM3 are intact), no IRQ.
- **No PXI in the queries**: WaveInList is polled tightly by the dialogues, so
  the channel state is mirrored in an ARM9 table (`wch[16]`) and the end of each
  sample is COMPUTED: `ticks = samples * hwtimer / 32` (exact: TICK_FREQ =
  SYSTEM_CLOCK/64, SOUND_CLOCK = SYSTEM_CLOCK/2) plus a 4 ms margin. Repeat=1 →
  SoundMode_OneShot (it stops itself in hardware); Repeat=0 → infinite
  SoundMode_Repeat (stopped by WaveStop*); Repeat>1 (rare, GERETRAK's
  BigSampleRepeat) → a hardware loop plus a **calico watchdog thread**
  (priority MAIN-1, 1.5 KB stack, 50 ms sleep) that calls `soundStop` when the
  time is up. The `wmutex` mutex serialises the table and the sound calls
  between the main thread and the watchdog.
- **Volumes**: the engine passes L/R already panned (0..128 for SFX, 512/512 for
  voice) → 11-bit hardware volume = (L+R)*2 (voice saturates at 2047 = exactly
  the DOS 4× voice/SFX ratio), pan = R*127/(L+R). Master = the CFG's
  WaveVolume*MasterVolume applied to the 7-bit hardware mixer volume
  (`soundSetMixerVolume`). WavePause = master to 0 (the channels keep running
  silently, so the tick accounting stays coherent), WaveContinue restores it.

### Sample memory (the ARM7 reads main RAM: bytes must sit still, plus a cache flush)

- **SFX**: copied XOR 0x80 (VOC unsigned → hardware signed) into an **LRU pool
  keyed on the HQR handle** (320 KB budget, 64 entries, one malloc per entry,
  padded to a word with silence, `armDCacheFlush` after the fill). This makes
  **WaveMove a pure memmove**: the hardware never reads the engine heap, so
  there are no pointer fixups. Entries with an active channel are never evicted.
  Steady-state budget use: a few tens to a hundred KB (the samples are small);
  the heap spare stays > 1 MB (the pool plus the engine's 200 KB SampleMem fit
  in the ~1.5 MB margin).
- **Voices (handle 0x1234)**: played DIRECTLY from BufSpeak, with an in-place
  XOR plus a flush (no 172 KB copy). This is safe because (MESSAGE.C):
  PlaySpeakVoc reloads a fresh BufSpeak before EVERY WavePlay, TestSpk loads the
  next part only when WaveInList says the previous one is done, and BufSpeak is
  a fixed DosMalloc block that WaveMove never touches. The single window: a new
  dialogue interrupting a voice in progress overwrites the buffer while the
  hardware is reading it — the same "speaker crack" artefact as the original DOS
  driver (commented in MESSAGE.C), not a regression.

### VOX / CFG

- The nitroFS carries ONLY `vox/en_gam.vox` (1.8 MB) + `vox/en_000.vox` (6 MB)
  = the English voices of the first island; total .nds ~17.8 MB. The nitroFS CFG
  already has `LanguageCD: English`, `WaveDriver: W_SB16.DLL`, `WaveRate: 22000`
  (read for protocol parity but ignored: every hardware channel has its own
  timer), `MixerDriver: NoMixer`, `FlagKeepVoice: ON`.
  The **final BYOA (libfat)** will bring all the languages and islands.
- nitroFS filenames are lowercase (NDS_fopen normalises the path).

### Known limits

- The DOS driver's linear interpolation on voices: the DS hardware does not
  interpolate (a slightly more "raw" sound, inaudible at 11 kHz on the DS
  speaker).
- Up to 3 tail samples truncated on voices (the hardware length is in words of
  4); SFX are padded with silence, so no click.
- WaveSaveState/RestoreState: stop without a positional resume (no callers in
  the game — verified).
- **Intro: the text starts a few seconds after the voice** (observed on
  melonDS): this is not the driver — the voice starts at the end of `Speak()`
  and the typewriter can only begin after the LZSS load of the page image from
  nitroFS (seconds on the DS, instant on PC). Same call order as on PC/DOS.
- Music: see "## Music" below (done, streamed from SD).

## Music (nds_music.c — music sessions)

**All the game's music lives in ONE index space**, the CD track number `N`, and
the engine reaches it through two different APIs:

| engine call | chain | index |
|---|---|---|
| `PlayCdTrack(num)` | → `PlayTrackCDR(num+1)` | `N = num+1` |
| `PlayMidiFile(num)` | → `PlayMidi(HQR_Get(HQR_Midi,num))` | `N = num+1` |

`PlayMusic()` (AMBIANCE.C:384) dispatches: with `CDEnable` and `num` in 1..9 it
goes to the CD, otherwise to MIDI; plus a handful of direct `PlayMidiFile()`
calls (GAMEMENU 203/2555/2613/3191, PERSO 321/463, PLAYFLA 309 = the "fla
flute").

### Asset source: Common/Midi, not just Common/Music

GOG ships **both halves** of the index space already rendered to audio:
`Common/Midi/LBA1-NN.mp3`, N = 01..33 (02..10 are the same recordings as
`Common/Music/Track_NN.mp3`). So that one directory is enough.

The identity **`LBA1-NN` = XMI entry `NN-1`** is not a guess; it is nailed down
by a byte-for-byte identity fingerprint: in MIDI_MI/MIDI_SB.HQR entries
{8, 9, 32} are identical to each other and are the **only** group of three
duplicates; among the mp3s the same is true of {LBA1-09, -10, -33}, also unique.
Offset +1, confirmed.

Transcoding: `tools/make_music_nds.sh` → for each track **two** mono IMA-ADPCM
WAVs at 22050 Hz (`musNN_l.wav` + `musNN_r.wav`), because the player uses two
separate hardware channels for L and R. ~52 MB in total, staged in
`platform/nds/music_sd/`, destination **SD `fat:/lba1/music/`** (BYOA, like the
saves). No ISO extraction.

**Why ADPCM and not MP3**: the DS audio hardware decodes only
PCM8/PCM16/IMA-ADPCM/PSG. MP3 would need a software decoder (Helix) on a CPU
that is already the bottleneck (12-13 fps in a full redraw). Note: the DS's
*hardware* ADPCM is not usable for streaming — it has a single state header at
the start of the stream and does not reload it at loop points, so you cannot
restart from the middle; we decode to PCM16 in the refill thread instead
(negligible cost).

### Player

Calico channels **14 = L, 15 = R**, reserved (nds_audio.c allocates SFX and
voices only from 0..13, via `NDS_SFX_CHANNELS=14`). Each cycles a double PCM16
buffer in `SoundMode_Repeat`; a 30 ms thread follows the playhead through ticks
(`elapsed*32/timer` — the sound timer and the tick share the same 33 MHz base,
so zero drift) and re-decodes the half just freed, plus `armDCacheFlush` (the
ARM7 DMAs from main RAM, not from our D$). A half = 8192 frames ≈ 0.37 s.

**One-shot, like DOS**: the CD played a track once and then went silent until
the game re-triggered it (the engine knows this through
`EndMusicCD`/`GetMusicCD`). It is needed for the jingles anyway, because the
engine polls `IsMidiPlaying()`. `MUS_LOOP 1` to go back to a seamless loop.

### Jingles without touching the engine

`PlayMidi()` receives the XMI blob, not the number — but **all three** call
sites (AMBIANCE.C:424, AMBIANCE.C:485, PLAYFLA.C:310) do `NumXmi = num`
immediately before. So our `PlayMidi()` reads that global: **zero engine
edits**. `Midi_Driver_Enable = 1` in stubs.c is the switch (ADELINE.C never
assigns it — on DOS A32MT32.DLL exported it).

**Overtaken since (build 3): `midi_mi.hqr` is NOT in the nitroFS.** It used to
be, because `Midi_Driver_Enable` also makes PERSO.C:1485 claim `HQR_Midi`, and
`PlayMidiFile()` dereferences it. But we never read a byte of XMI, so the
resource was 32000 + RECOVER_AREA of pure waste — and worse, it is claimed
*before* the `Malloc(-1)` probe that sizes SpriteMem/AnimMem/SampleMem, so
loading it shrank all three pools (see the RAM section). Now `HQR_Midi = 0`
under `#ifdef PORT_NDS` and the three `HQR_Get(HQR_Midi, …)` call sites are
guarded with `HQR_Midi ? … : 0`; the jingles still work, because they are gated
on `Midi_Driver_Enable`, not on the resource.

Fades (`FadeMidiDown/Up`) are asynchronous on the same thread, through
`soundChSetVolume`.

**Entries 0 and 18 are empty XMIs** (242 bytes ≈ 48 s of nothing) in both HQRs:
on DOS there was no music there, whereas the remaster's mp3 set puts real
tracks in those slots. Skipped (`MUS_IS_SILENT`, indices N = 1 and 19) for
fidelity.

### Locks (important)

Three locks, in a fixed order that is never inverted:
`mmutex` (the decoder and the player's FILE) → `PORT_SndLock` (calico/PXI,
shared with nds_audio.c) → `PORT_IoLock` (the card).

`PORT_IoLock` (nds_sys.c) was born with this session: the music thread reads
the SD **while** the main thread reads the nitroFS, and on a flashcart that is
the same device — neither libfat nor the nitroFS layer serialises, and two
interleaved reads corrupt each other (a savegame write being the worst case).
Now every file operation on both threads takes it, for one libc call at a time,
so a slow SD read never blocks a frame for more than one block. Covered:
`NDS_fopen/fclose/fread/fwrite/fseek/ftell/remove` and directory enumeration
(`SYS_FindFirst/Next/Close`, active in the save menu while the music plays).

## Multi-language (VOX on SD + language selector)

**The text is FREE**: the `text.hqr` we already ship contains **all five
languages** (EN/FR/DE/SP/IT). Indexing is
`Language*MAX_TEXT_LANG*2 + file*2` with `MAX_TEXT_LANG=14` → 28 entries per
language, 141 in total = 5×28+1. Verified by decompressing: Italian is there and
correct. Format note: the `BufText` blob **starts with its own offset table**
(the first WORD says where the text begins) — it is not text from byte 0.

**The voices are not**: `MESSAGE.C` builds the name as `VOX\` +
`ListLanguage[LanguageCD]` + `ListFileText[file]` + `.VOX` → **12 banks per
language** (`GAM`, `000`..`010`), ~33 MB each. Only `en_gam`+`en_000` are in the
nitroFS (this was the known "voices only in the first area" limitation): the
rest comes from **SD `fat:/lba1/vox/`**, staged into
`platform/nds/sd_files/lba1/vox` by `tools/make_vox_nds.sh`. GOG ships **only
EN/FR/DE** (SP and IT shipped subtitled) → 36 files, 99 MB.

`RouteSdPath()` (formerly `RouteSavePath`) routes `*.vox` to the SD **only if
the file is really there**, otherwise it leaves the path on the nitroFS: a cart
with no SD still speaks in the opening area. The same routing applies to the
enumeration (`InitVoiceFile`'s `SYS_FindFirst("VOX\*.VOX")`). Ceiling:
`MAX_FILE_VOICE 42` against the 36 we enumerate — it fits, but it is tight if
languages are added.

### Two traps found when moving the VOX onto writable media

1. **`ClearVoiceFile()` DELETES the banks** when `FlagKeepVoice` is off.
   Harmless while they lived in the read-only nitroFS; now it would be 99 MB of
   the user's own files. `LBA.CFG` ships `FlagKeepVoice: ON`, but `NDS_remove`
   now **refuses any `*.vox` regardless** (returning success: the engine only
   does bookkeeping with it).
2. **`InitFileNar()` had an infinite loop**, and the developers knew — the
   original comment reads `// Sans Filet`. `while (!offset) Read(...)` spins
   forever on a truncated file (Read returns 0 and `offset` stays 0): an
   interrupted copy to the SD = a hang. Added an exit on `wr != 4`, plus a bound
   on `offset > 2048` because the blob goes straight into `BufMemoSeek`, which
   is a fixed 2048-byte SmartMalloc (PERSO.C:1558) — the same family as the
   BufOrder overflow. A PORT-gated edit in MESSAGE.C.

### Language selector (nds_ui.c)

LBA1 **has no in-game language option at all**: `InitLanguage()` reads LBA.CFG
once at boot and that is it. The selector was built: it occupies the touch
screen **when gameplay is not active** (`!PlayStable`), where the eight buttons
would all be disabled anyway — no space stolen from the behaviour panel. A 3+2
grid, with a `TEXT xx / VOICE xx` footer.

Two independent settings, as in the original release: `Language` (subtitles,
all five) and `LanguageCD` (spoken); SP and IT automatically fall back to the
English voice (`LangVoice[]`).

Applying the change is **a single call**: clear the `LastFileInit` cache guard
and re-enter `InitDial(cur)` — it reloads the text for `Language` and, through
`InitSpeak()`, reopens the bank for `LanguageCD`.

**BUT `InitDial` does an HQR load and card I/O, and `nds_ui.c` runs in the
VBlank ISR.** So the tap only posts to a mailbox (`LangMail`), drained by
`PORT_PumpLang()` from the **main thread**: called from
`Vsync`/`Flip`/`CopyBlockPhys` in nds_video.c (the redraw points the engine
crosses both in gameplay and in the menus), with a reentrancy guard.

The choice is remembered in `fat:/lba1/save/lang.txt` (our own file, not
LBA.CFG — the CFG has DEF_FILE.C's CRLF constraint) and is reapplied at boot as
soon as `LastFileInit >= 0`, i.e. strictly after `InitLanguage()` has read the
config: that way we overwrite it rather than racing it.

Known limitation: the touch UI only appears after the first gameplay
(`UiAutoShown`), so on the very first run the language is changed by entering
the game and pausing. From the next boot on, `lang.txt` does the rest.

## Bottom-screen UI (nds_ui.c + nds_ui_glue.c — touch UI session)

**Done and verified on melonDS** (manual test by the user: every button works;
the real inventory-flag gating is active — at the start of a game
ball/sabre/holomap/penguin are off until they are obtained).

### Layout (sub BG2, 8bpp 256x192 bitmap)

```
┌──────────────────────────────────────────────┐
│ ♥ [██████ life ██████]   ⛁ kashes   ✤ n/m    │  status strip y 0..45
│ ✦ [█ magic (if tunic+lvl>0) █] ⚿ keys  FPSnn │  (redrawn only on change)
├──────────────────────────────────────────────┤
│ [NORMAL] [SPORTY] [AGGRO] [SNEAK]            │  behaviour y 50..111
│ [BALL]   [SABER]  [HOLOMAP] [PENGUIN]        │  actions   y 116..177
├──────────────────────────────────────────────┤
│    L: CTRL PANEL - HOLD SELECT: CONSOLE      │  hint y 184
└──────────────────────────────────────────────┘
```
Buttons are 59x62 (4 columns of 63 px, comfortable for a thumb), with a 16x16
icon drawn at 2x plus a 5x7 label; all the fonts and icons are C tables in
nds_ui.c (BYOA, zero external assets). States: off (dark face, dim glyphs,
touch ignored), active (bevel + gold text: the current behaviour and the current
weapon), pressed (light face). The UI palette is SUB entries 1..31 (0 and
240..255 stay with the console). LBA styling: very dark blue panels, gold
accents.

### Sub video / coexisting with the console

`PORT_UI_Init` (main_nds.c, after nitroFSInit): `videoSetModeSub(MODE_5_2D |
DISPLAY_BG0_ACTIVE)` — the libnds console stays on BG0 (map 22/tile 3, 44-56K of
VRAM C), the UI is BG2 ExRot bitmap at map base 4 (64K..128K, no overlap).
A gotcha found in the field: at equal priority BG0 beats BG2 → the console text
"bled" over the UI; you need `REG_BG0CNT_SUB |= 3` (console at priority 3, UI at
0).
Drawing: a 48K shadow buffer in bss (byte writes are free there) plus a blit of
the dirty rectangles only, in u32 (VRAM ignores byte writes). Redraw ONLY on a
state/value change; the worst case (8 buttons flipping) is a few KB of writes in
the VBlank ISR — the same ISR already does 64K/frame in the MCGA present.

- Boot: the console is visible (logging as before). At the FIRST gameplay scene
  the UI appears by itself (`PORT_UI_SceneSeen`).
- **SELECT held for 1 s = toggle console/UI** (only the BG2 bit of DISPCNT_SUB:
  deterministic). The console keeps printing even while covered.

### Touch → action (the map and the injection mechanism)

The VBlank ISR reads `touchRead()` and NEVER calls into the engine: it posts to
3 one-shot mailboxes consumed at ONE point in PERSO.C's MainLoop (edits
#29-31, `#ifdef PORT_NDS`). **The touch path is hardened** (the "flickering
icons + slowdown" bug, same session): a tap is accepted only with `KEY_TOUCH`
held **and** `touchRead()==true` **and** coordinates in range for 2 consecutive
VBlanks (a single spurious sample can never fire), ONE action per contact
(re-armed after 2 clean frames of release), a 250 ms cooldown between actions,
no-op taps on the current behaviour are discarded (SetComportement reloads the
body even for the same value = churn), and touch starts **DISARMED at boot**: a
clean release is required before the first accepted tap — melonDS can report a
phantom stylus "pressed forever" with noisy coordinates until the first real
click arrives (observed symptom: the behaviour row flickering on its own and
"healing" after the first genuine touch). On top of that the gameplay predicate
is **debounced** (10 stable VBlanks before flipping enabled/disabled: the engine
brackets SaveTimer/RestoreTimer inside the frame and the ISR can sample
mid-bracket), and AffScene's recency window is 1.5 s (full redraws from a
recentre can be up to ~0.7 s apart under autoenter — below the old 0.8 s
threshold the gate flapped).

| Button | Mechanism | Engine path |
|---|---|---|
| Normal/Sporty/Aggro/Sneak | `PORT_TouchComportement=0..3` | `SetComportement()` directly (= GERELIFE's SET_COMPORTEMENT opcode) — no behaviour-menu flash, no AffScene(TRUE) |
| Ball / Saber | `PORT_InjectKey=K_1/K_2` (MyKey for ONE iteration) | the existing K_1/K_2 keyboard handlers (equip + draw animation), with the engine's own flag gating |
| Holomap | `PORT_TouchInvAction=0` | the existing switch(InventoryAction), without opening the inventory |
| Penguin | `PORT_TouchInvAction=14` | same (spawning the penguin = the SAME code as selecting it from the inventory) |

Why not FuncKey: **the engine never reads FuncKey** (verified: only the
declaration in LIB_SYS.H exists) — the bring-up's X/Y/L/R→FK_F1..4 map was
dead; the keyboard behaviours in this source are K_F5..K_F8 through `Key` (with
a MenuComportement flash). Mailbox > per-frame injection: exactly-once
consumption even during a 235 ms full redraw.
Unconsumed mailboxes expire after ~200 ms (MAIL_TTL) — a tap posted while a
dialogue was opening never fires late.

### Gating (buttons active ONLY in interactive gameplay)

`GameplayActive()` = `CmptMemoTimerRef == 0` (SaveTimer depth: >0 in every
menu/inventory/holomap/options screen) AND `!FlagCredits` AND AffScene ran
< 40 ticks ago (stamped by `__wrap_AffScene` in nds_prof.c: false in the main
menu/intro/FLA/holomap) AND `PORT_UI_PersoManual()` (glue: `Body != -1 && Move
== MOVE_MANUAL`, false in cutscenes). On top of that, per item, the same
predicate as `Inventory()`: `ListFlagGame[item]==1 &&
ListFlagGame[FLAG_CONSIGNE]==0`; the penguin also requires `NumPingouin > 0`
(a valid object in the cube — default -1). The same guards are replicated
ENGINE-side in the hooks (#30/#31) anyway: belt and braces.

- `nds_ui_glue.c` is the only platform file compiled with ENGINE_CFLAGS (real
  engine headers) so it can read `ListObjet[0].LifePoint/Body/Move` without
  mirroring the T_OBJET struct; everything else is WORD/UBYTE externs
  (Comportement, Weapon, MagicPoint/Level, NbGoldPieces, NbLittleKeys,
  NbFourLeafClover/NbCloverBox, NumPingouin, ListFlagGame, CmptMemoTimerRef,
  FlagCredits).

### Live counters (status strip)

Life = `ListObjet[0].LifePoint` /50 (a green bar); magic = `MagicPoint` out of a
maximum of `MagicLevel*20`, visible only with `FLAG_TUNIQUE` and level > 0
(frame width proportional to the level, as in DrawInfoMenu); kashes
`NbGoldPieces`, keys `NbLittleKeys`, clovers
`NbFourLeafClover/NbCloverBox`. Sampled every VBlank, redrawn only if changed
(a memcmp on a struct).

### Pad controls (touch UI session remap)

| DS button | Function |
|---|---|
| dpad | movement (Joy) |
| B / A | action (space) / validate-recentre (return) |
| **L** | **the DOS CTRL**: hold = the classic behaviour panel (MenuComportement), choose with the dpad |
| X / Y / R | direct behaviour Normal / Sporty / Aggressive (the same mailbox as the touch, no menu flash); Discreet = touch or the L panel |
| SELECT | alt; held for 1 s = toggle console/UI on the sub screen |
| START | Esc (pause menu / skip) |

Legend on the touch UI: "X"/"Y"/"R" tags in the corner of the behaviour
buttons, and the hint "L: CTRL PANEL - HOLD SELECT: CONSOLE" at the bottom.

### Pause-menu crash (fixed in edit #32, same session)

START in game → a `data abort` guru in `_free_r` with an ASCII addr/r7
("%\nor"). Cause: `GetCustomizedMultiText` (MSG_CUST.C, community code)
returned the literal `""` when a text is missing from both TEXT.HQR and
ctxt.csv (which we do not ship), and the callers `free()` the result by
contract → `free("")` on .rodata: msvcrt ignored it, newlib walks the ASCII
bytes as a chunk header and aborts. Trigger: texts **950/951 "Load/Save game"**
(added to the pause menu by the community) do not exist in TEXT.HQR. Fix:
return a freeable `calloc(1,1)` (edit #32). Headless repro:
`nitrofiles/autopause`.

### Verification / regression

- melonDS: clean boot, first scene at an unchanged **50 fps** (P5 fps50 in the
  screenshots with the profiler active), heap unchanged (the UI uses only 48K of
  bss and 0 heap), DTCM stack untouched (no UI data in DTCM, an ISR with small
  locals and no printf).
- Touch verified by hand by the user: **every button works** (instant behaviour
  switch, ball/sabre, holomap, live counters). The gating was tested with a
  temporary `nitro:/uitest` hook (which forced the 3 flags), THEN REMOVED: the
  final build gates on the real flags.
- SDL build re-verified green after edits #29-31 (PORT_NDS guards).

### Remaining (future polish)

- "Real" icons rendered at runtime from INVOBJ.HQR (v1 = hand-drawn C glyphs;
  AGGRO/SPORTY are the most naive ones).
- The penguin has not been tried live (it needs an advanced save with
  FLAG_MECA_PINGOUIN: the path is the same switch as the holomap, which has
  been tried).
- Possibly audio feedback on a tap (SFX 41?) and a protopack/clover button.

## Open items / TODO

Rewritten 2026-08-11. Everything the previous version of this list called a
stub — the holomap, the touch UI, audio, music, multi-language, savegames — has
since been built and is documented in its own section above. What follows is
what is actually left.

### Verification, not code

- **A complete playthrough on real hardware.** The end-to-end run was on
  melonDS; a 3DS in DS mode has only been exercised through the opening hours.
  This is the stated criterion for tagging 1.0.
- **Music and the language selector have never been soaked.** They work, and
  the SD was present for the complete playthrough, but nobody has deliberately
  listened for clicks at the buffer halves, for dropouts when the music thread
  competes with a full redraw, or played a long session in French or Italian.
  The non-English text path in particular was a minefield until the signed-char
  fix (DEVLOG §7) and has had almost no exposure since.
- **Pixel-perfect comparison of the translated ASM against LBADC.EXE in
  DOSBox.** Still not done. The priorities are listed at the end of each module
  in `../../translate/TRANSLATION_NOTES.md` — EdgeDroite's sbb chain,
  SergeSort's tie order, the clipped mask/graph paths, s_fillv's Dith and
  Gouraud fillers.
- **HOLOMAP.C has never had the systematic unaligned-access audit** the rest of
  engine/ got. The holomap renders correctly in game, which is evidence but not
  proof; it was on the list when the holomap was still a stub, and it stayed
  there.
- **Expect more crashes.** Eight latent defects of the 1994 code surfaced
  between bring-up and the complete playthrough, all of them invisible on x86.
  There is no reason to think the supply is exhausted — see DEVLOG.md for the
  shapes they take and for how to read a guru screen.

### Known gaps

- **No FLA cutscenes.** They live inside the CD image, not in the DOS data
  directory, so they are not shipped. The MCGA present-from-Phys path exists
  and is correct in principle, but it has never been exercised — the first
  .FLA that runs will be its first test. (`HQM_Free`, reachable only from the
  end of a cutscene, has a fix that has therefore never executed.)
- **No loading screen**, and the first load on a real card is noticeably slower
  than on an emulator (card reads plus LZSS decompression). Read-ahead or DMA
  from the card is the obvious lever.
- `GetMouseDep`/mouse = zeros: the menus work from keyboard/pad.
- FICHE.C had a `GET_WORD` of its own: converted, but the "fiche" file (the
  character sheet, K_F?) has not been exercised thoroughly.
- `MAX_FILE_VOICE` is 42 and we enumerate 36 voice banks. It fits; it stops
  fitting if another language is added.

### Deliberate, not oversights

- The guard heap and the `[MEM]`/`[FIO]`/`[HQR]`/`[FNT]`/`[ANM]` logs are ACTIVE
  in the shipping build. They cost little and they are what turns a player's
  crash into a fixable report — the voice-bank handle leak and the accented
  character were both caught this way. Drop `PORT_HeapCheck` only if a
  measurement ever shows it mattering.
- The pools are deliberately oversized (see the RAM section): ~650 KB of heap
  bought total residency for sprites and animations. If "NOT ENOUGH MEMORY"
  ever appears at boot, `NDS_ANIM_MEM`/`NDS_SPRITE_MEM` are the knobs, and the
  A/B build that proved RAM pressure was *not* behind the collision bugs is
  worth repeating before turning them down.

### Performance, if it is ever wanted

The full grid redraw is still ~235 ms, one frame per screen-edge crossing. It
is a visible hitch, not a slideshow, and nobody has complained about it — but
if it is ever worth attacking, the order of expected return is in "Final state
/ remaining headroom" above: incremental AffGrille first, then inlining
AffGraph's RLE copy.
