/*----------------------------------------------------------------------------
 *  s_block3.c — C translation of LIB386/LIB_SVGA/S_BLOCK3.ASM (Adeline 1993)
 *---------------------------------------------------------------------------*/
#include "translate.h"

/* ASM: CopyBlockIncrust — copy rectangle src -> dst, colour 0 = transparent.
   No clipping (caller must pass valid coordinates). */
void CopyBlockIncrust(LONG x0, LONG y0, LONG x1, LONG y1, void *src,
                      LONG xd, LONG yd, void *dst)
{
	ULONG *pTabOffLine = TABOFFLINE;
	UBYTE *pSrc;
	UBYTE *pDest;
	LONG width, lines, stride;
	LONG n;
	UBYTE c;

	pSrc = (UBYTE *)src + pTabOffLine[y0] + x0;
	pDest = (UBYTE *)dst + pTabOffLine[yd] + xd;

	width = x1 - x0 + 1;
	lines = y1 - y0 + 1;

	stride = Screen_X - width;

	for (; lines; lines--)
	{
		for (n = width; n; n--)
		{
			c = *pSrc++;
			if (c)
			{
				*pDest = c;
			}
			pDest++;
		}
		pSrc += stride;
		pDest += stride;
	}
}
