/*
 * sdl_sys.c — SDL2 replacement for the DOS system layer
 * (LIB386/PLATFORM/DOS/LIB_SYS: TIMER.C/TIMER_A.C, KEYBOARD.C, KEYB.C,
 *  SYSTEM.C, TIME.C, FILESYSTEM.C, FILES_A.C, MALLOC.C DOS parts,
 *  and LIB_SVGA/S_MOUSE.C).
 *
 * Timer: the DOS build reprogrammed IRQ0 to 50 Hz and bumped
 * TimerRef/TimerSystem in the handler. Here PORT_TimerCatchUp() (called by
 * the video thread ~100x/s) derives elapsed 50 Hz ticks from SDL_GetTicks.
 *
 * Keyboard: reproduces KEYBOARD.C semantics —
 *   Joy  bits: 1=up 2=down 4=left 8=right          (arrow keys)
 *   Fire bits: 1=space 2=return 4=ctrl 8=alt 16=del 32=shift
 *   FuncKey  : F1..F8 in low byte, F9..F12 in high byte
 *   Key      : PC/XT set-1 scan code of the last "other" key held, 0 on release
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <time.h>
#include <sys/utime.h>
#include "port.h"

/* ------------------------------------------------------------------ */
/* timer (TIMER_A.C variables)                                         */
/* ------------------------------------------------------------------ */
#define TICK_HZ 50

volatile ULONG TimerRef = 0;
volatile ULONG TimerSystem = 0;
UWORD NbFramePerSecond = 0;
UWORD WaitNbTicks = 1;
UWORD CmptFrame = 0;
UWORD Cmpt_18 = 0;

static int TimerOn = 0;
static Uint32 TimerBaseMs = 0;
static ULONG TimerDone = 0; /* ticks already accounted for */

void PORT_TimerCatchUp(void)
{
	ULONG want;

	if (!TimerOn)
		return;

	want = (ULONG)((Uint64)(SDL_GetTicks() - TimerBaseMs) * TICK_HZ / 1000);

	while (TimerDone < want)
	{
		TimerDone++;
		TimerSystem++;
		TimerRef++;

		if (--WaitNbTicks == 0)
		{
			WaitNbTicks = TICK_HZ;
			NbFramePerSecond = CmptFrame;
			CmptFrame = 0;
		}
	}
}

void InitTimer(void)
{
	TimerBaseMs = SDL_GetTicks();
	TimerDone = 0;
	TimerRef = 0;
	TimerSystem = 0;
	WaitNbTicks = 1;
	TimerOn = 1;
}

void ClearTimer(void)
{
	TimerOn = 0;
}

void SetTimer(WORD divisor) /* engine never calls it in this build */
{
	(void)divisor;
}

WORD GetTimer(void)
{
	return (WORD)TimerRef;
}

/* ------------------------------------------------------------------ */
/* system (SYSTEM.C) — DOS installed a critical error handler          */
/* ------------------------------------------------------------------ */
void InitSystem(void) {}
void ClearSystem(void) {}

/* ------------------------------------------------------------------ */
/* keyboard (KEYBOARD.C)                                               */
/* ------------------------------------------------------------------ */
volatile UWORD Key = 0;
volatile UWORD FuncKey = 0;
volatile UWORD Joy = 0;
volatile UWORD Fire = 0;
UWORD AsciiMode = 0;

/* GetAscii ring buffer: AH=scan code, AL=ascii (BIOS int 16h format) */
#define ABUF_LEN 32
static volatile UWORD AsciiBuf[ABUF_LEN];
static volatile int AsciiHead = 0, AsciiTail = 0;

