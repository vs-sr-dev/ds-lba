/*
 * nds_sys.c — Nintendo DS replacement for the DOS system layer
 * (LIB386/PLATFORM/DOS/LIB_SYS: TIMER.C/TIMER_A.C, KEYBOARD.C, SYSTEM.C,
 *  TIME.C, FILESYSTEM.C, FILES_A.C, MALLOC.C DOS parts, S_MOUSE.C)
 * plus the newlib gaps the engine needs (_splitpath/_makepath, NDS_fopen,
 * accounting malloc wrappers hooked by watcom_compat.h).
 *
 * Timer: hardware timer 0 IRQ at 50.005 Hz (bus 33.513982 MHz / 256 / 2618)
 * bumping TimerRef/TimerSystem exactly like the DOS IRQ0 handler (TIMER_A.C).
 * NEVER derived from wall clock: RestoreTimer() (P_ANIM.C) rewinds TimerRef
 * backwards after menus, so the platform tick must only ever do TimerRef++.
 *
 * Input: scanKeys() on the VBlank IRQ (PORT_ScanInput, called by the video
 * VBlank handler in nds_video.c). KEYBOARD.C semantics preserved:
 *   Joy  bits: 1=up 2=down 4=left 8=right    (d-pad)
 *   Fire bits: 1=space(B) 2=return(A) 4=ctrl(L) 8=alt(SELECT or R)
 *   L        = the DOS CTRL key: opens the classic behaviour panel
 *              (MenuComportement — hold L, pick with the d-pad). Sets BOTH
 *              Fire bit 4 and Key=0x1D, exactly like the real keyboard.
 *   R        = the DOS ALT key (F_ALT): throw the magic ball / sabre. This
 *              is behaviour-agnostic in the engine (OBJECT.C:2251, gated only
 *              on the current weapon + its flag), so hold R to aim and throw
 *              in any behaviour, exactly like Alt on the PC keyboard. SELECT
 *              still sets F_ALT too, but hold-SELECT toggles the console, so
 *              R is the usable throw button.
 *   X        = direct behaviour Normal via the touch-UI mailbox
 *              (PORT_TouchComportement; Sporty/Aggressive/Discreet are on the
 *              touch screen or in the L panel). NB: the bring-up FuncKey F1-F4
 *              mapping was dead code — the engine never reads FuncKey.
 *   Y        = the DOS SHIFT key (F_SHIFT): open the inventory ring
 *              (PERSO.C:552) — reaches every item, not only the 4 touch icons.
 *   Key      : 1 (Esc scan code) while START held, 0x1D while L held
 *   GetAscii : ring buffer, BIOS int16h format (scan<<8 | ascii)
 */
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <calico/system/mutex.h>
#include "port.h"

/* ------------------------------------------------------------------ */
/* heap accounting (hooked into the engine by watcom_compat.h)         */
/* ------------------------------------------------------------------ */
extern char *fake_heap_end; /* devkitARM crt0 heap limit */

ULONG PORT_HeapUsed = 0;
ULONG PORT_HeapPeak = 0;

#define MEM_LOG_THRESHOLD (32 * 1024)

static void MemAccount(long delta, const char *what)
{
	struct mallinfo mi = mallinfo();

	PORT_HeapUsed = mi.uordblks;
	if (PORT_HeapUsed > PORT_HeapPeak)
		PORT_HeapPeak = PORT_HeapUsed;

	if (delta >= MEM_LOG_THRESHOLD || delta <= -MEM_LOG_THRESHOLD)
		PORT_Diag("[MEM]%+ldK %s use=%luK top=%luK\n",
			   delta / 1024, what,
			   PORT_HeapUsed / 1024,
			   (ULONG)(fake_heap_end - (char *)sbrk(0)) / 1024);
}

void PORT_MemReport(const char *tag)
{
	struct mallinfo mi = mallinfo();

	PORT_Diag("[MEM] %s: used=%luK peak=%luK arena=%luK spare=%luK\n",
		   tag, (ULONG)mi.uordblks / 1024, PORT_HeapPeak / 1024,
		   (ULONG)mi.arena / 1024,
		   (ULONG)(fake_heap_end - (char *)sbrk(0)) / 1024);
}

/* Guarded allocations: 16-byte header {prev,next,size,magic} + 4-byte tail
 * canary, all live blocks on a list so PORT_HeapCheck() can catch buffer
 * overflows at the moment they happen (bring-up diagnostic). */
#define GUARD_MAGIC 0x4C424131u /* "LBA1" */

typedef struct GuardHdr
{
	struct GuardHdr *prev, *next;
	size_t size;
	unsigned magic;
} GuardHdr;

static GuardHdr *GuardList = NULL;

void NDS_free(void *p);

static void GuardTailSet(GuardHdr *h)
{
	UBYTE *t = (UBYTE *)(h + 1) + h->size;

	t[0] = 0xC5;
	t[1] = 0xA5;
	t[2] = 0x5A;
	t[3] = 0x5C;
}

static int GuardTailOK(GuardHdr *h)
{
	UBYTE *t = (UBYTE *)(h + 1) + h->size;

	return t[0] == 0xC5 && t[1] == 0xA5 && t[2] == 0x5A && t[3] == 0x5C;
}

