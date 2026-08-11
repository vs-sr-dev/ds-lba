/*----------------------------------------------------------------------------
 *  texture.c — C translation of LIB386/LIB_SVGA/TEXTURE.ASM (Adeline 1993)
 *
 *  Textured (affine) triangle rasterizer — used ONLY by the holomap
 *  (engine/game/HOLOMAP.C, the rotating Twinsun planetarium).
 *
 *  Flow
 *  ----
 *  HOLOMAP.C fills the shared triplet buffers:
 *      TabPoly[1,2 / 4,5 / 7,8] = screen (x,y) of the 3 vertices
 *      TabText[1,2 / 4,5 / 7,8] = texture (u,v) of the 3 vertices, 8.8 fixed
 *                                 (0 .. 255*256+255 over the 256x256 map)
 *  then calls AsmTexturedTriangleNoClip() followed by
 *  FillTextPolyNoClip(LYmin, LYmax, PtrMap).
 *
 *  AsmTexturedTriangleNoClip scan-converts each of the 3 edges twice (once
 *  per direction) into six per-scanline WORD tables via the shared edge DDA
 *  A_FillPropNoClip:
 *      y_a < y_b edge -> left  side: TabGauche (x), TabX0 (u), TabY0 (v)
 *      y_a > y_b edge -> right side: TabDroite (x), TabX1 (u), TabY1 (v)
 *  and accumulates LYmin/LYmax (horizontal edges contribute nothing — a
 *  fully degenerate triangle leaves LYmin=32000/LYmax=-32000; HOLOMAP.C's
 *  TestVuePoly() rejects those before we are called).
 *
 *  In the ASM .data these six tables are one contiguous block addressed as
 *  TabGauche + 960*n.  In this port they are the separate arrays exported by
 *  translate/s_poly.c with the aliasing of the original layout:
 *      TabGauche = TabVerticG   TabDroite = TabVerticD
 *      TabX0     = TabCoulG     TabY0     = TabCoulD
 *      TabX1, TabY1 as themselves.
 *
 *  FillTextPolyNoClip then walks the scanlines: for each row it derives the
 *  affine texture steps  xstep = (u1-u0+1)/len,  ystep = (v1-v0+1)/len
 *  (note the faithful "+1" bias on both numerators) and samples the 256x256
 *  map with 16-bit 8.8 accumulators:  texel = map[(v & 0xFF00) | (u >> 8)]
 *  which wraps the planet texture for free in both axes.  Purely affine —
 *  no perspective correction.
 *
 *  Everything after the END directive in TEXTURE.ASM (M_FillTextPoly,
 *  M_AsmFillProp, disptexture, a second AsmTexturedTriangleNoClip) is dead
 *  code and is not translated.  AsmFillProp / FillTextPoly /
 *  FillTextPolyShade / AsmGouraudTriangleNoClip are live exported code with
 *  no C caller in LBA1: translated for completeness, kept faithful.
 *
 *  Faithful quirks kept (for the future pixel-perfect comparison):
 *  - edge DDA fraction seed: rem/2 + 0x7FFF in the NoClip entries, but the
 *    bare remainder in the clipped AsmFillProp (its shr/add pair is
 *    commented out in the ASM);
 *  - the DDA is one 32-bit adc/sbb per entry: the fraction lives in the
 *    high word, its overflow reaches the X low word only via the carry flag
 *    on the NEXT iteration, and a wrap of the X low word carries into the
 *    fraction — reproduced with the exact 32-bit carry chain;
 *  - deltaY+1 entries are written per edge; deltaY == 0 in the *FillProp*
 *    entries would be a division by zero exactly as on DOS (callers only
 *    invoke them with strictly different Ys);
 *  - triangle vertex coordinates are read with movzx: negative WORDs would
 *    become 32000+ positives (never happens from HOLOMAP.C);
 *  - span length is xd-xg pixels in FillTextPolyNoClip but
 *    min(xd,ClipXmax)-max(xg,ClipXmin)+1 pixels in the clipped
 *    FillTextPoly/FillTextPolyShade (one MORE pixel for the same span);
 *  - u/v accumulators and their per-pixel steps are 16-bit (add dx,si /
 *    add bx,bp): the 32-bit idiv quotient is truncated to its low word;
 *  - FillTextPoly/Shade left clip rewrites TabX0/TabY0 (TabCoulG/TabCoulD)
 *    in place with 16-bit products (imul word, low half kept);
 *  - row/pixel counters are 16-bit (dec word ptr): a row count of 0 would
 *    iterate 65536 times, as on x86;
 *  - the screen stride is hardcoded 640 (like S_LINE/S_FILLV);
 *  - AsmFillProp compares the clip bounds as 32 bits: on DOS ClipYmin/max
 *    are dd so this is simply the full value; here the WORDs are widened
 *    (identical for the 0..479 values the port uses).
 *---------------------------------------------------------------------------*/
