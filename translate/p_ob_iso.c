/*----------------------------------------------------------------------------
 *  p_ob_iso.c — C translation of LIB386/LIB_3D/P_OB_ISO.ASM (Adeline 1993)
 *
 *  3D object ("body") renderer.  Flow of AffObjetIso:
 *
 *   1. object position: TYPE_3D -> LongWorldRot(pos) - CameraXr/Yr/Zr,
 *      TYPE_ISO -> position used as-is (PosXWr/PosYWr/PosZWr).
 *   2. header: word[0]=flags, body data starts at 16 + word[14].
 *   3. vertex pipeline:
 *      - INFO_ANIM (all LBA1 bodies): AnimNuage
 *          . group 0 rotated by the object angles (matrix -> TabMat[0]),
 *          . each further group (38-byte records) either RotateGroupe or
 *            TranslateGroupe relative to its parent matrix (TabMat, org
 *            offset pre-multiplied by 36 by PatchObjet) around its org
 *            point; results accumulate in List_Anim_Point (WORD xyz),
 *          . projection List_Anim_Point -> List_Point (Xp,Yp,Zsort WORDs):
 *            ISO: Xp=((x+zr)*24>>9)+XCentre, Yp=((12(x-zr)-30y)>>9)+YCentre,
 *                 Zsort=(zr-x-y) low 16 bits, zr=-(z+PosZWr);
 *            3D:  perspective with 64-bit imul/idiv and 16-bit saturation.
 *            Screen bounding box tracked in ScreenXmin/... (16-bit).
 *          . ComputeAnimNormal: per group, TabMat matrix pre-multiplied by
 *            the light vector, normals dotted -> List_Normal intensities
 *            (16-bit division by the pre-normalized range).
 *      - static objects: RotateNuage + ComputeStaticNormal (NB: this path
 *        feeds Proj_ISO with the exact 16/32-bit register mix of the ASM,
 *        upper halves included — see the "composed" values below).
 *   4. entity build: polys (backface-culled with the 16-bit cross product),
 *      lines, spheres are copied into List_Entity (screen coords, colours
 *      resolved from List_Normal) and referenced by List_Tri records
 *      {Zsort, type, ptr}.
 *   5. sort: "SergeSort" — descending Z hybrid quicksort (explicit stack,
 *      selection sort under 8 elements) — transliterated 1:1 because tie
 *      order affects the painter's algorithm.
 *   6. draw: E_POLY -> TabPoly + ComputePoly_A + FillVertic_A,
 *      E_LIGNE -> Line_A (colour byte-swapped!), E_SPHERE -> radius
 *      projected (ISO: *34>>9) + ComputeSphere_A + FillVertic_A.
 *
 *  Returns 0 if at least one entity was displayed, 1 otherwise (and the
 *  Screen box is set to -1).
 *
 *  DS tuning notes: hot data = List_Anim_Point/List_Point (6 bytes/vertex),
 *  List_Entity, List_Tri, TabMat — ~22KB total, DTCM candidates; hot code =
 *  RotList/TransRotList (p_trigo.c), the projection loops, EdgeGauche/
 *  EdgeDroite + FillVertic (ITCM candidates).
 *
 *  All object data is read via 16-bit loads (memcpy word helpers): no
 *  unaligned 32-bit access is performed on ARM.  List_Entity records are
 *  only 2-aligned (poly records advance by 6*n+4 bytes), hence WORD-wise
 *  handling throughout.
 *---------------------------------------------------------------------------*/
#include <string.h>

#include "lib3d_p.h"

/*──────────────────────────── data (P_OB_ISO.ASM .data) ───────────────────*/

/* DTCM budget note: calico puts the MAIN THREAD STACK at the top of DTCM,
   growing down into whatever .dtcm.bss leaves free (16000 bytes total).
   Filling DTCM to ~15K starved the stack to ~1K -> guru meditation inside
   calico (ntrcard mutex) at boot.  Keep the DTCM set <= ~7K so the stack
   retains ~9K: per-scanline fill tables (s_poly.c) + List_Point only. */
PORT_FASTBSS WORD List_Point[500 * 3];  /* Xp Yp Zrot            (DTCM 3K)   */
static WORD List_Normal[500];           /* "surement plus"                   */
WORD List_Anim_Point[500 * 3];          /* Xr Yr Zr                          */
WORD List_Entity[5000];                 /* TAILLE à determinée               */

typedef struct                          /* List_Tri entry (8 bytes on ILP32) */
{
	WORD z;
	WORD type;
	WORD *ptr;
} T_TRI;

static T_TRI List_Tri[500];

WORD ScreenXmin = 0;
WORD ScreenYmin = 0;
WORD ScreenXmax = 0;
WORD ScreenYmax = 0;

