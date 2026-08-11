/*----------------------------------------------------------------------------
 *  lib3d_p.h — private shared declarations for the C translations of the
 *  LIB386 3D pipeline modules:
 *      p_trigo.c   (LIB_3D/P_TRIGO.ASM)   — fixed point trigonometry/matrices
 *      s_poly.c    (LIB_SVGA/S_POLY.ASM)  — polygon/sphere scan conversion
 *      p_ob_iso.c  (LIB_3D/P_OB_ISO.ASM)  — 3D object renderer
 *
 *  Only these three modules (and translate.h users) include this header.
 *  Types match engine/LIB_3D/LIB_3D.H / engine/LIB_SVGA/LIB_SVGA.H where a
 *  declaration exists there; data owned by the ASM keeps the ASM layout
 *  (dd -> LONG, dw -> WORD).
 *---------------------------------------------------------------------------*/
#ifndef TRANSLATE_LIB3D_P_H
#define TRANSLATE_LIB3D_P_H

#include "translate.h"

/* P_DEFINE.ASH */
#define NORMAL_UNIT     64
#define INFO_TRI        1
#define INFO_ANIM       2
#define INFO_TORTUE     4
#define E_LIGNE         0
#define E_POLY          1
#define E_SPHERE        2
#define E_POINT         3
#define TYPE_ROTATE     0
#define TYPE_TRANSLATE  1
#define TYPE_ZOOM       2
#define TYPE_3D         0
#define TYPE_ISO        1

/* svga.ash — NB: these are the *source data* material thresholds used by
   S_POLY (TypePoly >= POLY_GOURAUD selects the gouraud tables); they do NOT
   match the FillVertic jump-table indices for every entry (see s_fillv.c). */
#define POLY_GOURAUD    7

/* helpers: wrap-safe 32-bit ops (x86 imul/add wrap silently; signed overflow
   is UB in C, so go through unsigned) */
#define MUL32(a, b)  ((LONG)((ULONG)(a) * (ULONG)(b)))
#define ADD32(a, b)  ((LONG)((ULONG)(a) + (ULONG)(b)))
#define SUB32(a, b)  ((LONG)((ULONG)(a) - (ULONG)(b)))

/* ==== p_trigo.c data (P_TRIGO.ASM .data) ================================= */
extern WORD TypeProj;
extern LONG XCentre, YCentre;          /* dd in ASM (LIB_3D.H says WORD:
                                          little-endian low-word reads work) */
extern WORD Xp, Yp;
extern WORD IsoScale;
extern WORD Z_Min, Z_Max;
extern LONG KFactor, LFactorX, LFactorY;
extern LONG CameraX, CameraY, CameraZ;
extern LONG CameraXr, CameraYr, CameraZr;
extern LONG AlphaLight, BetaLight, GammaLight;
extern LONG Alpha, Beta, Gamma;
extern LONG lAlpha, lBeta, lGamma;
extern LONG NormalXLight, NormalYLight, NormalZLight;
extern LONG X0, Y0, Z0;
extern LONG LMatriceRot[9];
extern LONG LMatriceDummy[9];
extern LONG LMatriceWorld[9];
extern LONG LMatriceTempo[9];
extern LONG TabMat[9 * 30];
extern WORD compteur;

extern WORD P_SinTab[];                /* engine/LIB_3D/P_SINTAB.C */

/* ==== p_trigo.c internal entry points used by p_ob_iso.c ================= */
void Rot(WORD x, WORD y, WORD z);                  /* -> X0/Y0/Z0 */
void WorldRot(WORD x, WORD y, WORD z);             /* -> X0/Y0/Z0 */
void LongWorldRot(LONG x, LONG y, LONG z);         /* -> X0/Y0/Z0 (64-bit) */
void LongInverseRot(LONG x, LONG y, LONG z);       /* -> X0/Y0/Z0 (64-bit) */
void RotMatW(void);                                /* World -> Rot matrix */
void RotMatIndex2(const LONG *msrc, LONG *mdest);  /* rotate matrix by lA/lB/lG */
void Proj_3D(WORD x, WORD y, WORD z, WORD *xp, WORD *yp);
void Proj_ISO(LONG x, LONG y, LONG z, WORD *xp, WORD *yp);
void RotList(const WORD *src, WORD *dst, const LONG *m, UWORD nbpoints);
void TransRotList(const WORD *src, WORD *dst, const LONG *m, UWORD nbpoints);

/* ==== p_trigo.c public API (signatures = engine/LIB_3D/LIB_3D.H) ========= */
void Rotate(LONG coorx, LONG coory, LONG angle);
void RotatePoint(LONG X, LONG Y, LONG Z);
void LongInverseRotatePoint(LONG X, LONG Y, LONG Z);
void RotateMatriceWorld(LONG palpha, LONG pbeta, LONG pgamma);
void CopyMatrice(LONG *MatSrc, LONG *MatDest);
void LongWorldRotatePoint(LONG X, LONG Y, LONG Z);
void WorldRotatePoint(LONG X, LONG Y, LONG Z);
void SetFollowCamera(LONG targetx, LONG targety, LONG targetz,
                     LONG camalpha, LONG cambeta, LONG camgamma, LONG camzoom);
void SetPosCamera(LONG poswx, LONG poswy, LONG poswz);
void SetAngleCamera(LONG palpha, LONG pbeta, LONG pgamma);
void SetInverseAngleCamera(LONG palpha, LONG pbeta, LONG pgamma);
void SetProjection(LONG xc, LONG yc, LONG kfact, LONG lfactx, LONG lfacty);
void SetIsoProjection(LONG xc, LONG yc, LONG scale);
LONG ProjettePoint(LONG CoorX, LONG CoorY, LONG CoorZ);
LONG LongProjettePoint(LONG CoorX, LONG CoorY, LONG CoorZ);
LONG TestVuePoly(WORD *ptrpoly);
void SetLightVector(WORD alpha, WORD beta, WORD gamma);

/* ==== s_poly.c data (S_POLY.ASM .data) ==================================== */
extern WORD Xmin, Xmax;                /* Ymin/Ymax already in translate.h   */
extern WORD TypePoly;
extern WORD NbPolyPoints;
extern WORD TabPoly[];                 /* (coul,x,y)* — TabPolyClip follows
                                          contiguously as in the ASM         */
extern WORD TabX1[480], TabY1[480];    /* future TEXTURE.ASM                 */

/* ==== s_poly.c entry points =============================================== */
LONG ComputePoly(void);
LONG ComputePoly_A(void);
LONG ComputeSphere(LONG pxc, LONG pyc, LONG rayon);
LONG ComputeSphere_A(LONG xc, LONG yc, LONG rayon);

/* ==== p_ob_iso.c data (P_OB_ISO.ASM .data) ================================ */
extern WORD List_Point[500 * 3];       /* Xp Yp Zrot */
extern WORD List_Anim_Point[500 * 3];  /* Xr Yr Zr   */
extern WORD List_Entity[5000];
extern WORD ScreenXmin, ScreenYmin, ScreenXmax, ScreenYmax;
extern WORD NbPoints;
extern WORD FlagLight;

/* ==== p_ob_iso.c public API =============================================== */
LONG AffObjetIso(LONG xwr, LONG ywr, LONG zwr,
                 LONG palpha, LONG pbeta, LONG pgamma, void *ptrobj);
void PatchObjet(void *ptrobj);

#endif /* TRANSLATE_LIB3D_P_H */
