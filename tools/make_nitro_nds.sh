#!/bin/sh
# make_nitro_nds.sh — build platform/nds/nitrofiles/ from your own LBA1 install.
#
# This is the BYOA step: no game data lives in this repository, so the ROM
# cannot be built until you point this script at a copy of the game you own.
# It reads only, and writes only into the output directory.
#
# WHICH COPY OF THE GAME
#
# The DOS data — the one this port speaks — is in Speedrun/Windows in the GOG
# release (LBA.CFG, SETUP.LST and twelve .HQR archives, plus VOX/). Do NOT
# point this at Common/: that directory holds the 2023 remaster's archives,
# which carry the same names and different content (640x480 art the DS build
# has no path for). The script refuses a directory without LBA.CFG + SETUP.LST
# precisely because the remaster has neither.
#
# WHAT GOES IN THE ROM AND WHAT DOES NOT
#
# Everything the engine must have on the first frame, plus the two voice banks
# for the opening area (~9 MB of the 17.8 MB ROM). The remaining ten voice
# banks and the music are staged onto the SD card instead, by make_vox_nds.sh
# and make_music_nds.sh — the ROM would not fit otherwise, and a DS with no SD
# still speaks in the opening area because NDS_fopen falls back to nitro:/.
#
# MIDI_MI.HQR is deliberately NOT copied. The DS never parses a byte of XMI:
# PlayMidi() reads the NumXmi global that every call site sets just before it,
# and plays the matching transcoded track (see nds_music.c). Loading the
# archive would only cost heap — and it used to, before PERSO.C's pool sizing.
#
# Usage: tools/make_nitro_nds.sh [gog_dir] [outdir]

set -e

# Your own installed copy of the game: the directory holding Speedrun/ and
# Common/. Pass it as the first argument, or export LBA1_ASSETS once for the
# session. The repository carries no game data and no path to yours.
GOG="${1:-$LBA1_ASSETS}"
[ -n "$GOG" ] || {
	echo "usage: $0 <path-to-your-LBA1-install> [outdir]" >&2
	echo "   or: LBA1_ASSETS=<path-to-your-LBA1-install> $0" >&2
	exit 1
}
OUT="${2:-platform/nds/nitrofiles}"
SRC="$GOG/Speedrun/Windows"

[ -d "$SRC" ] || { echo "error: no such dir: $SRC" >&2; exit 1; }
[ -f "$SRC/LBA.CFG" ] && [ -f "$SRC/SETUP.LST" ] || {
	echo "error: $SRC has no LBA.CFG/SETUP.LST — that is not the DOS data." >&2
	echo "       (Common/ holds the 2023 remaster; this port needs the DOS files.)" >&2
	exit 1
}

mkdir -p "$OUT/vox"

# The twelve archives the engine opens by name, plus the scene index.
# NDS_fopen normalizes to lowercase, and so does the nitroFS image.
for f in ANIM.HQR BODY.HQR FILE3D.HQR INVOBJ.HQR LBA_BLL.HQR LBA_BRK.HQR \
         LBA_GRI.HQR RESS.HQR SAMPLES.HQR SCENE.HQR SPRITES.HQR TEXT.HQR \
         SETUP.LST; do
	[ -f "$SRC/$f" ] || { echo "error: missing $SRC/$f" >&2; exit 1; }
	cp "$SRC/$f" "$OUT/$(echo "$f" | tr 'A-Z' 'a-z')"
done

# TEXT.HQR carries all five languages at once (141 entries = 5 x 28 + 1), so
# the language selector on the touch screen costs nothing in ROM. The voices
# are the expensive half, and GOG has only EN/FR/DE of those.
for f in EN_GAM.VOX EN_000.VOX; do
	[ -f "$SRC/VOX/$f" ] || { echo "error: missing $SRC/VOX/$f" >&2; exit 1; }
	cp "$SRC/VOX/$f" "$OUT/vox/$(echo "$f" | tr 'A-Z' 'a-z')"
done

# LBA.CFG, patched. Four changes, each one load-bearing:
#
#  - MixerDriver: NoMixer      A DOS mixer DLL cannot be probed here, and the
#                              engine gates all sound on that probe failing
#                              cleanly. NEVER set this to a .DLL name.
#  - the five volumes to 255   The shipped values (WaveVolume 163 x
#                              MasterVolume 195) multiply out to 49% of the
#                              DS hardware master — the whole game, voices
#                              included, arrives at half volume.
#  - WindowsFilenameSaving     Absent from the GOG file; DEF_FILE.C strcpy()s
#                              the lookup result unchecked, so the key has to
#                              exist.
#  - CRLF, everywhere          DEF_FILE.C's parser terminates a line on CR. Fed
#                              an LF-only file it runs off the end of its
#                              buffer. Every line below is written back \r\n
#                              even if the input had a stray bare LF.
awk '
	{ sub(/\r$/, "") }
	$1 == "MixerDriver:" { $0 = "MixerDriver: NoMixer" }
	$1 ~ /^(WaveVolume|MusicVolume|CDVolume|LineVolume|MasterVolume):$/ { $0 = $1 " 255" }
	$1 == "WindowsFilenameSaving:" { seen = 1 }
	{ printf "%s\r\n", $0 }
	END {
		if (!seen) {
			printf "\r\n"
			printf "; Added by tools/make_nitro_nds.sh: the key is missing from the\r\n"
			printf "; GOG file and the engine copies the lookup result unchecked.\r\n"
			printf "WindowsFilenameSaving: OFF\r\n"
		}
	}
' "$SRC/LBA.CFG" > "$OUT/lba.cfg"

# Cheap proof the parser will not trip: one CR per LF, and the keys present.
cr=$(tr -cd '\r' < "$OUT/lba.cfg" | wc -c)
lf=$(tr -cd '\n' < "$OUT/lba.cfg" | wc -c)
[ "$cr" = "$lf" ] || { echo "error: lba.cfg is not CRLF ($cr CR / $lf LF)" >&2; exit 1; }

echo "--- nitroFS staged in $OUT ($(du -sh "$OUT" | cut -f1))"
echo "    $(ls "$OUT"/*.hqr | wc -l) archives, $(ls "$OUT/vox" | wc -l) voice banks, lba.cfg $lf lines CRLF"
echo
echo "next: tools/make_music_nds.sh and tools/make_vox_nds.sh stage the SD card,"
echo "      then build with 'make' in platform/nds (see README)."
