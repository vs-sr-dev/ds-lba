/*----------------------------------------------------------------------------
 *  mask_a.c — C translation of LIB386/LIB_SVGA/MASK_A.ASM (Adeline 1993)
 *
 *  Mask format:
 *      DWORD TabOffset[]  (at bank start)
 *      Mask:
 *          BYTE Delta X, BYTE Delta Y (nb lines), BYTE Hot X, BYTE Hot Y
 *          per line: BYTE NbBlock, then alternating counts:
 *              [nb pixels to skip][nb pixels to draw][skip][draw]...
 *
 *  Drawn pixels are written with ColMask (set by CoulMask).
 *  Faithful quirk kept: in the clipped path the line is first expanded in a
 *  buffer and pixels equal to 0 are treated as transparent, so a mask drawn
 *  with ColMask == 0 draws nothing when clipped (while unclipped it writes
 *  zeros).  The clipped path also uses a hard-coded 640 line stride.
 *---------------------------------------------------------------------------*/
#include <string.h>

#include "translate.h"

static UBYTE BufferClip[512];
static UBYTE ColMask = 0;       /* Noir par defaut */

/* ASM: CoulMask */
void CoulMask(LONG coulmask)
{
	ColMask = (UBYTE)coulmask;
}

/* ASM: AffMask / AffMask_Asm */
void AffMask(LONG nummask, LONG xmask, LONG ymask, void *bankmask)
{
	ULONG *pTabOffLine = TABOFFLINE;
	UBYTE *pData;
	UBYTE *pDest;
	LONG x, y, x1, y1;
	LONG dx;
	UBYTE nbline;               /* bh: 8-bit line counter, as the ASM */
	UBYTE nbblock;
	UBYTE skip, draw;
	LONG n;

	pData = (UBYTE *)bankmask + ((ULONG *)bankmask)[nummask];

	x = xmask + pData[2];       /* Hot X */
	y = ymask + pData[3];       /* Hot Y */

	dx = pData[0];              /* Delta X */
	nbline = pData[1];          /* Nb Line (Delta Y) */
	pData += 4;

	x1 = x + dx - 1;
	y1 = y + (LONG)nbline - 1;

	if (x >= ClipXmin && y >= ClipYmin && x1 <= ClipXmax && y1 <= ClipYmax)
	{
		/* ------------------------- no clipping ------------------------- */
		LONG nextline;

		pDest = Log + pTabOffLine[y] + x;
		nextline = Screen_X - dx;

		do
		{
			nbblock = *pData++;
			for (;;)
			{
				skip = *pData;          /* Nb Zero to Jump (not consumed) */
				pDest += skip;
				if (--nbblock == 0)     /* EndBlock */
				{
					pData++;
					break;
				}
				draw = pData[1];        /* Nb Zero to Write */
				pData += 2;
				memset(pDest, ColMask, draw);
				pDest += draw;
				if (--nbblock == 0)
				{
					break;
				}
			}
			pDest += nextline;
		} while (--nbline);
	}
	else
	{
		/* -------------------------- clipping --------------------------- */
		LONG OffsetBegin, NbPix;
		UBYTE *pScreenLine;
		UBYTE *pBuf;
		UBYTE c;

		if (x > ClipXmax || y > ClipYmax || x1 < ClipXmin || y1 < ClipYmin)
		{
			return;
		}

		if (y < ClipYmin)
		{
			/* skip (ClipYmin - y) lines: 1 count byte per block */
			for (n = ClipYmin - y; n; n--)
			{
				c = *pData;             /* NbBlock */
				pData += 1 + c;
			}
			y = ClipYmin;
		}

		if (y1 > ClipYmax)
		{
			y1 = ClipYmax;
		}

		OffsetBegin = 0;
		if (x < ClipXmin)
		{
			OffsetBegin = ClipXmin - x;
		}

		NbPix = x1 - x - OffsetBegin + 1;
		if (x1 > ClipXmax)
		{
			NbPix -= x1 - ClipXmax;
		}

		pScreenLine = Log + pTabOffLine[y] + x;
		nbline = (UBYTE)(y1 - y + 1);   /* inc al: 8-bit, as the ASM */

		do
		{
			/* expand one mask line into BufferClip */
			pBuf = BufferClip;
			nbblock = *pData++;
			for (;;)
			{
				skip = *pData;
				memset(pBuf, 0, skip);
				pBuf += skip;
				if (--nbblock == 0)     /* EndLine: consume the skip byte */
				{
					pData++;
					break;
				}
				draw = pData[1];
				pData += 2;
				memset(pBuf, ColMask, draw);
				pBuf += draw;
				if (--nbblock == 0)     /* dec esi / inc esi: net zero */
				{
					break;
				}
			}

			/* blit the visible part, 0 = transparent */
			{
				UBYTE *pSrc = BufferClip + OffsetBegin;
				pDest = pScreenLine + OffsetBegin;
				for (n = NbPix; n; n--)
				{
					c = *pSrc++;
					if (c)
					{
						*pDest = c;
					}
					pDest++;
				}
			}

			pScreenLine += 640;
		} while (--nbline);
	}
}

/* ASM: GetDxDyMask */
void GetDxDyMask(LONG num, LONG *dx, LONG *dy, void *bankmask)
{
	UBYTE *pData;

	pData = (UBYTE *)bankmask + ((ULONG *)bankmask)[num];
	*dx = (LONG)pData[0];
	*dy = (LONG)pData[1];
}