int PORT_HeapCheck(const char *tag)
{
	GuardHdr *h;
	int bad = 0, idx = 0;

	for (h = GuardList; h; h = h->next, idx++)
	{
		if (h->magic != GUARD_MAGIC)
		{
			PORT_Diag("[HEAP] %s: block %d HEADER smashed (%p)\n",
				   tag, idx, (void *)h);
			return ++bad; /* list unusable past this point */
		}
		if (!GuardTailOK(h))
		{
			PORT_Diag("[HEAP] %s: OVERFLOW after %lu-byte block %p\n",
				   tag, (ULONG)h->size, (void *)(h + 1));
			GuardTailSet(h); /* re-arm to catch the next one */
			bad++;
		}
	}
	return bad;
}

static void *GuardAlloc(size_t size, int zero, const char *what)
{
	GuardHdr *h = malloc(sizeof(GuardHdr) + size + 4);

	if (!h)
	{
		PORT_Diag("[MEM] %s %lu FAILED (use=%luK)\n",
			   what, (ULONG)size, PORT_HeapUsed / 1024);
		return NULL;
	}
	h->size = size;
	h->magic = GUARD_MAGIC;
	h->prev = NULL;
	h->next = GuardList;
	if (GuardList)
		GuardList->prev = h;
	GuardList = h;
	GuardTailSet(h);
	if (zero)
		memset(h + 1, 0, size);
	MemAccount((long)size, what);
	PORT_HeapCheck(what);
	return h + 1;
}

void *NDS_malloc(size_t size)
{
	return GuardAlloc(size, 0, "malloc");
}

void *NDS_calloc(size_t n, size_t size)
{
	return GuardAlloc(n * size, 1, "calloc");
}

void *NDS_realloc(void *old, size_t size)
{
	GuardHdr *h;
	void *p;

	if (!old)
		return GuardAlloc(size, 0, "realloc");

	h = (GuardHdr *)old - 1;
	if (h->magic != GUARD_MAGIC)
	{
		PORT_Diag("[MEM] realloc on foreign ptr %p\n", old);
		return realloc(old, size);
	}
	if (size <= h->size)
	{
		/* IN PLACE, mandatory: Mshrink callers (LoadMalloc_HQR) ignore the
		 * return value — a moving shrink leaves them dangling. */
		if (!GuardTailOK(h))
			PORT_Diag("[HEAP] realloc: OVERFLOW after %lu-byte block %p\n",
				   (ULONG)h->size, old);
		h->size = size;
		GuardTailSet(h);
		return old;
	}
	p = GuardAlloc(size, 0, "realloc");
	if (p)
	{
		memcpy(p, old, h->size);
		NDS_free(old);
	}
	return p;
}

void NDS_free(void *p)
{
	GuardHdr *h;

	if (!p)
		return;
	h = (GuardHdr *)p - 1;
	if (h->magic != GUARD_MAGIC)
	{
		PORT_Diag("[MEM] free on foreign ptr %p\n", p);
		free(p);
		return;
	}
	if (!GuardTailOK(h))
		PORT_Diag("[HEAP] free: OVERFLOW after %lu-byte block %p\n",
			   (ULONG)h->size, p);
	if (h->prev)
		h->prev->next = h->next;
	else
		GuardList = h->next;
	if (h->next)
		h->next->prev = h->prev;
	h->magic = 0;
	free(h);
	MemAccount(0, "free");
}

/* ------------------------------------------------------------------ */
/* Savegames on fat:/ (SD card via DLDI on flashcarts, SD image/folder */
/* on melonDS). nitroFS is read-only: everything the engine WRITES     */
/* (S*.LBA / AUTOSAVE.LBA savegames, the LBA.CFG volume settings and   */
/* its __tempo.def scratch file) is rerouted to fat:/lba1/save/.       */
/* Reads of those same names come back from fat:/ too (LBA.CFG falls   */
/* back to the nitroFS copy until the game first writes it).           */
/* If FAT init fails (emulator without DLDI/SD) the game runs exactly  */
/* as before: nitroFS-only, saves fail gracefully in-engine.           */
/* ------------------------------------------------------------------ */
#define SAVE_DIR "fat:/lba1/save"
#define VOX_DIR "fat:/lba1/vox"

int PORT_FatOK = 0;

void PORT_FatInit(void)
{
	PORT_FatOK = fatInitDefault() ? 1 : 0;
	if (!PORT_FatOK)
	{
		printf("fat: NO SD (DLDI) - saves DISABLED\n");
		return;
	}
	mkdir("fat:/lba1", 0777); /* EEXIST is fine */
	mkdir(SAVE_DIR, 0777);
	mkdir(VOX_DIR, 0777);
	printf("fat OK - saves -> " SAVE_DIR "/\n");
}

/* Does this engine path belong on the SD? Decides on the basename:
 *   *.lba        — savegames (the only .LBA names the engine ever opens;
 *                  game data is .HQR/.DAT/.DOT/.CFG/.LST)
 *   lba.cfg      — config rewritten by "save settings" (volumes)
 *   __tempo.def  — DEF_FILE.C scratch file ("c://__tempo.def")
 *   *.vox        — spoken-dialogue banks (MESSAGE.C: "VOX\" + LANG + file).
 *                  12 banks per language, ~33 MB each: only en_gam/en_000
 *                  fit in the ROM, the rest live on the SD.
 * `norm` must already be lowercase with '/' separators.
 * Read-only names (lba.cfg, the vox banks) take the SD copy only when it is
 * actually there, so a cart without them still boots off the nitroFS.
 * Returns the routed path in `out` or NULL for "leave alone". */
