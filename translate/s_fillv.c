/*----------------------------------------------------------------------------
 *  s_fillv.c — C translation of LIB386/LIB_SVGA/S_FILLV.ASM (Adeline 1993)
 *
 *  Scanline polygon fillers.  For each screen line i in [Ymin..Ymax] the
 *  rasterizer (S_POLY / P_OB_ISO) has stored the left/right X in
 *  TabVerticG[i]/TabVerticD[i] and the left/right 8.8 intensities in
 *  TabCoulG[i]/TabCoulD[i].  The original ASM addressed TabVerticD as
 *  TabVerticG+960 bytes and TabCoulD as TabCoulG+960 bytes; here the
 *  separate arrays are used explicitly (same data, 480 entries each).
 *
 *  All 16-bit register arithmetic (8.8 fixed point accumulators, staggered
 *  carry tricks, rol-based dithering) is reproduced bit-exactly with UWORD
 *  operations.  The line stride is hard-coded to 640, as in the ASM.
 *
 *  Fill type table (jmp TabJumpPoly[type*4]):
 *      0 Triste  1 Tele  2 Copper  3 Bopper  4 Marbre
 *      5 Trans   6 Trame 7 Gouraud 8 Dith
 *  SetFillDetails(level 0/1/2) swaps the table (level 0 replaces Tele with
 *  Triste and Gouraud/Dith with Triche; level 1 replaces Dith with Gouraud).
 *---------------------------------------------------------------------------*/
#include <string.h>

#include "translate.h"

#define MAX_TYPE_POLY 9

typedef void (*POLYPROC)(ULONG coul, LONG nblig, UBYTE *line,
                         WORD *xg, WORD *xd, WORD *cg, WORD *cd);

/* x86 "rol r8, cl": count is masked to 5 bits, and an 8-bit rotation is
   cyclic modulo 8. */
PORT_FASTCODE static UBYTE Rol8(UBYTE v, LONG count)
{
	count &= 7;
	if (count == 0)
	{
		return v;
	}
	return (UBYTE)((UBYTE)(v << count) | (v >> (8 - count)));
}

/*───────────────────────────── TRISTE (flat) ──────────────────────────────*/
/* ASM: SVGAPolyTriste */
PORT_FASTCODE static void SVGAPolyTriste(ULONG coul, LONG nblig, UBYTE *line,
                           WORD *xg, WORD *xd, WORD *cg, WORD *cd)
{
	UWORD g, d;
	LONG w;

	(void)cg; (void)cd;

	for (; nblig; nblig--)
	{
		d = (UWORD)*xd++;
		g = (UWORD)*xg++;
		if (d >= g)
		{
			w = (LONG)(UWORD)(d - g) + 1;
			memset(line + g, (UBYTE)coul, (size_t)w);
		}
		line += 640;
	}
}

/*──────────────────────────────── COPPER ──────────────────────────────────*/
/* ASM: SVGAPolyCopper — flat fill with the colour cycling up then down by 1
   every line inside a 16-colour ramp.  Faithful x86 quirks kept:
   - fill = [1 odd-address byte] + (len & ~3) dwords-bytes + (len & 2) bytes:
     when (len & 3) is 1 or 3 the last byte(s) of the span are NOT drawn;
   - in "up" mode the colour is decremented even on empty lines. */
PORT_FASTCODE static void SVGAPolyCopper(ULONG coul, LONG nblig, UBYTE *line,
                           WORD *xg, WORD *xd, WORD *cg, WORD *cd)
{
	UBYTE bl = (UBYTE)coul;
	UWORD g, d;
	LONG cnt, nfill;
	UBYTE *p;

	(void)cg; (void)cd;

blig:                               /* down mode: colour increments */
	d = (UWORD)*xd++;
	g = (UWORD)*xg++;
	if (d < g)
	{
		goto sensdown;
	}
	cnt = (LONG)(UWORD)(d - g) + 1;
	p = line + g;
	if (g & 1)                      /* test edi,1 (Log assumed even-aligned) */
	{
		*p++ = bl;
		cnt--;
	}
	nfill = (cnt & ~3L) + (cnt & 2L);
	memset(p, bl, (size_t)nfill);

	bl++;
	if ((bl & 15) == 0)
	{
		goto sensup;
	}
sensdown:
	line += 640;
	if (--nblig)
	{
		goto blig;
	}
	return;

blig2:                              /* up mode: colour decrements */
	d = (UWORD)*xd++;
	g = (UWORD)*xg++;
	if (d < g)
	{
		goto sensup;
	}
	cnt = (LONG)(UWORD)(d - g) + 1;
	p = line + g;
	if (g & 1)
	{
		*p++ = bl;
		cnt--;
	}
	nfill = (cnt & ~3L) + (cnt & 2L);
	memset(p, bl, (size_t)nfill);

sensup:
	bl--;
	if ((bl & 15) == 0)
	{
		goto sensdown;
	}
	line += 640;
	if (--nblig)
	{
		goto blig2;
	}
	return;
}

