/*----------------------------------------------------------------------------
 *  translate.h — shared declarations for the C translations of the
 *  LIB386/LIB_SVGA assembly blitters (LBA1 -> Nintendo DS port).
 *
 *  NOTE: the engine headers (engine/LIB_SVGA/LIB_SVGA.H etc.) are not yet
 *  cleanly includable with GCC (Watcom keywords such as __far / cdecl), so
 *  the required globals are re-declared here locally.  Names and types match
 *  engine/LIB_SVGA/LIB_SVGA.H and engine/LIB_SYS/ADELINE.H exactly.
 *---------------------------------------------------------------------------*/
#ifndef TRANSLATE_TRANSLATE_H
#define TRANSLATE_TRANSLATE_H

/* Fast-memory placement macros (ITCM/DTCM on the DS, empty on PC). */
#include "port_fast.h"

/* ---- Types: identical to engine/LIB_SYS/ADELINE.H (object-like macros; an
        identical redefinition is legal C if both headers are included). ---- */
#ifndef ULONG
#define ULONG unsigned long int
#endif
#ifndef LONG
#define LONG signed long int
#endif
#ifndef UWORD
#define UWORD unsigned short int
#endif
#ifndef WORD
#define WORD signed short int
#endif
#ifndef UBYTE
#define UBYTE unsigned char
#endif
#ifndef BYTE
#define BYTE signed char
#endif

/* ---- Globals of the SVGA layer (defined by the platform layer / engine) -- */
extern WORD ClipXmin;
extern WORD ClipYmin;
extern WORD ClipXmax;
extern WORD ClipYmax;

extern ULONG TabOffLine;        /* first entry of the line-offset table      */
extern UBYTE *Log;              /* logical (back buffer) frame buffer        */
extern WORD Screen_X;
extern WORD Screen_Y;

extern UBYTE Text_Ink;
extern UBYTE Text_Paper;

/* Poly scan-conversion tables (filled by S_POLY / P_OB_ISO layers).
   The original ASM assumed TabVerticD == TabVerticG + 480 WORDs (offset +960
   bytes) and TabCoulD == TabCoulG + 480 WORDs; here the separate arrays are
   addressed explicitly. Each array must have at least 480 entries. */
extern WORD Ymin;
extern WORD Ymax;
extern WORD TabVerticG[];
extern WORD TabVerticD[];
extern WORD TabCoulG[];
extern WORD TabCoulD[];

/* 8x8 system font: defined in engine/LIB_SVGA/FONT8X8.C
   (const unsigned char Font8X8[]) — same data as in S_STRING.ASM.           */
extern const UBYTE Font8X8[];

/* Convenience: the line offset table viewed as an array. */
#define TABOFFLINE ((ULONG *)&TabOffLine)

/* ---- Functions translated in translate/ --------------------------------- */
/* s_plot.c   */
void Plot(LONG x, LONG y, LONG couleur);
LONG GetPlot(LONG x, LONG y);
/* s_box.c    */
void Box(LONG x0, LONG y0, LONG x1, LONG y1, LONG couleur);
/* s_block3.c */
void CopyBlockIncrust(LONG x0, LONG y0, LONG x1, LONG y1, void *src,
                      LONG xd, LONG yd, void *dst);
/* s_block2.c */
void CopyBlockOnBlack(LONG x0, LONG y0, LONG x1, LONG y1, void *src, void *dst);
/* graphmsk.c */
LONG CalcGraphMsk(LONG num, void *bank, void *mask);
/* zoom.c     */
void ScaleLine(LONG xa0, LONG xa1, void *src, LONG xb0, LONG xb1, void *dst);
void ScaleBox(LONG xa0, LONG ya0, LONG xa1, LONG ya1, void *src,
              LONG xb0, LONG yb0, LONG xb1, LONG yb1, void *dst);
/* s_line.c   */
void Line(LONG x0, LONG y0, LONG x1, LONG y1, LONG couleur);
void Line_A(LONG x0, LONG y0, LONG x1, LONG y1, LONG couleur);
/* s_block.c  */
void CopyBlock(LONG x0, LONG y0, LONG x1, LONG y1, void *src,
               LONG xd, LONG yd, void *dst);
void SaveBlock(void *screen, void *buffer, LONG x, LONG y, LONG dx, LONG dy);
void RestoreBlock(void *screen, void *buffer, LONG x, LONG y, LONG dx, LONG dy);
/* mask_a.c   */
void CoulMask(LONG coulmask);
void AffMask(LONG num, LONG x, LONG y, void *bank);
void GetDxDyMask(LONG num, LONG *dx, LONG *dy, void *bank);
/* graph_a.c  */
void AffGraph(LONG num, LONG x, LONG y, void *bank);
void GetDxDyGraph(LONG num, LONG *dx, LONG *dy, void *bank);
/* s_string.c */
void AffString(LONG x, LONG y, void *text);
void CoulText(LONG coul0, LONG coul1);
/* s_fillv.c  */
void FillVertic(LONG typepoly, LONG coulpoly);
void FillVertic_A(LONG typepoly, LONG coulpoly);
void SetFillDetails(LONG level);
/* texture.c — holomap textured-triangle rasterizer (TEXTURE.ASM).
   TabText mirrors TabPoly's (c,x,y) triplet layout with texture (u,v) 8.8;
   LYmin/LYmax are the extent computed by AsmTexturedTriangleNoClip and fed
   to FillTextPolyNoClip by HOLOMAP.C. */
extern WORD TabText[];
extern LONG LYmin, LYmax;
void AsmTexturedTriangleNoClip(void);
void AsmGouraudTriangleNoClip(void);
void AsmFillProp(WORD *ptrdest, LONG x0, LONG y0, LONG x1, LONG y1);
void AsmFillPropNoClip(WORD *ptrdest, LONG x0, LONG y0, LONG x1, LONG y1);
void FillTextPoly(LONG pymin, LONG pymax, UBYTE *source);
void FillTextPolyNoClip(LONG pymin, LONG pymax, UBYTE *source);
void FillTextPolyShade(LONG pymin, LONG pymax, LONG dark, UBYTE *source);

#endif /* TRANSLATE_TRANSLATE_H */
