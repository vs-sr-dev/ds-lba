/*----------------------------------------------------------------------------
 *  graph_a.c — C translation of LIB386/LIB_SVGA/GRAPH_A.ASM (Adeline 1993)
 *
 *  Brick (graph) RLE format: see graphmsk.c header.
 *
 *  Faithful quirk kept: in the clipped path the line is first expanded into
 *  a buffer and then blitted with colour 0 = transparent, whereas the
 *  unclipped path writes colour 0 pixels found in Copy/Repeat blocks.  The
 *  clipped path uses a hard-coded 640 line stride (Screen_X elsewhere).
 *---------------------------------------------------------------------------*/
#include <string.h>

#include "translate.h"

static UBYTE BufferClip[512];

/* ASM: AffGraph */
PORT_FASTCODE void AffGraph(LONG numbrick, LONG xbrick, LONG ybrick, void *bankbrick)
{
	ULONG *pTabOffLine = TABOFFLINE;
	UBYTE *pData;
	UBYTE *pDest;
	LONG x, y, x1, y1;
	LONG dx;
	UBYTE nbline;               /* bh: 8-bit line counter */
	UBYTE nbblock;
	UBYTE op, cl;
	LONG n;

	pData = (UBYTE *)bankbrick + ((ULONG *)bankbrick)[numbrick];

	x = xbrick + pData[2];      /* Hot X */
	y = ybrick + pData[3];      /* Hot Y */

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
			do
			{
				op = *pData;
				cl = (UBYTE)(op & 0x3F);
				if ((op & 0xC0) == 0)
				{
					/* JumpZero: skip cl+1 pixels (incrust) */
					pData++;
					pDest += (LONG)cl + 1;
				}
				else
				{
					cl++;
					if (op & 0x40)
					{
						/* WriteDiffPix: copy cl pixels */
						pData++;
						memcpy(pDest, pData, cl);
						pData += cl;
						pDest += cl;
					}
					else
					{
						/* Repeat same colour, cl pixels */
						memset(pDest, pData[1], cl);
						pData += 2;
						pDest += cl;
					}
				}
			} while (--nbblock);
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
			/* skip (ClipYmin - y) lines of RLE data */
			for (n = ClipYmin - y; n; n--)
			{
				nbblock = *pData++;
				do
				{
					op = *pData++;
					if (op & 0xC0)
					{
						LONG nd = 0;
						if (op & 0x40)          /* Copy: cl+1 data bytes */
						{
							nd = op & 0x3F;
						}
						pData += nd + 1;        /* Repeat: 1 data byte   */
					}
				} while (--nbblock);
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
		nbline = (UBYTE)(y1 - y + 1);           /* inc al: 8-bit */

		do
		{
			/* expand one brick line into BufferClip */
			pBuf = BufferClip;
			nbblock = *pData++;
			do
			{
				op = *pData++;
				cl = (UBYTE)((op & 0x3F) + 1);
				if ((op & 0xC0) == 0)           /* JumpL: cl zeros */
				{
					memset(pBuf, 0, cl);
					pBuf += cl;
				}
				else if (op & 0x40)             /* WriteDiffL: copy */
				{
					memcpy(pBuf, pData, cl);
					pData += cl;
					pBuf += cl;
				}
				else                            /* repeat colour */
				{
					memset(pBuf, *pData, cl);
					pData++;
					pBuf += cl;
				}
			} while (--nbblock);

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

/* ASM: GetDxDyGraph */
void GetDxDyGraph(LONG num, LONG *dx, LONG *dy, void *bankgraph)
{
	UBYTE *pData;

	pData = (UBYTE *)bankgraph + ((ULONG *)bankgraph)[num];
	*dx = (LONG)pData[0];
	*dy = (LONG)pData[1];
}
