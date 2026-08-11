/*----------------------------------------------------------------------------
 *  s_poly.c — C translation of LIB386/LIB_SVGA/S_POLY.ASM (Adeline 1993)
 *
 *  Polygon / sphere scan conversion: fills Ymin/Ymax and the per-scanline
 *  edge tables TabVerticG/TabVerticD (left/right X) and TabCoulG/TabCoulD
 *  (left/right 8.8 intensity) consumed by FillVertic_A (translate/s_fillv.c).
 *
 *  Layout notes (faithful to the ASM .data):
 *  - TabPoly is (coul,x,y)* WORD triplets; TabPolyClip follows TabPoly
 *    contiguously (the ASM buffers are adjacent and ComputePoly_A appends a
 *    closing copy of the first point after the last one);
 *  - TabVerticD == TabVerticG+960 bytes in the ASM; here separate arrays,
 *    the same pairs used by s_fillv.c (see TRANSLATION_NOTES.md);
 *  - TabX0/TabY0 (TEXTURE.ASM) are aliases of TabCoulG/TabCoulD; TabX1/TabY1
 *    are provided for the future TEXTURE translation.
 *
 *  Faithful quirks kept:
 *  - the edge DDA reproduces the exact 16/32-bit add/adc (sub/sbb for the
 *    right buffer) accumulator sequences, including the "init carry" step
 *    and the entry-point-by-parity unrolling (deltaY+1 entries written);
 *  - the intensity DDA seeds its fraction with (remainder-low-byte >> 1)
 *    + 0x7F(h) exactly like `shr al,1 / add al,7Fh`;
 *  - horizontal edges (same Y) store nothing: a fully horizontal polygon
 *    passes the Ymin==Ymax test of the *unclipped* path and lets FillVertic
 *    read whatever the tables contained (DOS behaviour);
 *  - clip comparisons are 16-bit signed; the Xmin/Xmax globals are NOT
 *    recomputed after clipping (only Ymin/Ymax via the "rencadre" pass);
 *  - clipping normalizes each crossing edge to a fixed direction before the
 *    idiv ("clip 2 poly collés"), so shared edges clip identically.
 *---------------------------------------------------------------------------*/
#include <string.h>

#include "lib3d_p.h"

/*──────────────────────────── data (S_POLY.ASM .data) ─────────────────────*/

WORD Xmin = 0;                  /* polygon extent                            */
WORD Ymin = 0;
WORD Xmax = 0;
WORD Ymax = 0;

WORD TypePoly = 0;
WORD NbPolyPoints = 0;          /* 32 max                                    */

/* TabPoly: 1+32*3 words, TabPolyClip: 32*3 words, contiguous as in the ASM */
WORD TabPoly[97 + 96] = {0x1234};
#define TabPolyClip (TabPoly + 97)

/* "ne pas changer l'ordre de ce qui suit" — TabX0==TabCoulG, TabY0==TabCoulD */
PORT_FASTBSS WORD TabVerticG[480];  /* TabGauche  (DTCM 960B) */
PORT_FASTBSS WORD TabVerticD[480];  /* TabDroite  (DTCM 960B) */
PORT_FASTBSS WORD TabCoulG[480];    /* TabX0      (DTCM 960B) */
PORT_FASTBSS WORD TabCoulD[480];    /* TabY0      (DTCM 960B) */
WORD TabX1[480];
WORD TabY1[480];

static WORD boucle = 0;         /* nb points                                 */
static WORD newboucle = 0;
static WORD flagrencadre = 0;

static WORD *offtabpoly[2];

/*═════════════════════════════ SPHERE ═════════════════════════════════════*/

/* ASM: ComputeSphere_A — midpoint circle into the edge tables.
   Register entry ECX=xc ESI=yc EBP=rayon; returns 1 = draw, 0 = rejected. */