/* SDL_Scancode -> PC/XT set-1 scan code (position-based, like DOS) */
static UBYTE Sdl2Dos(SDL_Scancode sc)
{
	static const UBYTE tab[SDL_NUM_SCANCODES] = {
		[SDL_SCANCODE_ESCAPE] = 1,
		[SDL_SCANCODE_1] = 2, [SDL_SCANCODE_2] = 3, [SDL_SCANCODE_3] = 4,
		[SDL_SCANCODE_4] = 5, [SDL_SCANCODE_5] = 6, [SDL_SCANCODE_6] = 7,
		[SDL_SCANCODE_7] = 8, [SDL_SCANCODE_8] = 9, [SDL_SCANCODE_9] = 10,
		[SDL_SCANCODE_0] = 11, [SDL_SCANCODE_MINUS] = 12,
		[SDL_SCANCODE_EQUALS] = 13, [SDL_SCANCODE_BACKSPACE] = 14,
		[SDL_SCANCODE_TAB] = 15,
		[SDL_SCANCODE_Q] = 16, [SDL_SCANCODE_W] = 17, [SDL_SCANCODE_E] = 18,
		[SDL_SCANCODE_R] = 19, [SDL_SCANCODE_T] = 20, [SDL_SCANCODE_Y] = 21,
		[SDL_SCANCODE_U] = 22, [SDL_SCANCODE_I] = 23, [SDL_SCANCODE_O] = 24,
		[SDL_SCANCODE_P] = 25, [SDL_SCANCODE_LEFTBRACKET] = 26,
		[SDL_SCANCODE_RIGHTBRACKET] = 27, [SDL_SCANCODE_RETURN] = 28,
		[SDL_SCANCODE_LCTRL] = 29,
		[SDL_SCANCODE_A] = 30, [SDL_SCANCODE_S] = 31, [SDL_SCANCODE_D] = 32,
		[SDL_SCANCODE_F] = 33, [SDL_SCANCODE_G] = 34, [SDL_SCANCODE_H] = 35,
		[SDL_SCANCODE_J] = 36, [SDL_SCANCODE_K] = 37, [SDL_SCANCODE_L] = 38,
		[SDL_SCANCODE_SEMICOLON] = 39, [SDL_SCANCODE_APOSTROPHE] = 40,
		[SDL_SCANCODE_GRAVE] = 41, [SDL_SCANCODE_LSHIFT] = 42,
		[SDL_SCANCODE_BACKSLASH] = 43,
		[SDL_SCANCODE_Z] = 44, [SDL_SCANCODE_X] = 45, [SDL_SCANCODE_C] = 46,
		[SDL_SCANCODE_V] = 47, [SDL_SCANCODE_B] = 48, [SDL_SCANCODE_N] = 49,
		[SDL_SCANCODE_M] = 50, [SDL_SCANCODE_COMMA] = 51,
		[SDL_SCANCODE_PERIOD] = 52, [SDL_SCANCODE_SLASH] = 53,
		[SDL_SCANCODE_RSHIFT] = 54, [SDL_SCANCODE_KP_MULTIPLY] = 55,
		[SDL_SCANCODE_LALT] = 56, [SDL_SCANCODE_SPACE] = 57,
		[SDL_SCANCODE_CAPSLOCK] = 58,
		[SDL_SCANCODE_F1] = 59, [SDL_SCANCODE_F2] = 60, [SDL_SCANCODE_F3] = 61,
		[SDL_SCANCODE_F4] = 62, [SDL_SCANCODE_F5] = 63, [SDL_SCANCODE_F6] = 64,
		[SDL_SCANCODE_F7] = 65, [SDL_SCANCODE_F8] = 66, [SDL_SCANCODE_F9] = 67,
		[SDL_SCANCODE_F10] = 68, [SDL_SCANCODE_NUMLOCKCLEAR] = 69,
		[SDL_SCANCODE_SCROLLLOCK] = 70,
		[SDL_SCANCODE_KP_7] = 71, [SDL_SCANCODE_KP_8] = 72,
		[SDL_SCANCODE_KP_9] = 73, [SDL_SCANCODE_KP_MINUS] = 74,
		[SDL_SCANCODE_KP_4] = 75, [SDL_SCANCODE_KP_5] = 76,
		[SDL_SCANCODE_KP_6] = 77, [SDL_SCANCODE_KP_PLUS] = 78,
		[SDL_SCANCODE_KP_1] = 79, [SDL_SCANCODE_KP_2] = 80,
		[SDL_SCANCODE_KP_3] = 81, [SDL_SCANCODE_KP_0] = 82,
		[SDL_SCANCODE_KP_PERIOD] = 83,
		[SDL_SCANCODE_F11] = 87, [SDL_SCANCODE_F12] = 88,
		/* extended keys mapped onto their classic twins */
		[SDL_SCANCODE_UP] = 72, [SDL_SCANCODE_DOWN] = 80,
		[SDL_SCANCODE_LEFT] = 75, [SDL_SCANCODE_RIGHT] = 77,
		[SDL_SCANCODE_HOME] = 71, [SDL_SCANCODE_END] = 79,
		[SDL_SCANCODE_PAGEUP] = 73, [SDL_SCANCODE_PAGEDOWN] = 81,
		[SDL_SCANCODE_INSERT] = 82, [SDL_SCANCODE_DELETE] = 83,
		[SDL_SCANCODE_RCTRL] = 29, [SDL_SCANCODE_RALT] = 56,
		[SDL_SCANCODE_KP_ENTER] = 28,
	};

	if ((unsigned)sc >= SDL_NUM_SCANCODES)
		return 0;
	return tab[sc];
}

