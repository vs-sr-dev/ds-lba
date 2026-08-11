/*
 * stubs.c (NDS copy of platform/sdl/stubs.c) — neutral stubs for the DOS
 * driver/DLL layers: CD audio, AIL32 MIDI, Watcom DLL loader.
 * Everything reports "success but silent", with *_Driver_Enable flags
 * left FALSE so the engine takes its no-hardware paths (same as a DOS
 * machine without the hardware).
 * Wave + Mixer are no longer stubbed: real backend in nds_audio.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"

/* an empty identifier list: "while (**ptridentifier)" stops immediately */
static char *EmptyIdentList[] = {""};
static LONG DummyVars[8];

/* ================================================================== */
/* Watcom DLL loader (LIB_SYS/DLL.H, DLLLOAD.C)                        */
/* ================================================================== */
/* TODO: no DLLs on SDL/DS — only reached if LBA.CFG names a driver    */
ULONG DLL_size(void *source, ULONG flags)
{
	(void)source; (void)flags;
	return 0;
}

void *DLL_load(void *source, ULONG flags, void *dll)
{
	(void)source; (void)flags; (void)dll;
	return NULL;
}

LONG FILE_error(void) { return 0; }

LONG FILE_size(BYTE *filename)
{
	(void)filename;
	return 0;
}

void *FILE_read(BYTE *filename, void *dest)
{
	(void)filename; (void)dest;
	return NULL;
}

LONG FILE_write(BYTE *filename, void *buf, ULONG len)
{
	(void)filename; (void)buf; (void)len;
	return 0;
}

LONG FILE_append(BYTE *filename, void *buf, ULONG len)
{
	(void)filename; (void)buf; (void)len;
	return 0;
}

/* ================================================================== */
/* CD-ROM audio (PLATFORM/DOS/LIB_CD/CDROM.ASM)                        */
/* ================================================================== */
/* TODO: translate/replace CDROM.ASM — for now "no CD present": the    */
/* engine then uses the HD (GOG) file layout and skips CD audio.       */
/* DriveCDR = -1 like the DOS driver when no CD is found: MESSAGE.C    */
/* guards SpeakFromCD/TestSpeakFromCD with "if (DriveCDR < 0)", and    */
/* with 0 it would probe drive "A:" for every found-object voice.      */
WORD DriveCDR = -1;
LONG FileCD_Start = 0;
LONG FileCD_Sect = 0;
LONG FileCD_Size = 0;

LONG InitCDR(char *nameid)
{
	(void)nameid;
	return 0; /* FALSE: no CD — PERSO.C falls back to "FLA\" on disk */
}

void ClearCDR(void) {}

/* PlayTrackCDR / StopCDR / GetLengthTrackCDR now live in nds_music.c
 * (streaming CD-music player). CDEnable stays FALSE here at boot and is
 * re-enabled by the music player once SD tracks are found. */

LONG GetFileCDR(char *name)
{
	(void)name;
	return 0; /* not found */
}

LONG ReadLongCDR(LONG start, LONG nbsect, void *buffer)
{
	(void)start; (void)nbsect; (void)buffer;
	return 1; /* non-zero = read error (never reached: no CD) */
}

/* ================================================================== */
/* MIDI / AIL32 (LIB_MIDI/LIB_MIDI.H)                                  */
/* ================================================================== */
/* The playback half of this API (PlayMidi/StopMidi/IsMidiPlaying/the   */
/* fades/VolumeMidi/DoLoopMidi) lives in nds_music.c: jingles stream    */
/* from the SD like the area music. What stays here is the driver       */
/* bring-up the DOS build got from A32MT32.DLL.                         */
/* ADELINE.C never assigns Midi_Driver_Enable — in DOS the DLL exported */
/* it — so this TRUE is what turns the whole XMI path on. It also makes */
/* PERSO.C load HQR_Midi (midi_mi.hqr, since LBA.CFG says MidiType:     */
/* Midi -> MidiFM = 0): PlayMidiFile() dereferences it before calling   */
/* PlayMidi(), so the file must be in the nitroFS even though we play   */
/* the rendered audio and ignore the XMI bytes.                         */
WORD Midi_Driver_Enable = 1;
LONG MaxVolume = 100;

LONG InitMidiDLL(UBYTE *driverpathname)
{
	(void)driverpathname;
	return 1; /* succeed so a CFG naming a driver does not exit(1) */
}

void AskMidiVars(char ***listidentifier, LONG **ptrvars)
{
	*listidentifier = EmptyIdentList;
	*ptrvars = DummyVars;
}

LONG InitMidi(void) { return 1; }
void ClearMidi(void) {}
void InitPathMidiSampleFile(UBYTE *path) { (void)path; }

/* PlayMidi / StopMidi / IsMidiPlaying / FadeMidiDown / FadeMidiUp /
 * WaitFadeMidi / VolumeMidi / DoLoopMidi now live in nds_music.c. */

/* the DOS build drove the 50 Hz tick from the AIL timer when MIDI was
 * active; route to the platform timer so time always advances */
void InitTimer(void);
void InitMidiTimer(void)
{
	InitTimer();
}

/* ================================================================== */
/* Wave + Mixer: REAL drivers now, see nds_audio.c (calico sound API,  */
/* ARM7 hardware channels). Stubs removed in the audio session.        */
/* ================================================================== */
