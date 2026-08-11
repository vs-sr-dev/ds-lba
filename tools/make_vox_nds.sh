#!/bin/sh
# make_vox_nds.sh — stage the spoken-dialogue banks for the DS SD card.
#
# MESSAGE.C builds a VOX filename as  VOX\ + ListLanguage[LanguageCD] +
# ListFileText[file] + ".VOX", i.e. EN_GAM.VOX, EN_000.VOX ... EN_010.VOX —
# 12 banks per language, one per island/scene group. The nitroFS only carries
# en_gam + en_000 (the ROM would not fit otherwise), so everything else comes
# off the SD, exactly like the music and the savegames:
#   fat:/lba1/vox/en_003.vox ...
# NDS_fopen prefers that copy and falls back to nitro:/vox/ (see nds_sys.c),
# so a DS without an SD still speaks in the opening area.
#
# GOG ships EN, FR and DE only — there are no Spanish or Italian voice banks
# (those languages shipped subtitle-only). The TEXT.HQR we already bundle
# does contain all five languages.
#
# Usage: tools/make_vox_nds.sh [gog_dir] [outdir]

set -e

# Your own installed copy of the game (the directory holding Speedrun/ and
# Common/), as an argument or via LBA1_ASSETS. No game data, and no path to
# your machine, lives in this repository.
GOG="${1:-$LBA1_ASSETS}"
[ -n "$GOG" ] || {
	echo "usage: $0 <path-to-your-LBA1-install> [outdir]" >&2
	echo "   or: LBA1_ASSETS=<path-to-your-LBA1-install> $0" >&2
	exit 1
}
OUT="${2:-platform/nds/sd_files/lba1/vox}"
SRC="$GOG/Speedrun/Windows/VOX"

[ -d "$SRC" ] || { echo "error: no such dir: $SRC" >&2; exit 1; }
mkdir -p "$OUT"

n=0
for f in "$SRC"/*.VOX; do
	[ -f "$f" ] || continue
	base=$(basename "$f" | tr 'A-Z' 'a-z')	# NDS_fopen normalizes to lowercase
	cp "$f" "$OUT/$base"
	n=$((n + 1))
done

echo "--- $n vox banks -> $OUT ($(du -sh "$OUT" | cut -f1) total)"
echo "languages: $(ls "$OUT" | cut -c1-2 | sort -u | tr '\n' ' ')"
