/*----------------------------------------------------------------------------
 *  p_trigo.c — C translation of LIB386/LIB_3D/P_TRIGO.ASM (Adeline 1993)
 *
 *  Fixed-point (1.15 sin table, >>14 matrix scale) trigonometry, matrix and
 *  projection module of the 3D pipeline.  The sine table itself is NOT here:
 *  P_TRIGO.ASM imports P_SinTab which already exists in C in
 *  engine/LIB_3D/P_SINTAB.C (WORD P_SinTab[1024]).
 *
 *  Faithfulness notes:
 *  - all imul/add 32-bit ops wrap like x86 (done via MUL32/ADD32 unsigned
 *    helpers), shifts on negatives are arithmetic (GCC/devkitARM guarantee);
 *  - the "Long" variants use 64-bit intermediates exactly like the
 *    imul/adc/shrd sequences (result = low 32 bits of sum >> 14);
 *  - Proj_3D / ProjettePoint work on 16-bit registers like the ASM (WORD
 *    truncations kept, including the 16-bit `or cx,cx` Z sign test);
 *  - Proj_ISO takes full 32-bit inputs but performs the final neg/add on
 *    16 bits (`neg bx / add bx,[YCentre]`), reproduced with WORD casts;
 *  - divisions by zero (Proj_3D with z+KFactor == 0) crashed on DOS too:
 *    no guard added.
 *
 *  Register-based helpers (Rot, WorldRot, LongWorldRot, RotMatIndex2,
 *  Proj_3D/Proj_ISO, RotList, TransRotList) are exported as plain C
 *  functions for p_ob_iso.c; the stack-based entry points keep the
 *  signatures of engine/LIB_3D/LIB_3D.H.
 *---------------------------------------------------------------------------*/
#include <string.h>

#include "lib3d_p.h"

/*──────────────────────────── data (P_TRIGO.ASM .data) ────────────────────*/

PORT_FASTBSS WORD compteur = 0;                          /* locale, public for P_OB_ISO   */

WORD TypeProj = 0;

PORT_FASTDATA LONG XCentre = 320;                         /* dd in the ASM                 */
PORT_FASTDATA LONG YCentre = 200;

PORT_FASTBSS WORD Xp = 0;
PORT_FASTBSS WORD Yp = 0;

WORD IsoScale = 500;

WORD Z_Min = 0;
WORD Z_Max = 0;

LONG KFactor = 128;
LONG LFactorX = 1024;
LONG LFactorY = 840;

LONG CameraX = 0, CameraY = 0, CameraZ = 0;
LONG CameraXr = 0, CameraYr = 0, CameraZr = 0;

LONG AlphaLight = 0, BetaLight = 0, GammaLight = 0;
LONG Alpha = 0, Beta = 0, Gamma = 0;

PORT_FASTBSS LONG lAlpha = 0, lBeta = 0, lGamma = 0;

LONG NormalXLight = 61, NormalYLight = 0, NormalZLight = 0;

PORT_FASTBSS LONG X0 = 0, Y0 = 0, Z0 = 0;

/* matrix layout: index 0..8 == LT00,LT01,LT02,LT10,...,LT22 */
PORT_FASTBSS LONG LMatriceRot[9] = {0};
PORT_FASTBSS LONG LMatriceDummy[9] = {0};
PORT_FASTBSS LONG LMatriceWorld[9] = {0};
PORT_FASTBSS LONG LMatriceTempo[9] = {0};

PORT_FASTBSS LONG TabMat[9 * 30];                        /* 30 matrices max               */

/*──────────────────────────── list rotation ───────────────────────────────*/

/* ASM: RotList — rotate a list of 16-bit XYZ points through matrix m,
   adding (the low 16 bits of) X0/Y0/Z0, into 16-bit XYZ dst. */