/*──────────────────────────────── BOPPER ──────────────────────────────────*/
/* ASM: SVGAPolyBopper — like Copper but the colour changes every 2 lines
   (and the whole span is filled: no missing-byte quirk). */
PORT_FASTCODE static void SVGAPolyBopper(ULONG coul, LONG nblig, UBYTE *line,
                           WORD *xg, WORD *xd, WORD *cg, WORD *cd)
{
	UBYTE bl = (UBYTE)coul;
	UBYTE bh = 2;
	UWORD g, d;
	LONG w;

	(void)cg; (void)cd;

blig:                               /* down mode */
	d = (UWORD)*xd++;
	g = (UWORD)*xg++;
	if (d < g)
	{
		goto sensdown;
	}
	w = (LONG)(UWORD)(d - g) + 1;
	memset(line + g, bl, (size_t)w);

	if (--bh != 0)
	{
		goto sensdown;
	}
	bh = 2;
	bl++;
	if ((bl & 15) == 0)
	{
		goto sensup;
	}
sensdown:
	line += 640;
	if (--nblig)
	{
		goto blig;
	}
	return;

blig2:                              /* up mode */
	d = (UWORD)*xd++;
	g = (UWORD)*xg++;
	if (d < g)
	{
		goto sensup;
	}
	w = (LONG)(UWORD)(d - g) + 1;
	memset(line + g, bl, (size_t)w);

	if (--bh != 0)
	{
		goto plusloin;
	}
sensup:
	bh = 2;
	bl--;
	if ((bl & 15) == 0)
	{
		goto sensdown;
	}
plusloin:
	line += 640;
	if (--nblig)
	{
		goto blig2;
	}
	return;
}

/*──────────────────────────────── MARBRE ──────────────────────────────────*/
/* ASM: SVGAPolyMarbre — horizontal gradient between the two colours packed
   in coul (low byte = start, next byte = end).  8.8 accumulator with the
   byte-swapped-step / staggered-carry x86 trick, reproduced exactly. */
PORT_FASTCODE static void SVGAPolyMarbre(ULONG coul, LONG nblig, UBYTE *line,
                           WORD *xg, WORD *xd, WORD *cg, WORD *cd)
{
	UBYTE start = (UBYTE)(coul & 0xFF);
	UBYTE end = (UBYTE)((coul >> 8) & 0xFF);
	UWORD g, d;
	LONG diff, w, n;
	UBYTE *p;

	(void)cg; (void)cd;

	for (; nblig; nblig--)
	{
		d = (UWORD)*xd++;
		g = (UWORD)*xg++;
		diff = (LONG)d - (LONG)g;

		if (diff == 0)              /* justone: single pixel = end colour */
		{
			line[g] = end;
		}
		else if (diff > 0)
		{
			LONG num;
			WORD step;
			UWORD stepswap, acc;
			ULONG t;
			int carry;

			w = diff + 1;

			/* 16-bit: ((end<<8) - (start<<8) + 1), cwd, idiv w */
			num = (WORD)((UWORD)(end << 8) - (UWORD)(start << 8) + 1);
			step = (WORD)(num / w);
			stepswap = (UWORD)(((UWORD)step << 8) | ((UWORD)step >> 8));

			/* al = start colour, ah = fraction byte of the step */
			acc = (UWORD)((stepswap & 0xFF00) | start);
			carry = 0;

			p = line + g;
			for (n = w; n; n--)
			{
				*p++ = (UBYTE)(acc & 0xFF);
				t = (ULONG)acc + stepswap + (ULONG)carry;   /* adc ax,dx */
				acc = (UWORD)t;
				carry = (int)(t >> 16);
			}
		}
		line += 640;
	}
}

