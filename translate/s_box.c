/*----------------------------------------------------------------------------
 *  s_box.c — C translation of LIB386/LIB_SVGA/S_BOX.ASM (Adeline 1993)
 *---------------------------------------------------------------------------*/
#include <string.h>

#include "translate.h"

/* ASM: Box — filled rectangle, clipped */
void Box(LONG x0, LONG y0, LONG x1, LONG y1, LONG couleur)
{
	ULONG *pTabOffLine = TABOFFLINE;
	UBYTE *pDest;
	LONG dx, dy;

	if (x0 > ClipXmax || x1 < ClipXmin || y0 > ClipYmax || y1 < ClipYmin)
	{
		return;
	}

	if (x0 < ClipXmin)
	{
		x0 = ClipXmin;
	}
	if (x1 > ClipXmax)
	{
		x1 = ClipXmax;
	}
	if (y0 < ClipYmin)
	{
		y0 = ClipYmin;
	}
	if (y1 > ClipYmax)
	{
		y1 = ClipYmax;
	}

	pDest = Log + pTabOffLine[y0] + x0;

	dy = y1 - y0 + 1;
	dx = x1 - x0 + 1;

	for (; dy; dy--)
	{
		memset(pDest, (UBYTE)couleur, (size_t)dx);
		pDest += Screen_X;
	}
}