PORT_FASTCODE void RotList(const WORD *src, WORD *dst, const LONG *m, UWORD nbpoints)
{
	LONG x, y, z, t;

	compteur = (WORD)nbpoints;
	do
	{
		x = (WORD)src[0];               /* movsx */
		y = (WORD)src[1];
		z = (WORD)src[2];

		t = ADD32(ADD32(MUL32(m[0], x), MUL32(m[1], y)), MUL32(m[2], z)) >> 14;
		dst[0] = (WORD)((WORD)t + (WORD)X0);
		t = ADD32(ADD32(MUL32(m[3], x), MUL32(m[4], y)), MUL32(m[5], z)) >> 14;
		dst[1] = (WORD)((WORD)t + (WORD)Y0);
		t = ADD32(ADD32(MUL32(m[6], x), MUL32(m[7], y)), MUL32(m[8], z)) >> 14;
		dst[2] = (WORD)((WORD)t + (WORD)Z0);

		src += 3;
		dst += 3;
	} while (--compteur);                   /* dec [compteur]: 0 -> 65536    */
}

/* ASM: TransRotList — same but the (32-bit) translation step lAlpha/lBeta/
   lGamma is added to the source coordinates before rotating. */
PORT_FASTCODE void TransRotList(const WORD *src, WORD *dst, const LONG *m, UWORD nbpoints)
{
	LONG x, y, z, t;

	compteur = (WORD)nbpoints;
	do
	{
		x = (WORD)src[0] + lAlpha;
		y = (WORD)src[1] + lBeta;
		z = (WORD)src[2] + lGamma;

		t = ADD32(ADD32(MUL32(m[0], x), MUL32(m[1], y)), MUL32(m[2], z)) >> 14;
		dst[0] = (WORD)((WORD)t + (WORD)X0);
		t = ADD32(ADD32(MUL32(m[3], x), MUL32(m[4], y)), MUL32(m[5], z)) >> 14;
		dst[1] = (WORD)((WORD)t + (WORD)Y0);
		t = ADD32(ADD32(MUL32(m[6], x), MUL32(m[7], y)), MUL32(m[8], z)) >> 14;
		dst[2] = (WORD)((WORD)t + (WORD)Z0);

		src += 3;
		dst += 3;
	} while (--compteur);
}

/*──────────────────────────── point rotation ──────────────────────────────*/

/* ASM: Rot — rotate a 16-bit point through LMatriceRot -> X0/Y0/Z0 (LONG). */
PORT_FASTCODE void Rot(WORD x, WORD y, WORD z)
{
	LONG ex = x, ey = y, ez = z;

	X0 = ADD32(ADD32(MUL32(LMatriceRot[0], ex), MUL32(LMatriceRot[1], ey)),
	           MUL32(LMatriceRot[2], ez)) >> 14;
	Y0 = ADD32(ADD32(MUL32(LMatriceRot[3], ex), MUL32(LMatriceRot[4], ey)),
	           MUL32(LMatriceRot[5], ez)) >> 14;
	Z0 = ADD32(ADD32(MUL32(LMatriceRot[6], ex), MUL32(LMatriceRot[7], ey)),
	           MUL32(LMatriceRot[8], ez)) >> 14;
}

/* ASM: RotatePoint (C entry) */
PORT_FASTCODE void RotatePoint(LONG X, LONG Y, LONG Z)
{
	Rot((WORD)X, (WORD)Y, (WORD)Z);
}

/* ASM: WorldRot — 16-bit inputs, 32-bit wrap math on LMatriceWorld. */
void WorldRot(WORD x, WORD y, WORD z)
{
	LONG ex = x, ey = y, ez = z;

	X0 = ADD32(ADD32(MUL32(LMatriceWorld[0], ex), MUL32(LMatriceWorld[1], ey)),
	           MUL32(LMatriceWorld[2], ez)) >> 14;
	Y0 = ADD32(ADD32(MUL32(LMatriceWorld[3], ex), MUL32(LMatriceWorld[4], ey)),
	           MUL32(LMatriceWorld[5], ez)) >> 14;
	Z0 = ADD32(ADD32(MUL32(LMatriceWorld[6], ex), MUL32(LMatriceWorld[7], ey)),
	           MUL32(LMatriceWorld[8], ez)) >> 14;
}

/* ASM: WorldRotatePoint (C entry) */
void WorldRotatePoint(LONG X, LONG Y, LONG Z)
{
	WorldRot((WORD)X, (WORD)Y, (WORD)Z);
}

/* ASM: LongWorldRot — full 32-bit inputs, 64-bit accumulation
   (imul/add+adc/shrd 14), result = low 32 bits. */