/*───────────────────────────────── TELE ───────────────────────────────────*/
/* ASM: SVGAPolyTele — TV noise: colour + pseudo-random 0..3.  The 16-bit
   generator (ax accumulator seeded with xG, bx seeded with 17371 evolving
   by rol 2 / inc across the whole polygon) is reproduced exactly. */
PORT_FASTCODE static void SVGAPolyTele(ULONG coul, LONG nblig, UBYTE *line,
                         WORD *xg, WORD *xd, WORD *cg, WORD *cd)
{
	UBYTE dl = (UBYTE)coul;
	UWORD bx = 17371;
	UWORD ax;
	UWORD g, d;
	LONG w, n;
	UBYTE al;
	UBYTE *p;

	(void)cg; (void)cd;

	for (; nblig; nblig--)
	{
		d = (UWORD)*xd++;
		g = (UWORD)*xg++;
		if (d >= g)
		{
			w = (LONG)(UWORD)(d - g) + 1;
			p = line + g;
			ax = g;                     /* ax enters the loop as xG */
			for (n = w; n; n--)
			{
				ax = (UWORD)(ax + bx);
				al = (UBYTE)(((ax & 0xFF) & 3) + dl);
				ax = (UWORD)((ax & 0xFF00) | al);
				*p++ = al;
				bx = (UWORD)((UWORD)(bx << 2) | (bx >> 14));    /* rol bx,2 */
				bx++;
			}
		}
		line += 640;
	}
}

/*───────────────────────────────── TRANS ──────────────────────────────────*/
/* ASM: SVGAPolyTrans — transparency: replace the high nibble of the screen
   pixels with the high nibble of the colour (word writes done byte-wise:
   same result, no unaligned access). */
PORT_FASTCODE static void SVGAPolyTrans(ULONG coul, LONG nblig, UBYTE *line,
                          WORD *xg, WORD *xd, WORD *cg, WORD *cd)
{
	UBYTE nib = (UBYTE)(coul & 0xF0);
	UWORD g, d;
	LONG half, n;
	UBYTE *p;

	(void)cg; (void)cd;

	for (; nblig; nblig--)
	{
		d = (UWORD)*xd++;
		g = (UWORD)*xg++;
		if (d >= g)
		{
			LONG w = (LONG)(UWORD)(d - g) + 1;

			p = line + g;
			half = w >> 1;
			if (w & 1)
			{
				*p = (UBYTE)((*p & 0x0F) | nib);
				if (half == 0)
				{
					goto nextline;
				}
				p++;
			}
			for (n = half; n; n--)
			{
				p[0] = (UBYTE)((p[0] & 0x0F) | nib);
				p[1] = (UBYTE)((p[1] & 0x0F) | nib);
				p += 2;
			}
		}
nextline:
		line += 640;
	}
}

/*───────────────────────────────── TRAME ──────────────────────────────────*/
/* ASM: SVGAPolyTrame — checkerboard: one pixel out of two, the phase
   toggling on every non-empty line (and depending on the screen address
   parity; Log is assumed even-aligned so the parity is that of x). */
PORT_FASTCODE static void SVGAPolyTrame(ULONG coul, LONG nblig, UBYTE *line,
                          WORD *xg, WORD *xd, WORD *cg, WORD *cd)
{
	UBYTE bl = (UBYTE)coul;
	UBYTE bh = 0;
	UWORD g, d;
	LONG n, cnt;
	UBYTE *p;

	(void)cg; (void)cd;

	for (; nblig; nblig--)
	{
		d = (UWORD)*xd++;
		g = (UWORD)*xg++;
		if (d >= g)
		{
			LONG w = (LONG)(UWORD)(d - g) + 1;

			cnt = w >> 1;
			if (cnt != 0)
			{
				bh ^= 1;
				p = line + g;
				if ((UBYTE)((g & 1) ^ bh) != 0)
				{
					p++;
				}
				for (n = cnt; n; n--)
				{
					*p = bl;
					p += 2;
				}
			}
		}
		line += 640;
	}
}

