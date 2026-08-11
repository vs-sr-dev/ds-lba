/*
 * watcom_compat.h (NDS) — force-included (gcc -include) in front of every
 * ENGINE translation unit only (never the platform/nds shim .c files).
 *
 * Same job as platform/sdl/watcom_compat.h (kill Watcom keywords, min/max)
 * plus the devkitARM/newlib gaps that MinGW filled for free on the PC build:
 *   - stricmp/strcmpi/strnicmp  -> strcasecmp/strncasecmp (newlib strings.h)
 *   - _MAX_PATH/_MAX_DRIVE/...  -> MinGW values
 *   - fopen                     -> NDS_fopen (path normalizer: '\'->'/',
 *                                  lowercase, ".\": nitroFS is our cwd)
 *   - malloc/calloc/free        -> NDS_* accounting wrappers (RAM budget log)
 * All wrappers are implemented in platform/nds/nds_sys.c.
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

/* ---- newlib gaps ---- */
/* NOT <strings.h>: it also declares index(), which collides with engine
 * globals (GIF.C/PCX.C `WORD index`). Declare just what we alias. */
#include <stddef.h> /* size_t */
extern int strcasecmp(const char *, const char *);
extern int strncasecmp(const char *, const char *, size_t);

/* newlib <string.h> declares BSD index(); GIF.C/PCX.C use `index` as a
 * variable. Pull in <string.h> with the real name first, THEN rename the
 * engine identifier everywhere (consistent per-TU, later includes are
 * guarded so the function declaration is never macro-mangled). */
#include <string.h>
#define index lba1_index

#define stricmp(a, b) strcasecmp((const char *)(a), (const char *)(b))
#define strcmpi(a, b) strcasecmp((const char *)(a), (const char *)(b))
#define strnicmp(a, b, n) strncasecmp((const char *)(a), (const char *)(b), (n))

#ifndef _MAX_PATH
#define _MAX_PATH 260
#define _MAX_DRIVE 3
#define _MAX_DIR 256
#define _MAX_FNAME 256
#define _MAX_EXT 256
#endif

/* ---- file + heap hooks (see nds_sys.c) ---- */
#include <stdio.h>  /* FILE, before we rename fopen */
#include <stdlib.h> /* size_t protos, before we rename malloc */

FILE *NDS_fopen(const char *name, const char *mode);
int NDS_remove(const char *name);
int NDS_fclose(FILE *f);
size_t NDS_fread(void *p, size_t sz, size_t n, FILE *f);
size_t NDS_fwrite(const void *p, size_t sz, size_t n, FILE *f);
int NDS_fseek(FILE *f, long off, int whence);
long NDS_ftell(FILE *f);
void *NDS_malloc(size_t size);
void *NDS_calloc(size_t n, size_t size);
void *NDS_realloc(void *p, size_t size);
void NDS_free(void *p);

/* msvcrt (the PC reference build) validates FILE* and fails gracefully on
 * bad/closed handles; newlib data-aborts. The engine relies on the msvcrt
 * behaviour in a few places, so wrap the whole stdio surface it uses. */
#define fopen NDS_fopen
#define remove NDS_remove /* Delete() must hit the same fat:/ save routing */
#define fclose NDS_fclose
#define fread NDS_fread
#define fwrite NDS_fwrite
#define fseek NDS_fseek
#define ftell NDS_ftell
#define malloc NDS_malloc
#define calloc NDS_calloc
#define realloc NDS_realloc
#define free NDS_free

/* Engine TUs that carry PORT: diagnostics (HQ_RESS.C, FONT_A.C, ...) were
 * reaching PORT_Diag through gnu89's implicit declaration, which assumes
 * int f() — passing varargs through that is undefined, and it happened to
 * work only because AAPCS puts the first arguments in the same registers
 * either way. This header is force-included into every engine TU, so one
 * prototype here covers them all. */
void PORT_Diag(const char *fmt, ...);

#endif /* WATCOM_COMPAT_H */