/* KEYBOARD.C special-key tables (same order / same bits) */
#define NB_SPECIAL_KEY 27
static const UBYTE TabSpecialKey[NB_SPECIAL_KEY] = {
	72, 80, 75, 77, 71, 73, 81, 79,     /* joy directions */
	57, 28, 29, 56, 83, 42, 54,         /* fire keys */
	59, 60, 61, 62, 63, 64, 65, 66,     /* F1..F8 */
	67, 68, 87, 88};                    /* F9..F12 */
static const UBYTE TabSpecialFunc[NB_SPECIAL_KEY * 2] = {
	0, 1, 0, 2, 0, 4, 0, 8, 0, 1 + 4, 0, 1 + 8, 0, 2 + 8, 0, 2 + 4,
	1, 1, 1, 2, 1, 4, 1, 8, 1, 16, 1, 32, 1, 32,
	2, 1, 2, 2, 2, 4, 2, 8, 2, 16, 2, 32, 2, 64, 2, 128,
	3, 1, 3, 2, 3, 4, 3, 8};

/* mouse (S_MOUSE.C) */
volatile LONG Click = 0;
volatile LONG Mouse_X = 0;
volatile LONG Mouse_Y = 0;
LONG Mouse_X_Dep = 0;
LONG Mouse_Y_Dep = 0;
UBYTE *GphMouse = NULL;
static volatile LONG MouseAccX = 0, MouseAccY = 0;

/* Scripted input for headless smoke tests: LBA_AUTOENTER=<seconds> pulses
 * the Return key (Fire bit 2 + ascii buffer) every ~350ms for the first
 * <seconds> seconds — enough to walk the menus into the game. Called from
 * the video thread. */
void PORT_AutoInput(void)
{
	static int inited = 0;
	static Uint32 t0, lastEdge;
	static int durMs = 0;
	static int state = 0;
	Uint32 now;

	if (!inited)
	{
		const char *e = getenv("LBA_AUTOENTER");
		inited = 1;
		if (e)
			durMs = atoi(e) * 1000;
		t0 = SDL_GetTicks();
		lastEdge = t0;
	}
	if (durMs <= 0)
		return;

	now = SDL_GetTicks();
	if ((int)(now - t0) > durMs)
	{
		if (state)
		{
			Fire &= (UWORD)~2;
			state = 0;
		}
		return;
	}
	if (now - lastEdge < 350)
		return;
	lastEdge = now;
	state = !state;
	if (state)
	{
		int next = (AsciiHead + 1) % ABUF_LEN;

		Fire |= 2;
		if (next != AsciiTail)
		{
			AsciiBuf[AsciiHead] = (UWORD)((28 << 8) | '\r');
			AsciiHead = next;
		}
	}
	else
	{
		Fire &= (UWORD)~2;
	}
}