PORT_FASTCODE LONG ComputeSphere_A(LONG xc, LONG yc, LONG rayon)
{
	LONG px, py, sum;
	LONG e;
	LONG cXmin = ClipXmin, cXmax = ClipXmax;
	LONG cYmin = ClipYmin, cYmax = ClipYmax;
	int clip = 0;
	int carry;

	px = rayon;
	py = 0;
	sum = (LONG)(0 - (ULONG)rayon);

	e = yc - px;                        /* yc - rayon */
	if (e > cYmax)
	{
		return 0;
	}
	if (e < cYmin)
	{
		clip++;
		e = cYmin;
	}
	Ymin = (WORD)e;

	e = yc + px;                        /* yc + rayon */
	if (e < cYmin)
	{
		return 0;
	}
	if (e > cYmax)
	{
		clip++;
		e = cYmax;
	}
	Ymax = (WORD)e;

	e = xc + px;                        /* xc + rayon */
	if (e < cXmin)
	{
		return 0;
	}
	if (e > cXmax)
	{
		clip++;
	}

	e = xc - px;                        /* xc - rayon */
	if (e > cXmax)
	{
		return 0;
	}
	if (e < cXmin)
	{
		clip++;
	}

	if (!clip)
	{
		/* no-clip variant */
		while (py <= px)
		{
			LONG b;

			b = yc + py;
			TabVerticD[b] = (WORD)(xc + px);
			TabVerticG[b] = (WORD)(xc - px);
			b = yc - py;
			TabVerticG[b] = (WORD)(xc - px);
			TabVerticD[b] = (WORD)(xc + px);

			/* add ebp,edx ; jnc */
			{
				ULONG a = (ULONG)sum;
				ULONG r = a + (ULONG)py;

				carry = (r < a);
				sum = (LONG)r;
			}
			if (carry)
			{
				b = yc + px;
				TabVerticD[b] = (WORD)(xc + py);
				TabVerticG[b] = (WORD)(xc - py);
				b = yc - px;
				TabVerticG[b] = (WORD)(xc - py);
				TabVerticD[b] = (WORD)(xc + py);

				px--;
				sum = SUB32(sum, px);
			}
			py++;
		}
		return 1;
	}

	/* clipped variant: rows kept only when their 16-bit index is inside
	   [Ymin..Ymax]; spans crossing the clip window shrink Ymin/Ymax. */
	while (py <= px)
	{
		LONG b;
		WORD bw;

		b = yc + py;
		bw = (WORD)b;
		if (!(bw > Ymax || bw < Ymin))
		{
			/* gauche */
			e = xc - px;
			if (e < cXmin)
			{
				TabVerticG[b] = (WORD)cXmin;
			}
			else if (e <= cXmax)
			{
				TabVerticG[b] = (WORD)e;
			}
			else
			{
				Ymax = (WORD)(bw - 1);
				goto cs1;
			}
			/* droite */
			e = xc + px;
			if (e > cXmax)
			{
				TabVerticD[b] = (WORD)cXmax;
			}
			else if (e >= cXmin)
			{
				TabVerticD[b] = (WORD)e;
			}
			else
			{
				Ymax = (WORD)(bw - 1);
			}
		}
cs1:
		b = yc - py;
		bw = (WORD)b;
		if (!(bw > Ymax || bw < Ymin))
		{
			e = xc - px;
			if (e < cXmin)
			{
				TabVerticG[b] = (WORD)cXmin;
			}
			else if (e <= cXmax)
			{
				TabVerticG[b] = (WORD)e;
			}
			else
			{
				Ymin = (WORD)(bw + 1);
				goto cs2;
			}
			e = xc + px;
			if (e > cXmax)
			{
				TabVerticD[b] = (WORD)cXmax;
			}
			else if (e >= cXmin)
			{
				TabVerticD[b] = (WORD)e;
			}
			else
			{
				Ymin = (WORD)(bw + 1);
			}
		}
cs2:
		{
			ULONG a = (ULONG)sum;
			ULONG r = a + (ULONG)py;

			carry = (r < a);
			sum = (LONG)r;
		}
		if (carry)
		{
			b = yc + px;
			bw = (WORD)b;
			if (!(bw > Ymax || bw < Ymin))
			{
				e = xc - py;
				if (e < cXmin)
				{
					TabVerticG[b] = (WORD)cXmin;
				}
				else if (e <= cXmax)
				{
					TabVerticG[b] = (WORD)e;
				}
				else
				{
					Ymax = (WORD)(bw - 1);
					goto cs3;
				}
				e = xc + py;
				if (e > cXmax)
				{
					TabVerticD[b] = (WORD)cXmax;
				}
				else if (e >= cXmin)
				{
					TabVerticD[b] = (WORD)e;
				}
				else
				{
					Ymax = (WORD)(bw - 1);
				}
			}
cs3:
			b = yc - px;
			bw = (WORD)b;
			if (!(bw > Ymax || bw < Ymin))
			{
				e = xc - py;
				if (e < cXmin)
				{
					TabVerticG[b] = (WORD)cXmin;
				}
				else if (e <= cXmax)
				{
					TabVerticG[b] = (WORD)e;
				}
				else
				{
					Ymin = (WORD)(bw + 1);
					goto cs4;
				}
				e = xc + py;
				if (e > cXmax)
				{
					TabVerticD[b] = (WORD)cXmax;
				}
				else if (e >= cXmin)
				{
					TabVerticD[b] = (WORD)e;
				}
				else
				{
					Ymin = (WORD)(bw + 1);
				}
			}
cs4:
			px--;
			sum = SUB32(sum, px);
		}
		py++;
	}

	/* "modif pas de sphere d'une ligne ???" */
	if (Ymin >= Ymax)
	{
		return 0;
	}
	return 1;
}