WORD NbPoints = 0;
static WORD TotalEntite = 0;
static WORD ZMax = 0;
static WORD NbGroupes = 0;
static WORD Infos = 0;
static WORD Count1 = 0;
WORD FlagLight = 1;

static T_TRI *PointeurListTri;
static UBYTE *ListGroupe;
static UBYTE *OffsetDefPoint;
static LONG *Ptr1mat;                   /* [Ptr1] when used as matrix ptr    */

static LONG PosXWr, PosYWr, PosZWr;     /* pos World rotée de l'objet        */

/*──────────────────────────── word access helpers ─────────────────────────*/

PORT_FASTCODE static WORD RW(const UBYTE *p)
{
	WORD v;

	memcpy(&v, p, 2);
	return v;
}

PORT_FASTCODE static void WW(UBYTE *p, WORD v)
{
	memcpy(p, &v, 2);
}

/*═════════════════════════════ PatchObjet ═════════════════════════════════*/

/* ASM: PatchObjet — converts each group's org offset from *38 (file stride)
   to *36 (TabMat matrix stride).  Group 0 (org == -1) untouched. */
void PatchObjet(void *ptrobj)
{
	UBYTE *esi = (UBYTE *)ptrobj;
	UWORD cx, nb;

	if (!(RW(esi) & INFO_ANIM))
	{
		return;
	}

	esi += (UWORD)RW(esi + 14) + 16;    /* saute zone info */

	nb = (UWORD)RW(esi);                /* nb points */
	esi += 2;
	esi += (ULONG)nb * 6;               /* saute defpoint */

	cx = (UWORD)RW(esi);
	esi += 2;

	cx = (UWORD)(cx - 1);               /* - groupe 0 */
	if (cx == 0)
	{
		return;
	}
	do
	{
		UWORD v;

		esi += 38;                      /* size d'un groupe */
		v = (UWORD)RW(esi + 6);         /* orggroupe * 38 */
		WW(esi + 6, (WORD)(((ULONG)v * 36) / 38));
	} while (--cx);
}

/*═════════════════════════════ groupes ════════════════════════════════════*/

/* ASM: RotateGroupe */
PORT_FASTCODE static void RotateGroupe(const UBYTE *esi, LONG a, LONG b, LONG g)
{
	ULONG start, nb, org;
	const LONG *msrc;

	lAlpha = a;
	lBeta = b;
	lGamma = g;

	start = (UWORD)RW(esi);             /* Start point deja *6 (bytes)       */
	nb = (UWORD)RW(esi + 2);            /* nb points                         */

	org = (UWORD)RW(esi + 6);           /* orggroupe deja *36 (bytes)        */
	if ((WORD)org == -1)
	{
		msrc = LMatriceWorld;
		X0 = 0;                         /* XYZ0 ChgRepere */
		Y0 = 0;
		Z0 = 0;
	}
	else
	{
		ULONG opt = (UWORD)RW(esi + 4); /* Org Point index deja *6           */

		msrc = TabMat + org / 4;        /* EBP Matrice du groupe org         */
		X0 = (WORD)List_Anim_Point[opt / 2];
		Y0 = (WORD)List_Anim_Point[opt / 2 + 1];
		Z0 = (WORD)List_Anim_Point[opt / 2 + 2];
	}

	RotMatIndex2(msrc, Ptr1mat);        /* rot de Mtempo vers Mrot & Mgroup  */

	RotList((const WORD *)(const void *)(OffsetDefPoint + start),
	        List_Anim_Point + start / 2, Ptr1mat, (UWORD)nb);
}

/* ASM: TranslateGroupe — lAlpha/lBeta/lGamma are translation steps here. */
PORT_FASTCODE static void TranslateGroupe(const UBYTE *esi, LONG a, LONG b, LONG g)
{
	ULONG start, nb, org;
	const LONG *msrc;

	lAlpha = a;
	lBeta = b;
	lGamma = g;

	org = (UWORD)RW(esi + 6);           /* orggroupe deja *36                */
	if ((WORD)org == -1)
	{
		X0 = 0;
		Y0 = 0;
		Z0 = 0;
		msrc = LMatriceWorld;
	}
	else
	{
		ULONG opt = (UWORD)RW(esi + 4);

		msrc = TabMat + org / 4;
		X0 = (WORD)List_Anim_Point[opt / 2];
		Y0 = (WORD)List_Anim_Point[opt / 2 + 1];
		Z0 = (WORD)List_Anim_Point[opt / 2 + 2];
	}

	memcpy(Ptr1mat, msrc, 9 * sizeof(LONG));    /* Mgroup = Morg */

	start = (UWORD)RW(esi);
	nb = (UWORD)RW(esi + 2);

	TransRotList((const WORD *)(const void *)(OffsetDefPoint + start),
	             List_Anim_Point + start / 2, Ptr1mat, (UWORD)nb);
}