#include "lib3d_p.h"

/*──────────────────────────── data (TEXTURE.ASM .data) ─────────────────────*/

WORD TabText[32 * 3];               /* public — (c,u,v) triplets like TabPoly */

LONG LYmin = 0;                     /* public — triangle vertical extent      */
LONG LYmax = 0;

/*═════════════════════════════ EDGE DDA ════════════════════════════════════*/

/* Common stosw/adc (x ascending) and stosw/sbb (x descending) loops of the
   FillProp family.  dst points at entry y0, deltay+1 entries are written.
   noclipseed selects the fraction seed:
     1 -> rem/2 + 7FFFh   (A_FillPropNoClip / AsmFillPropNoClip)
     0 -> rem             (AsmFillProp: the shr/add pair is commented out)   */
static void FillPropDDA(WORD *dst, LONG x0, LONG x1, ULONG deltay,
                        int noclipseed)
{
	UWORD delta16, seed;
	ULONG quot, rem, step, acc, n;
	unsigned long long t64;
	int carry;

	n = deltay + 1;                     /* inc ecx                           */

	if (x0 <= x1)                       /* cmp ebx,edx / jg afp1             */
	{
		/* x ascending: mov ax,dx / sub ax,bx (16-bit delta) */
		delta16 = (UWORD)((UWORD)x1 - (UWORD)x0);

		/* shl eax,16 / xor edx,edx / div ecx */
		quot = ((ULONG)delta16 << 16) / deltay;
		rem  = ((ULONG)delta16 << 16) % deltay;

		if (noclipseed)
		{
			seed = (UWORD)(((UWORD)rem >> 1) + 0x7FFFu);   /* shr ax,1 / add ax,7FFFh */
		}
		else
		{
			seed = (UWORD)rem;
		}

		step = (quot << 16) | (quot >> 16);        /* rol edx,16            */
		acc  = ((ULONG)seed << 16) | (UWORD)x0;    /* shl eax,16 / mov ax,bx */
		carry = 0;                                 /* clc                   */

		do
		{
			*dst++ = (WORD)(UWORD)acc;             /* stosw                 */
			t64 = (unsigned long long)acc + step + (unsigned)carry;
			carry = (int)(t64 >> 32);              /* adc eax,edx           */
			acc = (ULONG)t64;
		}
		while (--n);
	}
	else
	{
		/* x descending: mov ax,bx / sub ax,dx */
		delta16 = (UWORD)((UWORD)x0 - (UWORD)x1);

		quot = ((ULONG)delta16 << 16) / deltay;
		rem  = ((ULONG)delta16 << 16) % deltay;

		if (noclipseed)
		{
			seed = (UWORD)(((UWORD)rem >> 1) + 0x7FFFu);
		}
		else
		{
			seed = (UWORD)rem;
		}

		step = (quot << 16) | (quot >> 16);
		acc  = ((ULONG)seed << 16) | (UWORD)x0;    /* mov ax,bx (start x)   */
		carry = 0;                                 /* clc — borrow here     */

		do
		{
			*dst++ = (WORD)(UWORD)acc;             /* stosw                 */
			t64 = (unsigned long long)step + (unsigned)carry;
			carry = ((unsigned long long)acc < t64);   /* sbb eax,edx       */
			acc = (ULONG)((unsigned long long)acc - t64);
		}
		while (--n);
	}
}

