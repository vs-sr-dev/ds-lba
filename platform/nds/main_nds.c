/*
 * main_nds.c — Nintendo DS entry point.
 *
 * Bottom screen: libnds debug console (all the engine printf()s land here).
 * Top screen: the game (set up lazily by nds_video.c when the engine calls
 * InitGraphSvga through InitAdelineSystem).
 *
 * The game's original entry point is void main(int, UBYTE*[]) in
 * engine/game/PERSO.C, renamed lba_main for the ports (logged edit #4).
 */
#include <nds.h>
#include <fat.h>
#include <filesystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "port.h"

void lba_main(int argc, UBYTE *argv[]);

int main(int argc, char *argv[])
{
	static UBYTE *fake_argv[2] = {(UBYTE *)"lba1ds.nds", NULL};

	(void)argc;
	(void)argv;

	defaultExceptionHandler(); /* guru meditation instead of white screen */
	{
		/* calico system tick (TM2): not started by crt0 — the profiler
		   (nds_prof.c) uses tickGetCount() as its time base */
		extern void tickInit(void);

		tickInit();
	}
	{
		/* DTCM stack high-water probe: paint the free gap between the end
		   of .dtcm.bss (PORT_FASTBSS data) and the current stack pointer;
		   nds_prof.c reports how much was never touched ("P6 stk<free>").
		   The main thread stack lives at the top of DTCM: every byte given
		   to PORT_FASTBSS is taken away from it (guru at ~1KB left). */
		extern char __dtcm_bss_end[];
		char probe;
		char *lo = __dtcm_bss_end;
		char *hi = &probe - 64;

		while (lo < hi)
			*lo++ = 0x5A;
	}
	consoleDemoInit();         /* sub screen console */
	printf("LBA1 DS - M4 bring-up\n");
	swiWaitForVBlank();
	swiWaitForVBlank();

	/* SD card (DLDI on flashcarts / SD image on melonDS) for savegames:
	   BEFORE nitroFSInit so a failure can never disturb the asset mount.
	   On failure the game still runs, with saving disabled gracefully. */
	{
		extern void PORT_FatInit(void);

		PORT_FatInit();
		PORT_DiagInit(); /* fat:/lba1/lba1ds.log — see nds_sys.c */
	}

	if (!nitroFSInit(NULL))
	{
		printf("nitroFSInit FAILED\n");
		goto idle;
	}
	printf("nitroFS OK\n");

	if (chdir("nitro:/") != 0)
		printf("chdir nitro:/ FAILED\n");

	/* smoke-test hooks: nitro:/autoenter = seconds of auto-Return pulses;
	   nitro:/autowalk = seconds of held UP (walk) once autoenter is done */
	{
		extern volatile int PORT_AutoEnterFrames;
		extern volatile int PORT_AutoWalkFrames;
		FILE *f = fopen("autoenter", "rb");

		if (f)
		{
			char buf[16] = {0};

			fread(buf, 1, 15, f);
			fclose(f);
			PORT_AutoEnterFrames = atoi(buf) * 60;
			printf("autoenter: %d s\n", PORT_AutoEnterFrames / 60);
		}
		f = fopen("autowalk", "rb");
		if (f)
		{
			char buf[16] = {0};

			fread(buf, 1, 15, f);
			fclose(f);
			PORT_AutoWalkFrames = atoi(buf) * 60;
			printf("autowalk: %d s\n", PORT_AutoWalkFrames / 60);
		}
		f = fopen("autopause", "rb"); /* N sec after boot: pulse START */
		if (f)
		{
			extern volatile int PORT_AutoPauseFrames;
			char buf[16] = {0};

			fread(buf, 1, 15, f);
			fclose(f);
			PORT_AutoPauseFrames = atoi(buf) * 60;
			printf("autopause: %d s\n", PORT_AutoPauseFrames / 60);
		}
		f = fopen("autosave", "rb"); /* N sec after boot: scripted pause-menu
		                                save under the name "DS" (nds_sys.c) */
		if (f)
		{
			extern volatile int PORT_AutoSaveFrames;
			char buf[16] = {0};

			fread(buf, 1, 15, f);
			fclose(f);
			PORT_AutoSaveFrames = atoi(buf) * 60;
			printf("autosave: %d s\n", PORT_AutoSaveFrames / 60);
		}
		f = fopen("autoload", "rb"); /* pulse Esc (logo skip) until N sec,
		                                then main menu -> load -> first save */
		if (f)
		{
			extern volatile int PORT_AutoLoadFrames;
			char buf[16] = {0};

			fread(buf, 1, 15, f);
			fclose(f);
			PORT_AutoLoadFrames = atoi(buf) * 60;
			printf("autoload: %d s\n", PORT_AutoLoadFrames / 60);
		}
	}

	PORT_MemReport("boot");

	PORT_UI_Init(); /* touch UI on the sub screen (console stays on BG0) */

	lba_main(1, fake_argv);

	PORT_MemReport("exit");
	printf("\nlba_main returned. START+SELECT to exit\n");

idle:
	while (pmMainLoop())
	{
		swiWaitForVBlank();
		scanKeys();
		if ((keysHeld() & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT))
			break;
	}
	return 0;
}
