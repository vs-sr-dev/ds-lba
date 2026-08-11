/*
 * sdl_video.c — SDL2 replacement for the DOS SVGA platform layer
 * (LIB386/PLATFORM/DOS/LIB_SVGA: INITSVGA.ASM, INITMODE.C, S_MODE.ASM,
 *  S_PAL.ASM, S_PHYS.ASM, S_DLL.C, S_MOUSE.C).
 *
 * Model kept identical to DOS:
 *   Log  = logical 8bpp framebuffer the engine draws into (640x480 or 320x200)
 *   Phys = "VGA memory": here a malloc'd 8bpp buffer; a dedicated video
 *          thread converts Phys + palette to the SDL window ~60 times/s and
 *          pumps events (the engine busy-waits on Key/Joy/Fire/TimerRef, so
 *          input and presentation must advance asynchronously — on the DS
 *          this role is played by the VBlank IRQ).
 *
 * Palette values arrive as 0..255 per gun; the DOS code shifted them >>2
 * into the 6-bit DAC. We keep the top 6 bits to reproduce DAC precision.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"

/* ------------------------------------------------------------------ */
/* engine-visible variables (were in INITSVGA.ASM)                    */
/* ------------------------------------------------------------------ */
UBYTE *Log = NULL;
UBYTE *MemoLog = NULL;
UBYTE *Phys = NULL;

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

UBYTE PORT_PalRGB[768]; /* current palette, 8-bit per gun (DAC-quantized) */
int PORT_PalDirty = 1;

/* current physical mode (Phys layout); Log/Screen_X stay 640x480 in the
 * SVGA build, mode 13h is only used during FLA playback */
static volatile int ModeX = 640;
static volatile int ModeY = 480;

/* ------------------------------------------------------------------ */
/* video thread                                                        */
/* ------------------------------------------------------------------ */
static SDL_Thread *VidThread = NULL;
static SDL_atomic_t VidRun;
static SDL_atomic_t VidReady;

extern void PORT_HandleKeyEvent(const SDL_Event *ev); /* sdl_sys.c */
extern void PORT_TimerCatchUp(void);                  /* sdl_sys.c */
extern void PORT_AutoInput(void);                     /* sdl_sys.c */

/* ------------------------------------------------------------------ */
/* screenshots: F12 saves shot_NNN.bmp in the cwd; LBA_SHOT_MS=<ms>    */
/* additionally auto-saves one every <ms> milliseconds (smoke tests).  */
/* ------------------------------------------------------------------ */
static void SaveShotBMP(const Uint32 *argb, int w, int h)
{
	static int shotNum = 0;
	char name[64];
	FILE *f;
	UBYTE hdr[54];
	int y, x;
	ULONG imgsize = (ULONG)w * h * 3;
	ULONG filesize = 54 + imgsize;

	sprintf(name, "shot_%03d.bmp", shotNum++);
	f = fopen(name, "wb");
	if (!f)
		return;

	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 'B'; hdr[1] = 'M';
	hdr[2] = (UBYTE)filesize; hdr[3] = (UBYTE)(filesize >> 8);
	hdr[4] = (UBYTE)(filesize >> 16); hdr[5] = (UBYTE)(filesize >> 24);
	hdr[10] = 54;
	hdr[14] = 40;
	hdr[18] = (UBYTE)w; hdr[19] = (UBYTE)(w >> 8);
	hdr[22] = (UBYTE)h; hdr[23] = (UBYTE)(h >> 8);
	hdr[26] = 1;            /* planes */
	hdr[28] = 24;           /* bpp    */
	hdr[34] = (UBYTE)imgsize; hdr[35] = (UBYTE)(imgsize >> 8);
	hdr[36] = (UBYTE)(imgsize >> 16); hdr[37] = (UBYTE)(imgsize >> 24);
	fwrite(hdr, 1, 54, f);

	for (y = h - 1; y >= 0; y--)        /* bottom-up, rows are 4n (w=640/320) */
	{
		const Uint32 *row = argb + (size_t)y * w;
		static UBYTE line[640 * 3];
		UBYTE *d = line;

		for (x = 0; x < w; x++)
		{
			Uint32 p = row[x];
			*d++ = (UBYTE)p;            /* B */
			*d++ = (UBYTE)(p >> 8);     /* G */
			*d++ = (UBYTE)(p >> 16);    /* R */
		}
		fwrite(line, 1, (size_t)w * 3, f);
	}
	fclose(f);
	printf("SDL shim: saved %s\n", name);
}

