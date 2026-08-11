/*
 * sdl_audio.c — Wave (sample) driver + Mixer (volume) driver for the SDL shim.
 *
 * Replaces the DOS DLL pair W_SB16.DLL + M_SB16.DLL:
 *   PLATFORM/DOS/LIB_SAMP/WAVE.C + WAVE_A.ASM  (software mixer, SB16 build:
 *   8-bit unsigned mono sources -> 16-bit stereo output)
 *   PLATFORM/DOS/LIB_MIX/MIXER_A.ASM           (hardware volume registers)
 *
 * Structure (kept deliberately in two halves for the DS port):
 *   1) SND_* channel layer — pure state, no SDL. On the DS each SND_Channel
 *      maps 1:1 onto an ARM7 hardware channel (start/stop/rate/vol/pan);
 *      the software mixing loop (SND_MixInto) disappears entirely.
 *   2) SDL2 backend — one output device, pull callback, software mix.
 *
 * Semantics reverse-engineered from WAVE_A.ASM (documented in
 * PORTING_NOTES.md, section "Audio"):
 *  - Buffer is a Creative VOC file: u16 header size at +0x14 (26 in all LBA
 *    data), then block type 1 (sound data): u24 size, u8 sr, u8 pack(=0),
 *    then unsigned 8-bit mono PCM. rate = 1000000/(256-sr). data length =
 *    blocksize - 2.
 *  - byte[0] of the buffer is abused by the game as a flag: 'C' (0x43) in
 *    SAMPLES.HQR (plain VOC), 0/1 in VOX voice entries (0 = last part,
 *    1 = a continuation VOC follows -> MESSAGE.C FlagNextVoc). The DOS
 *    driver reuses it: if byte[0]+1 < 10 linear interpolation is enabled
 *    for that channel (so voices are filtered, SFX are not).
 *  - Pitchbend: 4096 = 1.0; played rate = voc_rate * Pitchbend / 4096
 *    (DOS: (rate * (pb<<4)) >> 16 with carry rounding).
 *  - Repeat = number of plays, 0 means 65536 (word underflow) ~ infinite.
 *  - VolLeft/VolRight: 0..128 from the game (voices use 512 = 4x louder);
 *    contribution per channel = s8 * (vol >> 2)  (SHIFT_SAMPLE=3 preshift,
 *    8 full-volume channels saturate 16-bit exactly).
 *  - WaveStopOne(handle) stops ALL channels with that 16-bit handle.
 *  - WaveMove: memmove + rebase the data pointer of any playing channel
 *    whose sample start is >= source (HQ_RESS.C compacts the sample heap
 *    while samples are playing).
 *  - Follow/"son" chaining exists in the DOS driver but the game always
 *    passes Follow=0; not implemented (logged if ever seen).
 *
 * Debug helpers:
 *  - every WavePlay is logged to stdout ("[wave] ...");
 *  - env LBA_WAVDUMP=<file.wav> dumps the first ~20 s of the final mix.
 */

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"
#include "../audio_common.h" /* SND_VocInfo/SND_ParseVoc/SND_PitchedRate,
                                shared with platform/nds/nds_audio.c */

/* ================================================================== */
/* 1) Channel layer (backend-agnostic — this is what the DS reuses)   */
/* ================================================================== */

/* The DOS list had 50 slots but only 8 full-volume channels fit in the
 * 16-bit accumulator; the DS ARM7 has 16 hardware channels. Use 16. */
#define SND_MAX_CHANNELS 16

typedef struct
{
	int active;
	const UBYTE *data;  /* unsigned 8-bit mono PCM (VOC body)      */
	ULONG length;       /* in samples                              */
	/* Sample cursor = integer index + fractional accumulator, NOT a
	 * single 32-bit 16.16 value: voice narrations are LONGER than 64K
	 * samples (e.g. 206681) and a 16.16 cursor wraps at index 65536,
	 * silently restarting the sample from 0 in an endless loop (bug
	 * seen on the intro narration; WaveInList then never reports the
	 * voice as finished, so Dial() never ends either). DOS kept a
	 * byte pointer + separate 16-bit FRACT word: no wrap. */
	ULONG idx;          /* integer sample index                    */
	ULONG frac;         /* fractional accumulator, low 16 bits used*/
	ULONG step;         /* 16.16 fixed-point increment             */
	ULONG freq;         /* pitched rate in Hz (log / DS hw regs)   */
	ULONG plays_left;   /* Repeat: 0 on entry means 65536          */
	UWORD handle;       /* engine handle (HQR index, 0x1234 voice) */
	ULONG longhandle;   /* (seq<<16)|handle, unique, never 0       */
	ULONG info0;
	WORD vol_left;      /* engine units (0..128 sfx, 512 voice)    */
	WORD vol_right;
	UBYTE interpol;     /* linear interpolation flag (voices)      */
} SND_Channel;