/*──────────────────────────────── TRICHE ──────────────────────────────────*/
/* ASM: SVGAPolyTriche — "cheap gouraud": flat fill with the integer part of
   the left intensity.  Note (faithful): the intensity pointer advances only
   on non-empty lines, so it desynchronizes after a skipped line. */
PORT_FASTCODE static void SVGAPolyTriche(ULONG coul, LONG nblig, UBYTE *line,
                           WORD *xg, WORD *xd, WORD *cg, WORD *cd)
{
	UWORD g, d;
	LONG w;

	(void)coul; (void)cd;

	for (; nblig; nblig--)
	{
		d = (UWORD)*xd++;
		g = (UWORD)*xg++;
		if (d >= g)
		{
			w = (LONG)(UWORD)(d - g) + 1;
			memset(line + g, (UBYTE)((UWORD)*cg >> 8), (size_t)w);
			cg++;                   /* add ebx,2 — only when drawn */
		}
		line += 640;
	}
}

/*─────────────────────────────── GOURAUD ──────────────────────────────────*/
/* ASM: SVGAPolyGouraud — linear intensity interpolation between the 8.8
   left/right intensities.  The remainder of the 16-bit div is discarded
   (unlike the commented-out "S" version).  Two symmetric loops handle the
   increasing / decreasing cases; short spans (1..3 px) have special cases.
   Faithful: TabCoul pointers do not advance on skipped (negative) lines. */
PORT_FASTCODE static void SVGAPolyGouraud(ULONG coul, LONG nblig, UBYTE *line,
                            WORD *xg, WORD *xd, WORD *cg, WORD *cd)
{
	LONG newloop = nblig;
	UWORD cx, ax, dx;
	UWORD g, d;
	UBYTE *edi;
	LONG cnt;
	ULONG t;

	(void)coul;

blig0:                              /* forward loop (intensity increases) */
	d = (UWORD)*xd++;
	g = (UWORD)*xg++;
	edi = line + g;
	cx = (UWORD)(d - g);
	ax = (UWORD)*cd;                /* intensite 2 */
	dx = (UWORD)*cg;                /* intensite 1 */
	if (d == g)
	{
		goto l0;
	}
	if (d < g)
	{
		goto nextlig;
	}

	if (ax < dx)                    /* sub ax,dx ; jc GouraudInverse */
	{
		ax = (UWORD)(ax - dx);
		goto GouraudInverse;
	}
	ax = (UWORD)(ax - dx);
	if (cx <= 2)
	{
		goto opt;
	}

gouraud:
	ax = (UWORD)(ax / cx);          /* div cx: step, remainder discarded */
	dx = (UWORD)*cg;
	cg++;
	cd++;
	for (cnt = (LONG)cx + 1; cnt; cnt--)
	{
		*edi++ = (UBYTE)(dx >> 8);
		dx = (UWORD)(dx + ax);
	}
	goto nextlig;

GouraudNormal:                      /* from inverse loop: delta >= 0 again */
	if (cx <= 2)
	{
		goto opt;
	}
	ax = (UWORD)(0 - ax);
	goto gouraud;

opt:                                /* 2 or 3 pixels (opt0/opt) */
	ax = (UWORD)*cd;                /* intensite 2 */
	dx = (UWORD)*cg;                /* intensite 1 */
	cg++;
	cd++;
	edi[cx] = (UBYTE)(ax >> 8);
	if (cx == 2)
	{
		t = ((ULONG)ax + dx) >> 1;
		edi[1] = (UBYTE)((t >> 8) & 0xFF);
	}
	edi[0] = (UBYTE)(dx >> 8);
	goto nextlig;

l0:                                 /* 1 pixel: average of both */
	t = ((ULONG)ax + dx) >> 1;
	cg++;
	cd++;
	edi[0] = (UBYTE)((t >> 8) & 0xFF);
nextlig:
	line += 640;
	if (--newloop)
	{
		goto blig0;
	}
	return;

GouraudInverse:                     /* from forward loop: delta < 0 */
	if (cx <= 2)
	{
		goto iopt;
	}
	ax = (UWORD)(0 - ax);
	goto igouraud;

iblig0:                             /* inverse loop (intensity decreases) */
	d = (UWORD)*xd++;
	g = (UWORD)*xg++;
	edi = line + g;
	cx = (UWORD)(d - g);
	ax = (UWORD)*cg;                /* intensite 1 */
	dx = (UWORD)*cd;                /* intensite 2 */
	if (d == g)
	{
		goto il0;
	}
	if (d < g)
	{
		goto inextlig;
	}

	if (ax < dx)                    /* sub ax,dx ; jc GouraudNormal */
	{
		ax = (UWORD)(ax - dx);
		goto GouraudNormal;
	}
	ax = (UWORD)(ax - dx);
	if (cx <= 2)
	{
		goto iopt;
	}

igouraud:
	ax = (UWORD)(ax / cx);
	dx = (UWORD)*cg;
	cg++;
	cd++;
	for (cnt = (LONG)cx + 1; cnt; cnt--)
	{
		*edi++ = (UBYTE)(dx >> 8);
		dx = (UWORD)(dx - ax);
	}
	goto inextlig;

iopt:                               /* 2 or 3 pixels (iopt0/iopt) */
	ax = (UWORD)*cg;                /* intensite 1 */
	dx = (UWORD)*cd;                /* intensite 2 */
	cg++;
	cd++;
	edi[cx] = (UBYTE)(dx >> 8);
	if (cx == 2)
	{
		t = ((ULONG)dx + ax) >> 1;
		edi[1] = (UBYTE)((t >> 8) & 0xFF);
	}
	edi[0] = (UBYTE)(ax >> 8);
	goto inextlig;

il0:                                /* 1 pixel: average */
	t = ((ULONG)ax + dx) >> 1;
	cg++;
	cd++;
	edi[0] = (UBYTE)((t >> 8) & 0xFF);
inextlig:
	line += 640;
	if (--newloop)
	{
		goto iblig0;
	}
	return;
}

