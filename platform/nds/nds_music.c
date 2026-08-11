/*
 * nds_music.c — streaming music player for the DS port (area themes + jingles).
 *
 * The engine reaches music through two different APIs that share ONE index
 * space, the CD track number N:
 *
 *   area themes   PlayCdTrack(num)  -> PlayTrackCDR(num+1)          N = num+1
 *   jingles / FM  PlayMidiFile(num) -> PlayMidi(HQR_Get(Midi,num))  N = num+1
 *
 * (AMBIANCE.C: PlayMusic() picks the CD path for num 1..9 when CDEnable and
 * the MIDI path otherwise, plus a handful of direct PlayMidiFile() calls.)
 *
 * GOG ships both halves of that index space already rendered to audio, in
 * Common/Midi/LBA1-NN.mp3, N = 01..33. The identity LBA1-NN == XMI entry NN-1
 * is pinned by a byte-identity fingerprint: XMI entries {8,9,32} are the only
 * 3-way duplicate group in MIDI_MI/MIDI_SB.HQR, and {LBA1-09,-10,-33} are the
 * only one among the mp3s. tools/make_music_nds.sh transcodes them to a pair
 * of mono IMA-ADPCM WAVs each (16-bit stereo split into one stream per DS
 * hardware channel), staged for the SD at
 *   fat:/lba1/music/musNN_l.wav  and  musNN_r.wav
 * — same BYOA SD as the savegames.
 *
 * Playback: two calico hardware channels (14 = left, 15 = right, reserved away
 * from the SFX/voice allocator in nds_audio.c) each loop a PCM16 double buffer
 * in SoundMode_Repeat. A refill thread tracks the play head by elapsed ticks
 * (the DS sound timer and calico's tick share the 33 MHz base, so this does not
 * drift) and, each time the head crosses into a new half, decodes the next
 * chunk of ADPCM into the half just vacated and flushes it from the data cache
 * (the ARM7 DMAs main RAM, not our D$). The same thread runs the MIDI fades.
 *
 * Tracks are ONE-SHOT, like the DOS original: a CD track played once and then
 * fell silent until the game re-triggered it (the engine tracks that with
 * EndMusicCD/GetMusicCD). The jingle path needs one-shot anyway, because the
 * engine polls IsMidiPlaying() to decide whether a jingle is still running.
 * Set MUS_LOOP to 1 to loop area music seamlessly instead.
 *
 * Locking, always taken in this order and never the reverse: mmutex guards
 * the decoder + FILE state (a track change must not fclose under the refill
 * thread), then PORT_SndLock guards the calico/PXI calls shared with
 * nds_audio.c, and PORT_IoLock guards the card (the game reads the nitroFS
 * on the main thread while we read the SD here — one device on a flashcart).
 * Steady-state refill needs no calico call, so SD reads never stall the SFX.
 */
#include <nds.h>
#include <calico/arm/cache.h>
#include <calico/nds/arm9/sound.h>
#include <calico/system/mutex.h>
#include <calico/system/thread.h>
#include <calico/system/tick.h>
#include <stdio.h>
#include <string.h>
#include "port.h"

/* engine globals (game/GLOBAL.C) */
extern short CDEnable; /* WORD: gate for the CD music path                 */
extern short NumXmi;   /* WORD: entry the engine is about to hand PlayMidi */

/* ------------------------------------------------------------------ */
/* config                                                             */
/* ------------------------------------------------------------------ */
#define MUS_RATE 22050           /* keep in sync with tools/make_music_nds.sh */
#define MUS_LOOP 0               /* 0 = one-shot like the DOS CD              */
#define MUS_CH_L 14
#define MUS_CH_R 15
#define MUS_HALF 8192            /* frames per half buffer (~0.37 s @22k) */
#define MUS_BUF  (MUS_HALF * 2)  /* frames per channel (double buffer)    */
#define MUS_DIR  "fat:/lba1/music"
#define MUS_VOL  1400            /* 0..2047; sits just under voice (2047) */
#define MUS_MAX_BLOCK 4096       /* cap on WAV nBlockAlign we accept      */
#define MUS_MAX_SPB   (1 + (MUS_MAX_BLOCK - 4) * 2) /* samples per block  */
#define MUS_FADE_STEP 30         /* fade tick, ms (= the thread period)   */

/* Entries whose XMI is empty in MIDI_MI/MIDI_SB.HQR (242 bytes = ~48 s of
 * nothing): DOS was silent there, while the remaster's mp3 set fills them in
 * with real music. Stay faithful and skip them. Index N, not entry number. */
#define MUS_IS_SILENT(n) ((n) == 1 || (n) == 19)