static const char *RouteSdPath(const char *norm, const char *mode,
							   char *out, size_t outsz)
{
	const char *base;
	size_t blen;
	int is_lba, is_cfg, is_tmp, is_vox, fallback;

	if (!PORT_FatOK)
		return NULL;
	base = strrchr(norm, '/');
	base = base ? base + 1 : norm;
	blen = strlen(base);

	is_lba = blen > 4 && strcmp(base + blen - 4, ".lba") == 0;
	is_vox = blen > 4 && strcmp(base + blen - 4, ".vox") == 0;
	is_cfg = strcmp(base, "lba.cfg") == 0;
	is_tmp = strcmp(base, "__tempo.def") == 0;
	if (!is_lba && !is_cfg && !is_tmp && !is_vox)
		return NULL;

	snprintf(out, outsz, "%s/%s", is_vox ? VOX_DIR : SAVE_DIR, base);

	fallback = is_vox ||
			   (is_cfg && !strchr(mode, 'w') && !strchr(mode, 'a'));
	if (fallback)
	{
		struct stat st;
		int missing;

		PORT_IoLock();
		missing = stat(out, &st) != 0;
		PORT_IoUnlock();
		if (missing)
			return NULL; /* not on the SD: read the nitroFS copy */
	}
	return out;
}

/* ------------------------------------------------------------------ */
/* NDS stdio wrappers (hooked into the engine by watcom_compat.h)      */
/*                                                                     */
/* NDS_fopen: path normalizer — nitroFS is the cwd, nitro names are    */
/* stored lowercase, '\' becomes '/'.                                  */
/*                                                                     */
/* Handle registry: the engine sometimes hands stale/NULL FILE*s to    */
/* fseek/fread (it got away with it on DOS and on msvcrt, which        */
/* validate parameters). newlib data-aborts instead, so every wrapped  */
/* call checks the handle against the registry and fails gracefully,   */
/* logging the event on the console.                                   */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Card access lock                                                    */
/* ------------------------------------------------------------------ */
/* The music player (nds_music.c) streams from the SD on its own thread
 * while the game reads assets from the nitroFS on the main thread. On a
 * flashcart both go to the same physical device over the card bus, and
 * neither libfat nor the nitroFS layer serializes access: two reads can
 * interleave mid-transfer and corrupt either side — a savegame write being
 * the worst case. So every file operation on either thread takes this
 * lock. It is held for one libc call at a time, so a slow SD read in the
 * music thread can never stall a frame for more than a single block.
 *
 * Lock order, never reversed: nds_music.c's mmutex first, then this one.
 * The engine only ever takes this one — PlayMidi()/PlayTrackCDR() are
 * called from game code, never from inside a file operation. */
static Mutex iomutex;
void PORT_IoLock(void) { mutexLock(&iomutex); }
void PORT_IoUnlock(void) { mutexUnlock(&iomutex); }

/* ------------------------------------------------------------------ */
/* Diagnostic log                                                      */
/* ------------------------------------------------------------------ */
/* The in-game console needs SELECT held for a second, which assumes the
 * player has SELECT bound at all — on melonDS it is unbound by default, so
 * a tester can be unable to reach it. Diagnostics therefore also go to
 * fat:/lba1/lba1ds.log, flushed every line so a freeze still leaves the
 * trail. Under melonDS FolderSync the file appears in the synced folder
 * when the emulator is closed (same route the savegames take), so it can
 * simply be read afterwards instead of photographed. */
/* Each line is opened, appended and CLOSED again. That is deliberate and
 * not paranoia: fflush() only pushes the bytes into libfat, which caches
 * the data sectors AND the directory entry and commits them on close. A
 * log held open forever therefore does not exist as a file at all until
 * something closes it — and neither a freeze, nor killing the emulator,
 * nor melonDS's own shutdown ever does. (Savegames survive because the
 * engine closes them.) One SD write per diagnostic line is a fine price:
 * these lines are rare, and the whole point is that the last one before a
 * hang is on disc. */
#define LOG_FILE "fat:/lba1/lba1ds.log"

static int DiagReady = 0;

void PORT_DiagInit(void)
{
	FILE *f;

	if (!PORT_FatOK)
		return;

	/* APPEND, not truncate. A guru is normally followed by restarting the
	 * ROM, and truncating here would erase the run that had just failed —
	 * precisely the one worth reading. The banner separates runs, and
	 * carries the build stamp: a crash report that cannot be tied to a
	 * build costs an evening (the .text of two builds does not line up, so
	 * resolving an address against the wrong one invents a symbol). */
	PORT_IoLock();
	f = fopen(LOG_FILE, "a");
	if (f)
	{
		fputs("\n=== run (built " __DATE__ " " __TIME__ ") ===\n", f);
		fclose(f);
	}
	PORT_IoUnlock();

	DiagReady = (f != NULL);
	printf("[log] %s " LOG_FILE "\n", DiagReady ? "->" : "FAILED");
}

void PORT_Diag(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 1)
		n = (int)sizeof(buf) - 1;

	fputs(buf, stdout); /* the on-screen console, as before */

	if (DiagReady)
	{
		FILE *f;

		PORT_IoLock();
		f = fopen(LOG_FILE, "a");
		if (f)
		{
			fwrite(buf, 1, (size_t)n, f);
			fclose(f); /* the close is what commits it — see above */
		}
		PORT_IoUnlock();
	}
}