/* ASM: A_FillPropNoClip — register entry (EDI=dest, EBX=x0, EAX=y0, EDX=x1,
   ECX=y1).  Writes dest[y0..y1] (inclusive) with the interpolated X.
   y0 == y1 divides by zero exactly like the DOS build — the triangle
   builders only call it for strictly non-horizontal edges.               */
static void A_FillPropNoClip(WORD *ptrdest, LONG x0, LONG y0, LONG x1, LONG y1)
{
	LONG t;

	if (y0 > y1)                        /* cmp eax,ecx / jle                 */
	{
		t = y0; y0 = y1; y1 = t;        /* xchg eax,ecx                      */
		t = x0; x0 = x1; x1 = t;        /* xchg ebx,edx                      */
	}

	FillPropDDA(ptrdest + y0, x0, x1, (ULONG)(y1 - y0), 1);
}

/* ASM: AsmFillPropNoClip — stack-parameter twin of A_FillPropNoClip.       */
void AsmFillPropNoClip(WORD *ptrdest, LONG x0, LONG y0, LONG x1, LONG y1)
{
	A_FillPropNoClip(ptrdest, x0, y0, x1, y1);
}

/* ASM: AsmFillProp — Y-clipped variant (no C caller in LBA1).  Clips the
   edge to ClipYmin/ClipYmax, recomputing the X endpoints with the exact
   imul/idiv sequence; an edge horizontal after clipping fills nothing
   ("GASP mettre cas 1 ligne").  Fraction seed = raw remainder (quirk).    */
void AsmFillProp(WORD *ptrdest, LONG x0, LONG y0, LONG x1, LONG y1)
{
	LONG t, dx, dy;

	if (y0 > y1)
	{
		t = y0; y0 = y1; y1 = t;
		t = x0; x0 = x1; x1 = t;
	}

	if (y0 > (LONG)ClipYmax)            /* cmp eax,[ClipYmax] (32-bit dd)    */
	{
		return;
	}
	if (y1 < (LONG)ClipYmin)
	{
		return;
	}

	if (y0 < (LONG)ClipYmin)            /* clip_ymin                         */
	{
		dx = x0 - x1;
		dy = y0 - y1;
		if (dy == 0)                    /* jz nofill1                        */
		{
			return;
		}
		/* x0 += ((ClipYmin - y0) * dx) / dy ; y0 = ClipYmin */
		x0 = ADD32(x0, (LONG)(((long long)((LONG)ClipYmin - y0) * dx) / dy));
		y0 = (LONG)ClipYmin;
	}

	if (y1 > (LONG)ClipYmax)            /* clip_ymax (sees the updated x0)   */
	{
		dx = x0 - x1;
		dy = y0 - y1;
		if (dy == 0)                    /* jz nofill2                        */
		{
			return;
		}
		/* x1 += ((ClipYmax - y1) * dx) / dy ; y1 = ClipYmax */
		x1 = ADD32(x1, (LONG)(((long long)((LONG)ClipYmax - y1) * dx) / dy));
		y1 = (LONG)ClipYmax;
	}

	if (y1 - y0 == 0)                   /* sub ecx,eax / jz nofill           */
	{
		return;
	}

	FillPropDDA(ptrdest + y0, x0, x1, (ULONG)(y1 - y0), 0);
}

/*══════════════════════ TRIANGLE EDGE TABLE BUILDERS ═══════════════════════*/

