/*----------------------------------------------------------------------------
 *  s_line.c — C translation of LIB386/LIB_SVGA/S_LINE.ASM (Adeline 1993)
 *
 *  Cohen–Sutherland style clipping followed by a Bresenham draw.
 *  The clip intersections used 16-bit imul/idiv (imul si / idiv di, then
 *  movsx of the 16-bit quotient); this is reproduced with explicit WORD
 *  casts so results are bit-identical.
 *  Note: like the original, the drawing stride is hard-coded to 640.
 *---------------------------------------------------------------------------*/
#include "translate.h"

/* clip codes: 1 = left of ClipXmin, 2 = right of ClipXmax,
               8 = above ClipYmin,   4 = below ClipYmax */

/* ASM: Line_A — draw a clipped line (register-based entry in the original,
   also called by P_OB_ISO). */
PORT_FASTCODE void Line_A(LONG x0, LONG y0, LONG x1, LONG y1, LONG couleur)
{
	ULONG *pTabOffLine = TABOFFLINE;
	LONG code0, code1;      /* bp high byte / low byte */
	LONG dx, dy;
	LONG t;
	UBYTE *pDest;
	UBYTE col;
	LONG stride, cdx, cdy;
	LONG twoMaj, err, twoMin, cnt;

	if (x0 > x1)            /* force x0 <= x1 */
	{
		t = x0; x0 = x1; x1 = t;
		t = y0; y0 = y1; y1 = t;
	}

onceagain:
	code0 = 0;

	if (x0 < ClipXmin)
	{
		code0 |= 1;
	}
	else if (x0 > ClipXmax)
	{
		return;             /* notseen */
	}

	if (y0 < ClipYmin)
	{
		code0 |= 8;
	}
	else if (y0 > ClipYmax)
	{
		code0 |= 4;
	}

	code1 = 0;

	if (x1 < ClipXmin)
	{
		return;             /* notseen */
	}
	if (x1 > ClipXmax)
	{
		code1 |= 2;
	}

	if (y1 < ClipYmin)
	{
		code1 |= 8;
	}
	else if (y1 > ClipYmax)
	{
		code1 |= 4;
	}

	if (code0 & code1)
	{
		return;             /* notseen */
	}

	if ((code0 | code1) == 0)
	{
		goto draw;
	}

	dx = x1 - x0;           /* edi */
	dy = y1 - y0;           /* esi */

	if (code0 & 1)          /* x0 < ClipXmin */
	{
		t = (WORD)(((LONG)(WORD)(ClipXmin - x0) * (WORD)dy) / (WORD)dx);
		y0 += t;
		x0 = ClipXmin;
		goto onceagain;
	}
	if (code0 & 8)          /* y0 < ClipYmin */
	{
		t = (WORD)(((LONG)(WORD)(ClipYmin - y0) * (WORD)dx) / (WORD)dy);
		x0 += t;
		y0 = ClipYmin;
		goto onceagain;
	}
	if (code0 & 4)          /* y0 > ClipYmax */
	{
		t = (WORD)(((LONG)(WORD)(ClipYmax - y0) * (WORD)dx) / (WORD)dy);
		x0 += t;
		y0 = ClipYmax;
		goto onceagain;
	}
	if (code1 & 2)          /* x1 > ClipXmax */
	{
		t = (WORD)(((LONG)(WORD)(ClipXmax - x0) * (WORD)dy) / (WORD)dx);
		y1 = y0 + t;
		x1 = ClipXmax;
		goto onceagain;
	}
	if (code1 & 8)          /* y1 < ClipYmin */
	{
		t = (WORD)(((LONG)(WORD)(ClipYmin - y0) * (WORD)dx) / (WORD)dy);
		x1 = x0 + t;
		y1 = ClipYmin;
		goto onceagain;
	}
	if (code1 & 4)          /* y1 > ClipYmax */
	{
		t = (WORD)(((LONG)(WORD)(ClipYmax - y0) * (WORD)dx) / (WORD)dy);
		x1 = x0 + t;
		y1 = ClipYmax;
		goto onceagain;
	}
	goto onceagain;

draw:
	stride = 640;
	cdx = x1 - x0;
	cdy = y1 - y0;
	if (cdy < 0)
	{
		stride = -stride;
		cdy = -cdy;
	}

	pDest = Log + pTabOffLine[y0] + x0;
	col = (UBYTE)couleur;

	if (cdx >= cdy)
	{
		/* horizontal-major */
		twoMaj = cdx << 1;      /* ebp */
		err = cdx;              /* ebx */
		twoMin = cdy << 1;      /* edx */
		cnt = cdx + 1;

		for (; cnt; cnt--)
		{
			*pDest++ = col;
			err -= twoMin;
			if (err < 0)        /* borrow on sub ebx, edx */
			{
				err += twoMaj;
				pDest += stride;
			}
		}
	}
	else
	{
		/* vertical-major */
		twoMaj = cdy << 1;      /* ebp */
		err = cdy;              /* ebx */
		twoMin = cdx << 1;      /* edx */
		cnt = cdy + 1;

		for (; cnt; cnt--)
		{
			*pDest = col;
			err -= twoMin;
			if (err < 0)
			{
				err += twoMaj;
				pDest += stride + 1;    /* adc edi, esi (carry always set) */
			}
			else
			{
				pDest += stride;
			}
		}
	}
}

/* ASM: Line */
void Line(LONG x0, LONG y0, LONG x1, LONG y1, LONG couleur)
{
	Line_A(x0, y0, x1, y1, couleur);
}