/* ASM: ComputeSphere (stack entry) */
LONG ComputeSphere(LONG pxc, LONG pyc, LONG rayon)
{
	return ComputeSphere_A(pxc, pyc, rayon);
}

/*═════════════════════════════ POLYGONE ═══════════════════════════════════*/

/* ASM: ClipGauche/ClipDroit/ClipHaut/ClipBas — the four routines are the
   same code up to the clipped coordinate (primIdx: 1 = X, 2 = Y), the bound
   and the outside direction.  Sutherland-Hodgman against one boundary from
   offtabpoly[0] into offtabpoly[1], then buffer swap + closing point.
   Returns newboucle (0 = polygon fully clipped away). */
PORT_FASTCODE static WORD ClipPolyEdge(int primIdx, WORD bound, int outsideIsGreater)
{
	int secIdx = 3 - primIdx;
	WORD *src, *dp;
	WORD c0, p0, s0;
	WORD c1, p1, s1;
	UWORD n;
	int out0, out1;

	newboucle = 0;
	flagrencadre = 1;

	src = offtabpoly[0];
	dp = offtabpoly[1];

	c0 = src[0];
	p0 = src[primIdx];
	s0 = src[secIdx];
	src += 3;

	n = (UWORD)boucle;
	do
	{
		WORD origC1, origP1, origS1;

		c1 = src[0];
		p1 = src[primIdx];
		s1 = src[secIdx];
		src += 3;
		origC1 = c1;
		origP1 = p1;
		origS1 = s1;

		out0 = outsideIsGreater ? (p0 > bound) : (p0 < bound);
		out1 = outsideIsGreater ? (p1 > bound) : (p1 < bound);

		if (!out0)
		{
			dp[0] = c0;                 /* pas clippé on stock */
			dp[primIdx] = p0;
			dp[secIdx] = s0;
			dp += 3;
			newboucle++;
			if (!out1)
			{
				goto nextpt;            /* cfg2 */
			}
		}
		else if (out1)
		{
			goto nextpt;                /* both clipped: drop point 0 */
		}

		/* cfg1: edge crosses the boundary — interpolate */
		{
			WORD dC, dS, dP, dClip, q;

			/* Ajuste pour clip 2 poly collés: normalize edge direction */
			if (!(p1 < p0))             /* cmp ax,cx ; jl cfg4 */
			{
				WORD t;

				t = p0; p0 = p1; p1 = t;
				t = s0; s0 = s1; s1 = t;
				t = c0; c0 = c1; c1 = t;
			}

			dC = (WORD)(c1 - c0);
			dS = (WORD)(s1 - s0);
			dP = (WORD)(p1 - p0);
			dClip = (WORD)(bound - p0);

			q = (WORD)(((LONG)dClip * dS) / dP);    /* imul/idiv 16-bit */
			dp[primIdx] = bound;
			dp[secIdx] = (WORD)(s0 + q);

			if ((UWORD)TypePoly >= POLY_GOURAUD)
			{
				q = (WORD)(((LONG)dC * dClip) / dP);
				dp[0] = (WORD)(q + c0);
			}
			dp += 3;
			newboucle++;
		}

nextpt:
		c0 = origC1;                    /* XYC0 = XYC1 */
		p0 = origP1;
		s0 = origS1;
	} while (--n);

	/* swap buffers, close the new polygon */
	{
		WORD *t = offtabpoly[0];

		offtabpoly[0] = offtabpoly[1];
		offtabpoly[1] = t;
	}
	dp[0] = offtabpoly[0][0];           /* movsw/movsd: transitivité */
	dp[1] = offtabpoly[0][1];
	dp[2] = offtabpoly[0][2];

	boucle = newboucle;
	return newboucle;
}

