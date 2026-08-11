# Little Big Adventure — Nintendo DS

A port of the 1994 engine to the Nintendo DS, built from the GPL source
release. It runs the original game data at 50 fps on a stock 4 MB DS, with
voices, music, savegames, all five text languages, and a touch-screen panel on
the bottom display.

**No game data is included.** This repository holds source only. You need your
own copy of Little Big Adventure — the GOG release works and is what the port
was developed against — and the scripts in `tools/` stage the assets from it.

---

## Status

Honest version, because this matters more than a feature list:

- **One playtester has finished the game end to end, on melonDS.** No blocking
  crash on that run. That is a single complete playthrough, not a QA campaign.
- **Real hardware runs it**, verified on a 3DS in DS mode: identical behaviour
  to the emulator, 50 fps, longer initial load from the card. That test covered
  the opening hours, not the whole game. It has not been run on an original DS
  or a DSi.
- **Expect crashes.** Every bug found so far has been a latent defect of the
  1994 code that only a machine with alignment rules and an unmapped page zero
  takes seriously — see [DEVLOG.md](DEVLOG.md). There is no reason to believe
  the supply has run out. If you hit one, the section
  [Reporting a crash](#reporting-a-crash) turns it into something fixable.
- Sound, music and the language selector work, but have had far less exposure
  than the core game loop.

## Bring your own assets

The port reads the **DOS** data. In a GOG install that is
`Speedrun/Windows/` — twelve `.HQR` archives, `SETUP.LST`, `LBA.CFG` and
`VOX/`. Do **not** point the scripts at `Common/`: that holds the 2023
remaster's archives, which carry the same filenames and completely different
contents.

All three scripts take the path to your install — the directory containing
`Speedrun/` and `Common/` — as an argument, or read it from `$LBA1_ASSETS`.
Run them, and everything else in this file, from the repository root
(`ds-lba/`):

```sh
export LBA1_ASSETS="/path/to/Little Big Adventure"

tools/make_nitro_nds.sh    # game archives + 2 voice banks -> the ROM itself
tools/make_vox_nds.sh      # all 36 voice banks            -> SD card
tools/make_music_nds.sh    # 33 music tracks, needs ffmpeg -> SD card
```

`make_nitro_nds.sh` is the only one the build requires. The other two stage
about 151 MB onto the SD card: the ROM cannot hold the full voice set, and the
music is streamed. Without an SD card the game still runs, still speaks in the
opening area, and simply has no music and no savegames.

## Build

devkitARM with the calico-based libnds. The simplest route needs nothing
installed but Docker:

```sh
docker run --rm -v ${PWD}:/work -w /work/platform/nds \
    devkitpro/devkitarm:latest make -j8
```

The result is `platform/nds/lba1ds.nds`, around 17.8 MB.

There is a second build target, `platform/sdl/`, that runs the same engine and
the same translated modules on a PC through SDL2 (MinGW i686 — 32-bit on
purpose, to keep the DS's ILP32 data model). It exists to debug engine changes
with a debugger and a fast iteration loop; anything touching `engine/` should
be checked there too.

## Install

Copy `lba1ds.nds` to your flashcart, and the staged `lba1/` directory to the
root of the SD card:

```
SD:/lba1/music/mus01_l.wav …    streamed music
SD:/lba1/vox/en_003.vox …       voice banks past the opening area
SD:/lba1/save/                  savegames (created on first boot)
SD:/lba1/lba1ds.log             diagnostic log
```

On **melonDS**, point the DLDI settings at a folder-synced SD image:

```toml
[DLDI]
Enable = true
ImagePath = "<anywhere>/lba1_sd.img"   # must NOT exist; melonDS creates it
ImageSize = 512
ReadOnly = false
FolderSync = true
FolderPath = "<the folder holding lba1/>"
```

`ConsoleType = 0` (DS mode). In DSi mode the DLDI driver is not used at all.
Note that melonDS writes the synced folder back **when the emulator closes**,
so savegames and the log appear on disk then.

The DeSmuME core in RetroArch does not boot modern libnds homebrew — white
screen. melonDS 1.1 standalone is the reference.

## Controls

| Pad | |
|---|---|
| D-pad | move |
| **B** | action (space) |
| **A** | validate / confirm; recentres the camera in game |
| **L** (hold) | the DOS CTRL key: classic behaviour panel, pick with the d-pad |
| **R** (hold) | the DOS ALT key: aim and throw the magic ball / sabre |
| **Y** | the DOS SHIFT key: open the inventory ring |
| **X** | behaviour Normal |
| **START** | Esc — pause / menu |
| **SELECT** (hold 1 s) | toggle the debug console |

The bottom screen carries eight buttons — the four behaviours, ball, sabre,
holomap, penguin — plus the life and magic bars and the counters. Outside
gameplay it shows a language selector instead; the eight buttons would be
disabled there anyway.

The buttons are not a second input path bolted on the side: a tap posts to a
mailbox that the main loop drains, and behaviour changes go through the same
call the game's own script opcode uses.

## Languages

`TEXT.HQR` contains all five languages at once — English, French, German,
Spanish, Italian — so the selector costs nothing. Voices are the expensive
half and GOG ships only English, French and German; Spanish and Italian fall
back to the English voice track, exactly as the original did.

The original game had no in-game language option at all (it read `LBA.CFG`
once at boot), so the selector is ours. It lives on the touch screen when you
are not in gameplay, and your choice persists in `SD:/lba1/save/lang.txt`.

## Known limitations

- **No FLA cutscenes.** They live inside the CD image, not in the DOS data
  directory, and were never part of this port. The typewriter intro is there.
- Heavy scenes drop below 50 fps with five or more visible actors; a full grid
  redraw (camera recentre) costs a single long frame.
- The initial load on real hardware is noticeably slower than on an emulator —
  card reads plus LZSS decompression, with no loading screen yet.
- Savegames are this port's own format. They are stable across builds but not
  interchangeable with DOS `.LBA` saves.

## Reporting a crash

A DS guru screen is far more useful than it looks — the whole call chain is
usually recoverable from it. What helps:

1. **A photo or screenshot of the red screen.** Every register matters; `pc`
   and `addr` most of all.
2. **`SD:/lba1/lba1ds.log`.** Do *not* restart the ROM first — close the
   emulator normally, or the last lines may never reach the card. The file
   appends, so a long session across several sittings is fine, and each run
   stamps the build it came from.
3. **The md5 of the exact `.nds` you were running.**

Keep the `.elf` and `.map` of the build you shipped: without them the `pc` is
just a number.

## Repository map

| | |
|---|---|
| `engine/` | the 1994 game and library code, C, with the port's fixes |
| `translate/` | the 16 x86 assembly modules of the original, rewritten in C |
| `platform/nds/` | everything DS: video, audio, music, input, touch UI, file I/O |
| `platform/sdl/` | the PC build used to debug engine changes |
| `tools/` | asset staging scripts (BYOA) |
| `milestones/` | the two bring-up ROMs, kept for reference |
| [`DEVLOG.md`](DEVLOG.md) | the bugs, and what they teach about porting 1994 code |

Deeper engineering notes live next to the code they describe:
`platform/nds/PORTING_NOTES_NDS.md`, `platform/sdl/PORTING_NOTES.md`,
`translate/TRANSLATION_NOTES.md`. They are working documents and go into far
more detail than this file. The DS and translation notes are in English; the
SDL porting notes are still in Italian.

## Where the engine comes from

The engine is the community GPL release of the original source:
<https://github.com/2point21/lba1-classic-community>. `engine/` is derived
from it, with the port's changes applied in place; the assembly modules that
release left as `.ASM` are translated to C in `translate/`.

Little Big Adventure was developed by Adeline Software International in 1994.
This project is not affiliated with Adeline, [2.21] or GOG, and ships none of
their data.

## License

GPL v2, inherited from the upstream source release. See [LICENSE](LICENSE).
