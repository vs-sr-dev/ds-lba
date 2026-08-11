/*
 * nds_audio.c — Wave (sample) driver + Mixer (volume) driver for the DS.
 *
 * Replaces the DOS DLL pair W_SB16.DLL + M_SB16.DLL like sdl_audio.c does
 * on PC, but here the MIXING IS DONE BY THE HARDWARE: each engine sample
 * is mapped 1:1 onto one of the 16 DS sound channels, driven from the
 * ARM9 through calico's sound API (soundPreparePcm/soundStop/... which
 * PXI-forward to the sound server inside calico's stock ARM7, ds7_maine
 * — no custom ARM7 code).  Semantics follow the DOS driver as specified
 * in platform/sdl/PORTING_NOTES.md, "Audio (sdl_audio.c)":
 *
 *  - 16 channels, NO priority/steal: all busy => the sound is dropped.
 *  - Handle = HQR index (or 0x1234 = dialog voice); WaveStopOne/WaveInList
 *    operate on ALL channels with that handle. longhandle = seq<<16|handle.
 *  - Pitchbend 4096 = 1.0; Repeat: 1 = one-shot, 0 = infinite loop (DOS
 *    65536-underflow), >1 = finite loop count (rare; see watchdog below).
 *  - VolLeft/VolRight arrive already panned (GiveBalance), 0..128 for SFX
 *    and 512/512 for the voice: hw vol = (L+R)*2 (0..2047 11-bit calico
 *    volume, voice saturates at 2047 = exactly the DOS 4x voice/SFX ratio),
 *    hw pan = R*127/(L+R) (0=left, 127=right).
 *  - VOC PCM is 8-bit UNSIGNED, the DS hw wants SIGNED => XOR 0x80 on copy.
 *
 * Sample memory strategy (ARM7 reads main RAM directly, so the bytes must
 * stay put and be cache-flushed):
 *  - SFX (SAMPLES.HQR): copied XOR 0x80 into a dedicated LRU pool keyed by
 *    HQR handle (budget POOL_BUDGET). This makes WaveMove (HQ_RESS.C
 *    compacting its sample heap WHILE samples play) a plain memmove: the
 *    hardware never reads the engine heap. Entries referenced by an active
 *    channel are never evicted.
 *  - Voice (handle 0x1234): played DIRECTLY from BufSpeak, XOR'd in place.
 *    Rationale (MESSAGE.C): PlaySpeakVoc Read()s a fresh VOC into BufSpeak
 *    before every WavePlay and TestSpk only loads the next part after
 *    WaveInList says the current one ended; BufSpeak is a fixed DosMalloc
 *    block, never WaveMove'd. The only overwrite-while-playing window is a
 *    new dialog interrupting an old voice (the original driver had the
 *    same "crac HP" artifact — MESSAGE.C comments it). Copying would cost
 *    up to 172KB per part for nothing.
 *
 * WaveInList is polled hot by the dialog typewriter, so no PXI round-trip
 * there: the ARM9 mirrors the channel state and computes each channel's
 * end time itself. duration_ticks = samples * hwtimer / 32 exactly
 * (TICK_FREQ = SYSTEM_CLOCK/64, SOUND_CLOCK = SYSTEM_CLOCK/2), so the
 * estimate tracks the true hardware clock; a small margin is added so a
 * sound is never reported dead early. One-shot channels stop themselves
 * in hardware; a small watchdog thread (50ms period) reaps expired slots
 * and soundStop()s finite loop counts (Repeat>1, e.g. GERETRAK's
 * BigSampleRepeat) — no IRQs, no game-loop hooks, TM0-TM3 untouched.
 *
 * Verification: every WavePlay logs "[wave] #idx ch f vol..." on the DS
 * console (same info as the SDL backend's log).
 */
#include <nds.h>
#include <calico/arm/cache.h>
#include <calico/nds/arm9/sound.h>
#include <calico/system/thread.h>
#include <calico/system/mutex.h>
#include <calico/system/tick.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"
#include "../audio_common.h"

#define NDS_WAVE_CHANNELS 16
/* channels 14/15 are reserved for the streaming music player (nds_music.c);
 * SFX/voice allocate only 0..13 so music is never stolen */
