/*
 * nds_prof.c — frame-phase profiler for the DS port (bring-up perf work).
 *
 * Zero engine edits: the hot engine/translate entry points are intercepted
 * with GNU ld --wrap (see WRAPS in the Makefile).  Time source is calico's
 * system tick, SYSTEM_CLOCK/64 = 523656 Hz (1.9096 us per tick), read with
 * tickGetCount() — no hardware timer is touched (calico owns TM2/TM3 for
 * the tick; the 50 Hz game tick of nds_sys.c is on TM0; TM1 is free).
 *
 * Frame anchor: AffScene() (game/OBJECT.C) — one call per MainLoop frame.
 *   PH_LOGIC   = time from previous AffScene exit to this AffScene entry
 *                (game logic + scripts + anims; ALSO contains the 50 Hz
 *                tick-lock busy wait, so at the 50 fps cap it reads high —
 *                below the cap the wait is ~0 and it is pure logic)
 *   PH_CLSBOX  = ClsBoxes()        (incremental frame: Screen->Log restores)
 *   PH_CLS     = Cls()             (full frame: 300K memset of Log)
 *   PH_CPYSCR  = CopyScreen()      (full frame: Log->Screen 300K memcpy)
 *   PH_GRID    = AffGrille()       (full frame: iso brick grid redraw,
 *                                   includes the AffGraph brick blits)
 *   PH_ANIM    = SetInterAnimObjet2() (anim interpolation, LIB_3D/P_ANIM.C)
 *   PH_OBJ3D   = AffObjetIso()     (3D actor pipeline, translate/p_ob_iso.c)
 *   PH_OVER    = DrawOverBrick[3]() (bricks re-masked over actors, CopyMask)
 *   PH_PRESENT = PresentRect640    (Log->VRAM decimating blit, nds_video.c,
 *                                   hooked directly — covers Flip/FlipBoxes/
 *                                   CopyBlockPhys)
 *   PH_OTHER   = AffScene total minus all of the above measured inside it
 *                (sprite AffGraph, sort, shadows, projections, font...)
 *
 * Report: every PROF_WINDOW frames, 5 lines on the debug console, average
 * MICROSECONDS per frame per phase (console is 32 cols wide):
 *   P1 lg<us> cb<us> cl<us>
 *   P2 cp<us> gr<us> an<us>
 *   P3 ob<us> ov<us> pr<us>
 *   P4 ot<us> sf<us> si<us>   sf/si = avg AffScene us on full/incr frames
 *   P5 n<frames> q<full> o<objs/frame> fps<NbFramePerSecond>
 */
#include <nds.h>
#include <stdio.h>
#include "port.h"

u64 tickGetCount(void); /* calico/system/tick.h */

#define PROF_WINDOW 50
#define TICK_TO_US(t) ((u32)(((u64)(t) * 1000000u) / 523656u))

enum
{
	PH_LOGIC,
	PH_CLSBOX,
	PH_CLS,
	PH_CPYSCR,
	PH_GRID,
	PH_ANIM,
	PH_OBJ3D,
	PH_OVER,
	PH_PRESENT,
	PH_OTHER,
	PH_COUNT
};

static u32 Acc[PH_COUNT];
static u32 InSum;   /* wrapped-phase ticks inside the current AffScene call */
static int InScene; /* attribution flag for PH_OTHER                        */
static u32 LastSceneEnd;
static u32 Frames, FullFrames;
static u32 SceneFullTicks, SceneIncrTicks;
static u32 ObjCalls;

static inline u32 Now(void)
{
	return (u32)tickGetCount();
}

static void ProfAdd(int ph, u32 dt)
{
	Acc[ph] += dt;
	if (InScene)
		InSum += dt;
}

static u32 AvgUs(u32 ticks, u32 n)
{
	return n ? TICK_TO_US(ticks) / n : 0;
}

static u32 LastReport;

static void ProfReport(void)
{
	u32 incr = Frames - FullFrames;
	u32 nowt = Now();

	/* window wall time per frame (sanity: all phases must sum below it) */
	printf("P0 w%lu\n", AvgUs(nowt - LastReport, Frames));
	LastReport = nowt;
	printf("P1 lg%lu cb%lu cl%lu\n",
		   AvgUs(Acc[PH_LOGIC], Frames),
		   AvgUs(Acc[PH_CLSBOX], Frames),
		   AvgUs(Acc[PH_CLS], Frames));
	printf("P2 cp%lu gr%lu an%lu\n",
		   AvgUs(Acc[PH_CPYSCR], Frames),
		   AvgUs(Acc[PH_GRID], Frames),
		   AvgUs(Acc[PH_ANIM], Frames));
	printf("P3 ob%lu ov%lu pr%lu\n",
		   AvgUs(Acc[PH_OBJ3D], Frames),
		   AvgUs(Acc[PH_OVER], Frames),
		   AvgUs(Acc[PH_PRESENT], Frames));
	printf("P4 ot%lu sf%lu si%lu\n",
		   AvgUs(Acc[PH_OTHER], Frames),
		   AvgUs(SceneFullTicks, FullFrames),
		   AvgUs(SceneIncrTicks, incr));
	printf("P5 n%lu q%lu o%lu fps%u\n",
		   Frames, FullFrames,
		   Frames ? ObjCalls / Frames : 0,
		   NbFramePerSecond);
	{
		/* DTCM stack high-water: bytes of the 0x5A boot paint (main_nds.c)
		   still intact above .dtcm.bss = worst-case margin before the
		   PORT_FASTBSS data gets smashed. */
		extern char __dtcm_bss_end[];
		char *p = __dtcm_bss_end;
		u32 free = 0;

		while (p[free] == 0x5A)
			free++;
		printf("P6 stk%lu\n", free);
	}

	memset(Acc, 0, sizeof(Acc));
	Frames = FullFrames = 0;
	SceneFullTicks = SceneIncrTicks = 0;
	ObjCalls = 0;
}