void LongWorldRot(LONG x, LONG y, LONG z)
{
	long long t;

	t = (long long)LMatriceWorld[0] * x + (long long)LMatriceWorld[1] * y
	  + (long long)LMatriceWorld[2] * z;
	X0 = (LONG)(t >> 14);
	t = (long long)LMatriceWorld[3] * x + (long long)LMatriceWorld[4] * y
	  + (long long)LMatriceWorld[5] * z;
	Y0 = (LONG)(t >> 14);
	t = (long long)LMatriceWorld[6] * x + (long long)LMatriceWorld[7] * y
	  + (long long)LMatriceWorld[8] * z;
	Z0 = (LONG)(t >> 14);
}

/* ASM: LongWorldRotatePoint (C entry) */
void LongWorldRotatePoint(LONG X, LONG Y, LONG Z)
{
	LongWorldRot(X, Y, Z);
}

/* ASM: LongInverseRot — LMatriceWorld transposed (inverse of a rotation). */
void LongInverseRot(LONG x, LONG y, LONG z)
{
	long long t;

	t = (long long)LMatriceWorld[0] * x + (long long)LMatriceWorld[3] * y
	  + (long long)LMatriceWorld[6] * z;
	X0 = (LONG)(t >> 14);
	t = (long long)LMatriceWorld[1] * x + (long long)LMatriceWorld[4] * y
	  + (long long)LMatriceWorld[7] * z;
	Y0 = (LONG)(t >> 14);
	t = (long long)LMatriceWorld[2] * x + (long long)LMatriceWorld[5] * y
	  + (long long)LMatriceWorld[8] * z;
	Z0 = (LONG)(t >> 14);
}

/* ASM: LongInverseRotatePoint (C entry) */
void LongInverseRotatePoint(LONG X, LONG Y, LONG Z)
{
	LongInverseRot(X, Y, Z);
}

/*──────────────────────────── matrix work ─────────────────────────────────*/

/* ASM: RotMatIndex2 — rotate the matrix at msrc by lAlpha/lGamma/lBeta into
   mdest (LMatriceDummy used as intermediate exactly like the ASM).
   NB: the ASM comments say ">>15" but the code does sar 14. */
PORT_FASTCODE void RotMatIndex2(const LONG *msrc, LONG *mdest)
{
	const LONG *m = msrc;
	LONG e, s, c;

	e = lAlpha;
	if (e != 0)
	{
		e &= 1023;
		s = (WORD)P_SinTab[e];
		c = (WORD)P_SinTab[(e + 256) & 1023];

		mdest[0] = m[0];
		mdest[3] = m[3];
		mdest[6] = m[6];
		mdest[1] = ADD32(MUL32(m[1], c), MUL32(m[2], s)) >> 14;
		mdest[2] = SUB32(MUL32(m[2], c), MUL32(m[1], s)) >> 14;
		mdest[4] = ADD32(MUL32(m[4], c), MUL32(m[5], s)) >> 14;
		mdest[5] = SUB32(MUL32(m[5], c), MUL32(m[4], s)) >> 14;
		mdest[7] = ADD32(MUL32(m[7], c), MUL32(m[8], s)) >> 14;
		mdest[8] = SUB32(MUL32(m[8], c), MUL32(m[7], s)) >> 14;

		m = mdest;
	}

	e = lGamma;
	if (e != 0)
	{
		e &= 1023;
		s = (WORD)P_SinTab[e];
		c = (WORD)P_SinTab[(e + 256) & 1023];

		LMatriceDummy[2] = m[2];
		LMatriceDummy[5] = m[5];
		LMatriceDummy[8] = m[8];
		LMatriceDummy[0] = ADD32(MUL32(m[0], c), MUL32(m[1], s)) >> 14;
		LMatriceDummy[1] = SUB32(MUL32(m[1], c), MUL32(m[0], s)) >> 14;
		LMatriceDummy[3] = ADD32(MUL32(m[3], c), MUL32(m[4], s)) >> 14;
		LMatriceDummy[4] = SUB32(MUL32(m[4], c), MUL32(m[3], s)) >> 14;
		LMatriceDummy[6] = ADD32(MUL32(m[6], c), MUL32(m[7], s)) >> 14;
		LMatriceDummy[7] = SUB32(MUL32(m[7], c), MUL32(m[6], s)) >> 14;

		m = LMatriceDummy;
	}

	e = lBeta;
	if (e == 0)
	{
		if (m != mdest)
		{
			memcpy(mdest, m, 9 * sizeof(LONG));
		}
		return;
	}

	if (m == mdest)
	{
		memcpy(LMatriceDummy, mdest, 9 * sizeof(LONG));
		m = LMatriceDummy;
	}

	e &= 1023;
	s = (WORD)P_SinTab[e];
	c = (WORD)P_SinTab[(e + 256) & 1023];

	mdest[1] = m[1];
	mdest[4] = m[4];
	mdest[7] = m[7];
	mdest[0] = SUB32(MUL32(m[0], c), MUL32(m[2], s)) >> 14;
	mdest[2] = ADD32(MUL32(m[2], c), MUL32(m[0], s)) >> 14;
	mdest[3] = SUB32(MUL32(m[3], c), MUL32(m[5], s)) >> 14;
	mdest[5] = ADD32(MUL32(m[5], c), MUL32(m[3], s)) >> 14;
	mdest[6] = SUB32(MUL32(m[6], c), MUL32(m[8], s)) >> 14;
	mdest[8] = ADD32(MUL32(m[8], c), MUL32(m[6], s)) >> 14;
}