static s16 bufL[MUS_BUF] __attribute__((aligned(32)));
static s16 bufR[MUS_BUF] __attribute__((aligned(32)));

int PORT_MusicOK = 0; /* SD present and at least one track file readable */

/* ------------------------------------------------------------------ */
/* IMA-ADPCM (mono WAV) streaming decoder                             */
/* ------------------------------------------------------------------ */
static const int ImaIndexTab[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static const int ImaStepTab[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
	41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
	190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
	7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
	20350, 22385, 24623, 27086, 29794, 32767};

/* small, stack-safe header (used for probing / track length) */
typedef struct
{
	FILE *f;
	long data_start; /* file offset of the first ADPCM block */
	long data_end;   /* file offset one past the last block  */
	int block_align; /* bytes per block                      */
	int spb;         /* samples per block                    */
} WavHdr;

/* full streaming decoder (big buffers — only the two static instances) */
typedef struct
{
	WavHdr h;
	unsigned char blk[MUS_MAX_BLOCK];
	s16 lo[MUS_MAX_SPB]; /* decoded leftover of the current block */
	int lo_len, lo_idx;
} MusDec;

static MusDec decL, decR;
static Mutex mmutex;         /* guards decL/decR + the mus_* state below */
static volatile int mus_playing;
static volatile int mus_is_midi; /* current stream is a jingle (IsMidiPlaying) */
static u64 mus_start_tick;
static unsigned mus_timer;
static u64 mus_filled; /* number of half-buffers decoded so far (monotonic) */
static u64 mus_total;  /* frames in the track (one-shot end), 0 = unknown   */
static unsigned mus_vol = MUS_VOL; /* base volume, scaled by VolumeMidi()   */
static int fade_cur;    /* current fade gain, 0..256                        */
static int fade_target; /* where the fade is heading, 0..256                */
/* volatile: WaitFadeMidi() spins on this from the main thread while the
 * refill thread walks it down, so the read must not be hoisted */
static volatile int fade_step; /* gain units per thread tick (0 = no fade)  */

/* Every card touch goes through the shared lock in nds_sys.c: this thread
 * reads the SD while the main thread reads the nitroFS, and on a flashcart
 * that is one device. Locked per call, so a slow read never holds up a frame
 * for more than a single block. */
static FILE *io_fopen(const char *p, const char *m)
{
	FILE *f;

	PORT_IoLock();
	f = fopen(p, m);
	PORT_IoUnlock();
	return f;
}

static size_t io_fread(void *p, size_t sz, size_t n, FILE *f)
{
	size_t r;

	PORT_IoLock();
	r = fread(p, sz, n, f);
	PORT_IoUnlock();
	return r;
}

static int io_fseek(FILE *f, long o, int w)
{
	int r;

	PORT_IoLock();
	r = fseek(f, o, w);
	PORT_IoUnlock();
	return r;
}

static long io_ftell(FILE *f)
{
	long r;

	PORT_IoLock();
	r = ftell(f);
	PORT_IoUnlock();
	return r;
}

static void io_fclose(FILE *f)
{
	PORT_IoLock();
	fclose(f);
	PORT_IoUnlock();
}

static u32 rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static u32 rd32(const unsigned char *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* parse a mono IMA-ADPCM WAV header; returns 1 on success */
static int wav_open(WavHdr *d, const char *path)
{
	unsigned char h[12];
	unsigned char cid[8];

	memset(d, 0, sizeof(*d));
	d->f = io_fopen(path, "rb");
	if (!d->f)
		return 0;

	if (io_fread(h, 1, 12, d->f) != 12 ||
		memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
		goto fail;

	/* walk chunks: keep fmt, stop at data */
	for (;;)
	{
		u32 csz;

		if (io_fread(cid, 1, 8, d->f) != 8)
			goto fail;
		csz = rd32(cid + 4);

		if (memcmp(cid, "fmt ", 4) == 0)
		{
			unsigned char fmt[40];
			u32 n = csz > sizeof(fmt) ? sizeof(fmt) : csz;

			if (io_fread(fmt, 1, n, d->f) != n)
				goto fail;
			if (csz > n)
				io_fseek(d->f, (long)(csz - n), SEEK_CUR);
			if (rd16(fmt) != 0x11 || rd16(fmt + 2) != 1) /* IMA ADPCM, mono */
				goto fail;
			d->block_align = (int)rd16(fmt + 12);
			if (d->block_align < 8 || d->block_align > MUS_MAX_BLOCK)
				goto fail;
			d->spb = (n >= 20) ? (int)rd16(fmt + 18)
							   : 1 + (d->block_align - 4) * 2;
			if (d->spb < 1 || d->spb > MUS_MAX_SPB)
				goto fail;
			if (csz & 1)
				io_fseek(d->f, 1, SEEK_CUR); /* chunks are word-aligned */
		}
		else if (memcmp(cid, "data", 4) == 0)
		{
			if (d->block_align == 0)
				goto fail; /* need fmt before data */
			d->data_start = io_ftell(d->f);
			d->data_end = d->data_start + (long)csz;
			return 1;
		}
		else
		{
			io_fseek(d->f, (long)csz + (long)(csz & 1), SEEK_CUR); /* skip */
		}
	}

fail:
	if (d->f)
		io_fclose(d->f);
	d->f = NULL;
	return 0;
}

static void wav_close(WavHdr *d)
{
	if (d->f)
		io_fclose(d->f);
	d->f = NULL;
}

static void dec_reset(MusDec *d)
{
	d->lo_len = d->lo_idx = 0; /* force a block read on the next fill */
	if (d->h.f)
		io_fseek(d->h.f, d->h.data_start, SEEK_SET);
}

/* decode one block into d->lo[]; past the end emit silence (or loop) */
static void dec_block(MusDec *d)
{
	int predictor, index, i, o;

	if (!d->h.f || io_ftell(d->h.f) >= d->h.data_end)
	{
		if (!d->h.f || !MUS_LOOP)
		{
			/* one-shot: the thread stops the channels on the frame count,
			 * this just keeps the ring buffer clean until it does */
			memset(d->lo, 0, sizeof(d->lo));
			d->lo_len = MUS_MAX_SPB;
			d->lo_idx = 0;
			return;
		}
		io_fseek(d->h.f, d->h.data_start, SEEK_SET);
	}

	if (io_fread(d->blk, 1, d->h.block_align, d->h.f) != (size_t)d->h.block_align)
	{
		memset(d->lo, 0, d->h.spb * sizeof(s16)); /* silence on short read */
		d->lo_len = d->h.spb;
		d->lo_idx = 0;
		return;
	}

	predictor = (short)rd16(d->blk); /* signed initial predictor */
	index = d->blk[2];
	if (index > 88)
		index = 88;

	d->lo[0] = (s16)predictor;
	o = 1;

	for (i = 4; i < d->h.block_align && o < d->h.spb; i++)
	{
		int nib;

		for (nib = 0; nib < 2 && o < d->h.spb; nib++)
		{
			int code = nib ? (d->blk[i] >> 4) : (d->blk[i] & 0x0F);
			int step = ImaStepTab[index];
			int diff = step >> 3;

			if (code & 4)
				diff += step;
			if (code & 2)
				diff += step >> 1;
			if (code & 1)
				diff += step >> 2;
			if (code & 8)
				predictor -= diff;
			else
				predictor += diff;

			if (predictor > 32767)
				predictor = 32767;
			else if (predictor < -32768)
				predictor = -32768;

			index += ImaIndexTab[code];
			if (index < 0)
				index = 0;
			else if (index > 88)
				index = 88;

			d->lo[o++] = (s16)predictor;
		}
	}

	d->lo_len = o;
	d->lo_idx = 0;
}

/* pull `count` frames into dst, decoding as needed */
static void dec_fill(MusDec *d, s16 *dst, int count)
{
	while (count > 0)
	{
		int k;

		if (d->lo_idx >= d->lo_len)
			dec_block(d);
		k = d->lo_len - d->lo_idx;
		if (k > count)
			k = count;
		memcpy(dst, d->lo + d->lo_idx, k * sizeof(s16));
		dst += k;
		d->lo_idx += k;
		count -= k;
	}
}

/* refill one half of both channel buffers and flush it to main RAM.
 * Caller holds mmutex. */
static void refill_half(int half)
{
	s16 *pL = bufL + half * MUS_HALF;
	s16 *pR = bufR + half * MUS_HALF;

	dec_fill(&decL, pL, MUS_HALF);
	dec_fill(&decR, pR, MUS_HALF);
	armDCacheFlush(pL, MUS_HALF * sizeof(s16));
	armDCacheFlush(pR, MUS_HALF * sizeof(s16));
}

/* silence + release the hardware. Caller holds mmutex. */
static void mus_halt(void)
{
	if (mus_playing)
	{
		mus_playing = 0;
		mus_is_midi = 0;
		PORT_SndLock();
		soundStop((1u << MUS_CH_L) | (1u << MUS_CH_R));
		PORT_SndUnlock();
	}
	fade_step = 0;
	fade_cur = fade_target = 256;
	wav_close(&decL.h);
	wav_close(&decR.h);
}

/* push the current base volume * fade gain to the hardware */
static void mus_apply_vol(void)
{
	unsigned v = (mus_vol * (unsigned)fade_cur) >> 8;

	if (v > 2047)
		v = 2047;
	PORT_SndLock();
	soundChSetVolume(MUS_CH_L, v);
	soundChSetVolume(MUS_CH_R, v);
	PORT_SndUnlock();
}

/* ------------------------------------------------------------------ */
/* refill thread                                                      */
/* ------------------------------------------------------------------ */
static Thread mthread;
static u8 mstack[4096] __attribute__((aligned(8)));

static int music_thread_main(void *arg)
{
	(void)arg;
	for (;;)
	{
		threadSleep(MUS_FADE_STEP * 1000); /* halves are ~370 ms: big margin */

		/* keep the engine's CD path enabled once music is available (the
		 * boot-time InitCDR() clears CDEnable; re-assert it here, after) */
		if (PORT_MusicOK && !CDEnable)
			CDEnable = 1;

		mutexLock(&mmutex);

		if (mus_playing)
		{
			u64 elapsed = tickGetCount() - mus_start_tick;
			/* samples = ticks * 32 / hwtimer  (TICK_FREQ = SOUND_CLOCK/32) */
			u64 played = (elapsed * 32) / mus_timer;

			if (!MUS_LOOP && mus_total && played >= mus_total)
			{
				mus_halt(); /* one-shot: the track has run out */
			}
			else
			{
				u64 head = played / MUS_HALF;

				/* keep the half after the play head decoded; fill the
				 * one the head has just vacated */
				while (mus_filled < head + 2)
				{
					refill_half((int)(mus_filled & 1));
					mus_filled++;
				}
			}
		}

		/* fades run on the same tick */
		if (mus_playing && fade_step)
		{
			fade_cur += fade_step;
			if ((fade_step > 0 && fade_cur >= fade_target) ||
				(fade_step < 0 && fade_cur <= fade_target))
			{
				fade_cur = fade_target;
				fade_step = 0;
			}
			mus_apply_vol();
			if (!fade_step && fade_cur == 0)
				mus_halt(); /* faded all the way out: release the channels */
		}

		mutexUnlock(&mmutex);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* start a track by index N (1..33); caller holds mmutex               */
/* ------------------------------------------------------------------ */
static int mus_start(long n, int is_midi)
{
	char pl[64], pr[64];
	unsigned words;
	long blocks;

	if (!PORT_MusicOK || !PORT_SndReady() || MUS_IS_SILENT(n))
		return 0;

	mus_halt();

	snprintf(pl, sizeof(pl), "%s/mus%02ld_l.wav", MUS_DIR, n);
	snprintf(pr, sizeof(pr), "%s/mus%02ld_r.wav", MUS_DIR, n);
	if (!wav_open(&decL.h, pl) || !wav_open(&decR.h, pr))
	{
		wav_close(&decL.h);
		wav_close(&decR.h);
		return 0; /* track missing: silent, not fatal */
	}
	dec_reset(&decL);
	dec_reset(&decR);

	blocks = (decL.h.data_end - decL.h.data_start) / decL.h.block_align;
	mus_total = (u64)blocks * (u64)decL.h.spb;

	/* prefill both halves before the channels read them */
	refill_half(0);
	refill_half(1);
	mus_filled = 2;

	mus_timer = soundTimerFromHz(MUS_RATE);
	words = (MUS_BUF * sizeof(s16)) / 4; /* PCM16 length in 32-bit words */
	fade_cur = fade_target = 256;
	fade_step = 0;

	PORT_SndLock();
	/* prepare both, then start together so L/R stay sample-locked */
	soundPreparePcm(MUS_CH_L, mus_vol, 0, mus_timer,
					SoundMode_Repeat, SoundFmt_Pcm16, bufL, 0, words);
	soundPreparePcm(MUS_CH_R, mus_vol, 127, mus_timer,
					SoundMode_Repeat, SoundFmt_Pcm16, bufR, 0, words);
	soundStart((1u << MUS_CH_L) | (1u << MUS_CH_R));
	mus_start_tick = tickGetCount();
	mus_playing = 1;
	mus_is_midi = is_midi;
	PORT_SndUnlock();

	printf("[music] play mus%02ld (%s, %ld s)\n", n,
		   is_midi ? "jingle" : "cd", (long)(mus_total / MUS_RATE));
	return 1;
}

/* ------------------------------------------------------------------ */
/* public init                                                        */
/* ------------------------------------------------------------------ */
void PORT_MusicInit(void)
{
	WavHdr probe;

	/* mmutex needs no init: a calico Mutex is just an owner pointer, and
	 * static zero-init means "free" (same as wmutex in nds_audio.c) */
	fade_cur = fade_target = 256;

	/* SD + music present? (fatInitDefault already ran in PORT_FatInit) */
	if (wav_open(&probe, MUS_DIR "/mus02_l.wav"))
	{
		wav_close(&probe);
		PORT_MusicOK = 1;
	}

	threadPrepare(&mthread, music_thread_main, NULL,
				  &mstack[sizeof(mstack)], MAIN_THREAD_PRIO - 1);
	threadStart(&mthread);

	printf("[music] streaming player up: %s\n",
		   PORT_MusicOK ? "SD tracks found" : "no SD music (silent)");
}

/* ================================================================== */
/* LIB_CD API used by the engine (overrides the stubs in stubs.c)     */
/* ================================================================== */

void StopCDR(void)
{
	mutexLock(&mmutex);
	mus_halt();
	mutexUnlock(&mmutex);
}

LONG PlayTrackCDR(LONG track)
{
	mutexLock(&mmutex);
	mus_start((long)track, 0);
	mutexUnlock(&mmutex);
	return 1;
}

/* length in HSG frames (75/s): the engine derives EndMusicCD from this */
LONG GetLengthTrackCDR(LONG track)
{
	WavHdr d;
	char p[64];
	long blocks, samples;

	if (!PORT_MusicOK)
		return 0;

	snprintf(p, sizeof(p), "%s/mus%02ld_l.wav", MUS_DIR, (long)track);
	if (!wav_open(&d, p))
		return 0;

	blocks = (d.data_end - d.data_start) / d.block_align;
	samples = blocks * d.spb;
	wav_close(&d);

	return (LONG)((samples * 75) / MUS_RATE);
}

/* ================================================================== */
/* LIB_MIDI API — jingles, same streaming player, same index space    */
/* ================================================================== */
/* The engine hands PlayMidi() the XMI blob from HQR_Midi, but every call
 * site (AMBIANCE.C:424, AMBIANCE.C:485, PLAYFLA.C:310) sets NumXmi = num
 * immediately before it — so we read the number off that global and play
 * the matching rendered track, with no engine edit at all. HQR_Midi is
 * still loaded (midi_mi.hqr must be in the nitroFS) because PlayMidiFile()
 * dereferences it; we just ignore the bytes. */

void PlayMidi(UBYTE *ail_buffer)
{
	(void)ail_buffer;
	mutexLock(&mmutex);
	mus_start((long)NumXmi + 1, 1);
	mutexUnlock(&mmutex);
}

void StopMidi(void)
{
	mutexLock(&mmutex);
	if (mus_is_midi)
		mus_halt();
	mutexUnlock(&mmutex);
}

LONG IsMidiPlaying(void)
{
	return mus_playing && mus_is_midi;
}

void VolumeMidi(WORD volume)
{
	if (volume < 0)
		volume = 0;
	if (volume > 100)
		volume = 100;

	mutexLock(&mmutex);
	mus_vol = (MUS_VOL * (unsigned)volume) / 100;
	if (mus_playing)
		mus_apply_vol();
	mutexUnlock(&mmutex);
}

/* Fades are asynchronous: the refill thread walks fade_cur toward
 * fade_target and calls mus_halt() when it reaches 0. The engine fades a
 * jingle down (FadeMusicMidi(1)) right before starting an area track, and
 * that PlayTrackCDR takes the channels over anyway — so a fade that never
 * completes is harmless, it is simply superseded. */
static void mus_fade_to(int target, int nbsec)
{
	int ms = nbsec * 1000;

	mutexLock(&mmutex);
	if (mus_playing)
	{
		int ticks = ms / MUS_FADE_STEP;

		if (ticks < 1)
			ticks = 1;
		fade_target = target;
		fade_step = (target - fade_cur) / ticks;
		if (fade_step == 0) /* always make progress */
			fade_step = (target > fade_cur) ? 1 : -1;
	}
	mutexUnlock(&mmutex);
}

void FadeMidiDown(WORD nbsec) { mus_fade_to(0, nbsec); }
void FadeMidiUp(WORD nbsec) { mus_fade_to(256, nbsec); }

void WaitFadeMidi(void)
{
	while (mus_playing && fade_step)
		threadSleep(MUS_FADE_STEP * 1000);
}

/* the DOS driver looped the current XMI; our streams are one-shot and the
 * engine re-triggers, so there is nothing to arm here */
void DoLoopMidi(void) {}
