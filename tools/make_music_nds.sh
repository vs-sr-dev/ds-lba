#!/bin/sh
# make_music_nds.sh — build the DS music set from the GOG assets.
#
# The engine drives every piece of music through one index space: the CD
# track number N.
#   PlayCdTrack(num)   -> PlayTrackCDR(num+1)      -> N = num+1  (area themes)
#   PlayMidiFile(num)  -> HQR_Get(HQR_Midi, num)   -> N = num+1  (jingles)
# GOG ships both halves of that index space already rendered to audio:
#   Common/Music/Track_NN.mp3   N = 02..10  (Red Book area themes)
#   Common/Midi/LBA1-NN.mp3     N = 01..33  (the XMI set, incl. 02..10)
# The mapping LBA1-NN == XMI entry NN-1 is confirmed by a byte-identity
# fingerprint: XMI entries {8,9,32} are identical to each other, and so are
# {LBA1-09,LBA1-10,LBA1-33} — the only 3-way duplicate group on either side.
#
# So Common/Midi alone covers everything: we transcode LBA1-01..33 into
#   musNN_l.wav + musNN_r.wav   (mono IMA-ADPCM, 16 kHz)
# which nds_music.c streams on two hardware channels (see platform/nds).
# The user copies the staged lba1/ tree onto the SD (BYOA, like the saves).
#
# Usage: tools/make_music_nds.sh [gog_dir] [outdir]

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
OUT="${2:-platform/nds/sd_files/lba1/music}"
SRC="$GOG/Common/Midi"
RATE=22050	# keep in sync with MUS_RATE in platform/nds/nds_music.c

[ -d "$SRC" ] || { echo "error: no such dir: $SRC" >&2; exit 1; }
mkdir -p "$OUT"

n=0
for i in $(seq -w 1 33); do
	in="$SRC/LBA1-$i.mp3"
	[ -f "$in" ] || { echo "skip  LBA1-$i.mp3 (missing)"; continue; }

	# One mono stream per DS hardware channel: the player runs L and R as two
	# separate PCM16 voices, so stereo has to be split at transcode time.
	ffmpeg -v error -y -i "$in" -af "pan=mono|c0=c0" \
		-ar $RATE -c:a adpcm_ima_wav "$OUT/mus${i}_l.wav"
	ffmpeg -v error -y -i "$in" -af "pan=mono|c0=c1" \
		-ar $RATE -c:a adpcm_ima_wav "$OUT/mus${i}_r.wav"

	echo "ok    mus$i  ($(du -k "$OUT/mus${i}_l.wav" | cut -f1) KB/channel)"
	n=$((n + 1))
done

echo "--- $n tracks -> $OUT ($(du -sh "$OUT" | cut -f1) total)"