/* ASM: RotMatW — rotate MatriceWorld into MatriceRot. */
void RotMatW(void)
{
	RotMatIndex2(LMatriceWorld, LMatriceRot);
}

/* ASM: RotateMatriceWorld (C entry) */
void RotateMatriceWorld(LONG palpha, LONG pbeta, LONG pgamma)
{
	lAlpha = palpha;
	lBeta = pbeta;
	lGamma = pgamma;
	RotMatIndex2(LMatriceWorld, LMatriceRot);
}

/* ASM: CopyMatrice */
void CopyMatrice(LONG *MatSrc, LONG *MatDest)
{
	memcpy(MatDest, MatSrc, 9 * sizeof(LONG));
}

/* ASM: FlipMatrice (internal: not public in the ASM either) */
static void FlipMatrice(const LONG *matsour, LONG *matdest)
{
	matdest[0] = matsour[0];
	matdest[3] = matsour[1];
	matdest[6] = matsour[2];
	matdest[1] = matsour[3];
	matdest[4] = matsour[4];
	matdest[7] = matsour[5];
	matdest[2] = matsour[6];
	matdest[5] = matsour[7];
	matdest[8] = matsour[8];
}

/*──────────────────────────── light ───────────────────────────────────────*/

/* ASM: SetLightVector */
void SetLightVector(WORD alpha, WORD beta, WORD gamma)
{
	AlphaLight = alpha;
	lAlpha = alpha;
	BetaLight = beta;
	lBeta = beta;
	GammaLight = gamma;                     /* "pfeu" */
	lGamma = gamma;

	RotMatW();

	Rot(0, 0, NORMAL_UNIT - 5);             /* "je sais je sais..." */

	NormalXLight = X0;
	NormalYLight = Y0;
	NormalZLight = Z0;
}

/*──────────────────────────── camera ──────────────────────────────────────*/

/* ASM: SetPosCamera */
void SetPosCamera(LONG poswx, LONG poswy, LONG poswz)
{
	CameraX = poswx;
	CameraY = poswy;
	CameraZ = poswz;
}

/* ASM: SetAngleCamera — build LMatriceWorld from the three angles then
   rotate the camera position into CameraXr/Yr/Zr. */
