/*
 * nds_video.c — Nintendo DS replacement for the DOS SVGA platform layer
 * (same API surface as platform/sdl/sdl_video.c).
 *
 * Video path (proven by milestones/m2-hqrview):
 *   main engine, MODE_5_2D, BG3 rotoscale bitmap 8bpp 512x256 in VRAM bank A.
 *   - SVGA mode (640x480, gameplay + menus of the CD build): Flip and
 *     CopyBlockPhys[Clip] decimate Log 2x on the fly (indexed pixels: sample,
 *     don't average) into a 320x240 image, hardware-scaled to 256x192.
 *   - MCGA mode (320x200: FLA playback / SceZoom): the engine memcpy()s into
 *     Phys itself (game/MCGA.C), so Phys is a real 64000-byte buffer and the
 *     VBlank IRQ pushes it to VRAM every frame, scaled 320x200 -> 256x192.
 *   VRAM is 16-bit-write-only: all blits write u32 pairs, never bytes.
 *
 * Palette: engine hands 0..255 per gun; DOS kept 6 DAC bits. We quantize the
 * same way, then >>3 to BGR555. Hardware palette -> fades need no re-blit.
 */
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"

/* ------------------------------------------------------------------ */
/* engine-visible variables (were in INITSVGA.ASM)                    */
/* ------------------------------------------------------------------ */
UBYTE *Log = NULL;
UBYTE *MemoLog = NULL;
UBYTE *Phys = NULL; /* 64000 bytes: only the MCGA path writes into it */

WORD Screen_X = 640;
WORD Screen_Y = 480;

ULONG TabOffLine[481];

WORD ClipXmin = 0, ClipYmin = 0, ClipXmax = 639, ClipYmax = 479;
static WORD MemoClipXmin = 0, MemoClipYmin = 0, MemoClipXmax = 639, MemoClipYmax = 479;

UBYTE Text_Ink = 15;
UBYTE Text_Paper = (UBYTE)-1;
UBYTE OldVideo = (UBYTE)-1;
WORD SizeCar = 8;

/* referenced by engine but only meaningful with banked VGA */
void *BankChange = NULL;
LONG BankCurrent = -1;
WORD Svga_Card = 0; /* SVGA_VESA */

UBYTE PORT_PalRGB[768];

volatile int PORT_ModeX = 640;
volatile int PORT_ModeY = 480;

static int VideoUp = 0;
static int Bg = -1;
static u16 *BgGfx = NULL;

#define BG_STRIDE 512

/* ------------------------------------------------------------------ */
/* VBlank IRQ: input pump + MCGA present                               */
/* ------------------------------------------------------------------ */
static void PresentMcga(void)
{
	const ULONG *src = (const ULONG *)Phys;
	int y, x;

	if (!src || !BgGfx)
		return;

	for (y = 0; y < 200; y++)
	{
		ULONG *dst = (ULONG *)(BgGfx + y * (BG_STRIDE / 2));

		for (x = 0; x < 320 / 4; x++)
			*dst++ = *src++;
	}
}

static void VBlankISR(void)
{
	PORT_ScanInput(); /* nds_sys.c */
	PORT_UI_VBlank(); /* nds_ui.c: touch UI on the sub screen */

	if (PORT_ModeX == 320)
		PresentMcga();
}

/* ------------------------------------------------------------------ */
/* SVGA present: Log (640-stride) -> VRAM, 2x decimated                */
/* ------------------------------------------------------------------ */
void PROF_PresentBegin(void); /* nds_prof.c */
void PROF_PresentEnd(void);

