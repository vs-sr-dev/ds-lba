/*----------------------------------------------------------------------------
 *  s_plot.c — C translation of LIB386/LIB_SVGA/S_PLOT.ASM (Adeline 1993)
 *---------------------------------------------------------------------------*/
#include "translate.h"

/* ASM: Plot */
void Plot(LONG x, LONG y, LONG couleur)
{
	ULONG *pTabOffLine = TABOFFLINE;

	if (x < ClipXmin || x > ClipXmax || y < ClipYmin || y > ClipYmax)
	{
		return;
	}

	*(Log + pTabOffLine[y] + x) = (UBYTE)couleur;
}

/* ASM: GetPlot */
LONG GetPlot(LONG x, LONG y)
{
	ULONG *pTabOffLine = TABOFFLINE;

	if (x < ClipXmin || x > ClipXmax || y < ClipYmin || y > ClipYmax)
	{
		return 0;
	}

	return (LONG)*(Log + pTabOffLine[y] + x);
}
