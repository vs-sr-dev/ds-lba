/*---------------------------------------------------------------------------------

	$Id: main.cpp,v 1.13 2008-12-02 20:21:20 dovoto Exp $

	Simple console print demo
	-- dovoto


---------------------------------------------------------------------------------*/
#include <nds.h>

#include <stdio.h>

static volatile int frame = 0;

//---------------------------------------------------------------------------------
// VBlank interrupt handler. This function is executed in IRQ mode - be careful!
//---------------------------------------------------------------------------------
static void Vblank() {
//---------------------------------------------------------------------------------
	frame++;
}

//---------------------------------------------------------------------------------
int main(void) {
//---------------------------------------------------------------------------------
	touchPosition touchXY;

	irqSet(IRQ_VBLANK, Vblank);

	consoleDemoInit();

	iprintf("   LBA1 DS - pipeline OK!\n");
	iprintf("  \x1b[32mdevkitARM + libnds via Docker\n");
	iprintf("  \x1b[32;1mMilestone 1 - 2026-07-12\x1b[39m");

	while(pmMainLoop()) {

		swiWaitForVBlank();
		scanKeys();
		int keys = keysDown();
		if (keys & KEY_START) break;

		touchRead(&touchXY);

		// print at using ansi escape sequence \x1b[line;columnH
		iprintf("\x1b[10;0HFrame = %d",frame);
		iprintf("\x1b[16;0HTouch x = %04X, %04X\n", touchXY.rawx, touchXY.px);
		iprintf("Touch y = %04X, %04X\n", touchXY.rawy, touchXY.py);

	}

	return 0;
}