/* called from the video thread for every SDL event */
void PORT_HandleKeyEvent(const SDL_Event *ev)
{
	UBYTE dos, bl, bh;
	int i, down;

	switch (ev->type)
	{
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		if (ev->key.repeat)
			return;
		down = (ev->type == SDL_KEYDOWN);
		dos = Sdl2Dos(ev->key.keysym.scancode);
		if (!dos)
			return;

		for (i = 0; i < NB_SPECIAL_KEY; i++)
		{
			if (TabSpecialKey[i] != dos)
				continue;
			bl = TabSpecialFunc[i * 2];
			bh = TabSpecialFunc[i * 2 + 1];
			if (bl == 0)
			{
				if (down)
					Joy |= bh;
				else
					Joy &= ~bh;
			}
			else if (bl == 1)
			{
				if (down)
					Fire |= bh;
				else
					Fire &= ~bh;
			}
			else if (bl == 2)
			{
				if (down)
					FuncKey |= bh;
				else
					FuncKey &= (UWORD)~bh;
			}
			else
			{
				if (down)
					FuncKey |= (UWORD)(bh << 8);
				else
					FuncKey &= (UWORD)~(bh << 8);
			}
			goto ascii;
		}

		Key = down ? dos : 0;

	ascii:
		if (down)
		{
			SDL_Keycode k = ev->key.keysym.sym;
			UWORD ascii = 0;

			if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
				ascii = '\r';
			else if (k == SDLK_BACKSPACE)
				ascii = 8;
			else if (k == SDLK_ESCAPE)
				ascii = 27;
			else if (k == SDLK_TAB)
				ascii = 9;
			else if (k >= 32 && k < 127)
			{
				ascii = (UWORD)k;
				if ((ev->key.keysym.mod & KMOD_SHIFT) && k >= 'a' && k <= 'z')
					ascii = (UWORD)(k - 'a' + 'A');
			}

			if (ascii || dos)
			{
				int next = (AsciiHead + 1) % ABUF_LEN;
				if (next != AsciiTail)
				{
					AsciiBuf[AsciiHead] = (UWORD)((dos << 8) | (ascii & 0xFF));
					AsciiHead = next;
				}
			}
		}
		break;

	case SDL_MOUSEMOTION:
		Mouse_X = ev->motion.x;
		Mouse_Y = ev->motion.y;
		MouseAccX += ev->motion.xrel;
		MouseAccY += ev->motion.yrel;
		break;

	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		down = (ev->type == SDL_MOUSEBUTTONDOWN);
		if (ev->button.button == SDL_BUTTON_LEFT)
			Click = down ? (Click | 1) : (Click & ~1);
		else if (ev->button.button == SDL_BUTTON_RIGHT)
			Click = down ? (Click | 2) : (Click & ~2);
		break;

	default:
		break;
	}
}

void InitKeyboard(void)
{
	Key = 0;
	Joy = 0;
	Fire = 0;
	FuncKey = 0;
	AsciiHead = AsciiTail = 0;
}

void ClearKeyboard(void)
{
	InitKeyboard();
}

UWORD GetAscii(void)
{
	UWORD v;

	if (AsciiTail == AsciiHead)
		return 0;
	v = AsciiBuf[AsciiTail];
	AsciiTail = (AsciiTail + 1) % ABUF_LEN;
	return v;
}

void ClearAsciiBuffer(void)
{
	AsciiTail = AsciiHead;
}

/* ------------------------------------------------------------------ */
/* mouse API (S_MOUSE.C)                                               */
/* ------------------------------------------------------------------ */
void GetMouseDep(void)
{
	Mouse_X_Dep = MouseAccX;
	Mouse_Y_Dep = MouseAccY;
	MouseAccX = 0;
	MouseAccY = 0;
}

void ShowMouse(long flag)
{
	SDL_ShowCursor(flag ? SDL_ENABLE : SDL_DISABLE);
}

void AffMouse(void) {}
void SetMouse(short flag) { (void)flag; }
void InitMouse(void) {}
void ClearMouse(void) {}
void SetMousePos(long x, long y)
{
	Mouse_X = x;
	Mouse_Y = y;
}
void SetMouseBox(long x0, long y0, long x1, long y1) { (void)x0; (void)y0; (void)x1; (void)y1; }
void SetMouseSpeed(long sx, long sy) { (void)sx; (void)sy; }

