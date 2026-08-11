/*
 * port.h — internal header for the platform/sdl shim.
 *
 * Deliberately does NOT include the engine headers (they need the Watcom
 * compat flags); it re-declares the Adeline base types and the engine
 * variables the shim implements/uses. Types match LIB_SYS/ADELINE.H
 * (ILP32 on both i686 PC build and ARM/DS).
 */
#ifndef PORT_H
#define PORT_H

typedef unsigned long ULONG;
typedef signed long LONG;
typedef unsigned short UWORD;
typedef signed short WORD;
typedef unsigned char UBYTE;
typedef signed char BYTE;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* ---- video layer (sdl_video.c) ---- */
void PORT_VideoPresent(void);      /* convert Phys+palette -> window   */
void PORT_PumpEvents(void);        /* SDL event pump + keyboard state  */
extern UBYTE PORT_PalRGB[768];     /* current 8-bit palette (0..255)   */
extern int PORT_PalDirty;

/* engine-visible video state (defined in sdl_video.c) */
extern UBYTE *Log;
extern UBYTE *MemoLog;
extern UBYTE *Phys;
extern WORD Screen_X;
extern WORD Screen_Y;
extern ULONG TabOffLine[481];
extern WORD ClipXmin, ClipYmin, ClipXmax, ClipYmax;

/* engine-visible input/timer state (defined in sdl_sys.c) */
extern volatile UWORD Key;
extern volatile UWORD FuncKey;
extern volatile UWORD Joy;
extern volatile UWORD Fire;
extern UWORD AsciiMode;
extern volatile LONG Click;
extern volatile LONG Mouse_X;
extern volatile LONG Mouse_Y;
extern volatile ULONG TimerSystem;
extern volatile ULONG TimerRef;
extern UWORD NbFramePerSecond;
extern UWORD WaitNbTicks;
extern UWORD CmptFrame;
extern UWORD Cmpt_18;

#endif /* PORT_H */