#define NDS_SFX_CHANNELS 14
#define SPEAK_HANDLE 0x1234 /* MESSAGE.C SPEAK_SAMPLE */

/* report "still playing" for this long after the estimated end, to cover
 * PXI latency + timer rounding (~4 ms) */
#define END_MARGIN_TICKS 2048

/* ================================================================== */
/* Channel state mirror (ARM9-visible, WaveInList never hits the PXI)  */
/* ================================================================== */

typedef struct
{
	UBYTE active;
	UBYTE loop;    /* hw loop mode (Repeat != 1) */
	UWORD handle;  /* engine handle (HQR index, 0x1234 voice) */
	ULONG longhandle;
	ULONG info0;
	WORD pool;     /* pool entry index, -1 = external (BufSpeak) */
	UWORD vol_left, vol_right;
	u64 end_tick;  /* estimated end (0 = plays until stopped) */
} WCHAN;

static WCHAN wch[NDS_WAVE_CHANNELS];
static ULONG wseq;
static Mutex wmutex; /* guards wch/pool AND serializes calico sound calls */
static int wpause;
static int snd_ready;

/* ================================================================== */
/* Sample pool — LRU cache of XOR'd SFX, keyed by HQR handle           */
/* ================================================================== */

#define POOL_BUDGET (320 * 1024)
#define POOL_ENTRIES 64

typedef struct
{
	UBYTE *data;  /* malloc'd, word-aligned, signed PCM, padded to 4 */
	ULONG bytes;  /* padded size */
	ULONG samples;
	UWORD handle;
	UBYTE used;
	u64 lastuse;
} POOLENT;

static POOLENT pool[POOL_ENTRIES];
static ULONG pool_total;

static int pool_entry_playing(int idx)
{
	int n;
	for (n = 0; n < NDS_WAVE_CHANNELS; n++)
		if (wch[n].active && wch[n].pool == idx)
			return 1;
	return 0;
}

static int pool_evict_lru(void)
{
	int n, victim = -1;
	u64 oldest = (u64)-1;

	for (n = 0; n < POOL_ENTRIES; n++)
		if (pool[n].used && !pool_entry_playing(n) && pool[n].lastuse < oldest)
		{
			oldest = pool[n].lastuse;
			victim = n;
		}
	if (victim < 0)
		return 0;
	free(pool[victim].data);
	pool_total -= pool[victim].bytes;
	pool[victim].used = 0;
	pool[victim].data = NULL;
	return 1;
}

/* find-or-load; returns pool index or -1 (call with wmutex held) */
static int pool_get(UWORD handle, const SND_VocInfo *voc)
{
	ULONG bytes = (voc->length + 3) & ~3UL;
	int n;

	for (n = 0; n < POOL_ENTRIES; n++)
		if (pool[n].used && pool[n].handle == handle &&
			pool[n].samples == voc->length)
		{
			pool[n].lastuse = tickGetCount();
			return n;
		}

	if (!bytes || bytes > POOL_BUDGET)
		return -1;

	for (;;)
	{
		int slot = -1;

		for (n = 0; n < POOL_ENTRIES; n++)
			if (!pool[n].used)
			{
				slot = n;
				break;
			}

		if (slot >= 0 && pool_total + bytes <= POOL_BUDGET)
		{
			UBYTE *dst = (UBYTE *)malloc(bytes);
			ULONG i;

			if (!dst)
			{
				if (!pool_evict_lru())
					return -1;
				continue;
			}
			/* VOC unsigned -> DS signed (src may be unaligned: HQR heap) */
			for (i = 0; i < voc->length; i++)
				dst[i] = voc->data[i] ^ 0x80;
			for (; i < bytes; i++)
				dst[i] = 0; /* signed silence padding to the word boundary */
			armDCacheFlush(dst, bytes); /* ARM7 reads main RAM, not our D$ */

			pool[slot].data = dst;
			pool[slot].bytes = bytes;
			pool[slot].samples = voc->length;
			pool[slot].handle = handle;
			pool[slot].used = 1;
			pool[slot].lastuse = tickGetCount();
			pool_total += bytes;
			return slot;
		}

		if (!pool_evict_lru())
			return -1;
	}
}

/* voice: XOR BufSpeak's PCM body in place (fresh data every play, body is
 * word-aligned: DosMalloc block + 26B header + 6B block header = +32) */