/*───────────────────────────────── DITH ───────────────────────────────────*/
/* ASM: SVGAPolyDith — gouraud with ordered dithering: the low (fractional)
   byte of the accumulator is rotated by the current loop counter and added
   back as noise before taking the integer part.  Bit-exact transliteration. */
PORT_FASTCODE static void SVGAPolyDith(ULONG coul, LONG nblig, UBYTE *line,
                         WORD *xg, WORD *xd, WORD *cg, WORD *cd)
{
	LONG newloop = nblig;
	UWORD cx, ax, dx, si;
	UWORD g, d;
	UBYTE *edi;
	LONG cnt;

	(void)coul;

blig0:
	d = (UWORD)*xd++;
	g = (UWORD)*xg++;
	cx = (UWORD)(d - g);
	if (d < g)
	{
		goto nextlig;
	}

	edi = line + g;
	ax = (UWORD)*cd;                /* intensite 2 */
	si = (UWORD)*cg;                /* intensite 1 */
	if (d == g)
	{
		goto cx0;
	}

	cg++;
	cd++;
	ax = (UWORD)(ax - si);          /* delta intensite (may wrap) */

	if (cx <= 2)
	{
		goto opt;
	}

	/* cwd ; idiv cx */
	ax = (UWORD)((LONG)(WORD)ax / (LONG)cx);

	dx = si;
	cnt = ((LONG)cx + 1) >> 1;
	if ((cx + 1) & 1)
	{
		/* odd entry */
		dx &= 0x00FF;               /* xor dh,dh */
		dx = Rol8((UBYTE)dx, cnt);  /* rol dl,cl */
		dx = (UWORD)(dx + si);
		edi[0] = (UBYTE)(dx >> 8);
		if (cnt == 0)
		{
			goto nextlig;           /* dead branch in practice (cx > 2) */
		}
		edi++;
		si = (UWORD)(si + ax);      /* start2 */
	}

	for (;;)
	{
		/* start */
		dx = (UWORD)((dx & 0x00FF) + si);
		edi[0] = (UBYTE)(dx >> 8);
		dx &= 0x00FF;
		si = (UWORD)(si + ax);
		dx = Rol8((UBYTE)dx, cnt);  /* rol dl,cl — cl is the counter */
		dx = (UWORD)(dx + si);
		cnt--;
		edi[1] = (UBYTE)(dx >> 8);
		if (cnt == 0)
		{
			break;
		}
		edi += 2;
		si = (UWORD)(si + ax);      /* start2 */
	}

nextlig:
	line += 640;
	if (--newloop)
	{
		goto blig0;
	}
	return;

cx0:                                /* 1 pixel: exact 17-bit average */
	{
		ULONG t = (ULONG)ax + si;   /* add ax,si (keeps carry) */
		UWORD r = (UWORD)(t >> 1);  /* rcr ax,1 */
		cg++;
		cd++;
		edi[0] = (UBYTE)(r >> 8);
	}
	line += 640;
	if (--newloop)
	{
		goto blig0;
	}
	return;

opt:                                /* 2 or 3 pixels; ax = raw delta */
	dx = si;
	if ((cx + 1) & 1)               /* cx == 2 (3 pixels) */
	{
		dx &= 0x00FF;
		ax = (UWORD)((WORD)ax >> 1);        /* sar ax,1 */
		dx = Rol8((UBYTE)dx, 1);            /* cl = 1 */
		edi++;
		dx = (UWORD)(dx + si);
		edi[-1] = (UBYTE)(dx >> 8);
		si = (UWORD)(si + ax);
	}
	/* two */
	dx = (UWORD)((dx & 0x00FF) + si);
	edi[0] = (UBYTE)(dx >> 8);
	dx &= 0x00FF;
	si = (UWORD)(si + ax);
	dx = Rol8((UBYTE)dx, 1);                /* cl = 1 */
	line += 640;
	dx = (UWORD)(dx + si);
	edi[1] = (UBYTE)(dx >> 8);
	if (--newloop)
	{
		goto blig0;
	}
	return;
}