static int VideoThreadMain(void *arg)
{
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex640, *tex320, *tex;
	static Uint32 argb[640 * 480];
	int x, y, w, h;

	win = SDL_CreateWindow("LBA1 SDL shim",
						   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
						   640, 480, 0);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
	if (!ren)
		ren = SDL_CreateRenderer(win, -1, 0);
	tex640 = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
							   SDL_TEXTUREACCESS_STREAMING, 640, 480);
	tex320 = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
							   SDL_TEXTUREACCESS_STREAMING, 320, 200);

	SDL_AtomicSet(&VidReady, 1);

	while (SDL_AtomicGet(&VidRun))
	{
		SDL_Event ev;

		while (SDL_PollEvent(&ev))
		{
			if (ev.type == SDL_QUIT)
			{
				/* hard exit like closing a DOS box */
				exit(0);
			}
			PORT_HandleKeyEvent(&ev);
		}

		PORT_TimerCatchUp();
		PORT_AutoInput();

		w = ModeX;
		h = ModeY;
		tex = (w == 320) ? tex320 : tex640;

		if (Phys)
		{
			Uint32 pal32[256];
			const UBYTE *p = PORT_PalRGB;
			const UBYTE *src = Phys;
			Uint32 *dst = argb;

			for (x = 0; x < 256; x++, p += 3)
				pal32[x] = 0xFF000000u | (p[0] << 16) | (p[1] << 8) | p[2];

			for (y = 0; y < w * h; y++)
				*dst++ = pal32[*src++];

			SDL_UpdateTexture(tex, NULL, argb, w * 4);

			/* screenshot triggers */
			{
				static int shotEvery = -2;
				static Uint32 lastShot = 0;
				static int f12Prev = 0;
				const Uint8 *ks = SDL_GetKeyboardState(NULL);
				Uint32 now = SDL_GetTicks();
				int doShot = 0;

				if (shotEvery == -2)
				{
					const char *e = getenv("LBA_SHOT_MS");
					shotEvery = e ? atoi(e) : 0;
				}
				if (ks[SDL_SCANCODE_F12] && !f12Prev)
					doShot = 1;
				f12Prev = ks[SDL_SCANCODE_F12];
				if (shotEvery > 0 && now - lastShot >= (Uint32)shotEvery)
					doShot = 1;
				if (doShot)
				{
					lastShot = now;
					SaveShotBMP(argb, w, h);
				}
			}
		}

		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, tex, NULL, NULL);
		SDL_RenderPresent(ren);

		SDL_Delay(10);
	}

	SDL_DestroyTexture(tex320);
	SDL_DestroyTexture(tex640);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	return 0;
}

static void EnsureVideoThread(void)
{
	if (VidThread)
		return;
	SDL_AtomicSet(&VidRun, 1);
	SDL_AtomicSet(&VidReady, 0);
	VidThread = SDL_CreateThread(VideoThreadMain, "lba-video", NULL);
	while (!SDL_AtomicGet(&VidReady))
		SDL_Delay(1);
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

static void InitSvga(void)
{
	ModeX = 640;
	ModeY = 480;
	if (!Phys)
		Phys = calloc(1, 640 * 480);
	BuildTabOffLine();
	BankCurrent = -1;
	EnsureVideoThread();
}

void InitMcgaMode(void) /* TODO: real mode 13h switch — here: present Phys as 320x200 */
{
	if (!Phys)
		Phys = calloc(1, 640 * 480);
	memset(Phys, 0, 640 * 480);
	ModeX = 320;
	ModeY = 200;
	EnsureVideoThread();
}

void SimpleInitSvga(void)
{
	memset(Phys, 0, 640 * 480);
	ModeX = 640;
	ModeY = 480;
	BankCurrent = -1;
}

static void ClearVideo(void)
{
	/* DOS: int 10h restore old mode. Keep window (game exits right after). */
}

/* INITMODE.C (DOS platform) equivalents */
void *Malloc(LONG); /* engine LIB_SYS */
void Free(void *);

void InitGraphSvga(void)
{
	InitSvga();
	Log = Malloc(640L * 480L);
	MemoLog = Log;
}

void ClearGraphSvga(void)
{
	Free(MemoLog);
	ClearVideo();
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
	ClearVideo();
}

/* S_DLL.C: the DOS build loaded a UNIVBE-style SVGA driver DLL. Nothing to
 * load here — report success so LBA.CFG "SvgaDriver" entries keep working. */
LONG SvgaInitDLL(char *driverpathname)
{
	(void)driverpathname;
	printf("SDL shim: SVGA driver '%s' ignored (built-in SDL video)\n",
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

void Palette(void *pal)
{
	const UBYTE *src = pal;
	int i;

	for (i = 0; i < 768; i++)
		PORT_PalRGB[i] = Dac6(src[i]);
	PORT_PalDirty = 1;
}

void PalMulti(WORD startcoul, WORD nbcoul, UBYTE *pal)
{
	int i;
	UBYTE *dst = PORT_PalRGB + startcoul * 3;

	for (i = 0; i < nbcoul * 3; i++)
		dst[i] = Dac6(pal[i]);
	PORT_PalDirty = 1;
}

void PalOne(UBYTE coul, UBYTE r, UBYTE v, UBYTE b)
{
	UBYTE *dst = PORT_PalRGB + coul * 3;

	dst[0] = Dac6(r);
	dst[1] = Dac6(v);
	dst[2] = Dac6(b);
	PORT_PalDirty = 1;
}

/* ------------------------------------------------------------------ */
/* Log -> Phys (S_PHYS.ASM)                                            */
/* ------------------------------------------------------------------ */
void Vsync(void)
{
	SDL_Delay(14); /* DOS: wait vertical retrace (~70 Hz) */
}

void Flip(void)
{
	if (Log && Phys)
		memcpy(Phys, Log, (size_t)Screen_X * Screen_Y);
}

void CopyBlockPhys(LONG x0, LONG y0, LONG x1, LONG y1)
{
	LONG y, w;

	if (!Log || !Phys)
		return;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 >= Screen_X)
		x1 = Screen_X - 1;
	if (y1 >= Screen_Y)
		y1 = Screen_Y - 1;
	w = x1 - x0 + 1;
	if (w <= 0)
		return;

	for (y = y0; y <= y1; y++)
		memcpy(Phys + y * Screen_X + x0, Log + y * Screen_X + x0, w);
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