/*═════════════════════════════ ANIM NUAGE ═════════════════════════════════*/

/* ASM: ComputeAnimNormal — per-group light intensities into List_Normal.
   The group matrix is pre-multiplied by the light vector; the full 9-term
   dot product accumulates with 32-bit wrap; negative -> intensity 0, else
   (WORD)(sum>>14) / prenormalized-range (cwd+idiv: 16-bit dividend!). */
PORT_FASTCODE static UBYTE *ComputeAnimNormal(UBYTE *esi)
{
	WORD *outp;
	const LONG *mat;
	const UBYTE *grpn;

	if (RW(esi) == 0)                   /* nbobj normal faces/points */
	{
		return esi + 2;
	}
	esi += 2;

	outp = List_Normal;                 /* Ptr1 */
	mat = TabMat;                       /* Ptr2 */
	grpn = ListGroupe + 18;             /* Save1: sur NbNormal */

	Count1 = NbGroupes;
	do
	{
		UWORD cnt = (UWORD)RW(grpn);

		if (cnt != 0)
		{
			LONG l;

			compteur = (WORD)cnt;

			/* copy Mgroup vers Mrot & Premultiply par Normallight */
			l = NormalXLight;
			LMatriceRot[0] = MUL32(mat[0], l);
			LMatriceRot[1] = MUL32(mat[1], l);
			LMatriceRot[2] = MUL32(mat[2], l);
			l = NormalYLight;
			LMatriceRot[3] = MUL32(mat[3], l);
			LMatriceRot[4] = MUL32(mat[4], l);
			LMatriceRot[5] = MUL32(mat[5], l);
			l = NormalZLight;
			LMatriceRot[6] = MUL32(mat[6], l);
			LMatriceRot[7] = MUL32(mat[7], l);
			LMatriceRot[8] = MUL32(mat[8], l);

			do
			{
				LONG x = (WORD)RW(esi);
				LONG y = (WORD)RW(esi + 2);
				LONG z = (WORD)RW(esi + 4);
				LONG acc;
				WORD inten = 0;

				acc = ADD32(ADD32(MUL32(LMatriceRot[0], x),
				                  MUL32(LMatriceRot[1], y)),
				            MUL32(LMatriceRot[2], z));
				acc = ADD32(acc, MUL32(LMatriceRot[3], x));
				acc = ADD32(acc, MUL32(LMatriceRot[4], y));
				acc = ADD32(acc, MUL32(LMatriceRot[5], z));
				acc = ADD32(acc, MUL32(LMatriceRot[6], x));
				acc = ADD32(acc, MUL32(LMatriceRot[7], y));
				acc = ADD32(acc, MUL32(LMatriceRot[8], z));

				if (acc >= 0)           /* js nointensity */
				{
					/* sar eax,14 ; cwd ; idiv word[esi+6] */
					inten = (WORD)((LONG)(WORD)(acc >> 14) / (WORD)RW(esi + 6));
				}
				*outp++ = inten;        /* stock intensity */

				esi += 8;
			} while (--compteur);
		}

		grpn += 38;
		mat += 9;
	} while (--Count1);

	return esi;
}

/* ASM: AnimNuage — rotate all groups, project, compute normals.
   Returns the data pointer positioned on the polygon section. */