/*────────────────────────────── dispatcher ────────────────────────────────*/

static POLYPROC TabJumpPoly[MAX_TYPE_POLY] =
{
	SVGAPolyTriste, SVGAPolyTele, SVGAPolyCopper, SVGAPolyBopper,
	SVGAPolyMarbre, SVGAPolyTrans, SVGAPolyTrame, SVGAPolyGouraud,
	SVGAPolyDith
};

static const POLYPROC TabPoly_0[MAX_TYPE_POLY] =
{
	SVGAPolyTriste, SVGAPolyTriste, SVGAPolyCopper, SVGAPolyBopper,
	SVGAPolyMarbre, SVGAPolyTrans, SVGAPolyTrame, SVGAPolyTriche,
	SVGAPolyTriche
};

static const POLYPROC TabPoly_1[MAX_TYPE_POLY] =
{
	SVGAPolyTriste, SVGAPolyTele, SVGAPolyCopper, SVGAPolyBopper,
	SVGAPolyMarbre, SVGAPolyTrans, SVGAPolyTrame, SVGAPolyGouraud,
	SVGAPolyGouraud
};

static const POLYPROC TabPoly_2[MAX_TYPE_POLY] =
{
	SVGAPolyTriste, SVGAPolyTele, SVGAPolyCopper, SVGAPolyBopper,
	SVGAPolyMarbre, SVGAPolyTrans, SVGAPolyTrame, SVGAPolyGouraud,
	SVGAPolyDith
};

/* ASM: SetFillDetails */
void SetFillDetails(LONG level)
{
	const POLYPROC *src;

	if ((ULONG)level > 2)           /* cmp/jbe: unsigned clamp */
	{
		level = 2;
	}

	if (level == 0)
	{
		src = TabPoly_0;
	}
	else if (level == 1)
	{
		src = TabPoly_1;
	}
	else
	{
		src = TabPoly_2;
	}

	memcpy(TabJumpPoly, src, sizeof(TabJumpPoly));
}

/* ASM: FillVertic_A — register-based entry (also called by P_OB_ISO). */
PORT_FASTCODE void FillVertic_A(LONG typepoly, LONG coulpoly)
{
	ULONG *pTabOffLine = TABOFFLINE;
	ULONG y0 = (UWORD)Ymin;
	ULONG y1 = (UWORD)Ymax;
	UBYTE *line;
	LONG nblig;

	line = Log + pTabOffLine[y0];
	nblig = (LONG)y1 - (LONG)y0 + 1;

	TabJumpPoly[typepoly]((ULONG)coulpoly, nblig, line,
	                      &TabVerticG[y0], &TabVerticD[y0],
	                      &TabCoulG[y0], &TabCoulD[y0]);
}

/* ASM: FillVertic */
void FillVertic(LONG typepoly, LONG coulpoly)
{
	FillVertic_A(typepoly, coulpoly);
}