/* ITCM + ARM32: the decimating pack loop below is the whole present cost */
__attribute__((section(".itcm"), long_call, target("arm")))
static void PresentRect640(int x0, int y0, int x1, int y1)
{
	int ox0, ox1, y;

	if (!Log || !BgGfx)
		return;

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > 639)
		x1 = 639;
	if (y1 > 479)
		y1 = 479;
	if (x0 > x1 || y0 > y1)
		return;

	/* output columns, aligned to u32 (4 out px = 8 source px) */
	ox0 = (x0 >> 1) & ~3;
	ox1 = ((x1 >> 1) | 3);
	if (ox1 > 319)
		ox1 = 319;

	PROF_PresentBegin();
	for (y = y0 & ~1; y <= y1; y += 2)
	{
		/* Log is 4-aligned and ox0*2 is a multiple of 8: u32 reads are safe.
		   2 loads + 1 VRAM store per 4 output px (was 8 byte loads). */
		const ULONG *s = (const ULONG *)(Log + (ULONG)y * 640 + ox0 * 2);
		ULONG *d = (ULONG *)(BgGfx + (y >> 1) * (BG_STRIDE / 2) + (ox0 >> 1));
		int n;

		for (n = (ox1 - ox0 + 1) >> 2; n > 0; n--)
		{
			ULONG w0 = s[0], w1 = s[1];

			s += 2;
			*d++ = (w0 & 0xFF) | ((w0 >> 8) & 0xFF00) |
				   ((w1 & 0xFF) << 16) | ((w1 << 8) & 0xFF000000u);
		}
	}
	PROF_PresentEnd();
}

void PORT_PresentAll(void)
{
	if (PORT_ModeX == 320)
		PresentMcga();
	else
		PresentRect640(0, 0, 639, 479);
}

/* ------------------------------------------------------------------ */
/* mode init (INITSVGA.ASM / INITMODE.C equivalents)                   */
/* ------------------------------------------------------------------ */
static void BuildTabOffLine(void)
{
	int i;
	ULONG off = 0;

	for (i = 0; i < 481; i++, off += (ULONG)Screen_X)
		TabOffLine[i] = (i < Screen_Y) ? off : 0;
}

static void SetScaleFor(int w, int h)
{
	/* affine scale to 256x192 (8.8 fixed), same math as m2-hqrview */
	bgSetScale(Bg, (w << 8) / 256, ((h << 8) + 191) / 192);
	bgUpdate();
}

static void EnsureVideo(void)
{
	if (VideoUp)
		return;

	videoSetMode(MODE_5_2D);
	vramSetBankA(VRAM_A_MAIN_BG);
	Bg = bgInit(3, BgType_Bmp8, BgSize_B8_512x256, 0, 0);
	BgGfx = bgGetGfxPtr(Bg);
	dmaFillHalfWords(0, BgGfx, BG_STRIDE * 256);

	irqSet(IRQ_VBLANK, VBlankISR);
	irqEnable(IRQ_VBLANK);

	VideoUp = 1;
}

void *Malloc(LONG); /* engine LIB_SYS */
void Free(void *);

static void InitSvga(void)
{
	EnsureVideo();
	if (!Phys)
		Phys = NDS_calloc(1, 64000);
	PORT_ModeX = 640;
	PORT_ModeY = 480;
	SetScaleFor(320, 240);
	BankCurrent = -1;
	BuildTabOffLine();
}

void InitMcgaMode(void)
{
	EnsureVideo();
	if (!Phys)
		Phys = NDS_calloc(1, 64000);
	memset(Phys, 0, 64000);
	dmaFillHalfWords(0, BgGfx, BG_STRIDE * 256);
	PORT_ModeX = 320;
	PORT_ModeY = 200;
	SetScaleFor(320, 200);
}

void SimpleInitSvga(void)
{
	if (Phys)
		memset(Phys, 0, 64000);
	if (BgGfx)
		dmaFillHalfWords(0, BgGfx, BG_STRIDE * 256);
	PORT_ModeX = 640;
	PORT_ModeY = 480;
	SetScaleFor(320, 240);
	BankCurrent = -1;
}

void InitGraphSvga(void)
{
	InitSvga();
	Log = Malloc(640L * 480L);
	MemoLog = Log;
}

void ClearGraphSvga(void)
{
	Free(MemoLog);
}

void InitGraphMcga(void)
{
	InitMcgaMode();
	Log = Malloc(320L * 200L);
	MemoLog = Log;
}

void ClearGraphMcga(void)
{
	Free(MemoLog);
}

/* S_DLL.C: no SVGA driver DLLs here */
LONG SvgaInitDLL(char *driverpathname)
{
	printf("NDS shim: SVGA driver '%s' ignored\n",
		   driverpathname ? driverpathname : "(null)");
	return 1;
}