/* ASM: AsmTexturedTriangleNoClip — no parameters: reads TabPoly (screen x,y)
   and TabText (texture u,v), all movzx'd to unsigned.  Each edge feeds the
   left tables when it goes down (y_a < y_b) and the right tables when it
   goes up (y_a > y_b); horizontal edges are skipped.  Updates LYmin/LYmax
   (initialized to 32000/-32000; compares are <= / >= like jg/jl-skip).
   Does NOT clamp to the clip window and does NOT draw: the caller chains
   FillTextPolyNoClip(LYmin, LYmax, map).                                   */
void AsmTexturedTriangleNoClip(void)
{
	LONG px0 = (UWORD)TabPoly[1], py0 = (UWORD)TabPoly[2];
	LONG px1 = (UWORD)TabPoly[4], py1 = (UWORD)TabPoly[5];
	LONG px2 = (UWORD)TabPoly[7], py2 = (UWORD)TabPoly[8];
	LONG tx0 = (UWORD)TabText[1], ty0 = (UWORD)TabText[2];
	LONG tx1 = (UWORD)TabText[4], ty1 = (UWORD)TabText[5];
	LONG tx2 = (UWORD)TabText[7], ty2 = (UWORD)TabText[8];

	LYmin = 32000;
	LYmax = -32000;

	if (py0 < py1)                                          /* ttnc0  */
	{
		if (py0 <= LYmin) LYmin = py0;
		if (py1 >= LYmax) LYmax = py1;
		A_FillPropNoClip(TabVerticG, px0, py0, px1, py1);   /* TabGauche */
		A_FillPropNoClip(TabCoulG,   tx0, py0, tx1, py1);   /* TabX0     */
		A_FillPropNoClip(TabCoulD,   ty0, py0, ty1, py1);   /* TabY0     */
	}
	if (py0 > py1)                                          /* ttnc0a */
	{
		if (py1 <= LYmin) LYmin = py1;
		if (py0 >= LYmax) LYmax = py0;
		A_FillPropNoClip(TabVerticD, px0, py0, px1, py1);   /* TabDroite */
		A_FillPropNoClip(TabX1,      tx0, py0, tx1, py1);
		A_FillPropNoClip(TabY1,      ty0, py0, ty1, py1);
	}
	if (py1 < py2)                                          /* ttnc0b */
	{
		if (py1 <= LYmin) LYmin = py1;
		if (py2 >= LYmax) LYmax = py2;
		A_FillPropNoClip(TabVerticG, px1, py1, px2, py2);
		A_FillPropNoClip(TabCoulG,   tx1, py1, tx2, py2);
		A_FillPropNoClip(TabCoulD,   ty1, py1, ty2, py2);
	}
	if (py1 > py2)                                          /* ttnc0c */
	{
		if (py2 <= LYmin) LYmin = py2;
		if (py1 >= LYmax) LYmax = py1;
		A_FillPropNoClip(TabVerticD, px1, py1, px2, py2);
		A_FillPropNoClip(TabX1,      tx1, py1, tx2, py2);
		A_FillPropNoClip(TabY1,      ty1, py1, ty2, py2);
	}
	if (py2 < py0)                                          /* ttnc0d */
	{
		if (py2 <= LYmin) LYmin = py2;
		if (py0 >= LYmax) LYmax = py0;
		A_FillPropNoClip(TabVerticG, px2, py2, px0, py0);
		A_FillPropNoClip(TabCoulG,   tx2, py2, tx0, py0);
		A_FillPropNoClip(TabCoulD,   ty2, py2, ty0, py0);
	}
	if (py2 > py0)                                          /* ttnc0e */
	{
		if (py0 <= LYmin) LYmin = py0;
		if (py2 >= LYmax) LYmax = py2;
		A_FillPropNoClip(TabVerticD, px2, py2, px0, py0);
		A_FillPropNoClip(TabX1,      tx2, py2, tx0, py0);
		A_FillPropNoClip(TabY1,      ty2, py2, ty0, py0);
	}
}