/* ------------------------------------------------------------------ */
/* DOS memory (MALLOC.C DOS part)                                      */
/* ------------------------------------------------------------------ */
void *DosMalloc(LONG size, ULONG *handle)
{
	if (size == -1)
		return (void *)0; /* query: engine only uses it for debug stats */
	if (handle)
		*handle = 0;
	return calloc(1, (size_t)size);
}

void DosFree(ULONG handle)
{
	(void)handle; /* engine never frees BufSpeak; nothing sensible to do */
}

/* ------------------------------------------------------------------ */
/* misc runtime helpers                                                */
/* ------------------------------------------------------------------ */
/* Watcom ultoa (S_TEXT.C uses it); MinGW only exports _ultoa */
char *ultoa(unsigned long value, char *buf, int radix)
{
	static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	char tmp[40];
	int i = 0, j = 0;

	if (radix < 2 || radix > 36)
		radix = 10;
	do
	{
		tmp[i++] = digits[value % (unsigned)radix];
		value /= (unsigned)radix;
	} while (value);
	while (i > 0)
		buf[j++] = tmp[--i];
	buf[j] = 0;
	return buf;
}

/* FILES_A.C: Touch(filename) — refresh the file date */
void Touch(UBYTE *filename)
{
	_utime((const char *)filename, NULL);
}

/* ------------------------------------------------------------------ */
/* SYS_FILESYSTEM (FILESYSTEM.C DOS)                                   */
/* ------------------------------------------------------------------ */
#include "../../engine/LIB_SYS/SYS_FILESYSTEM.H"

int SYS_FindFirst(const char *pattern, unsigned attr, SYS_FileInfo *info)
{
	struct _finddata_t fd;
	intptr_t h;

	(void)attr;
	h = _findfirst(pattern, &fd);
	if (h == -1)
		return 1;

	strncpy(info->name, fd.name, sizeof(info->name) - 1);
	info->name[sizeof(info->name) - 1] = 0;
	info->size = (unsigned long)fd.size;
	info->wr_date = 0;
	info->wr_time = (unsigned long)fd.time_write;
	info->attrib = (unsigned char)fd.attrib;
	info->platform_handle = (void *)h;
	return 0;
}

int SYS_FindNext(SYS_FileInfo *info)
{
	struct _finddata_t fd;

	if (_findnext((intptr_t)info->platform_handle, &fd) != 0)
		return 1;

	strncpy(info->name, fd.name, sizeof(info->name) - 1);
	info->name[sizeof(info->name) - 1] = 0;
	info->size = (unsigned long)fd.size;
	info->wr_date = 0;
	info->wr_time = (unsigned long)fd.time_write;
	info->attrib = (unsigned char)fd.attrib;
	return 0;
}

void SYS_FindClose(SYS_FileInfo *info)
{
	if (info->platform_handle)
	{
		_findclose((intptr_t)info->platform_handle);
		info->platform_handle = NULL;
	}
}

unsigned SYS_GetDrive(void)
{
	return 3; /* pretend C: — the game only memorizes/restores it */
}

void SYS_SetDrive(unsigned drive, unsigned *total_drives)
{
	(void)drive;
	if (total_drives)
		*total_drives = 26;
}

unsigned long SYS_GetDiskFreeSpace(void)
{
	return 100UL * 1024UL * 1024UL; /* plenty for savegames */
}

/* ------------------------------------------------------------------ */
/* SYS_TIME (TIME.C DOS): packed DOS date/time                         */
/* ------------------------------------------------------------------ */
unsigned long SYS_ComputeTime(void)
{
	time_t t = time(NULL);
	struct tm *lt = localtime(&t);
	unsigned long cpttime, cptdate;

	cpttime = (unsigned long)lt->tm_hour;
	cpttime <<= 6;
	cpttime |= (unsigned long)lt->tm_min;
	cpttime <<= 5;
	cpttime |= (unsigned long)(lt->tm_sec / 2);

	cptdate = (unsigned long)(lt->tm_year + 1900 - 1980);
	cptdate <<= 4;
	cptdate |= (unsigned long)(lt->tm_mon + 1);
	cptdate <<= 5;
	cptdate |= (unsigned long)lt->tm_mday;

	return cpttime + cptdate; /* same odd packing as DOS TIME.C */
}