/*───────────────────────────── edge rasterizer ────────────────────────────*/

/* ASM: ComputePoly_A "Y descend" path — left buffer (TabVerticG/TabCoulG).
   x0/y0 = current point, x1/y1 = next point, y1 > y0.
   i1/i2 = previous/current vertex colour bytes ([intensite_1]/[intensite_2]).
   Writes deltaY+1 X entries (and intensity entries for gouraud types),
   downward from y0, or upward from y1 when the edge was X-swapped (std). */
PORT_FASTCODE static void EdgeGauche(WORD x0, WORD y0, WORD x1, WORD y1, UBYTE i1, UBYTE i2)
{
	WORD deltaY = (WORD)(y1 - y0);      /* bp: abs delta y (> 0)             */
	int dir = 1;                        /* cld                               */
	UWORD startY;
	UWORD deltaX;
	ULONG quot, rem, acc, step;
	UWORD cnt;
	int carry;
	WORD *vp;
	unsigned long long t64;

	if (x0 > x1)                        /* remet X0 < X1 (+ std)             */
	{
		WORD t;
		UBYTE u;

		t = x0; x0 = x1; x1 = t;
		t = y0; y0 = y1; y1 = t;
		u = i1; i1 = i2; i2 = u;
		dir = -1;
	}

	startY = (UWORD)y0;
	vp = TabVerticG + startY;

	deltaX = (UWORD)(x1 - x0);
	quot = ((ULONG)deltaX << 16) / (UWORD)deltaY;
	rem = ((ULONG)deltaX << 16) % (UWORD)deltaY;

	/* EAX = .cumul:X — fraction seeded with rem/2 + 7FFFh                  */
	acc = ((ULONG)(UWORD)(((UWORD)rem >> 1) + 0x7FFF) << 16) | (UWORD)x0;
	/* EDX = .DeltaX:DeltaX (rol edx,16 of the 16.16 quotient)              */
	step = (quot << 16) | (quot >> 16);

	cnt = (UWORD)((UWORD)(deltaY + 2) >> 1);

	/* init carry: add eax,edx ; (save CF) ; sub ax,dx ; (restore CF)       */
	t64 = (unsigned long long)acc + step;
	carry = (int)(t64 >> 32);
	acc = (ULONG)t64;
	acc = (acc & 0xFFFF0000UL) | (UWORD)((UWORD)acc - (UWORD)step);

	if (deltaY & 1)
	{
		goto lt0;
	}
	goto lt01;

lt0:
	*vp = (WORD)(UWORD)acc;
	vp += dir;
	t64 = (unsigned long long)acc + step + (unsigned)carry;
	carry = (int)(t64 >> 32);
	acc = (ULONG)t64;
lt01:
	*vp = (WORD)(UWORD)acc;
	vp += dir;
	t64 = (unsigned long long)acc + step + (unsigned)carry;
	carry = (int)(t64 >> 32);
	acc = (ULONG)t64;
	if (--cnt)
	{
		goto lt0;
	}

	/* fill tab coul G (gouraud/dither only) */
	if ((UWORD)TypePoly >= POLY_GOURAUD)
	{
		UWORD n2 = (UWORD)deltaY;
		WORD *cp = TabCoulG + startY;
		UWORD stepI, remI, accI, c2;

		if (i2 >= i1)                   /* sub ah,al ; jc ftg0               */
		{
			UBYTE dI = (UBYTE)(i2 - i1);

			stepI = (UWORD)(((UWORD)((UWORD)dI << 8)) / n2);
			remI = (UWORD)(((UWORD)((UWORD)dI << 8)) % n2);
			accI = (UWORD)(((UWORD)i1 << 8)
			             | (UBYTE)((UBYTE)((UBYTE)remI >> 1) + 0x7F));
			c2 = (UWORD)((UWORD)(n2 + 2) >> 1);
			if (n2 & 1)
			{
				goto xg0;
			}
			goto xg1;
xg0:
			*cp = (WORD)accI;
			cp += dir;
			accI = (UWORD)(accI + stepI);
xg1:
			*cp = (WORD)accI;
			cp += dir;
			accI = (UWORD)(accI + stepI);
			if (--c2)
			{
				goto xg0;
			}
		}
		else                            /* ftg0: decreasing intensity        */
		{
			UBYTE dI = (UBYTE)(i1 - i2);        /* neg ah                    */

			stepI = (UWORD)(((UWORD)((UWORD)dI << 8)) / n2);
			remI = (UWORD)(((UWORD)((UWORD)dI << 8)) % n2);
			accI = (UWORD)(((UWORD)i1 << 8)
			             | (UBYTE)((UBYTE)(0 - (UBYTE)((UBYTE)remI >> 1)) + 0x7F));
			c2 = (UWORD)((UWORD)(n2 + 2) >> 1);
			if (n2 & 1)
			{
				goto xg0n;
			}
			goto xg1n;
xg0n:
			*cp = (WORD)accI;
			cp += dir;
			accI = (UWORD)(accI - stepI);
xg1n:
			*cp = (WORD)accI;
			cp += dir;
			accI = (UWORD)(accI - stepI);
			if (--c2)
			{
				goto xg0n;
			}
		}
	}
}