static SND_Channel snd_chan[SND_MAX_CHANNELS];
static SND_Channel snd_back[SND_MAX_CHANNELS]; /* WaveSaveState */
static int snd_back_pause;
static int snd_pause;
static ULONG snd_seq;

/* backend state (PC: SDL device; DS: nothing, hw regs are the state) */
static SDL_AudioDeviceID snd_dev; /* 0 = silent mode (no device)     */
static LONG snd_outfreq = 22000;  /* actual output rate              */
static LONG snd_gain = 65536;     /* 16.16 wave*master gain          */

static void snd_lock(void)
{
	if (snd_dev)
		SDL_LockAudioDevice(snd_dev);
}

static void snd_unlock(void)
{
	if (snd_dev)
		SDL_UnlockAudioDevice(snd_dev);
}

static SND_Channel *snd_find_free(void)
{
	int n;
	for (n = 0; n < SND_MAX_CHANNELS; n++)
		if (!snd_chan[n].active)
			return &snd_chan[n];
	return NULL;
}

/* ================================================================== */
/* Software mixer (PC only — on the DS the ARM7 hardware does this)   */
/* ================================================================== */

#define SND_MAX_FRAMES 4096
static int snd_acc[SND_MAX_FRAMES * 2]; /* int32 stereo accumulator */

/* WAV dump of the final mix (env LBA_WAVDUMP=<file>) */
static FILE *wav_fp;
static ULONG wav_frames, wav_max_frames;

static void wav_finalize(void)
{
	ULONG datalen;
	UBYTE hdr[44];

	if (!wav_fp)
		return;

	datalen = wav_frames * 4;
	memcpy(hdr, "RIFF", 4);
	*(ULONG *)(hdr + 4) = 36 + datalen;
	memcpy(hdr + 8, "WAVEfmt ", 8);
	*(ULONG *)(hdr + 16) = 16;
	*(UWORD *)(hdr + 20) = 1; /* PCM */
	*(UWORD *)(hdr + 22) = 2; /* stereo */
	*(ULONG *)(hdr + 24) = (ULONG)snd_outfreq;
	*(ULONG *)(hdr + 28) = (ULONG)snd_outfreq * 4;
	*(UWORD *)(hdr + 32) = 4;  /* block align */
	*(UWORD *)(hdr + 34) = 16; /* bits */
	memcpy(hdr + 36, "data", 4);
	*(ULONG *)(hdr + 40) = datalen;

	fseek(wav_fp, 0, SEEK_SET);
	fwrite(hdr, 1, 44, wav_fp);
	fclose(wav_fp);
	wav_fp = NULL;
	printf("[wave] WAV dump closed (%lu frames)\n", (unsigned long)wav_frames);
}