/* ------------------------------------------------------------------ */
/* clip (INITSVGA.ASM)                                                 */
/* ------------------------------------------------------------------ */
void SetClip(LONG x0, LONG y0, LONG x1, LONG y1)
{
	ClipXmin = (WORD)((x0 < 0) ? 0 : x0);
	ClipYmin = (WORD)((y0 < 0) ? 0 : y0);
	ClipXmax = (WORD)((x1 >= Screen_X) ? Screen_X - 1 : x1);
	ClipYmax = (WORD)((y1 >= Screen_Y) ? Screen_Y - 1 : y1);
}

void UnSetClip(void)
{
	ClipXmin = 0;
	ClipYmin = 0;
	ClipXmax = Screen_X - 1;
	ClipYmax = Screen_Y - 1;
}

void MemoClip(void)
{
	MemoClipXmin = ClipXmin;
	MemoClipYmin = ClipYmin;
	MemoClipXmax = ClipXmax;
	MemoClipYmax = ClipYmax;
}

void RestoreClip(void)
{
	ClipXmin = MemoClipXmin;
	ClipYmin = MemoClipYmin;
	ClipXmax = MemoClipXmax;
	ClipYmax = MemoClipYmax;
}

/* ------------------------------------------------------------------ */
/* palette (S_PAL.ASM) — input 0..255 per gun, DAC keeps 6 bits        */
/* ------------------------------------------------------------------ */
static UBYTE Dac6(UBYTE v)
{
	UBYTE v6 = v >> 2;

	return (UBYTE)((v6 << 2) | (v6 >> 4));
}

static void ApplyPalRange(int start, int count)
{
	const UBYTE *p = PORT_PalRGB + start * 3;
	int i;

	for (i = start; i < start + count; i++, p += 3)
		BG_PALETTE[i] = RGB15(p[0] >> 3, p[1] >> 3, p[2] >> 3);
}

void Palette(void *pal)
{
	const UBYTE *src = pal;
	int i;

	for (i = 0; i < 768; i++)
		PORT_PalRGB[i] = Dac6(src[i]);
	ApplyPalRange(0, 256);
}

void PalMulti(WORD startcoul, WORD nbcoul, UBYTE *pal)
{
	int i;
	UBYTE *dst = PORT_PalRGB + startcoul * 3;

	for (i = 0; i < nbcoul * 3; i++)
		dst[i] = Dac6(pal[i]);
	ApplyPalRange(startcoul, nbcoul);
}

void PalOne(UBYTE coul, UBYTE r, UBYTE v, UBYTE b)
{
	UBYTE *dst = PORT_PalRGB + coul * 3;

	dst[0] = Dac6(r);
	dst[1] = Dac6(v);
	dst[2] = Dac6(b);
	ApplyPalRange(coul, 1);
}

/* ------------------------------------------------------------------ */
/* Log -> Phys/VRAM (S_PHYS.ASM)                                       */
/* ------------------------------------------------------------------ */
/* The engine calls these from the main thread on every redraw path, in
 * gameplay and in the menus alike, so they are where the touch-UI mailbox
 * that needs main-thread context gets drained (PORT_PumpLang re-enters
 * InitDial: HQR loads and card I/O, illegal from the VBlank ISR). */
void Vsync(void)
{
	PORT_PumpLang();
	swiWaitForVBlank();
}

void Flip(void)
{
	PORT_PumpLang();
	if (PORT_ModeX == 320)
	{
		/* MCGA: DOS Phys was live VGA; the VBlank IRQ presents it */
		if (Log && Phys)
			memcpy(Phys, Log, 64000);
		return;
	}
	PresentRect640(0, 0, 639, 479);
}

void CopyBlockPhys(LONG x0, LONG y0, LONG x1, LONG y1)
{
	PORT_PumpLang();
	if (PORT_ModeX == 320)
		return; /* engine uses Mcga_Flip/CopyBlockPhysMCGA in MCGA mode */
	PresentRect640(x0, y0, x1, y1);
}

void CopyBlockPhysClip(LONG x0, LONG y0, LONG x1, LONG y1)
{
	if (x0 < ClipXmin)
		x0 = ClipXmin;
	if (y0 < ClipYmin)
		y0 = ClipYmin;
	if (x1 > ClipXmax)
		x1 = ClipXmax;
	if (y1 > ClipYmax)
		y1 = ClipYmax;
	CopyBlockPhys(x0, y0, x1, y1);
}