PORT_FASTCODE static UBYTE *AnimNuage(UBYTE *esi)
{
	UWORD nbp, cx;
	UBYTE *grp;

	nbp = (UWORD)RW(esi);               /* nb points */
	esi += 2;
	NbPoints = (WORD)nbp;

	OffsetDefPoint = esi;
	esi += (ULONG)nbp * 6;              /* saute defpoint */

	NbGroupes = RW(esi);
	esi += 2;
	ListGroupe = esi;                   /* memo start list groupe */

	/* rotations du groupe 0 (aux valeurs de rot locale) */
	Ptr1mat = TabMat;
	RotateGroupe(esi, lAlpha, lBeta, lGamma);
	grp = esi + 38;                     /* size d'un groupe */

	/* rotations/translations/zooms des groupes */
	cx = (UWORD)(NbGroupes - 1);        /* - groupe 0 */
	if (cx != 0)
	{
		Count1 = (WORD)cx;
		Ptr1mat = TabMat + 9;
		do
		{
			WORD w8 = RW(grp + 8);      /* type anim groupe */
			LONG alpha = (WORD)RW(grp + 10);    /* Alpha ou stepX (sar 16)   */
			LONG beta = (WORD)RW(grp + 12);     /* Beta ou stepY (movsx dx)  */
			LONG gamma = (WORD)RW(grp + 14);    /* Gamma ou stepZ (sar 16)   */

			if (w8 == TYPE_ROTATE)
			{
				RotateGroupe(grp, alpha, beta, gamma);
			}
			else if (w8 == TYPE_TRANSLATE)
			{
				TranslateGroupe(grp, alpha, beta, gamma);
			}

			Ptr1mat += 9;               /* next matrix */
			grp += 38;
		} while (--Count1);
	}

	/* projette liste */
	Count1 = NbPoints;
	{
		const WORD *src = List_Anim_Point;
		WORD *dst = List_Point;

		if (TypeProj == TYPE_3D)
		{
			/* boucleproj3D */
			do
			{
				LONG ex = (WORD)src[0] + PosXWr;
				LONG ey = (WORD)src[1] + PosYWr;
				LONG ep = (WORD)src[2] + PosZWr;
				LONG q;
				long long t;
				WORD w;

				src += 3;
				ep = (LONG)(0 - (ULONG)ep);         /* Z rot */
				ep = ADD32(ep, KFactor);
				if (ep <= 0)
				{
					ep = 0x7FFFFFFF;                /* overflow: max value */
				}

				t = (long long)ex * LFactorX;       /* imul/idiv 64/32 */
				q = ADD32((LONG)(t / ep), XCentre);
				if ((ULONG)q > 0xFFFFUL)
				{
					w = (WORD)(((ULONG)q >> 16) | 0x7FFF);  /* overX */
				}
				else
				{
					w = (WORD)q;
				}
				dst[0] = w;                         /* stock Xp */
				if (w < ScreenXmin)
				{
					ScreenXmin = w;
				}
				if (w > ScreenXmax)
				{
					ScreenXmax = w;
				}

				t = (long long)(LONG)(0 - (ULONG)ey) * LFactorY;    /* -Y */
				q = ADD32((LONG)(t / ep), YCentre);
				if ((ULONG)q > 0xFFFFUL)
				{
					w = (WORD)(((ULONG)q >> 16) | 0x7FFF);  /* overY */
				}
				else
				{
					w = (WORD)q;
				}
				dst[1] = w;                         /* stock Yp */
				if (w < ScreenYmin)
				{
					ScreenYmin = w;
				}
				if (w > ScreenYmax)
				{
					ScreenYmax = w;
				}

				if ((ULONG)ep > 0xFFFFUL)
				{
					w = (WORD)(((ULONG)ep >> 16) | 0x7FFF); /* overZ */
				}
				else
				{
					w = (WORD)ep;
				}
				dst[2] = w;                         /* stock Zrot pour tri */
				dst += 3;
			} while (--Count1);
		}
		else
		{
			/* boucleproj (ISO) */
			do
			{
				LONG ex = (WORD)src[0] + PosXWr;    /* eax X rot */
				LONG ey = (WORD)src[1] + PosYWr;    /* ebx Y rot */
				LONG ep = (WORD)src[2] + PosZWr;
				LONG ec, sv;
				WORD w, zw;

				src += 3;
				ep = (LONG)(0 - (ULONG)ep);         /* Z rot (zrot) */

				ec = MUL32(ADD32(ep, ex), 24) >> 9; /* (x+zrot)*24 /512 */
				w = (WORD)((WORD)ec + (WORD)XCentre);
				dst[0] = w;                         /* stock Xp */
				if (w < ScreenXmin)
				{
					ScreenXmin = w;
				}
				if (w > ScreenXmax)
				{
					ScreenXmax = w;
				}

				sv = ex;
				ex = SUB32(ex, ep);                 /* x - zrot */
				ep = SUB32(ep, sv);                 /* zrot - x pour bon tri */

				ec = MUL32(ex, 12);                 /* (x-zrot) * 12 */

				/* essai de correction tri par influence du y (16 bit) */
				zw = (UWORD)((WORD)ep - (WORD)ey);

				ec = ADD32(ec, SUB32(MUL32(ey, 2), MUL32(ey, 32))); /* -y*30 */
				ec >>= 9;                           /* /512 IsoScale */
				w = (WORD)((WORD)ec + (WORD)YCentre);
				dst[1] = w;                         /* stock Yp */
				if (w < ScreenYmin)
				{
					ScreenYmin = w;
				}
				if (w > ScreenYmax)
				{
					ScreenYmax = w;
				}

				dst[2] = (WORD)zw;                  /* stock Zrot pour tri */
				dst += 3;
			} while (--Count1);
		}
	}

	return ComputeAnimNormal(grp);      /* esi = Save1 (after groups) */
}

/*═════════════════════════════ ROTATE NUAGE (static) ══════════════════════*/

