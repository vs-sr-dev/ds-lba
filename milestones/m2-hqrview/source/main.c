/* LBA1 DS — Milestone 2: HQR viewer
 * Proves the full asset->screen path: nitroFS -> HQR/LZSS -> palette ->
 * 320x200 8bpp bitmap hardware-scaled to 256x192 (the definitive video path
 * for the engine port). Shows the LBA logo (RESS.HQR 27/28) on the top screen.
 */
#include <nds.h>
#include <fat.h>
#include <filesystem.h>
#include <stdio.h>
#include <stdlib.h>

#include "hqr.h"

#define RESS_LOGO_PCR 27
#define RESS_LOGO_PAL 28

#define LBA_W 320
#define LBA_H 200

/* Display a paletted LBA screen on the main engine.
 * 320x200 (MCGA gameplay) is blitted 1:1; 640x480 (SVGA menu/intro screens
 * of the CD version) is decimated 2x to 320x240 — indexed pixels, so sample,
 * don't average. The affine BG then hardware-scales to 256x192. */
static void showImage(const uint8_t *img, int w, int h, const uint8_t *pal)
{
	int i, x, y;
	int step = (w == 640) ? 2 : 1;
	int ow = w / step, oh = h / step;
	int shift = 0;
	u16 *dst;

	videoSetMode(MODE_5_2D);
	vramSetBankA(VRAM_A_MAIN_BG);
	int bg = bgInit(3, BgType_Bmp8, BgSize_B8_512x256, 0, 0);

	/* VGA palettes store 0..63; detect 6-bit vs 8-bit components */
	for (i = 0; i < 768; i++)
		if (pal[i] >= 64)
			break;
	shift = (i == 768) ? 1 : 3; /* 6bit->5bit or 8bit->5bit */

	for (i = 0; i < 256; i++)
		BG_PALETTE[i] = RGB15(pal[i * 3 + 0] >> shift,
		                      pal[i * 3 + 1] >> shift,
		                      pal[i * 3 + 2] >> shift);

	/* clear the 512x256 bitmap, then blit (16-bit writes only) */
	dst = bgGetGfxPtr(bg);
	dmaFillHalfWords(0, dst, 512 * 256);
	for (y = 0; y < oh; y++) {
		const uint8_t *srow = img + (y * step) * w;
		u16 *drow = dst + y * (512 / 2);
		for (x = 0; x < ow; x += 2)
			drow[x >> 1] = srow[x * step] | (srow[(x + 1) * step] << 8);
	}

	/* affine scale to 256x192 (8.8 fixed) */
	bgSetScale(bg, (ow << 8) / 256, ((oh << 8) + 191) / 192);
	bgUpdate();
}

int main(void)
{
	uint32_t isz = 0, psz = 0;
	uint8_t *img, *pal;

	consoleDemoInit();
	iprintf("LBA1 DS - M2 HQR viewer\n\n");

	if (!nitroFSInit(NULL)) {
		iprintf("nitroFSInit FAILED\n");
		goto idle;
	}
	iprintf("nitroFS OK\n");

	iprintf("RESS.HQR: %d entries\n", HQR_NumEntries("nitro:/RESS.HQR"));

	img = HQR_LoadEntryAlloc("nitro:/RESS.HQR", RESS_LOGO_PCR, &isz);
	pal = HQR_LoadEntryAlloc("nitro:/RESS.HQR", RESS_LOGO_PAL, &psz);
	iprintf("logo: %u bytes\npal:  %u bytes\n", (unsigned)isz, (unsigned)psz);

	if (img && pal && psz >= 768 &&
	    (isz == LBA_W * LBA_H || isz == 640 * 480)) {
		if (isz == 640 * 480)
			showImage(img, 640, 480, pal);
		else
			showImage(img, LBA_W, LBA_H, pal);
		iprintf("\x1b[32m\nLBA logo on top screen!\x1b[39m\n");
	} else {
		iprintf("load FAILED\n");
	}

idle:
	iprintf("\nSTART to exit\n");
	while (pmMainLoop()) {
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & KEY_START)
			break;
	}
	return 0;
}
