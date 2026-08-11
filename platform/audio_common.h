/*
 * audio_common.h — backend-agnostic pieces of the Wave (sample) driver,
 * shared between platform/sdl/sdl_audio.c (software mixer) and
 * platform/nds/nds_audio.c (ARM7 hardware channels via calico).
 *
 * Include AFTER the platform "port.h" (needs the Adeline base types
 * UBYTE/UWORD/ULONG).  Header-only on purpose: the two Makefiles do not
 * share object directories, and the shared part is small.
 *
 * VOC format + pitchbend semantics are documented in
 * platform/sdl/PORTING_NOTES.md, section "Audio (sdl_audio.c)":
 *  - every sample the engine hands to WavePlay is a Creative VOC file:
 *    u16 header size at +0x14 (26 in all LBA data), then one block of
 *    type 1 (sound data): u24 blocksize, u8 sr, u8 pack(=0), then
 *    unsigned 8-bit mono PCM. rate = 1000000/(256-sr); data length =
 *    blocksize - 2.  Real rates in the GOG data: 4098..17544 Hz.
 *  - byte[0] of the buffer is abused by the game: 'C' (0x43) for plain
 *    SAMPLES.HQR entries, 0/1 for VOX voice parts (MESSAGE.C FlagNextVoc).
 *    The DOS driver reuses it as interpolation flag: byte0+1 < 10 =>
 *    linear interpolation on (voices yes, SFX no).
 *  - Pitchbend: 4096 = 1.0; DOS: rate_eff = (rate * (pb<<4)) >> 16 with
 *    carry rounding.
 */
#ifndef AUDIO_COMMON_H
#define AUDIO_COMMON_H

typedef struct
{
	const UBYTE *data;
	ULONG length;   /* samples (bytes, 8-bit mono) */
	ULONG rate;     /* Hz, before pitch bend */
	UBYTE interpol; /* from patched byte[0] */
} SND_VocInfo;

static int SND_ParseVoc(const UBYTE *buf, SND_VocInfo *out)
{
	UWORD hdrsize;
	const UBYTE *body;
	ULONG blksize;

	if (!buf)
		return 0;

	hdrsize = (UWORD)(buf[0x14] | (buf[0x15] << 8));
	body = buf + hdrsize;

	if (body[0] != 1) /* only block type 1 (sound data) supported */
		return 0;
	if (body[5] != 0) /* pack method must be raw 8-bit */
		return 0;

	blksize = (ULONG)body[1] | ((ULONG)body[2] << 8) | ((ULONG)body[3] << 16);
	if (blksize < 2)
		return 0;

	out->length = blksize - 2; /* blocksize includes sr + pack bytes */
	out->rate = 1000000UL / (256 - (ULONG)body[4]);
	out->data = body + 6;
	/* first byte of the file doubles as interpolation flag when < 9
	 * (VOX voice entries have 0/1 there instead of 'C') */
	out->interpol = ((UBYTE)(buf[0] + 1) < 10) ? (UBYTE)(buf[0] + 1) : 0;

	return 1;
}

/* pitch: 4096 = 1.0 (DOS: (rate*(pb<<4))>>16 with carry rounding) */
static ULONG SND_PitchedRate(ULONG rate, UWORD pitchbend)
{
	return (ULONG)(((unsigned long long)rate * ((ULONG)pitchbend << 4)
					+ 0x8000) >> 16);
}

#endif /* AUDIO_COMMON_H */