/* ASM: RotateNuage + ComputeStaticNormal — static (non-anim) objects.
   NB (faithful): the ASM feeds Proj_ISO with 16-bit values whose upper
   register halves are leftovers (eax: Y0's, ebx: Z0's, ebp: x*LMat20's) —
   reproduced exactly below ("passer les param en long !!!" in the source). */
PORT_FASTCODE static UBYTE *RotateNuage(UBYTE *esi)
{
	UWORD n;
	WORD *edi;

	RotMatW();                          /* Rotate MWorld vers Mrot */

	n = (UWORD)RW(esi);                 /* nbobj points */
	esi += 2;
	NbPoints = (WORD)n;

	edi = List_Point;
	Count1 = (WORD)n;

	do
	{
		WORD x = RW(esi);
		WORD y = RW(esi + 2);
		WORD z = RW(esi + 4);
		LONG eaxc, ebxc, ebpc;
		WORD xp, yp, bp16;

		esi += 6;

		Rot(x, y, z);                   /* voir pour Coor rotées */

		/* change repere — 16-bit ops on full registers (upper halves kept) */
		eaxc = (LONG)(((ULONG)Y0 & 0xFFFF0000UL)
		            | (UWORD)((WORD)X0 - (WORD)PosXWr));
		ebxc = (LONG)(((ULONG)Z0 & 0xFFFF0000UL)
		            | (UWORD)((WORD)Y0 - (WORD)PosYWr));
		bp16 = (WORD)(0 - (WORD)((WORD)Z0 + (WORD)PosZWr));
		ebpc = (LONG)(((ULONG)MUL32((WORD)x, LMatriceRot[6]) & 0xFFFF0000UL)
		            | (UWORD)bp16);

		Proj_ISO(eaxc, ebxc, ebpc, &xp, &yp);

		edi[0] = xp;                    /* stock Xp */
		if (xp < ScreenXmin)
		{
			ScreenXmin = xp;
		}
		if (xp > ScreenXmax)
		{
			ScreenXmax = xp;
		}
		edi[1] = yp;                    /* stock Yp */
		if (yp < ScreenYmin)
		{
			ScreenYmin = yp;
		}
		if (yp > ScreenYmax)
		{
			ScreenYmax = yp;
		}
		edi[2] = bp16;                  /* stock Zrot */
		edi += 3;
	} while (--Count1);

	/* ComputeStaticNormal */
	n = (UWORD)RW(esi);                 /* nbobj normal faces/points */
	esi += 2;
	if (n != 0)
	{
		WORD *outp = List_Normal;

		Count1 = (WORD)n;
		do
		{
			WORD x = RW(esi);
			WORD y = RW(esi + 2);
			WORD z = RW(esi + 4);
			WORD range = RW(esi + 6);
			LONG acc;

			esi += 8;

			Rot(x, y, z);               /* DX (range) inchangé */

			acc = ADD32(ADD32((LONG)(WORD)X0 * (WORD)NormalXLight,
			                  (LONG)(WORD)Y0 * (WORD)NormalYLight),
			            (LONG)(WORD)Z0 * (WORD)NormalZLight);

			if (acc < 0)                /* js nointensity */
			{
				*outp++ = 0;
			}
			else
			{
				*outp++ = (WORD)(acc / range);      /* idiv bp */
			}
		} while (--Count1);
	}

	return esi;
}

/*═════════════════════════════ TRI (SergeSort) ════════════════════════════*/

/* ASM: SergeSort — descending sort of List_Tri[0..nm1] on the (signed) Z
   word.  Hybrid: quicksort with explicit stack, selection sort for ranges
   of <= 8 elements, dedicated 2-element case.  Transliterated 1:1 (the
   permutation of equal keys is part of the visual result). */