/* ASM: AsmGouraudTriangleNoClip — same edge walk with the intensities
   TabPoly[0/3/6] interpolated into TabCoulG/TabCoulD instead of texture
   coordinates (the "shl 8" of the intensities is commented out in the ASM).
   Also stores the low words of LYmin/LYmax into Ymin/Ymax at the end.
   No C caller in LBA1 (holomap uses the textured path only).              */
void AsmGouraudTriangleNoClip(void)
{
	LONG pi0 = (UWORD)TabPoly[0], px0 = (UWORD)TabPoly[1], py0 = (UWORD)TabPoly[2];
	LONG pi1 = (UWORD)TabPoly[3], px1 = (UWORD)TabPoly[4], py1 = (UWORD)TabPoly[5];
	LONG pi2 = (UWORD)TabPoly[6], px2 = (UWORD)TabPoly[7], py2 = (UWORD)TabPoly[8];

	LYmin = 32000;
	LYmax = -32000;

	if (py0 < py1)
	{
		if (py0 <= LYmin) LYmin = py0;
		if (py1 >= LYmax) LYmax = py1;
		A_FillPropNoClip(TabVerticG, px0, py0, px1, py1);
		A_FillPropNoClip(TabCoulG,   pi0, py0, pi1, py1);
	}
	if (py0 > py1)
	{
		if (py1 <= LYmin) LYmin = py1;
		if (py0 >= LYmax) LYmax = py0;
		A_FillPropNoClip(TabVerticD, px0, py0, px1, py1);
		A_FillPropNoClip(TabCoulD,   pi0, py0, pi1, py1);
	}
	if (py1 < py2)
	{
		if (py1 <= LYmin) LYmin = py1;
		if (py2 >= LYmax) LYmax = py2;
		A_FillPropNoClip(TabVerticG, px1, py1, px2, py2);
		A_FillPropNoClip(TabCoulG,   pi1, py1, pi2, py2);
	}
	if (py1 > py2)
	{
		if (py2 <= LYmin) LYmin = py2;
		if (py1 >= LYmax) LYmax = py1;
		A_FillPropNoClip(TabVerticD, px1, py1, px2, py2);
		A_FillPropNoClip(TabCoulD,   pi1, py1, pi2, py2);
	}
	if (py2 < py0)
	{
		if (py2 <= LYmin) LYmin = py2;
		if (py0 >= LYmax) LYmax = py0;
		A_FillPropNoClip(TabVerticG, px2, py2, px0, py0);
		A_FillPropNoClip(TabCoulG,   pi2, py2, pi0, py0);
	}
	if (py2 > py0)
	{
		if (py0 <= LYmin) LYmin = py0;
		if (py2 >= LYmax) LYmax = py2;
		A_FillPropNoClip(TabVerticD, px2, py2, px0, py0);
		A_FillPropNoClip(TabCoulD,   pi2, py2, pi0, py0);
	}

	Ymin = (WORD)LYmin;                 /* mov ax,word ptr[LYmin]            */
	Ymax = (WORD)LYmax;
}

/*═══════════════════════════ SPAN FILLERS ══════════════════════════════════*/

/* ASM: FillTextPolyNoClip — draws rows pymin..pymax from the edge tables.
   Per row: len = TabDroite[y] - TabGauche[y] pixels (NO +1: one less than
   the clipped fillers), affine steps (u1-u0+1)/len and (v1-v0+1)/len, then
   len texels map[(v & 0xFF00) | (u >> 8)] with 16-bit 8.8 accumulators.
   No horizontal or vertical clipping at all; stride hardcoded 640.        */
