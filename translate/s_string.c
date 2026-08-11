/*----------------------------------------------------------------------------
 *  s_string.c — C translation of LIB386/LIB_SVGA/S_STRING.ASM (Adeline 1993)
 *
 *  The Font8X8 glyph data of the ASM file is already available in C form in
 *  engine/LIB_SVGA/FONT8X8.C (const unsigned char Font8X8[]), so it is not
 *  duplicated here.
 *  Note: like the original, rows advance by Screen_X but the rewind to the
 *  next character cell is hard-coded as (640*8)-8.  No clipping.
 *---------------------------------------------------------------------------*/
#include "translate.h"

/* ASM: AffString */
void AffString(LONG x, LONG y, void *text)
{
	ULONG *pTabOffLine = TABOFFLINE;
	UBYTE *pText;
	UBYTE *pCell;               /* ebp: top-left of current char cell */
	UBYTE *pDest;
	const UBYTE *pGlyph;
	UBYTE c, bits;
	LONG row, col;

	pCell = Log + pTabOffLine[y] + x;
	pText = (UBYTE *)text;

	while ((c = *pText++) != 0)
	{
		pGlyph = Font8X8 + (LONG)c * 8;

		for (row = 8; row; row--)
		{
			pDest = pCell;
			bits = *pGlyph++;

			for (col = 8; col; col--)
			{
				if (bits & 0x80)
				{
					*pDest = Text_Ink;          /* coulencre */
				}
				else if (Text_Paper != 0xFF)
				{
					*pDest = Text_Paper;        /* coulfond  */
				}
				pDest++;
				bits = (UBYTE)(bits << 1);
			}

			pCell += Screen_X;
		}

		pCell -= (640 * 8) - 8;     /* next char cell */
	}
}

/* ASM: CoulText */
void CoulText(LONG ink, LONG paper)
{
	Text_Ink = (UBYTE)ink;
	Text_Paper = (UBYTE)paper;
}
