/*----------------------------------------------------------------------------
 *  port_fast.h — portable fast-memory placement macros (DS port tuning).
 *
 *  On PC builds (SDL/mingw) every macro is empty: the translate sources stay
 *  fully portable.  On the NDS build (-DPORT_NDS -DARM9, ARM9/calico):
 *
 *    PORT_FASTCODE  places the function in ITCM (32KB, 0-wait, 32-bit bus)
 *                   and compiles it as ARM32 (Thumb gains nothing in ITCM;
 *                   ARM does ~30% more work per instruction there).
 *                   Section ".itcm" + long_call as in libnds's ITCM_CODE
 *                   (calico ds9.ld collects .itcm/.itcm.*).
 *    PORT_FASTDATA  places initialized data in DTCM (16KB data TCM,
 *                   0-wait; calico ds9.ld section ".dtcm").
 *    PORT_FASTBSS   places zero-initialized data in DTCM bss (".sbss",
 *                   NOLOAD).  ~15.5KB are free after calico's own usage —
 *                   check build/lba1ds.map (.dtcm.bss) when adding entries.
 *
 *  NOTE for engine files: include this header with a relative path
 *  ("../../translate/port_fast.h") — it has no dependencies.
 *---------------------------------------------------------------------------*/
#ifndef PORT_FAST_H
#define PORT_FAST_H

#if defined(PORT_NDS) && defined(ARM9)
#define PORT_FASTCODE __attribute__((section(".itcm"), long_call, target("arm")))
#define PORT_FASTDATA __attribute__((section(".dtcm")))
#define PORT_FASTBSS  __attribute__((section(".sbss")))
#else
#define PORT_FASTCODE
#define PORT_FASTDATA
#define PORT_FASTBSS
#endif

#endif /* PORT_FAST_H */