PORT_FASTCODE static void SergeSort(T_TRI *lo, T_TRI *hi, LONG cx)
{
	T_TRI *stk[1024];
	int sp = 0;
	T_TRI tmp;
	T_TRI *piv, *bas, *h;

	goto nocalccx;

dopop:
	lo = stk[--sp];
nopop:
	cx = (LONG)(hi - lo);
nocalccx:
	if (cx == 1)
	{
		/* permut2 */
		if (lo->z < hi->z)              /* cmp ax,bx ; jge skipswap */
		{
			tmp = *lo;
			*lo = *hi;
			*hi = tmp;
		}
		if (sp == 0)
		{
			return;
		}
		hi = stk[--sp];
		goto dopop;
	}

	if ((ULONG)cx <= 7)                 /* cmp cx,7 ; jbe */
	{
		/* permut: selection sort, cx = nb entite - 1 */
		LONG bp = cx;

		do
		{
			T_TRI *best = 0;            /* ebx */
			WORD bz = lo->z;
			T_TRI *scan = lo + 1;

			do
			{
				if (scan->z > bz)       /* jg plusgrand */
				{
					best = scan;
					bz = scan->z;
				}
				scan++;
			} while (--cx);

			if (best != 0)
			{
				tmp = *lo;              /* permutte */
				*lo = *best;
				*best = tmp;
			}
			lo++;
			cx = --bp;
		} while (bp);

		if (sp == 0)
		{
			return;
		}
		hi = stk[--sp];
		goto dopop;
	}

	/* quicksort partition */
	piv = lo;                           /* pospivot */
	tmp = *lo;                          /* pivot (eax = Z+type dword) */
	bas = lo + 1;                       /* bas++ */
	h = hi;                             /* h = haut */

w1:
	for (;;)
	{
		if (bas->z < tmp.z)
		{
			goto w2;
		}
		bas++;                          /* si [bas] >= pivot */
		if (--cx == 0)
		{
			goto w4;
		}
	}
w2:
	for (;;)
	{
		if (h->z > tmp.z)
		{
			/* w3: xchg [bas],[h] */
			T_TRI t2 = *bas;

			*bas = *h;
			*h = t2;
			goto w1;
		}
		h--;                            /* si [h] <= pivot */
		if (--cx == 0)
		{
			goto w4;
		}
	}
w4:
	{
		T_TRI *t = bas;                 /* xchg esi,ebx */

		bas = h;
		h = t;
	}
	{
		T_TRI t2 = *piv;                /* xchg [pospivot],[bas] */

		*piv = *bas;
		*bas = t2;
	}

	if (h < hi)                         /* cmp ebx,ebp ; jae nopush */
	{
		stk[sp++] = h;                  /* push h / haut */
		stk[sp++] = hi;
	}

	bas--;                              /* sub esi,8 */
	if (piv >= bas)                     /* cmp edi,esi ; jae norecur */
	{
		if (sp == 0)
		{
			return;
		}
		hi = stk[--sp];
		goto dopop;
	}
	hi = bas;
	lo = piv;
	goto nopop;
}

/*═════════════════════════════ AffObjetIso ════════════════════════════════*/