/* mix `frames` stereo frames into out (interleaved S16) */
static void SND_MixInto(WORD *out, int frames)
{
	int n, i;

	memset(snd_acc, 0, frames * 2 * sizeof(int));

	if (!snd_pause)
	{
		for (n = 0; n < SND_MAX_CHANNELS; n++)
		{
			SND_Channel *ch = &snd_chan[n];
			int vl, vr;

			if (!ch->active)
				continue;

			/* DOS SB16 preshift: vol >> (SHIFT_SAMPLE-1) */
			vl = ch->vol_left >> 2;
			vr = ch->vol_right >> 2;

			for (i = 0; i < frames && ch->active; i++)
			{
				int s;

				if (ch->idx >= ch->length)
				{
					/* end of sample: repeat or die (DOS resets FRACT=0) */
					if (--ch->plays_left == 0)
					{
						ch->active = 0;
						printf("[wave] end  #%u (h=%04X)\n",
							   ch->handle, ch->handle);
						break;
					}
					ch->idx = 0;
					ch->frac = 0;
				}

				s = (int)ch->data[ch->idx] - 128;

				if (ch->interpol && ch->idx + 1 < ch->length)
				{
					int s2 = (int)ch->data[ch->idx + 1] - 128;
					s += ((s2 - s) * (int)((ch->frac >> 8) & 0xFF)) >> 8;
				}

				snd_acc[i * 2 + 0] += s * vl;
				snd_acc[i * 2 + 1] += s * vr;

				ch->frac += ch->step;
				ch->idx += ch->frac >> 16;
				ch->frac &= 0xFFFF;
			}
		}
	}

	for (i = 0; i < frames * 2; i++)
	{
		int v = (int)(((long long)snd_acc[i] * snd_gain) >> 16);
		if (v > 32767)
			v = 32767;
		if (v < -32768)
			v = -32768;
		out[i] = (WORD)v;
	}

	if (wav_fp && wav_frames < wav_max_frames)
	{
		ULONG left = wav_max_frames - wav_frames;
		ULONG todo = (ULONG)frames < left ? (ULONG)frames : left;
		fwrite(out, 4, todo, wav_fp);
		wav_frames += todo;
		if (wav_frames >= wav_max_frames)
			wav_finalize();
	}
}

static void SDLCALL snd_callback(void *userdata, Uint8 *stream, int len)
{
	int frames = len / 4;
	(void)userdata;
	if (frames > SND_MAX_FRAMES)
		frames = SND_MAX_FRAMES; /* cannot happen with samples=1024 */
	SND_MixInto((WORD *)stream, frames);
}

/* ================================================================== */
/* VOC parsing: SND_ParseVoc/SND_PitchedRate live in ../audio_common.h */
/* (shared with the DS backend)                                        */
/* ================================================================== */
/* 2) Engine API — LIB_SAMP/LIB_WAVE.H                                 */
/* ================================================================== */

LONG Wave_Driver_Enable = 0;
char Wave_Driver[260];
char Wave_Driver_Name[64] = "SDL2 Wave Driver (SB16 semantics)";

/* vars requested from LBA.CFG through WaveAskVars (same protocol as the
 * DOS DLL: W_SB16 asked WaveBase+WaveRate; we only need WaveRate). */
static LONG CfgWaveRate = 22000;
static char *WaveIdentList[] = {"WaveRate", ""};

LONG WaveInitDLL(char *dlldriver)
{
	/* the CFG names a DOS DLL (e.g. W_SB16.DLL); accept anything */
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
	SDL_AudioSpec want, have;
	const char *dump;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
	{
		printf("Error WaveDriver: SDL audio init failed: %s\n", SDL_GetError());
		/* keep running silently: WavePlay will refuse channels, so
		 * WaveInList never blocks dialogs */
		return TRUE;
	}

	if (CfgWaveRate < 8000 || CfgWaveRate > 48000)
		CfgWaveRate = 22000;

	SDL_zero(want);
	want.freq = CfgWaveRate;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024; /* = DOS half-buffer BUFFER_SIZE, ~46ms @22kHz */
	want.callback = snd_callback;

	snd_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
								  SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (!snd_dev)
	{
		printf("Error WaveDriver: SDL_OpenAudioDevice: %s\n", SDL_GetError());
		return TRUE; /* silent mode, see above */
	}

	snd_outfreq = have.freq;
	printf("[wave] device open: %d Hz, %d samples/buf (WaveRate %ld)\n",
		   have.freq, have.samples, (long)CfgWaveRate);

	dump = getenv("LBA_WAVDUMP");
	if (dump && *dump)
	{
		wav_fp = fopen(dump, "wb");
		if (wav_fp)
		{
			UBYTE zero[44] = {0};
			fwrite(zero, 1, 44, wav_fp); /* placeholder header */
			wav_frames = 0;
			wav_max_frames = (ULONG)snd_outfreq * 20; /* ~20 s */
			atexit(wav_finalize);
			printf("[wave] dumping mix to '%s' (first 20s)\n", dump);
		}
	}

	SDL_PauseAudioDevice(snd_dev, 0);
	return TRUE;
}