void FillTextPolyNoClip(LONG pymin, LONG pymax, UBYTE *source)
{
	UBYTE *ptrlig;
	LONG idx;
	UWORD loopy;

	ptrlig = Log + TABOFFLINE[pymin];               /* ptrlig                */
	idx = pymin;                                    /* ptrtab                */
	loopy = (UWORD)(pymax - pymin + 1);             /* 16-bit row counter    */

	do
	{
		UBYTE *dst = ptrlig;                        /* mov edi,[ptrlig]      */
		LONG xg  = (WORD)TabVerticG[idx];           /* movsx TAB_GAUCHE      */
		LONG len = (LONG)(WORD)TabVerticD[idx] - xg;

		if (len > 0)                                /* jle l4                */
		{
			LONG ystep, xstep;
			UWORD u, v, us, vs, boucle;

			/* ystep = (y1 - y0 + 1) / len ; xstep = (x1 - x0 + 1) / len
			   (texture coords movzx'd; idiv truncates toward zero)         */
			ystep = ((LONG)(UWORD)TabY1[idx] - (LONG)(UWORD)TabCoulD[idx] + 1) / len;
			xstep = ((LONG)(UWORD)TabX1[idx] - (LONG)(UWORD)TabCoulG[idx] + 1) / len;

			dst += xg;                              /* add edi,eax (signed)  */

			u = (UWORD)TabCoulG[idx];               /* dx = TAB_X0           */
			v = (UWORD)TabCoulD[idx];               /* bx = TAB_Y0           */
			us = (UWORD)xstep;                      /* add dx,si (low word)  */
			vs = (UWORD)ystep;                      /* add bx,bp             */

			boucle = (UWORD)len;
			do
			{
				/* xchg bl,dh: index = (v & FF00h) | (u >> 8) — wraps 256x256 */
				*dst++ = source[(UWORD)((v & 0xFF00u) | (u >> 8))];
				u = (UWORD)(u + us);
				v = (UWORD)(v + vs);
			}
			while (--boucle);
		}

		ptrlig += 640;                              /* add [ptrlig],640      */
		idx++;                                      /* add [ptrtab],2        */
	}
	while (--loopy);
}

/* ASM: FillTextPoly — X-clipped variant (no C caller in LBA1).  Left clip
   rewinds u/v by (xg-ClipXmin)*step using 16-bit products and REWRITES
   TabX0/TabY0 (TabCoulG/TabCoulD) in place; span length gets a +1 the
   NoClip filler does not have.                                            */
void FillTextPoly(LONG pymin, LONG pymax, UBYTE *source)
{
	UBYTE *ptrlig;
	LONG idx;
	UWORD loopy;

	ptrlig = Log + TABOFFLINE[pymin];
	idx = pymin;
	loopy = (UWORD)(pymax - pymin + 1);

	do
	{
		UBYTE *dst = ptrlig;
		LONG xg  = (WORD)TabVerticG[idx];           /* movsx                 */
		LONG len = (LONG)(WORD)TabVerticD[idx] - xg;

		if (len > 0)                                /* jle l4                */
		{
			LONG ystep, xstep;
			UWORD ax;
			WORD xd, cnt;

			ystep = ((LONG)(UWORD)TabY1[idx] - (LONG)(UWORD)TabCoulD[idx] + 1) / len;
			xstep = ((LONG)(UWORD)TabX1[idx] - (LONG)(UWORD)TabCoulG[idx] + 1) / len;

			ax = (UWORD)TabVerticG[idx];            /* xor eax,eax / mov ax  */
			if ((WORD)ax < ClipXmin)                /* 16-bit signed cmp     */
			{
				/* clip x min: d = xg - ClipXmin (negative);
				   TAB_X0 -= low16(d * xstep) ; TAB_Y0 -= low16(d * ystep)   */
				UWORD d = (UWORD)(ax - (UWORD)ClipXmin);

				TabCoulG[idx] = (WORD)(UWORD)((UWORD)TabCoulG[idx]
				                              - (UWORD)((ULONG)d * (UWORD)xstep));
				TabCoulD[idx] = (WORD)(UWORD)((UWORD)TabCoulD[idx]
				                              - (UWORD)((ULONG)d * (UWORD)ystep));
				ax = (UWORD)ClipXmin;
			}
			dst += ax;                              /* add edi,eax (zero-ext) */

			xd = (WORD)TabVerticD[idx];             /* mov cx,TAB_DROITE     */
			if (xd > ClipXmax)                      /* clip x max            */
			{
				xd = ClipXmax;
			}
			cnt = (WORD)(xd - (WORD)ax + 1);        /* sub cx,ax / inc cx    */
			if (cnt > 0)                            /* jle l4                */
			{
				UWORD u  = (UWORD)TabCoulG[idx];    /* (possibly rewritten)  */
				UWORD v  = (UWORD)TabCoulD[idx];
				UWORD us = (UWORD)xstep;
				UWORD vs = (UWORD)ystep;
				UWORD boucle = (UWORD)cnt;

				do
				{
					*dst++ = source[(UWORD)((v & 0xFF00u) | (u >> 8))];
					u = (UWORD)(u + us);
					v = (UWORD)(v + vs);
				}
				while (--boucle);
			}
		}

		ptrlig += 640;
		idx++;
	}
	while (--loopy);
}

