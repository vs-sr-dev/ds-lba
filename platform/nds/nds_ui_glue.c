/*
 * nds_ui_glue.c — the only platform file compiled with ENGINE_CFLAGS
 * (real engine headers, watcom_compat force-included; see the dedicated
 * Makefile rule). Purpose: let the touch UI (nds_ui.c) read T_OBJET
 * fields without mirroring the 60-field struct in the shim.
 *
 * READ-ONLY accessors. No engine state is modified here.
 */
#include "c_extern.h"
#include <stddef.h>

/* Compile-time guards for engine edit #33 (OBJECT.C/PERSO.C): the DOS code
 * type-puns a 32-bit timer over T_OBJET.Info/Info1 with *(ULONG *)(&Info).
 * On this ABI Info sits at offset ≡ 2 (mod 4): a raw ULONG store on the
 * ARM9 ignores the low address bits and stomps OffsetLife instead (life
 * script executing from a wild offset = erratic behaviour / freeze).
 * The fix composes the value over Info/Info1 with LE_W32/LE_R32, which
 * requires Info1 to sit exactly 2 bytes after Info. */
typedef char port_assert_info_pair[
	(offsetof(T_OBJET, Info1) == offsetof(T_OBJET, Info) + 2) ? 1 : -1];
typedef char port_assert_info_misaligned[
	(offsetof(T_OBJET, Info) % 4 == 2) ? 1 : -1]; /* documents WHY #33 exists */

WORD PORT_UI_LifePoint(void)
{
	return ListObjet[NUM_PERSO].LifePoint;
}

/* TRUE while Twinsen exists and is under manual control — the same guard
 * PERSO.C uses for inventory/behaviour keys (FALSE during cutscenes). */
WORD PORT_UI_PersoManual(void)
{
	return (ListObjet[NUM_PERSO].Body != -1)
		   AND(ListObjet[NUM_PERSO].Move == MOVE_MANUAL);
}