/* ASM: ComputePoly_A "Y monte" path — right buffer (TabVerticD/TabCoulD).
   y1 < y0; the DDA runs with sub/sbb (X stored decreasing along the walk). */
PORT_FASTCODE static void EdgeDroite(WORD x0, WORD y0, WORD x1, WORD y1, UBYTE i1, UBYTE i2)
{
	WORD deltaY = (WORD)(y0 - y1);
	int dir = -1;                       /* std                               */
	UWORD startY;
	UWORD deltaX;
	ULONG quot, rem, acc, step;
	UWORD cnt;
	int borrow;
	WORD *vp;
	unsigned long long s64;

	if (x0 < x1)                        /* remet X0 > X1 (+ cld)             */
	{
		WORD t;
		UBYTE u;

		t = x0; x0 = x1; x1 = t;
		t = y0; y0 = y1; y1 = t;
		u = i1; i1 = i2; i2 = u;
		dir = 1;
	}

	startY = (UWORD)y0;
	vp = TabVerticD + startY;

	deltaX = (UWORD)(x0 - x1);
	quot = ((ULONG)deltaX << 16) / (UWORD)deltaY;
	rem = ((ULONG)deltaX << 16) % (UWORD)deltaY;

	/* fraction seeded with -(rem/2) + 7FFFh (shr/neg/add)                  */
	acc = ((ULONG)(UWORD)((UWORD)(0 - (UWORD)((UWORD)rem >> 1)) + 0x7FFF) << 16)
	    | (UWORD)x0;
	step = (quot << 16) | (quot >> 16);

	cnt = (UWORD)((UWORD)(deltaY + 2) >> 1);

	/* init borrow: sub eax,edx ; (save CF) ; add ax,dx ; (restore CF)      */
	borrow = ((ULONG)acc < step);
	acc = (ULONG)(acc - step);
	acc = (acc & 0xFFFF0000UL) | (UWORD)((UWORD)acc + (UWORD)step);

	if (deltaY & 1)
	{
		goto lt0m;
	}
	goto lt01m;

lt0m:
	*vp = (WORD)(UWORD)acc;
	vp += dir;
	s64 = (unsigned long long)step + (unsigned)borrow;      /* sbb eax,edx */
	borrow = ((unsigned long long)acc < s64);
	acc = (ULONG)((unsigned long long)acc - s64);
lt01m:
	*vp = (WORD)(UWORD)acc;
	vp += dir;
	s64 = (unsigned long long)step + (unsigned)borrow;      /* sbb eax,edx */
	borrow = ((unsigned long long)acc < s64);
	acc = (ULONG)((unsigned long long)acc - s64);
	if (--cnt)
	{
		goto lt0m;
	}

	/* fill tab coul D */
	if ((UWORD)TypePoly >= POLY_GOURAUD)
	{
		UWORD n2 = (UWORD)deltaY;
		WORD *cp = TabCoulD + startY;
		UWORD stepI, remI, accI, c2;

		if (i2 >= i1)
		{
			UBYTE dI = (UBYTE)(i2 - i1);

			stepI = (UWORD)(((UWORD)((UWORD)dI << 8)) / n2);
			remI = (UWORD)(((UWORD)((UWORD)dI << 8)) % n2);
			accI = (UWORD)(((UWORD)i1 << 8)
			             | (UBYTE)((UBYTE)((UBYTE)remI >> 1) + 0x7F));
			c2 = (UWORD)((UWORD)(n2 + 2) >> 1);
			if (n2 & 1)
			{
				goto xd0;
			}
			goto xd1;
xd0:
			*cp = (WORD)accI;
			cp += dir;
			accI = (UWORD)(accI + stepI);
xd1:
			*cp = (WORD)accI;
			cp += dir;
			accI = (UWORD)(accI + stepI);
			if (--c2)
			{
				goto xd0;
			}
		}
		else                            /* ftd0 */
		{
			UBYTE dI = (UBYTE)(i1 - i2);

			stepI = (UWORD)(((UWORD)((UWORD)dI << 8)) / n2);
			remI = (UWORD)(((UWORD)((UWORD)dI << 8)) % n2);
			accI = (UWORD)(((UWORD)i1 << 8)
			             | (UBYTE)((UBYTE)(0 - (UBYTE)((UBYTE)remI >> 1)) + 0x7F));
			c2 = (UWORD)((UWORD)(n2 + 2) >> 1);
			if (n2 & 1)
			{
				goto xd0n;
			}
			goto xd1n;
xd0n:
			*cp = (WORD)accI;
			cp += dir;
			accI = (UWORD)(accI - stepI);
xd1n:
			*cp = (WORD)accI;
			cp += dir;
			accI = (UWORD)(accI - stepI);
			if (--c2)
			{
				goto xd0n;
			}
		}
	}
}