static void voice_prepare(const SND_VocInfo *voc)
{
	UBYTE *p = (UBYTE *)voc->data;
	ULONG n = voc->length;

	if (!((ULONG)p & 3))
	{
		ULONG *w = (ULONG *)p;
		ULONG nw = n >> 2;
		while (nw--)
			*w++ ^= 0x80808080UL;
		p += (n & ~3UL);
		n &= 3;
	}
	while (n--)
		*p++ ^= 0x80;

	armDCacheFlush(voc->data, voc->length);
}

/* ================================================================== */
/* Volume/pan mapping (see header comment)                             */
/* ================================================================== */

static void volpan(UWORD l, UWORD r, unsigned *vol, unsigned *pan)
{
	unsigned sum = (unsigned)l + (unsigned)r; /* 0..256 SFX, 1024 voice */
	unsigned v = sum * 2;

	if (v > 2047)
		v = 2047;
	*vol = v;
	*pan = sum ? (r * 127U + sum / 2) / sum : 64;
	if (*pan > 127)
		*pan = 127;
}

/* ================================================================== */
/* Expiry bookkeeping + watchdog thread                                */
/* ================================================================== */

/* reap ended one-shots (hw already stopped them) and stop finite loop
 * counts whose time is up (call with wmutex held) */
static void wave_expire(void)
{
	u32 stopmask = 0;
	u64 now = tickGetCount();
	int n;

	for (n = 0; n < NDS_WAVE_CHANNELS; n++)
	{
		if (!wch[n].active || !wch[n].end_tick)
			continue;
		if (now >= wch[n].end_tick)
		{
			if (wch[n].loop)
				stopmask |= 1u << n;
			wch[n].active = 0;
		}
	}
	if (stopmask)
		soundStop(stopmask);
}

static Thread wthread;
static u8 wstack[1536] __attribute__((aligned(8)));

static int wave_thread_main(void *arg)
{
	(void)arg;
	for (;;)
	{
		threadSleep(50000); /* 50 ms; no libc calls in here (no TLS) */
		mutexLock(&wmutex);
		if (snd_ready)
			wave_expire();
		mutexUnlock(&wmutex);
	}
	return 0;
}

/* ================================================================== */
/* Mixer master volume (LBA.CFG WaveVolume/MasterVolume, volume menu)  */
/* ================================================================== */

static LONG MixVol[5] = {255, 255, 255, 255, 255}; /* wave midi cd line master */

static void apply_master(void)
{
	/* wave * master onto the hw mixer's 7-bit master volume */
	unsigned v = wpause ? 0
				 : (unsigned)((MixVol[0] * MixVol[4] * 127L) / (255L * 255L));

	if (snd_ready)
		soundSetMixerVolume(v);
}

/* ================================================================== */
/* Engine API — LIB_SAMP/LIB_WAVE.H                                    */
/* ================================================================== */

LONG Wave_Driver_Enable = 0;
char Wave_Driver[260];
char Wave_Driver_Name[64] = "NDS calico sound driver (SB16 semantics)";

/* vars requested from LBA.CFG through WaveAskVars (protocol parity with
 * the DOS DLL / SDL shim; the DS ignores the value: every hw channel has
 * its own rate timer and the mixer output is fixed at ~32728 Hz). */
static LONG CfgWaveRate = 22000;
static char *WaveIdentList[] = {"WaveRate", ""};

LONG WaveInitDLL(char *dlldriver)
{
	printf("%s,\nreplacing '%s'.\n", Wave_Driver_Name,
		   dlldriver ? dlldriver : "(null)");
	Wave_Driver_Enable = TRUE;
	return TRUE;
}

void WaveAskVars(char ***listidentifier, LONG **ptrvars)
{
	*listidentifier = WaveIdentList;
	*ptrvars = &CfgWaveRate;
}