void SetAngleCamera(LONG palpha, LONG pbeta, LONG pgamma)
{
	LONG s, c, s2, c2, h, e;

	e = palpha & 1023;
	Alpha = e;
	s = (WORD)P_SinTab[e];
	c = (WORD)P_SinTab[(e + 256) & 1023];

	e = pgamma & 1023;
	Gamma = e;
	s2 = (WORD)P_SinTab[e];
	c2 = (WORD)P_SinTab[(e + 256) & 1023];

	LMatriceWorld[0] = c2;                                   /* LMatW00 */
	LMatriceWorld[1] = -s2;                                  /* LMatW01 */
	LMatriceWorld[3] = MUL32(s2, c) >> 14;                   /* LMatW10 */
	LMatriceWorld[4] = MUL32(c2, c) >> 14;                   /* LMatW11 */
	LMatriceWorld[6] = MUL32(s2, s) >> 14;                   /* LMatW20 */
	LMatriceWorld[7] = MUL32(c2, s) >> 14;                   /* LMatW21 */

	e = pbeta & 1023;
	Beta = e;
	s2 = (WORD)P_SinTab[e];
	c2 = (WORD)P_SinTab[(e + 256) & 1023];

	h = LMatriceWorld[0];
	LMatriceWorld[0] = MUL32(c2, h) >> 14;                   /* LMatW00 */
	LMatriceWorld[2] = MUL32(s2, h) >> 14;                   /* LMatW02 */
	h = LMatriceWorld[3];
	LMatriceWorld[3] = ADD32(MUL32(c2, h), MUL32(s2, s)) >> 14;
	LMatriceWorld[5] = SUB32(MUL32(s2, h), MUL32(c2, s)) >> 14;
	h = LMatriceWorld[6];
	LMatriceWorld[6] = SUB32(MUL32(c2, h), MUL32(s2, c)) >> 14;
	LMatriceWorld[8] = ADD32(MUL32(s2, h), MUL32(c2, c)) >> 14;

	LongWorldRot(CameraX, CameraY, CameraZ);

	CameraXr = X0;
	CameraYr = Y0;
	CameraZr = Z0;
}

/* ASM: SetInverseAngleCamera */
void SetInverseAngleCamera(LONG palpha, LONG pbeta, LONG pgamma)
{
	SetAngleCamera(palpha, pbeta, pgamma);

	FlipMatrice(LMatriceWorld, LMatriceDummy);
	CopyMatrice(LMatriceDummy, LMatriceWorld);

	LongWorldRot(CameraX, CameraY, CameraZ);

	CameraXr = X0;
	CameraYr = Y0;
	CameraZr = Z0;
}

/* ASM: SetFollowCamera */
void SetFollowCamera(LONG targetx, LONG targety, LONG targetz,
                     LONG camalpha, LONG cambeta, LONG camgamma, LONG camzoom)
{
	CameraX = targetx;
	CameraY = targety;
	CameraZ = targetz;

	SetAngleCamera(camalpha, cambeta, camgamma);

	CameraZr += camzoom;

	LongInverseRot(CameraXr, CameraYr, CameraZr);

	CameraX = X0;
	CameraY = Y0;
	CameraZ = Z0;
}

/*──────────────────────────── 2D rotation ─────────────────────────────────*/

/* ASM: RotXY / Rotate — X' = X*cos+Z*sin, Z' = Z*cos-X*sin (results LONG). */
void Rotate(LONG coorx, LONG coory, LONG angle)
{
	WORD x = (WORD)coorx;
	WORD z = (WORD)coory;
	UWORD t = (UWORD)angle;
	LONG s, c;

	if ((WORD)t == 0)                       /* or bp,bp (16 bit) */
	{
		X0 = x;
		Y0 = z;
		return;
	}

	s = (WORD)P_SinTab[t & 0x3FF];
	c = (WORD)P_SinTab[(t + 256) & 0x3FF];

	X0 = ADD32(MUL32(x, c), MUL32(z, s)) >> 14;
	Y0 = SUB32(MUL32(z, c), MUL32(x, s)) >> 14;
}

/*──────────────────────────── projection ──────────────────────────────────*/

/* ASM: Proj_3D — 16-bit register perspective projection.
   z + KFactor == 0 divides by zero (same crash as the DOS idiv). */
PORT_FASTCODE void Proj_3D(WORD x, WORD y, WORD z, WORD *xp, WORD *yp)
{
	WORD bp = (WORD)(z + (WORD)KFactor);
	LONG t;

	if (bp < 0)
	{
		bp = 32767;                         /* overflow: max value */
	}

	t = (LONG)x * (WORD)LFactorX;           /* imul: 16x16 -> 32   */
	*xp = (WORD)((WORD)(t / bp) + (WORD)XCentre);

	t = (LONG)(WORD)(-y) * (WORD)LFactorY;  /* neg ax; imul        */
	*yp = (WORD)((WORD)(t / bp) + (WORD)YCentre);
}

/* ASM: Proj_ISO — fixed isometric projection (IsoScale hardwired to 512):
     Xp = ((x - z) * 24) >> 9 + XCentre
     Yp = -((y*30 - (z + x)*12) >> 9) + YCentre     (neg/add on 16 bits)   */