/* 32 slots, and names long enough to survive "fat:/lba1/music/mus01_l.wav":
 * the old 20 bytes truncated every SD path to the same prefix, which made
 * the registry dump — the one thing that names a leak — unreadable. */
#define MAX_OPEN_FILES 32
static FILE *OpenTab[MAX_OPEN_FILES];
static char OpenName[MAX_OPEN_FILES][48];

static int FileOK(FILE *f, const char *op)
{
	int i;

	for (i = 0; i < MAX_OPEN_FILES; i++)
		if (OpenTab[i] == f && f)
			return 1;
	PORT_Diag("[FIO] bad %s handle %p\n", op, (void *)f);
	PORT_HeapCheck(op);
	return 0;
}

FILE *NDS_fopen(const char *name, const char *mode)
{
	char buf[280];
	char fatbuf[280];
	const char *routed;
	int i = 0;
	const char *s = name;
	FILE *f;

	if (!s) /* e.g. FileSize(getenv("ADELINE")) with no env on the DS */
		return NULL;
	if (s[0] == '.' && (s[1] == '\\' || s[1] == '/'))
		s += 2;
	while (*s && i < 259)
	{
		char c = *s++;

		if (c == '\\')
			c = '/';
		else
			c = (char)tolower((unsigned char)c);
		buf[i++] = c;
	}
	buf[i] = 0;

	/* savegames + rewritten config: reroute onto the SD (fat:/lba1/save) */
	routed = RouteSdPath(buf, mode, fatbuf, sizeof(fatbuf));
	if (routed)
	{
		PORT_Diag("[SAV] fopen(%s) %s\n", mode, routed);
		strcpy(buf, routed); /* fits: buf is as large as fatbuf */
	}

	PORT_IoLock();
	f = fopen(buf, mode);
	PORT_IoUnlock();
	if (f)
	{
		for (i = 0; i < MAX_OPEN_FILES; i++)
			if (!OpenTab[i])
			{
				OpenTab[i] = f;
				strncpy(OpenName[i], buf, sizeof(OpenName[i]) - 1);
				OpenName[i][sizeof(OpenName[i]) - 1] = 0;
				return f;
			}

		/* No slot left. Handing the caller this handle — which is what used
		 * to happen — is a trap: it is open and perfectly valid, but every
		 * later fread/fseek fails FileOK and returns 0, and NDS_fclose then
		 * refuses to close it, so the FILE leaks and the shortage can only
		 * ever get worse. The engine reads zeroes and builds its bookkeeping
		 * out of them. Failing the open is far kinder: callers already have
		 * a path for a file that will not open.
		 *
		 * The dump is NOT latched behind a static any more. It used to print
		 * once per run, so a report photographed later showed the fallout
		 * with the cause long gone off the top of the console. */
		PORT_Diag("[FIO] REGISTRY FULL, refusing %s\n", buf);
		for (i = 0; i < MAX_OPEN_FILES; i++)
			PORT_Diag("[FIO]   %2d: %s\n", i, OpenName[i]);
		PORT_IoLock();
		fclose(f);
		PORT_IoUnlock();
		return NULL;
	}
	return f;
}

int NDS_fclose(FILE *f)
{
	int i;

	for (i = 0; i < MAX_OPEN_FILES; i++)
		if (OpenTab[i] == f && f)
		{
			int r;

			OpenTab[i] = NULL;
			PORT_IoLock();
			r = fclose(f);
			PORT_IoUnlock();
			return r;
		}
	/* Deliberately NOT closed. An unregistered handle can no longer be a
	 * valid-but-unregistered one (NDS_fopen refuses rather than hand one
	 * out), so reaching here means a stale or already-closed pointer, and
	 * fclose() on a freed FILE would corrupt the heap — the very failure
	 * we are chasing. Refusing leaks nothing that was ever ours. */
	PORT_Diag("[FIO] bad/double fclose %p\n", (void *)f);
	return -1;
}

size_t NDS_fread(void *p, size_t sz, size_t n, FILE *f)
{
	size_t r;

	if (!FileOK(f, "fread"))
		return 0;
	PORT_IoLock();
	r = fread(p, sz, n, f);
	PORT_IoUnlock();
	return r;
}

size_t NDS_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
	size_t r;

	if (!FileOK(f, "fwrite"))
		return 0;
	PORT_IoLock();
	r = fwrite(p, sz, n, f);
	PORT_IoUnlock();
	return r;
}

int NDS_fseek(FILE *f, long off, int whence)
{
	int r;

	if (!FileOK(f, "fseek"))
		return -1;
	PORT_IoLock();
	r = fseek(f, off, whence);
	PORT_IoUnlock();
	return r;
}

long NDS_ftell(FILE *f)
{
	long r;

	if (!FileOK(f, "ftell"))
		return -1;
	PORT_IoLock();
	r = ftell(f);
	PORT_IoUnlock();
	return r;
}

/* FILES.C Delete() → remove(): must follow the same normalization and
 * fat:/ rerouting as NDS_fopen (destroy-a-savegame menu, __tempo.def). */