ULONG InitWave(void)
{
	soundInit();   /* ARM9 iface to the ARM7 sound server (ds7_maine) */
	soundPowerOn();
	soundSetMixerConfig(SoundOutSrc_Mixer, SoundOutSrc_Mixer, false, false);

	snd_ready = 1;
	apply_master();

	threadPrepare(&wthread, wave_thread_main, NULL,
				  &wstack[sizeof(wstack)], MAIN_THREAD_PRIO - 1);
	threadStart(&wthread);

	PORT_MusicInit(); /* streaming CD-music player, nds_music.c (ch 14/15) */

	printf("[wave] calico sound up: 16 hw ch (14/15=music), pool %dKB\n",
		   POOL_BUDGET / 1024);
	return TRUE;
}

/* Shared with nds_music.c: the music player runs soundPreparePcm/soundStop
 * on channels 14/15 from its own thread; those calico calls must serialize
 * against the SFX/voice ones here (wmutex also serializes the PXI). Steady-
 * state refill (buffer writes + DC flush) needs NO calico call, so the music
 * thread only takes this lock at track start/stop/volume. */
void PORT_SndLock(void) { mutexLock(&wmutex); }
void PORT_SndUnlock(void) { mutexUnlock(&wmutex); }
int PORT_SndReady(void) { return snd_ready; }

void ClearWave(void)
{
	if (!snd_ready)
		return;
	mutexLock(&wmutex);
	soundStop(0xFFFF);
	memset(wch, 0, sizeof(wch));
	mutexUnlock(&wmutex);
}

ULONG WavePlay(UWORD Handle, UWORD Pitchbend, UWORD Repeat, UBYTE Follow,
			   UWORD VolLeft, UWORD VolRight, void *Buffer)
{
	SND_VocInfo voc;
	ULONG pitched, lh, words, play_samples;
	unsigned timer, vol, pan;
	int n, poolidx = -1;
	u64 now, dur;
	SoundMode mode;
	const UBYTE *data;
	UBYTE loop;

	if (!snd_ready)
		return 0;
	if (!SND_ParseVoc((const UBYTE *)Buffer, &voc))
		return 0;

	pitched = SND_PitchedRate(voc.rate, Pitchbend);
	if (!pitched || !voc.length)
		return 0;

	if (Follow)
		printf("[wave] WARNING: Follow=%u not implemented\n", Follow);

	timer = soundTimerFromHz(pitched);
	if (timer > 0xFFFF)
		return 0; /* < 256 Hz: cannot happen with LBA data */

	mutexLock(&wmutex);
	wave_expire();

	for (n = 0; n < NDS_SFX_CHANNELS; n++)
		if (!wch[n].active)
			break;
	if (n == NDS_SFX_CHANNELS) /* DOS semantics: no steal, drop */
	{
		mutexUnlock(&wmutex);
		printf("[wave] play #%u DROPPED (no free channel)\n", Handle);
		return 0;
	}

	if (Handle == SPEAK_HANDLE)
	{
		/* voice: direct from BufSpeak (see header comment) */
		voice_prepare(&voc);
		words = voc.length >> 2; /* truncate: never play tail garbage */
		if (!words)
		{
			mutexUnlock(&wmutex);
			return 0;
		}
		data = voc.data;
	}
	else
	{
		poolidx = pool_get(Handle, &voc);
		if (poolidx < 0)
		{
			mutexUnlock(&wmutex);
			printf("[wave] play #%u DROPPED (pool full/oversize %lu)\n",
				   Handle, (unsigned long)voc.length);
			return 0;
		}
		data = pool[poolidx].data;
		words = pool[poolidx].bytes >> 2; /* padded with silence */
	}
	play_samples = words << 2;

	now = tickGetCount();
	/* ticks = samples * hwtimer / 32 (TICK_FREQ = SOUND_CLOCK/32): exact */
	dur = ((u64)play_samples * timer) >> 5;

	if (Repeat == 1)
	{
		mode = SoundMode_OneShot; /* hw stops the channel by itself */
		loop = 0;
		wch[n].end_tick = now + dur + END_MARGIN_TICKS;
	}
	else if (Repeat == 0) /* DOS underflow: 65536 plays = infinite */
	{
		mode = SoundMode_Repeat;
		loop = 1;
		wch[n].end_tick = 0; /* until stopped */
	}
	else /* rare finite loop count: watchdog thread stops it on time */
	{
		mode = SoundMode_Repeat;
		loop = 1;
		wch[n].end_tick = now + dur * Repeat + END_MARGIN_TICKS;
	}

	volpan(VolLeft, VolRight, &vol, &pan);

	if (++wseq == 0)
		wseq = 1;
	lh = (wseq << 16) | Handle;

	wch[n].active = 1;
	wch[n].loop = loop;
	wch[n].handle = Handle;
	wch[n].longhandle = lh;
	wch[n].info0 = (ULONG)-1;
	wch[n].pool = (WORD)poolidx;
	wch[n].vol_left = VolLeft;
	wch[n].vol_right = VolRight;

	soundPreparePcm((unsigned)n | SOUND_START, vol, pan, timer,
					mode, SoundFmt_Pcm8, data, 0, words);

	mutexUnlock(&wmutex);

	printf("[wave] #%u ch%d %luHz rep=%u v=%u/%u len=%lu\n",
		   Handle, n, (unsigned long)pitched, Repeat, VolLeft, VolRight,
		   (unsigned long)voc.length);

	return lh;
}