void ClearWave(void)
{
	if (snd_dev)
	{
		snd_lock();
		wav_finalize();
		snd_unlock();
		SDL_CloseAudioDevice(snd_dev);
		snd_dev = 0;
	}
	memset(snd_chan, 0, sizeof(snd_chan));
}

ULONG WavePlay(UWORD Handle, UWORD Pitchbend, UWORD Repeat, UBYTE Follow,
			   UWORD VolLeft, UWORD VolRight, void *Buffer)
{
	SND_VocInfo voc;
	SND_Channel *ch;
	ULONG pitched, lh;

	if (!SND_ParseVoc((const UBYTE *)Buffer, &voc))
		return 0;

	pitched = SND_PitchedRate(voc.rate, Pitchbend);
	if (!pitched || !voc.length)
		return 0;

	if (Follow)
		printf("[wave] WARNING: Follow=%u not implemented\n", Follow);

	if (!snd_dev)
		return 0; /* silent mode */

	snd_lock();

	ch = snd_find_free();
	if (!ch)
	{
		snd_unlock();
		printf("[wave] play #%u DROPPED (no free channel)\n", Handle);
		return 0;
	}

	if (++snd_seq == 0)
		snd_seq = 1;
	lh = (snd_seq << 16) | Handle;

	ch->data = voc.data;
	ch->length = voc.length;
	ch->idx = 0;
	ch->frac = 0;
	ch->freq = pitched;
	ch->step = (ULONG)(((unsigned long long)pitched << 16) / snd_outfreq);
	ch->plays_left = Repeat ? Repeat : 0x10000; /* 0 = 65536 (DOS underflow) */
	ch->handle = Handle;
	ch->longhandle = lh;
	ch->info0 = (ULONG)-1;
	ch->vol_left = (WORD)VolLeft;
	ch->vol_right = (WORD)VolRight;
	ch->interpol = voc.interpol;
	ch->active = 1;

	snd_unlock();

	printf("[wave] play #%u (h=%04X) %luHz pb=%u rep=%u vol=%u/%u len=%lu%s\n",
		   Handle, Handle, (unsigned long)pitched, Pitchbend, Repeat,
		   VolLeft, VolRight, (unsigned long)voc.length,
		   voc.interpol ? " interp" : "");

	return lh;
}

void WaveGiveInfo0(ULONG LongHandle, ULONG Info0)
{
	int n;
	snd_lock();
	for (n = 0; n < SND_MAX_CHANNELS; n++)
		if (snd_chan[n].active && snd_chan[n].longhandle == LongHandle)
			snd_chan[n].info0 = Info0;
	snd_unlock();
}

void WaveStop(void)
{
	snd_lock();
	memset(snd_chan, 0, sizeof(snd_chan));
	snd_pause = 0;
	snd_unlock();
}

void WaveStopOne(UWORD Handle)
{
	int n, stopped = 0;
	snd_lock();
	for (n = 0; n < SND_MAX_CHANNELS; n++)
		if (snd_chan[n].active && snd_chan[n].handle == Handle)
		{
			snd_chan[n].active = 0;
			stopped++;
		}
	snd_unlock();
	if (stopped)
		printf("[wave] stop #%u (h=%04X) x%d\n", Handle, Handle, stopped);
}

void WaveStopOneLong(ULONG LongHandle)
{
	int n;
	snd_lock();
	for (n = 0; n < SND_MAX_CHANNELS; n++)
		if (snd_chan[n].active && snd_chan[n].longhandle == LongHandle)
			snd_chan[n].active = 0;
	snd_unlock();
}

int WaveInList(UWORD handle)
{
	int n, found = 0;
	snd_lock();
	for (n = 0; n < SND_MAX_CHANNELS; n++)
		if (snd_chan[n].active && snd_chan[n].handle == handle)
		{
			found = 1;
			break;
		}
	snd_unlock();
	return found;
}

/* snapshot entries: {LongHandle, Info0} pairs (DOS SNAP_SIZE = 8 bytes) */
static ULONG snd_snap[SND_MAX_CHANNELS][2];

int WaveGetSnap(void **Buffer)
{
	int n, count = 0;
	snd_lock();
	for (n = 0; n < SND_MAX_CHANNELS; n++)
		if (snd_chan[n].active)
		{
			snd_snap[count][0] = snd_chan[n].longhandle;
			snd_snap[count][1] = snd_chan[n].info0;
			count++;
		}
	snd_unlock();
	*Buffer = snd_snap;
	return count;
}

