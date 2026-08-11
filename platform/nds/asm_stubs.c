/*
 * asm_stubs.c — C stand-ins for the x86 ASM modules that have not been
 * translated yet (see ../../translate/ for the real translations).
 *
 * Remaining: NONE — the ASM translation campaign is complete (16/16).
 *
 * Translated & removed from here:
 *   S_PLOT, S_BOX, S_BLOCK 1-3, GRAPHMSK, ZOOM, S_LINE, MASK_A, GRAPH_A,
 *   S_STRING, S_FILLV -> translate/ (12 blitters)
 *   P_TRIGO  -> translate/p_trigo.c   (incl. X0/Y0/Z0, Xp/Yp, cameras,
 *               matrices, Rotate, SetLightVector, projections, TestVuePoly)
 *   S_POLY   -> translate/s_poly.c    (incl. TabPoly, TypePoly,
 *               NbPolyPoints, Ymin/Ymax, TabVerticG/D, TabCoulG/D)
 *   P_OB_ISO -> translate/p_ob_iso.c  (incl. ScreenXmin/..., List_*,
 *               AffObjetIso/PatchObjet)
 *   TEXTURE  -> translate/texture.c   (incl. TabText, LYmin/LYmax,
 *               AsmTexturedTriangleNoClip, FillTextPolyNoClip — holomap)
 */
#include "port.h"