void WaveGiveInfo0(ULONG LongHandle, ULONG Info0)
{
	int n;
	mutexLock(&wmutex);
	for (n = 0; n < NDS_WAVE_CHANNELS; n++)
		if (wch[n].active && wch[n].longhandle == LongHandle)
			wch[n].info0 = Info0;
	mutexUnlock(&wmutex);
}

void WaveStop(void)
{
	if (!snd_ready)
		return;
	mutexLock(&wmutex);
	soundStop(0xFFFF);
	memset(wch, 0, sizeof(wch));
	wpause = 0;
	apply_master();
	mutexUnlock(&wmutex);
}

void WaveStopOne(UWORD Handle)
{
	u32 mask = 0;
	int n;

	if (!snd_ready)
		return;
	mutexLock(&wmutex);
	for (n = 0; n < NDS_WAVE_CHANNELS; n++)
		if (wch[n].active && wch[n].handle == Handle)
		{
			mask |= 1u << n;
			wch[n].active = 0;
		}
	if (mask)
		soundStop(mask);
	mutexUnlock(&wmutex);
}

void WaveStopOneLong(ULONG LongHandle)
{
	u32 mask = 0;
	int n;

	if (!snd_ready)
		return;
	mutexLock(&wmutex);
	for (n = 0; n < NDS_WAVE_CHANNELS; n++)
		if (wch[n].active && wch[n].longhandle == LongHandle)
		{
			mask |= 1u << n;
			wch[n].active = 0;
		}
	if (mask)
		soundStop(mask);
	mutexUnlock(&wmutex);
}

int WaveInList(UWORD handle)
{
	int n, found = 0;

	if (!snd_ready)
		return 0;
	mutexLock(&wmutex);
	wave_expire();
	for (n = 0; n < NDS_WAVE_CHANNELS; n++)
		if (wch[n].active && wch[n].handle == handle)
		{
			found = 1;
			break;
		}
	mutexUnlock(&wmutex);
	return found;
}

/* snapshot entries: LIB_WAVE.H T_WAVE {LongHandle, Info0, Position} */
typedef struct
{
	ULONG LongHandle;
	ULONG Info0;
	ULONG Position;
} NDS_T_WAVE;

static NDS_T_WAVE wave_snap[NDS_WAVE_CHANNELS];

int WaveGetSnap(void **Buffer)
{
	int n, count = 0;

	mutexLock(&wmutex);
	wave_expire();
	for (n = 0; n < NDS_WAVE_CHANNELS; n++)
		if (wch[n].active)
		{
			wave_snap[count].LongHandle = wch[n].longhandle;
			wave_snap[count].Info0 = wch[n].info0;
			wave_snap[count].Position = 0;
			count++;
		}
	mutexUnlock(&wmutex);
	*Buffer = wave_snap;
	return count;
}

/* global pause (menus): mute the hw mixer, channels keep running — the
 * tick-based end estimates keep counting during the pause too, so the
 * bookkeeping stays consistent with the hardware (per PORTING_NOTES:
 * "basta silenzio", no positional resume needed) */
int WavePause(void)
{
	mutexLock(&wmutex);
	wpause = 1;
	apply_master();
	mutexUnlock(&wmutex);
	return 1; /* TRUE = paused (engine spins "while (!WavePause());") */
}