/* ASM: FillTextPolyShade — FillTextPoly plus a per-texel darkening of the
   low palette nibble (palette = 16 ramps of 16): l = (texel & 15) - dark,
   clamped at the ramp base on borrow, ramp bits (texel & 240) preserved.
   Only the low byte of `dark` is used.  No C caller in LBA1.              */
void FillTextPolyShade(LONG pymin, LONG pymax, LONG dark, UBYTE *source)
{
	UBYTE darkdec = (UBYTE)dark;                    /* sub al,byte[darkdec]  */
	UBYTE *ptrlig;
	LONG idx;
	UWORD loopy;

	ptrlig = Log + TABOFFLINE[pymin];
	idx = pymin;
	loopy = (UWORD)(pymax - pymin + 1);

	do
	{
		UBYTE *dst = ptrlig;
		LONG xg  = (WORD)TabVerticG[idx];
		LONG len = (LONG)(WORD)TabVerticD[idx] - xg;

		if (len > 0)
		{
			LONG ystep, xstep;
			UWORD ax;
			WORD xd, cnt;

			ystep = ((LONG)(UWORD)TabY1[idx] - (LONG)(UWORD)TabCoulD[idx] + 1) / len;
			xstep = ((LONG)(UWORD)TabX1[idx] - (LONG)(UWORD)TabCoulG[idx] + 1) / len;

			ax = (UWORD)TabVerticG[idx];
			if ((WORD)ax < ClipXmin)
			{
				UWORD d = (UWORD)(ax - (UWORD)ClipXmin);

				TabCoulG[idx] = (WORD)(UWORD)((UWORD)TabCoulG[idx]
				                              - (UWORD)((ULONG)d * (UWORD)xstep));
				TabCoulD[idx] = (WORD)(UWORD)((UWORD)TabCoulD[idx]
				                              - (UWORD)((ULONG)d * (UWORD)ystep));
				ax = (UWORD)ClipXmin;
			}
			dst += ax;

			xd = (WORD)TabVerticD[idx];
			if (xd > ClipXmax)
			{
				xd = ClipXmax;
			}
			cnt = (WORD)(xd - (WORD)ax + 1);
			if (cnt > 0)
			{
				UWORD u  = (UWORD)TabCoulG[idx];
				UWORD v  = (UWORD)TabCoulD[idx];
				UWORD us = (UWORD)xstep;
				UWORD vs = (UWORD)ystep;
				UWORD boucle = (UWORD)cnt;

				do
				{
					UBYTE t  = source[(UWORD)((v & 0xFF00u) | (u >> 8))];
					UBYTE lo = (UBYTE)(t & 15);

					if (lo < darkdec)               /* sub al,dark / jc      */
					{
						t = (UBYTE)(t & 240);       /* ovfshd: ramp base     */
					}
					else
					{
						t = (UBYTE)((UBYTE)(lo - darkdec) | (t & 240));
					}
					*dst++ = t;

					u = (UWORD)(u + us);
					v = (UWORD)(v + vs);
				}
				while (--boucle);
			}
		}

		ptrlig += 640;
		idx++;
	}
	while (--loopy);
}
