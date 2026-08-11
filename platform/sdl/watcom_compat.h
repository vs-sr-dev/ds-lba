/*
 * watcom_compat.h — force-included (gcc -include) in front of every ENGINE
 * translation unit only (not the platform/sdl shim .c files).
 *
 * Neutralizes Watcom C keywords and provides the min/max macros that
 * Watcom's <stdlib.h> defined. No engine source edit needed for these.
 * Portable: works identically on the ARM/DS GCC toolchain.
 */
#ifndef WATCOM_COMPAT_H
#define WATCOM_COMPAT_H

/* Watcom calling-convention / memory-model keywords -> nothing */
#define cdecl
#define __far
#define __near
#define __loadds

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#endif /* WATCOM_COMPAT_H */