/*───────────────────────────── ComputePoly ────────────────────────────────*/

/* ASM: ComputePoly_A — scan-convert TabPoly (NbPolyPoints, TypePoly) into
   the edge tables; returns 1 = fill it, 0 = fully clipped / degenerate. */
PORT_FASTCODE LONG ComputePoly_A(void)
{
	WORD xmi, xma, ymi, yma;
	WORD *p;
	UWORD n;
	WORD ax;

	offtabpoly[0] = TabPoly;
	offtabpoly[1] = TabPolyClip;
	boucle = NbPolyPoints;
	flagrencadre = 0;

	xmi = 32767;                        /* bx */
	ymi = 32767;                        /* bp */
	yma = -32768;                       /* di */
	xma = -32768;                       /* dx */

	p = TabPoly;
	n = (UWORD)NbPolyPoints;
	do
	{
		p++;                            /* saute couleur */
		ax = *p++;                      /* X */
		if (ax < xmi)
		{
			xmi = ax;
		}
		if (ax > xma)
		{
			xma = ax;
		}
		ax = *p++;                      /* Y */
		if (ax < ymi)
		{
			ymi = ax;
		}
		if (ax > yma)
		{
			yma = ax;
		}
	} while (--n);

	p[0] = TabPoly[0];                  /* transitivité der point */
	p[1] = TabPoly[1];
	p[2] = TabPoly[2];

	Ymin = ymi;
	Xmax = xma;
	Ymax = yma;
	Xmin = xmi;

	if (yma < ymi)                      /* si Ymin > Ymax fin poly */
	{
		return 0;
	}

	/* CLIP — NB: Xmin/Xmax globals stay pre-clip (faithful) */
	if (Xmin < (WORD)ClipXmin)
	{
		if (Xmax < (WORD)ClipXmin)
		{
			return 0;                   /* outside screen */
		}
		if (ClipPolyEdge(1, (WORD)ClipXmin, 0) == 0)
		{
			return 0;
		}
	}
	if (Xmax > (WORD)ClipXmax)
	{
		if (Xmin > (WORD)ClipXmax)
		{
			return 0;
		}
		if (ClipPolyEdge(1, (WORD)ClipXmax, 1) == 0)
		{
			return 0;
		}
	}
	if (Ymin < (WORD)ClipYmin)
	{
		if (Ymax < (WORD)ClipYmin)
		{
			return 0;
		}
		if (ClipPolyEdge(2, (WORD)ClipYmin, 0) == 0)
		{
			return 0;
		}
	}
	if (Ymax > (WORD)ClipYmax)
	{
		if (Ymin > (WORD)ClipYmax)
		{
			return 0;
		}
		if (ClipPolyEdge(2, (WORD)ClipYmax, 1) == 0)
		{
			return 0;
		}
	}

	if (flagrencadre)
	{
		/* rencadre: new Ymin/Ymax of the clipped polygon */
		WORD bx = 32767;
		WORD dx = -32768;               /* mov dx,32767 ; inc dx */

		p = offtabpoly[0];
		n = (UWORD)boucle;
		do
		{
			ax = p[2];                  /* read Y */
			if (ax < bx)
			{
				bx = ax;
			}
			if (ax > dx)
			{
				dx = ax;
			}
			p += 3;
		} while (--n);

		Ymin = bx;
		Ymax = dx;

		/* modif pas de poly d'une ligne (jg -> jge) */
		if (bx >= dx)
		{
			return 0;
		}
	}

	/* filltabvertic: draw les lines inter sommets */
	{
		WORD *sp = offtabpoly[0];
		UBYTE lastI, i1, i2;
		WORD bx, cx;                    /* X0 / Y0 */

		lastI = (UBYTE)sp[0];           /* couleur sommet */
		i2 = lastI;
		bx = sp[1];
		cx = sp[2];
		sp += 3;

		do
		{
			WORD dxv, axv;              /* X1 / Y1 */

			i1 = lastI;
			i2 = (UBYTE)sp[0];
			lastI = i2;
			dxv = sp[1];
			axv = sp[2];
			sp += 3;

			if (axv == cx)              /* same_y: nothing stored */
			{
				bx = dxv;
			}
			else if (axv > cx)          /* Y descend: buffer gauche */
			{
				EdgeGauche(bx, cx, dxv, axv, i1, i2);
				bx = dxv;               /* pop cx/bx: XY1 = XY2 (originals) */
				cx = axv;
			}
			else                        /* Y monte: buffer droit */
			{
				EdgeDroite(bx, cx, dxv, axv, i1, i2);
				bx = dxv;
				cx = axv;
			}
		} while (--boucle);
	}

	return 1;                           /* ok poly drawed (perhaps clipped) */
}

/* ASM: ComputePoly (stack entry) */
LONG ComputePoly(void)
{
	return ComputePoly_A();
}