void WaveContinue(void)
{
	mutexLock(&wmutex);
	wpause = 0;
	apply_master();
	mutexUnlock(&wmutex);
}

/* Save/Restore: not used by LBA1 (verified: no engine caller). Stop all
 * on save; restoring mid-sample positions is not possible on hw channels
 * without keeping copies, so restore just logs. */
void WaveSaveState(void)
{
	printf("[wave] WaveSaveState (unused by game): stopping all\n");
	WaveStop();
}

void WaveRestoreState(void)
{
	printf("[wave] WaveRestoreState: resume not supported on DS\n");
}

void WaveChangeVolume(ULONG longhandle, ULONG VolGauche, ULONG VolDroit)
{
	int n;
	unsigned vol, pan;

	if (!snd_ready)
		return;
	mutexLock(&wmutex);
	for (n = 0; n < NDS_WAVE_CHANNELS; n++)
		if (wch[n].active && wch[n].longhandle == longhandle)
		{
			wch[n].vol_left = (UWORD)VolGauche;
			wch[n].vol_right = (UWORD)VolDroit;
			volpan((UWORD)VolGauche, (UWORD)VolDroit, &vol, &pan);
			soundChSetVolume((unsigned)n, vol);
			soundChSetPan((unsigned)n, pan);
		}
	mutexUnlock(&wmutex);
}

/* HQ_RESS.C compacts its sample heap under playing samples. On the DS the
 * hardware plays from the pool/BufSpeak copies, never from the engine
 * heap, so no channel pointer rebase is needed: plain memmove. */
void WaveMove(void *DestAddr, void *SrcAddr, ULONG Size)
{
	if (!DestAddr || !SrcAddr || !Size)
		return;
	memmove(DestAddr, SrcAddr, Size);
}

void *WaveGetAddr(void)
{
	return NULL; /* "current playing data" pointer: no engine caller */
}

/* ================================================================== */
/* Mixer — LIB_MIX/LIB_MIX.H (M_SB16.DLL volume registers)             */
/* ================================================================== */
/* LBA.CFG keeps "MixerDriver: NoMixer" (MixerInitDLL is real engine
 * code needing a Watcom DLL image); the engine still calls
 * MixerGetVolume/MixerChangeVolume directly (PERSO.C ReadVolumeSettings,
 * GAMEMENU volume menu). Wave*Master land on the hw mixer master volume. */

LONG Mixer_Driver_Enable = 0;
void *Mixer_listfcts = NULL;

static char *MixerIdentList[] = {""};
static LONG MixerDummyVars[4];

void MixerAskVars(char ***listidentifier, LONG **ptrvars)
{
	*listidentifier = MixerIdentList;
	*ptrvars = MixerDummyVars;
}

void MixerChangeVolume(LONG VolWave, LONG VolMidi, LONG VolCD,
					   LONG VolLine, LONG VolMaster)
{
	mutexLock(&wmutex);
	if (VolWave != -1)
		MixVol[0] = VolWave & 255;
	if (VolMidi != -1)
		MixVol[1] = VolMidi & 255;
	if (VolCD != -1)
		MixVol[2] = VolCD & 255;
	if (VolLine != -1)
		MixVol[3] = VolLine & 255;
	if (VolMaster != -1)
		MixVol[4] = VolMaster & 255;
	apply_master();
	mutexUnlock(&wmutex);
}

static void GiveVol(LONG *p, LONG v)
{
	if (p)
		*p = v;
}

void MixerGetVolume(LONG *VolWave, LONG *VolMidi, LONG *VolCD,
					LONG *VolLine, LONG *VolMaster)
{
	GiveVol(VolWave, MixVol[0]);
	GiveVol(VolMidi, MixVol[1]);
	GiveVol(VolCD, MixVol[2]);
	GiveVol(VolLine, MixVol[3]);
	GiveVol(VolMaster, MixVol[4]);
}

void MixerGetInfo(LONG *VolWave, LONG *VolMidi, LONG *VolCD,
				  LONG *VolLine, LONG *VolMaster)
{
	/* SB16 mixer: all five sliders available */
	GiveVol(VolWave, 1);
	GiveVol(VolMidi, 1);
	GiveVol(VolCD, 1);
	GiveVol(VolLine, 1);
	GiveVol(VolMaster, 1);
}