/* ------------------------------------------------------------------ */
/* present hook (called from nds_video.c PresentRect640)               */
/* ------------------------------------------------------------------ */
static u32 PresentT0;

void PROF_PresentBegin(void)
{
	PresentT0 = Now();
}

void PROF_PresentEnd(void)
{
	ProfAdd(PH_PRESENT, Now() - PresentT0);
}

/* ------------------------------------------------------------------ */
/* --wrap interceptors                                                 */
/* ------------------------------------------------------------------ */
void __real_AffScene(LONG flagflip);

/* touch UI (nds_ui.c): "the MainLoop is alive and rendering" stamp used
 * by its gameplay gate — piggybacks on the existing AffScene wrap */
extern volatile ULONG PORT_UI_SceneStamp;
extern volatile int PORT_UI_SceneSeen;

void __wrap_AffScene(LONG flagflip)
{
	u32 t0 = Now(), t1, scene;

	/* discard absurd gaps (menu screens, cube changes: no AffScene calls
	   for seconds) so PH_LOGIC stays meaningful */
	if (LastSceneEnd && (t0 - LastSceneEnd) < 523656 / 2)
		ProfAdd(PH_LOGIC, t0 - LastSceneEnd);

	InSum = 0;
	InScene = 1;
#ifdef PROF_FORCE_FULL
	/* diagnostic build (make PROFFULL=1): force the full-redraw path every
	   frame — reproduces the sustained "redraw pieno" case (camera scroll)
	   deterministically, since the walled first scene never scrolls. */
	flagflip = 1;
#endif
	__real_AffScene(flagflip);
	InScene = 0;

	PORT_UI_SceneStamp = TimerSystem;
	PORT_UI_SceneSeen = 1;

	t1 = Now();
	scene = t1 - t0;
	ProfAdd(PH_OTHER, scene - InSum);
	if (flagflip)
	{
		FullFrames++;
		SceneFullTicks += scene;
	}
	else
		SceneIncrTicks += scene;

	LastSceneEnd = t1;
	if (++Frames >= PROF_WINDOW)
		ProfReport();
}

#define WRAP_VOID(name, phase, proto, args)     \
	void __real_##name proto;                   \
	void __wrap_##name proto                    \
	{                                           \
		u32 t0 = Now();                         \
		__real_##name args;                     \
		ProfAdd(phase, Now() - t0);             \
	}

#define WRAP_LONG(name, phase, proto, args)     \
	LONG __real_##name proto;                   \
	LONG __wrap_##name proto                    \
	{                                           \
		u32 t0 = Now();                         \
		LONG r = __real_##name args;            \
		ProfAdd(phase, Now() - t0);             \
		return r;                               \
	}

WRAP_VOID(Cls, PH_CLS, (void), ())
WRAP_VOID(CopyScreen, PH_CPYSCR, (void *src, void *dst), (src, dst))
WRAP_VOID(ClsBoxes, PH_CLSBOX, (void), ())
WRAP_VOID(AffGrille, PH_GRID, (void), ())
WRAP_VOID(DrawOverBrick, PH_OVER, (WORD xm, WORD ym, WORD zm), (xm, ym, zm))
WRAP_VOID(DrawOverBrick3, PH_OVER, (WORD xm, WORD ym, WORD zm), (xm, ym, zm))
WRAP_LONG(SetInterAnimObjet2, PH_ANIM,
		  (WORD framedest, UBYTE *ptranimdest, UBYTE *ptrobj),
		  (framedest, ptranimdest, ptrobj))

LONG __real_AffObjetIso(LONG xwr, LONG ywr, LONG zwr,
						LONG palpha, LONG pbeta, LONG pgamma, void *ptrobj);

LONG __wrap_AffObjetIso(LONG xwr, LONG ywr, LONG zwr,
						LONG palpha, LONG pbeta, LONG pgamma, void *ptrobj)
{
	u32 t0 = Now();
	LONG r = __real_AffObjetIso(xwr, ywr, zwr, palpha, pbeta, pgamma, ptrobj);

	ProfAdd(PH_OBJ3D, Now() - t0);
	ObjCalls++;
	return r;
}
