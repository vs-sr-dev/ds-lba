/*----------------------------------------------------------------------------
 *  s_block2.c — C translation of LIB386/LIB_SVGA/S_BLOCK2.ASM (Adeline 1993)
 *---------------------------------------------------------------------------*/
#include "translate.h"

/* ASM: CopyBlockOnBlack — copy the rectangle from src to dst, but only on
   destination pixels that are black (0).  The original used a scasb/movsd
   run-scanning loop; the net per-pixel effect is exactly:
   if (dst == 0) dst = src.  No clipping. */
void CopyBlockOnBlack(LONG x0, LONG y0, LONG x1, LONG y1, void *src, void *dst)
{
	ULONG *pTabOffLine = TABOFFLINE;
	UBYTE *pSrc;
	UBYTE *pDest;
	LONG width, lines, stride;
	LONG n;

	width = x1 - x0 + 1;
	lines = y1 - y0 + 1;

	n = pTabOffLine[y0] + x0;
	pSrc = (UBYTE *)src + n;
	pDest = (UBYTE *)dst + n;

	stride = Screen_X - width;

	for (; lines; lines--)
	{
		for (n = width; n; n--)
		{
			if (*pDest == 0)
			{
				*pDest = *pSrc;
			}
			pSrc++;
			pDest++;
		}
		pSrc += stride;
		pDest += stride;
	}
}