int NDS_remove(const char *name)
{
	char buf[280];
	char fatbuf[280];
	const char *routed;
	int i = 0;
	const char *s = name;

	if (!s)
		return -1;
	if (s[0] == '.' && (s[1] == '\\' || s[1] == '/'))
		s += 2;
	while (*s && i < 259)
	{
		char c = *s++;

		if (c == '\\')
			c = '/';
		else
			c = (char)tolower((unsigned char)c);
		buf[i++] = c;
	}
	buf[i] = 0;

	/* NEVER delete a voice bank. MESSAGE.C's ClearVoiceFile() erases the
	 * VOX files it thinks it copied to the "hard disk" whenever
	 * FlagKeepVoice is off — harmless while they sat in the read-only
	 * nitroFS, but they now live on a writable SD and that is ~99 MB of
	 * the user's own assets. LBA.CFG ships FlagKeepVoice: ON, so this only
	 * ever fires if that key is lost or edited; refuse regardless. */
	{
		size_t bl = strlen(buf);

		if (bl > 4 && strcmp(buf + bl - 4, ".vox") == 0)
		{
			PORT_Diag("[FIO] refused delete of voice bank %s\n", buf);
			return 0; /* report success: the engine only bookkeeps */
		}
	}

	routed = RouteSdPath(buf, "w", fatbuf, sizeof(fatbuf));
	if (routed)
	{
		PORT_Diag("[SAV] remove %s\n", routed);
		strcpy(buf, routed);
	}
	{
		int r;

		PORT_IoLock();
		r = remove(buf);
		PORT_IoUnlock();
		return r;
	}
}

/* ------------------------------------------------------------------ */
/* timer (TIMER_A.C variables) — hardware timer 0, 50 Hz               */
/* ------------------------------------------------------------------ */
#define TICK_HZ 50

volatile ULONG TimerRef = 0;
volatile ULONG TimerSystem = 0;
UWORD NbFramePerSecond = 0;
UWORD WaitNbTicks = 1;
UWORD CmptFrame = 0;
UWORD Cmpt_18 = 0;

static volatile int TimerOn = 0;

static void TimerISR(void)
{
	if (!TimerOn)
		return;

	TimerSystem++;
	TimerRef++; /* relative ++ only: RestoreTimer() rewinds it */

	if (--WaitNbTicks == 0)
	{
		WaitNbTicks = TICK_HZ;
		NbFramePerSecond = CmptFrame;
		CmptFrame = 0;
	}
}

void InitTimer(void)
{
	TimerRef = 0;
	TimerSystem = 0;
	WaitNbTicks = 1;
	TimerOn = 1;
	/* 33513982 / 256 / 2618 = 50.005 Hz (never the 59.83 Hz VBlank!)
	 * HW TIMER 0: calico owns TM2 (system tick, tickGetCount) and TM3
	 * (tick tasks) on the ARM9 — timerStart(2/3) would stomp them (it
	 * did, during M4 bring-up: tickGetCount was frozen). TM0/TM1 are
	 * the only free ones; TM1 stays free (profiler spare / cascade). */
	timerStart(0, ClockDivider_256, TIMER_FREQ_256(TICK_HZ), TimerISR);
}