PORT_FASTCODE LONG AffObjetIso(LONG xwr, LONG ywr, LONG zwr,
                 LONG palpha, LONG pbeta, LONG pgamma, void *ptrobj)
{
	UBYTE *esi;
	WORD *edi;
	UWORD cx;
	T_TRI *tri;

	lAlpha = palpha;
	lBeta = pbeta;
	lGamma = pgamma;

	ScreenXmin = 32767;
	ScreenYmin = 32767;
	ScreenXmax = -32767;                /* mov ax,32767 ; neg ax */
	ScreenYmax = -32767;

	/* rotation world org obj */
	if (TypeProj == TYPE_3D)
	{
		LongWorldRot(xwr, ywr, zwr);
		PosXWr = X0 - CameraXr;
		PosYWr = Y0 - CameraYr;
		PosZWr = Z0 - CameraZr;
	}
	else
	{
		PosXWr = xwr;
		PosYWr = ywr;
		PosZWr = zwr;
	}

	/* recup infos */
	TotalEntite = 0;
	PointeurListTri = List_Tri;

	esi = (UBYTE *)ptrobj;
	Infos = RW(esi);
	esi += (UWORD)RW(esi + 14) + 16;    /* saute zone info */

	/* rotation nuage/normal face/normal point */
	if (Infos & INFO_ANIM)
	{
		esi = AnimNuage(esi);           /* Objet Animé */
	}
	else
	{
		esi = RotateNuage(esi);         /* Objet Normal */
	}

	/* ==== finnuage: POLYGONES ============================================ */
	edi = List_Entity;

	cx = (UWORD)RW(esi);                /* nb polys */
	esi += 2;
	if (cx != 0)
	{
		Count1 = (WORD)cx;
		do
		{
			WORD *startDI = edi;        /* memo pointeur List_Coor */
			WORD w0 = RW(esi);          /* matiere + nb point poly */
			WORD w1 = RW(esi + 2);      /* coul1/coul2 */
			UBYTE matiere = (UBYTE)w0;
			UBYTE nbpt = (UBYTE)((UWORD)w0 >> 8);
			WORD zmax = -32000;
			WORD *ptr1;
			UBYTE np;

			esi += 2;

			if (matiere >= 9)           /* >= MAT_GOURAUD */
			{
				UBYTE coul1 = (UBYTE)w1;

				/* stock type translated (cl-2) + nbp + coul1/coul2 */
				edi[0] = (WORD)((w0 & 0xFF00) | (UBYTE)(matiere - 2));
				edi[1] = w1;
				esi += 2;
				edi += 2;

				ptr1 = edi;             /* memo start poly somm */

				np = nbpt;
				do
				{
					UWORD nidx = (UWORD)RW(esi);        /* normal point */
					UWORD pidx = (UWORD)RW(esi + 2);    /* deja *6 */
					WORD it = List_Normal[nidx];
					WORD zr;

					esi += 4;
					/* add dl,ch: colour byte only */
					it = (WORD)((it & 0xFF00)
					          | (UBYTE)((UBYTE)it + coul1));
					edi[0] = it;        /* stock intensity point */
					edi[1] = List_Point[pidx / 2];      /* Xscr */
					edi[2] = List_Point[pidx / 2 + 1];  /* Yscr */
					zr = List_Point[pidx / 2 + 2];      /* Zrot */
					if (zr > zmax)
					{
						zmax = zr;
					}
					edi += 3;
				} while (--np);
			}
			else if (matiere >= 7)      /* >= MAT_FLAT */
			{
				WORD coul;
				UWORD nidx;

				/* stock mat translaté (cl-7) + nbp (WORD only) */
				edi[0] = (WORD)((w0 & 0xFF00) | (UBYTE)(matiere - 7));

				coul = RW(esi);         /* coul1/coul2 */
				nidx = (UWORD)RW(esi + 2);      /* 1st coul = normal face */
				esi += 4;
				coul = (WORD)(coul + List_Normal[nidx]);
				edi[1] = coul;          /* stock coul + intensity */
				edi += 2;

				ptr1 = edi;

				np = nbpt;
				do
				{
					UWORD pidx = (UWORD)RW(esi);
					WORD zr;

					esi += 2;
					edi[1] = List_Point[pidx / 2];
					edi[2] = List_Point[pidx / 2 + 1];
					zr = List_Point[pidx / 2 + 2];
					if (zr > zmax)
					{
						zmax = zr;
					}
					edi += 3;
				} while (--np);
			}
			else                        /* MAT_TRISTE..MAT_TRAME */
			{
				edi[0] = w0;            /* stock type + nbp */
				edi[1] = w1;            /* stock coul1/coul2 */
				esi += 2;
				edi += 2;

				ptr1 = edi;

				np = nbpt;
				do
				{
					UWORD pidx = (UWORD)RW(esi);
					WORD zr;

					esi += 2;
					edi[1] = List_Point[pidx / 2];
					edi[2] = List_Point[pidx / 2 + 1];
					zr = List_Point[pidx / 2 + 2];
					if (zr > zmax)
					{
						zmax = zr;
					}
					edi += 3;
				} while (--np);
			}

			/* testpoly: backface (oublie face si bx:bp < dx:ax) */
			ZMax = zmax;
			{
				LONG p1 = (LONG)(WORD)(ptr1[2] - ptr1[8])
				        * (WORD)(ptr1[4] - ptr1[1]);
				LONG p2 = (LONG)(WORD)(ptr1[1] - ptr1[7])
				        * (WORD)(ptr1[5] - ptr1[2]);

				if ((long long)p2 - (long long)p1 >= 0)
				{
					edi = startDI;      /* badpoly */
				}
				else
				{
					TotalEntite++;      /* okpoly */
					PointeurListTri->z = ZMax;
					PointeurListTri->type = E_POLY;
					PointeurListTri->ptr = startDI;
					PointeurListTri++;
				}
			}
		} while (--Count1);
	}

	/* ==== LIGNES ========================================================= */
	cx = (UWORD)RW(esi);                /* nb lignes */
	esi += 2;
	if (cx != 0)
	{
		tri = PointeurListTri;
		TotalEntite = (WORD)(TotalEntite + cx);

		do
		{
			UWORD i1, i2;
			WORD bp16;

			edi[0] = RW(esi);           /* stock matiere/coul */
			edi[1] = RW(esi + 2);

			i1 = (UWORD)RW(esi + 4);    /* index point 1 (deja *6) */
			edi[2] = List_Point[i1 / 2];        /* X1scr */
			edi[3] = List_Point[i1 / 2 + 1];    /* Y1scr */
			bp16 = List_Point[i1 / 2 + 2];      /* Zrot */

			i2 = (UWORD)RW(esi + 6);    /* index point 2 */
			esi += 8;
			edi[4] = List_Point[i2 / 2];        /* X2scr */
			edi[5] = List_Point[i2 / 2 + 1];    /* Y2scr */
			if (List_Point[i2 / 2 + 2] >= bp16) /* ZMin ZMax */
			{
				bp16 = List_Point[i2 / 2 + 2];
			}

			tri->z = bp16;
			tri->type = E_LIGNE;
			tri->ptr = edi;
			tri++;
			edi += 6;
		} while (--cx);

		PointeurListTri = tri;
	}

	/* ==== SPHERES ======================================================== */
	cx = (UWORD)RW(esi);                /* nb spheres */
	esi += 2;
	if (cx != 0)
	{
		tri = PointeurListTri;
		TotalEntite = (WORD)(TotalEntite + cx);

		do
		{
			UWORD pidx;
			WORD zr;

			edi[0] = RW(esi);           /* stock matiere/coul */
			edi[1] = RW(esi + 2);
			edi[2] = RW(esi + 4);       /* rayon */
			pidx = (UWORD)RW(esi + 6);  /* index point 1 */
			esi += 8;

			edi[3] = List_Point[pidx / 2];      /* X1scr */
			edi[4] = List_Point[pidx / 2 + 1];  /* Y1scr */
			zr = List_Point[pidx / 2 + 2];      /* Zrot */
			edi[5] = zr;

			tri->z = zr;
			tri->type = E_SPHERE;
			tri->ptr = edi;
			tri++;
			edi += 6;
		} while (--cx);

		PointeurListTri = tri;
	}

	/* ==== TRI ============================================================ */
	{
		LONG nm1 = (LONG)(UWORD)TotalEntite - 1;

		if (nm1 > 0)
		{
			SergeSort(List_Tri, List_Tri + nm1, nm1);
		}
	}

	/* ==== DISPLAY ======================================================== */
	if (TotalEntite == 0)
	{
		/* badfinafobj_1: pas d'entite to aff */
		ScreenXmax = -1;
		ScreenYmax = -1;
		ScreenXmin = -1;
		ScreenYmin = -1;
		return 1;
	}

	Count1 = TotalEntite;
	tri = List_Tri;
	do
	{
		WORD *rec = tri->ptr;

		switch (tri->type)
		{
		case E_LIGNE:
		{
			/* coul: mov ax,[esi] ; xchg al,ah */
			UWORD coul = (UWORD)(((UWORD)rec[0] << 8)
			                   | ((UWORD)rec[0] >> 8));

			Line_A((WORD)rec[2], (WORD)rec[3],
			       (WORD)rec[4], (WORD)rec[5], (LONG)coul);
			break;
		}

		case E_POLY:
		{
			UWORD w0 = (UWORD)rec[0];
			UWORD coul = (UWORD)rec[1];
			UWORD type = (UBYTE)w0;
			UWORD nbp = (UWORD)(w0 >> 8);

			TypePoly = (WORD)type;
			NbPolyPoints = (WORD)nbp;

			memcpy(TabPoly, rec + 2, (size_t)nbp * 3 * sizeof(WORD));

			if (ComputePoly_A())
			{
				FillVertic_A((LONG)type, (LONG)coul);
			}
			break;
		}

		case E_SPHERE:
		{
			UWORD type = (UBYTE)(UWORD)rec[0];          /* byte[esi]   */
			/* coul: mov ax,[esi+1] (unaligned!) = bytes 1 and 2 */
			UWORD coul = (UWORD)(((UWORD)rec[0] >> 8)
			                   | (UWORD)(((UWORD)rec[1] & 0xFF) << 8));
			LONG x = (UWORD)rec[3];                     /* zero-extended */
			LONG y = (UWORD)rec[4];                     /* movzx         */
			LONG r;
			WORD w;

			if (TypeProj == TYPE_3D)
			{
				/* rayon perspective: 16-bit imul/idiv */
				LONG t = (LONG)rec[2] * (WORD)LFactorX;
				WORD bp16 = (WORD)((WORD)rec[5] + (WORD)KFactor);

				r = (UWORD)(WORD)(t / bp16);            /* movzx ebp,ax  */
			}
			else
			{
				r = MUL32((LONG)(UWORD)rec[2], 34) >> 9;/* racine sx²+sy² */
			}

			/* reajuste coordonnée box */
			w = (WORD)((WORD)x + (WORD)r);
			if (w > ScreenXmax)
			{
				ScreenXmax = w;
			}
			w = (WORD)((WORD)x - (WORD)r);
			if (w < ScreenXmin)
			{
				ScreenXmin = w;
			}
			w = (WORD)((WORD)y + (WORD)r);
			if (w > ScreenYmax)
			{
				ScreenYmax = w;
			}
			w = (WORD)((WORD)y - (WORD)r);
			if (w < ScreenYmin)
			{
				ScreenYmin = w;
			}

			if (ComputeSphere_A(x, y, r))
			{
				FillVertic_A((LONG)type, (LONG)coul);
			}
			break;
		}

		default:                        /* jmp [TabJump_2+ebx*4]: no E_POINT */
			break;
		}

		tri++;
	} while (--Count1);

	return 0;                           /* OK affiché au moins 1 entité */
}