PORT_FASTCODE void Proj_ISO(LONG x, LONG y, LONG z, WORD *xp, WORD *yp)
{
	LONG a, p, yy;

	a = SUB32(x, z);
	p = ADD32(z, x);

	a = MUL32(a, 24) >> 9;
	*xp = (WORD)((WORD)a + (WORD)XCentre);

	yy = SUB32(MUL32(y, 30), MUL32(p, 12)) >> 9;
	*yp = (WORD)((WORD)(-(WORD)yy) + (WORD)YCentre);
}

/* ASM: ProjettePoint — camera-relative projection into Xp/Yp.
   Returns -1 if projected, 0 if Z-clipped (16-bit sign test, faithful). */
PORT_FASTCODE LONG ProjettePoint(LONG CoorX, LONG CoorY, LONG CoorZ)
{
	if (TypeProj != TYPE_ISO)
	{
		LONG ex = CoorX - CameraXr;
		LONG ey = CoorY - CameraYr;
		LONG ez = CameraZr - CoorZ;

		if ((WORD)ez < 0)                   /* or cx,cx ; js error */
		{
			Xp = 0;
			Yp = 0;
			return 0;
		}

		Proj_3D((WORD)ex, (WORD)ey, (WORD)ez, &Xp, &Yp);
		return -1;
	}

	Proj_ISO(CoorX, CoorY, CoorZ, &Xp, &Yp);
	return -1;
}

/* ASM: LongProjettePoint — 32-bit projection with 16-bit saturation
   (positive overflow -> 0x7FFF, negative -> 0x8000). */
PORT_FASTCODE LONG LongProjettePoint(LONG CoorX, LONG CoorY, LONG CoorZ)
{
	LONG ex = CoorX - CameraXr;
	LONG ey = CoorY - CameraYr;
	LONG ez = CameraZr - CoorZ;
	LONG qx, qy;
	long long t;

	if (ez < 0)
	{
		goto error;
	}

	ez = ADD32(ez, KFactor);
	if (ez < 0)
	{
		goto error;
	}

	t = (long long)ex * LFactorX;
	qx = ADD32((LONG)(t / ez), XCentre);

	t = (long long)(LONG)(0 - (ULONG)ey) * LFactorY;
	qy = ADD32((LONG)(t / ez), YCentre);

	if (qx > 32767 || qx < -32768)
	{
		Xp = (WORD)(0x7FFF + (((ULONG)qx >> 31) & 1));  /* shl/adc trick */
	}
	else
	{
		Xp = (WORD)qx;
	}

	if (qy > 32767 || qy < -32768)
	{
		Yp = (WORD)(0x7FFF + (((ULONG)qy >> 31) & 1));
	}
	else
	{
		Yp = (WORD)qy;
	}

	return -1;

error:
	Xp = 0;
	Yp = 0;
	return 0;
}

/* ASM: SetProjection */
void SetProjection(LONG xc, LONG yc, LONG kfact, LONG lfactx, LONG lfacty)
{
	XCentre = xc;
	YCentre = yc;
	KFactor = kfact;
	LFactorX = lfactx;
	LFactorY = lfacty;
	TypeProj = TYPE_3D;
}

/* ASM: SetIsoProjection */
void SetIsoProjection(LONG xc, LONG yc, LONG scale)
{
	XCentre = xc;
	YCentre = yc;
	IsoScale = (WORD)scale;
	TypeProj = TYPE_ISO;
}

/*──────────────────────────── backface test ───────────────────────────────*/

/* ASM: TestVuePoly — cross product sign on the first 3 points of a
   (coul,x,y) list; 1 = visible.  16-bit deltas, 32-bit products, exact
   sub/sbb comparison (done in 64 bits). */
LONG TestVuePoly(WORD *ptrpoly)
{
	LONG p1, p2;

	p1 = (LONG)(WORD)(ptrpoly[2] - ptrpoly[8])
	   * (WORD)(ptrpoly[4] - ptrpoly[1]);
	p2 = (LONG)(WORD)(ptrpoly[1] - ptrpoly[7])
	   * (WORD)(ptrpoly[5] - ptrpoly[2]);

	if ((long long)p2 - (long long)p1 < 0)  /* sub/sbb ; jnl */
	{
		return 1;
	}
	return 0;
}