void ClearTimer(void)
{
	TimerOn = 0;
	timerStop(0);
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
/* system (SYSTEM.C)                                                   */
/* ------------------------------------------------------------------ */
void InitSystem(void) {}
void ClearSystem(void) {}

/* ------------------------------------------------------------------ */
/* keyboard (KEYBOARD.C semantics on the DS pad)                       */
/* ------------------------------------------------------------------ */
volatile UWORD Key = 0;
volatile UWORD FuncKey = 0;
volatile UWORD Joy = 0;
volatile UWORD Fire = 0;
UWORD AsciiMode = 0;

#define ABUF_LEN 32
static volatile UWORD AsciiBuf[ABUF_LEN];
static volatile int AsciiHead = 0, AsciiTail = 0;

static void PushAscii(UWORD scan_ascii)
{
	int next = (AsciiHead + 1) % ABUF_LEN;

	if (next != AsciiTail)
	{
		AsciiBuf[AsciiHead] = scan_ascii;
		AsciiHead = next;
	}
}

/* Scripted input for headless smoke tests (same idea as the SDL shim's
 * LBA_AUTOENTER): if nitro:/autoenter exists (read by main_nds.c), pulse
 * Return every ~700ms for N seconds. Driven from the VBlank tick. */
volatile int PORT_AutoEnterFrames = 0;
volatile int PORT_AutoWalkFrames = 0; /* nitro:/autowalk: hold UP (walk)
                                         after the autoenter phase ends,
                                         with periodic LEFT turns — drives
                                         sustained scene redraw for perf
                                         measurements */
volatile int PORT_AutoPauseFrames = 0; /* nitro:/autopause: N seconds after
                                          boot, hold START (Esc) for the
                                          last ~0.4 s of the countdown —
                                          headless pause-menu repro */
static int AutoPhase = 0;
static int AutoState = 0;

/* --- scripted save/load smoke tests (diag ROM only, nitro:/autosave and
 * nitro:/autoload) — deterministic input sequences that drive the real
 * engine menus, end to end through the fat:/ savegame routing. ---------- */
volatile int PORT_AutoSaveFrames = 0; /* countdown to the save script */
volatile int PORT_AutoLoadFrames = 0; /* countdown to the load script
                                         (Esc pulsed while counting down:
                                         skips the boot logos) */

typedef struct
{
	UWORD frames; /* how many vblanks this step lasts (0 = end)   */
	UWORD joy;    /* Joy bits held during the step                */
	UWORD fire;   /* Fire bits held                               */
	UWORD key;    /* Key scan code held (1 = Esc)                 */
	UWORD ascii;  /* pushed ONCE on step entry (int16h format)    */
} AutoStep;

/* Pause-menu save under the name "DS".
 * GameQuitMenu = [continue, load game, save game, abandon].
 * FOUR Esc pulses: a stray idle dialog (seen live: "what if I try to
 * get the guard's attention?") eats up to two of them (skip typewriter
 * + close); extra pulses are no-ops once the menu is open (the K_ESC
 * break in DoGameMenu is commented out in GAMEMENU.C). */
static const AutoStep SaveScript[] = {
	{20, 0, 0, 1, 0},      /* Esc — close a dialog / pause menu      */
	{70, 0, 0, 0, 0},
	{20, 0, 0, 1, 0},      /* Esc                                    */
	{70, 0, 0, 0, 0},
	{20, 0, 0, 1, 0},      /* Esc                                    */
	{70, 0, 0, 0, 0},
	{20, 0, 0, 1, 0},      /* Esc — by now the menu is open          */
	{70, 0, 0, 0, 0},
	{6, 2, 0, 0, 0},       /* DOWN -> load game                      */
	{40, 0, 0, 0, 0},
	{6, 2, 0, 0, 0},       /* DOWN -> save game                      */
	{40, 0, 0, 0, 0},
	{6, 0, 2, 0, 0},       /* Return -> ChoosePlayerName(951)        */
	{90, 0, 0, 0, 0},
	{6, 0, 2, 0, 0},       /* Return -> "create new" -> name input   */
	{150, 0, 0, 0, 0},     /* InputPlayerName drains the ascii ring  */
	{2, 0, 0, 0, 'D'},
	{30, 0, 0, 0, 0},
	{2, 0, 0, 0, 'S'},
	{30, 0, 0, 0, 0},
	{2, 0, 0, 0, 0x1C0D},  /* Return: validate -> SaveGameWithName   */
	{0, 0, 0, 0, 0}};

/* Main-menu load of the first listed save.
 * GameMainMenu = [new game, continue saved game, options, quit]. */
static const AutoStep LoadScript[] = {
	{60, 0, 0, 0, 0},
	{6, 2, 0, 0, 0},       /* DOWN -> continue saved game            */
	{40, 0, 0, 0, 0},
	{6, 0, 2, 0, 0},       /* Return -> ChoosePlayerName(21)         */
	{120, 0, 0, 0, 0},     /* save list drawn (reads every S*.LBA)   */
	{6, 0, 2, 0, 0},       /* Return -> first save -> LoadGame()     */
	{0, 0, 0, 0, 0}};

static const AutoStep *ScriptCur = NULL;
static int ScriptStep = 0;
static int ScriptLeft = 0;

/* returns the active step (input merged by the caller), or NULL */
static const AutoStep *AutoScriptTick(void)
{
	if (!ScriptCur)
	{
		if (PORT_AutoSaveFrames > 0 && --PORT_AutoSaveFrames == 0)
		{
			ScriptCur = SaveScript;
			ScriptStep = 0;
			ScriptLeft = -1;
		}
		else if (PORT_AutoLoadFrames > 0)
		{
			/* pulse Esc on the way to the menu: skips logo/bumper,
			   harmless once the main menu is up */
			static const AutoStep escpulse = {0, 0, 0, 1, 0};

			if (--PORT_AutoLoadFrames == 0)
			{
				ScriptCur = LoadScript;
				ScriptStep = 0;
				ScriptLeft = -1;
				return NULL;
			}
			return (PORT_AutoLoadFrames % 45) < 6 ? &escpulse : NULL;
		}
		return NULL;
	}

	if (ScriptLeft < 0) /* entering ScriptStep */
	{
		if (ScriptCur[ScriptStep].frames == 0)
		{
			ScriptCur = NULL; /* script done (no printf: IRQ context) */
			return NULL;
		}
		ScriptLeft = ScriptCur[ScriptStep].frames;
		if (ScriptCur[ScriptStep].ascii)
			PushAscii(ScriptCur[ScriptStep].ascii);
	}
	{
		const AutoStep *st = &ScriptCur[ScriptStep];

		if (--ScriptLeft == 0)
		{
			ScriptStep++;
			ScriptLeft = -1;
		}
		return st;
	}
}

static UWORD AutoInputJoy(void)
{
	static int walkphase = 0;

	if (PORT_AutoEnterFrames > 0 || PORT_AutoWalkFrames <= 0)
		return 0;
	PORT_AutoWalkFrames--;
	walkphase++;
	/* UP held; every ~3.5s turn LEFT for ~1.5s to change heading (enough
	   to rotate away from walls instead of pushing into them) */
	if ((walkphase % 210) < 90)
		return 1 | 4;
	return 1;
}

static UWORD AutoInputFire(void)
{
	if (PORT_AutoEnterFrames <= 0)
	{
		AutoState = 0;
		return 0;
	}
	PORT_AutoEnterFrames--;

	if (++AutoPhase >= 21)
	{
		AutoPhase = 0;
		AutoState = !AutoState;
		if (AutoState)
			PushAscii((28 << 8) | '\r');
	}
	return AutoState ? 2 : 0;
}

/* called from the VBlank IRQ (nds_video.c) */
void PORT_ScanInput(void)
{
	u32 held, down;
	UWORD v;
	const AutoStep *script = AutoScriptTick(); /* diag save/load scripts */

	scanKeys();
	held = keysHeld();
	down = keysDown();

	v = 0;
	if (held & KEY_UP)
		v |= 1;
	if (held & KEY_DOWN)
		v |= 2;
	if (held & KEY_LEFT)
		v |= 4;
	if (held & KEY_RIGHT)
		v |= 8;
	v |= AutoInputJoy();
	if (script)
		v |= script->joy;
	Joy = v;

	v = 0;
	if (held & KEY_B)
		v |= 1; /* space  (action)   */
	if (held & KEY_A)
		v |= 2; /* return (validate) */
	if (held & KEY_L)
		v |= 4; /* ctrl: classic behaviour panel (MenuComportement) */
	if (held & KEY_SELECT)
		v |= 8; /* alt               */
	if (held & KEY_R)
		v |= 8; /* alt: throw magic ball / sabre (F_ALT, OBJECT.C:2251,
		           behaviour-agnostic, held while aiming) */
	if (held & KEY_Y)
		v |= 32; /* shift: open the inventory ring (F_SHIFT, PERSO.C:552 —
		            gives access to every item, not just the 4 touch icons) */
	v |= AutoInputFire();
	if (script)
		v |= script->fire;
	Fire = v;

	FuncKey = 0; /* the engine never reads it (dead extern in LIB_SYS.H) */

	if (held & KEY_START)
		Key = 1; /* Esc scan code (dominant) */
	else if (held & KEY_L)
		Key = 0x1D; /* CTRL scan code, like the DOS keyboard */
	else if (script && script->key)
		Key = script->key;
	else
		Key = 0;

	/* nitro:/autopause repro hook: hold Esc for the countdown's tail */
	if (PORT_AutoPauseFrames > 0)
	{
		PORT_AutoPauseFrames--;
		if (PORT_AutoPauseFrames < 25)
			Key = 1;
	}

	/* X/Y/R = direct behaviour switch, same mailbox as the touch UI
	   (consumed once by the PERSO.C MainLoop, expires in ~200 ms).
	   No-op presses are dropped: SetComportement reloads the body even
	   for the same value. */
	{
		extern volatile WORD PORT_TouchComportement; /* nds_ui.c   */
		extern WORD Comportement;                    /* engine     */

		if ((down & KEY_X) && Comportement != 0)
			PORT_TouchComportement = 0; /* normal     */
		/* Sporty (was KEY_Y) and Aggressive (was KEY_R) are now only on the
		   touch UI / L panel — the pad buttons were freed up: Y opens the
		   inventory (F_SHIFT above), R throws the ball/sabre (F_ALT above). */
	}

	if (down & KEY_A)
		PushAscii((28 << 8) | '\r');
	if (down & KEY_B)
		PushAscii((57 << 8) | ' ');
	if (down & KEY_START)
		PushAscii((1 << 8) | 27);
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
/* mouse (S_MOUSE.C) — no mouse on the DS                              */
/* ------------------------------------------------------------------ */
volatile LONG Click = 0;
volatile LONG Mouse_X = 0;
volatile LONG Mouse_Y = 0;
LONG Mouse_X_Dep = 0;
LONG Mouse_Y_Dep = 0;
UBYTE *GphMouse = NULL;

void GetMouseDep(void)
{
	Mouse_X_Dep = 0;
	Mouse_Y_Dep = 0;
}

void ShowMouse(long flag) { (void)flag; }
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
	return NDS_calloc(1, (size_t)size);
}

void DosFree(ULONG handle)
{
	(void)handle;
}

/* ------------------------------------------------------------------ */
/* misc runtime helpers                                                */
/* ------------------------------------------------------------------ */
/* FILES_A.C: Touch(filename) — nothing sensible on nitroFS */
void Touch(UBYTE *filename)
{
	(void)filename;
}

/* Watcom ultoa/ltoa (S_TEXT.C uses them); newlib does not ship them */
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

char *ltoa(long value, char *buf, int radix)
{
	if (value < 0 && radix == 10)
	{
		buf[0] = '-';
		ultoa((unsigned long)-value, buf + 1, radix);
		return buf;
	}
	return ultoa((unsigned long)value, buf, radix);
}

/* MinGW _splitpath/_makepath used by FILES.C AddExt and DISKFUNC.C */
void _splitpath(const char *path, char *drive, char *dir,
				char *fname, char *ext)
{
	const char *p, *slash, *dot, *body;

	if (drive)
		drive[0] = 0;
	if (dir)
		dir[0] = 0;
	if (fname)
		fname[0] = 0;
	if (ext)
		ext[0] = 0;
	if (!path)
		return;

	p = path;
	if (isalpha((unsigned char)p[0]) && p[1] == ':')
	{
		if (drive)
		{
			drive[0] = p[0];
			drive[1] = ':';
			drive[2] = 0;
		}
		p += 2;
	}

	slash = NULL;
	for (body = p; *body; body++)
		if (*body == '/' || *body == '\\')
			slash = body;

	if (slash)
	{
		if (dir)
		{
			size_t n = (size_t)(slash - p) + 1;

			if (n > 255)
				n = 255;
			memcpy(dir, p, n);
			dir[n] = 0;
		}
		p = slash + 1;
	}

	dot = NULL;
	for (body = p; *body; body++)
		if (*body == '.')
			dot = body;

	if (dot)
	{
		if (fname)
		{
			size_t n = (size_t)(dot - p);

			if (n > 255)
				n = 255;
			memcpy(fname, p, n);
			fname[n] = 0;
		}
		if (ext)
		{
			strncpy(ext, dot, 255);
			ext[255] = 0;
		}
	}
	else if (fname)
	{
		strncpy(fname, p, 255);
		fname[255] = 0;
	}
}

void _makepath(char *path, const char *drive, const char *dir,
			   const char *fname, const char *ext)
{
	path[0] = 0;
	if (drive && drive[0])
	{
		strcat(path, drive);
		if (path[strlen(path) - 1] != ':')
			strcat(path, ":");
	}
	if (dir && dir[0])
	{
		strcat(path, dir);
		if (path[strlen(path) - 1] != '/' && path[strlen(path) - 1] != '\\')
			strcat(path, "\\");
	}
	if (fname)
		strcat(path, fname);
	if (ext && ext[0])
	{
		if (ext[0] != '.')
			strcat(path, ".");
		strcat(path, ext);
	}
}

/* ------------------------------------------------------------------ */
/* SYS_FILESYSTEM (FILESYSTEM.C DOS) — dirent over nitroFS             */
/* ------------------------------------------------------------------ */
#include "../../engine/LIB_SYS/SYS_FILESYSTEM.H"

static DIR *FindDir = NULL;
static char FindPat[64];
static char FindBase[260];

/* shell-style '*'/'?' match, case-insensitive */
static int PatMatch(const char *pat, const char *str)
{
	while (*pat)
	{
		if (*pat == '*')
		{
			pat++;
			if (!*pat)
				return 1;
			for (; *str; str++)
				if (PatMatch(pat, str))
					return 1;
			return PatMatch(pat, str); /* empty tail */
		}
		if (!*str)
			return 0;
		if (*pat != '?' &&
			tolower((unsigned char)*pat) != tolower((unsigned char)*str))
			return 0;
		pat++;
		str++;
	}
	return *str == 0;
}

static void FindDirClose(void)
{
	if (FindDir)
	{
		PORT_IoLock();
		closedir(FindDir);
		PORT_IoUnlock();
		FindDir = NULL;
	}
}

/* holds the card lock across the scan: the music thread must not read the
 * SD between our readdir() and the stat() of the entry it just returned */
static int FindFill(SYS_FileInfo *info)
{
	struct dirent *de;
	int done = 1;

	PORT_IoLock();
	while ((de = readdir(FindDir)) != NULL)
	{
		struct stat st;

		if (de->d_type == DT_DIR)
			continue;
		if (!PatMatch(FindPat, de->d_name))
			continue;

		strncpy(info->name, de->d_name, sizeof(info->name) - 1);
		info->name[sizeof(info->name) - 1] = 0;
		info->size = 0;
		{
			char full[520];

			snprintf(full, sizeof(full), "%s/%s", FindBase, de->d_name);
			if (stat(full, &st) == 0)
				info->size = (unsigned long)st.st_size;
		}
		info->wr_date = 0;
		info->wr_time = 0;
		info->attrib = 0;
		info->platform_handle = FindDir;
		done = 0;
		break;
	}
	PORT_IoUnlock();
	return done;
}

int SYS_FindFirst(const char *pattern, unsigned attr, SYS_FileInfo *info)
{
	char norm[260];
	char dir[260];
	const char *pat;
	char *slash;
	int i;

	(void)attr;

	FindDirClose();

	for (i = 0; pattern[i] && i < 259; i++)
		norm[i] = (pattern[i] == '\\') ? '/'
				 : (char)tolower((unsigned char)pattern[i]);
	norm[i] = 0;

	slash = strrchr(norm, '/');
	if (slash)
	{
		size_t n = (size_t)(slash - norm);

		memcpy(dir, norm, n);
		dir[n] = 0;
		pat = slash + 1;
	}
	else
	{
		strcpy(dir, ".");
		pat = norm;
	}

	/* enumeration follows the same routing as NDS_fopen: savegames
	 * (PlayerGameList/FindPlayerFile: "*.LBA", "AUTOSAVE.LBA") live in
	 * fat:/lba1/save, the voice banks (InitVoiceFile: "VOX\*.VOX") in
	 * fat:/lba1/vox */
	{
		size_t pl = strlen(pat);

		if (PORT_FatOK && pl > 4 && strcmp(pat + pl - 4, ".lba") == 0)
			strcpy(dir, SAVE_DIR);
		else if (PORT_FatOK && pl > 4 && strcmp(pat + pl - 4, ".vox") == 0)
			strcpy(dir, VOX_DIR);
	}

	strncpy(FindPat, pat, sizeof(FindPat) - 1);
	FindPat[sizeof(FindPat) - 1] = 0;
	strcpy(FindBase, dir);

	PORT_IoLock();
	FindDir = opendir(dir);
	PORT_IoUnlock();
	if (!FindDir)
		return 1;

	if (FindFill(info))
	{
		FindDirClose();
		return 1;
	}
	return 0;
}

int SYS_FindNext(SYS_FileInfo *info)
{
	if (!FindDir)
		return 1;
	return FindFill(info);
}

void SYS_FindClose(SYS_FileInfo *info)
{
	(void)info;
	FindDirClose();
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

	if (!lt)
		return 0;

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