int WavePause(void)
{
	snd_lock();
	snd_pause = 1;
	snd_unlock();
	return 1; /* TRUE = paused (engine spins "while (!WavePause());") */
}

void WaveContinue(void)
{
	snd_lock();
	snd_pause = 0;
	snd_unlock();
}

void WaveSaveState(void)
{
	snd_lock();
	memcpy(snd_back, snd_chan, sizeof(snd_chan));
	snd_back_pause = snd_pause;
	memset(snd_chan, 0, sizeof(snd_chan));
	snd_pause = 0;
	snd_unlock();
}

void WaveRestoreState(void)
{
	snd_lock();
	memcpy(snd_chan, snd_back, sizeof(snd_chan));
	snd_pause = snd_back_pause;
	snd_unlock();
}

void WaveChangeVolume(ULONG longhandle, ULONG VolGauche, ULONG VolDroit)
{
	int n;
	snd_lock();
	for (n = 0; n < SND_MAX_CHANNELS; n++)
		if (snd_chan[n].active && snd_chan[n].longhandle == longhandle)
		{
			snd_chan[n].vol_left = (WORD)VolGauche;
			snd_chan[n].vol_right = (WORD)VolDroit;
		}
	snd_unlock();
}

/* HQ_RESS.C compacts the sample heap under playing samples: move the
 * bytes AND rebase every channel whose sample data was moved.
 * DOS ShiftSamples rebased anything with START >= src — safe there
 * because BufSpeak (voice) lived in DOS low memory, always BELOW the
 * samples heap. In this port BufSpeak comes from the same malloc heap
 * and can sit ABOVE SrcAddr: rebase only pointers inside the moved
 * range [src, src+Size) or the voice channel gets silently shifted. */
void WaveMove(void *DestAddr, void *SrcAddr, ULONG Size)
{
	int n;
	const UBYTE *src = (const UBYTE *)SrcAddr;
	long delta = (long)((UBYTE *)DestAddr - (UBYTE *)SrcAddr);

	if (!DestAddr || !SrcAddr || !Size)
		return;

	snd_lock();
	memmove(DestAddr, SrcAddr, Size);
	for (n = 0; n < SND_MAX_CHANNELS; n++)
		if (snd_chan[n].active &&
			snd_chan[n].data >= src && snd_chan[n].data < src + Size)
			snd_chan[n].data += delta;
	snd_unlock();
}

void *WaveGetAddr(void)
{
	int n;
	void *addr = NULL;
	snd_lock();
	for (n = 0; n < SND_MAX_CHANNELS; n++)
		if (snd_chan[n].active)
		{
			addr = (void *)(snd_chan[n].data + snd_chan[n].idx);
			break;
		}
	snd_unlock();
	return addr;
}

/* ================================================================== */
/* Mixer — LIB_MIX/LIB_MIX.H (M_SB16.DLL volume registers)             */
/* ================================================================== */
/* LBA.CFG keeps "MixerDriver: NoMixer" (MixerInitDLL is real engine
 * code that needs a Watcom DLL image), so the engine never loads a
 * mixer DLL; it still calls MixerGetVolume/MixerChangeVolume directly
 * (PERSO.C ReadVolumeSettings, GAMEMENU volume menu). We emulate the
 * SB16 mixer chip: 5 stored volumes, Wave+Master applied to our mix.  */

LONG Mixer_Driver_Enable = 0;
void *Mixer_listfcts = NULL;

static LONG MixVol[5] = {255, 255, 255, 255, 255}; /* wave midi cd line master */

static char *MixerIdentList[] = {""};
static LONG MixerDummyVars[4];

static void snd_update_gain(void)
{
	/* wave * master, 16.16 */
	snd_gain = (LONG)(((long long)MixVol[0] * MixVol[4] * 65536) / (255 * 255));
}

void MixerAskVars(char ***listidentifier, LONG **ptrvars)
{
	*listidentifier = MixerIdentList;
	*ptrvars = MixerDummyVars;
}

void MixerChangeVolume(LONG VolWave, LONG VolMidi, LONG VolCD,
					   LONG VolLine, LONG VolMaster)
{
	snd_lock();
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
	snd_update_gain();
	snd_unlock();
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
