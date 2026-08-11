/*----------------------------------------------------------------------------
 *  zoom.c — C translation of LIB386/LIB_SVGA/ZOOM.ASM (Adeline 1993)
 *
 *  16.16 fixed point DDA scalers.  The 16-bit fractional accumulator and its
 *  carry (add bx,dx / adc esi,ebp) are reproduced exactly.
 *  Note: like the original, the destination line width is hard-coded to 640
 *  in ScaleBox (OffsetLine = 640 - Largeur), and TabOffLine is also used as
 *  a "N lines forward" byte offset (assumes a linear layout).
 *  Division by zero when xd1==xd0 / yd1==yd0 would crash, as the original.
 *---------------------------------------------------------------------------*/
#include "translate.h"

/* ASM: ScaleLine — scale one pixel row.  Only the deltas of the coordinates
   are used: src/dst are read/written starting at ptrs/ptrd as-is. */
void ScaleLine(LONG xs0, LONG xs1, void *ptrs, LONG xe0, LONG xe1, void *ptrd)
{
	UBYTE *pSrc = (UBYTE *)ptrs;
	UBYTE *pDest = (UBYTE *)ptrd;
	LONG cnt;
	ULONG q, stepInt, t;
	UWORD frac, acc;

	cnt = xe1 - xe0;                            /* ECX delta x screen */

	q = ((ULONG)(xs1 - xs0) << 16) / (ULONG)cnt;/* note: no +1 here   */
	stepInt = q >> 16;                          /* poids fort         */
	frac = (UWORD)q;                            /* virgule            */

	acc = 0;
	for (; cnt; cnt--)
	{
		*pDest++ = *pSrc;
		t = (ULONG)acc + frac;                  /* add bx, dx         */
		acc = (UWORD)t;
		pSrc += stepInt + (t >> 16);            /* adc esi, poids fort */
	}
}

/* ASM: ScaleBox — scale a rectangle from src to dst (dst width 640). */
void ScaleBox(LONG xs0, LONG ys0, LONG xs1, LONG ys1, void *ptrs,
              LONG xd0, LONG yd0, LONG xd1, LONG yd1, void *ptrd)
{
	ULONG *pTabOffLine = TABOFFLINE;
	UBYTE *pSrc, *pDest, *pSrcLine;
	LONG largeur, hauteur, offsetline;
	LONG cnt, n;
	ULONG q, xStepInt, yStepInt, t;
	UWORD xFrac, yFrac, xAcc, yAcc;

	pSrc = (UBYTE *)ptrs + xs0;
	pDest = (UBYTE *)ptrd + xd0;

	cnt = xd1 - xd0;                            /* ECX delta x screen */
	q = ((ULONG)(xs1 - xs0 + 1) << 16) / (ULONG)cnt;
	xFrac = (UWORD)q;                           /* Virgule            */
	xStepInt = q >> 16;                         /* PoidFort           */

	largeur = cnt + 1;                          /* Largeur            */
	offsetline = 640 - largeur;                 /* OffsetLine         */

	pDest += pTabOffLine[yd0];
	cnt = yd1 - yd0;

	pSrc += pTabOffLine[ys0];
	q = ((ULONG)(ys1 - ys0 + 1) << 16) / (ULONG)cnt;
	hauteur = cnt + 1;                          /* Hauteur            */
	yStepInt = q >> 16;
	yFrac = (UWORD)q;

	yAcc = 0;

	for (; hauteur; hauteur--)                  /* yloop */
	{
		pSrcLine = pSrc;

		xAcc = 0;
		for (n = largeur; n; n--)               /* xloop */
		{
			*pDest++ = *pSrc;
			t = (ULONG)xAcc + xFrac;
			xAcc = (UWORD)t;
			pSrc += xStepInt + (t >> 16);
		}

		pDest += offsetline;                    /* 640 - delta x screen */

		pSrc = pSrcLine;

		t = (ULONG)yAcc + yFrac;
		yAcc = (UWORD)t;
		pSrc += pTabOffLine[yStepInt + (t >> 16)];  /* skip N source lines */
	}
}
