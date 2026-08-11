/*
 * port.h — internal header for the platform/nds shim.
 *
 * Mirror of platform/sdl/port.h: deliberately does NOT include the engine
 * headers; re-declares the Adeline base types and the engine variables the
 * shim implements/uses. ILP32 on ARM, same as the DOS/Watcom build.
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

/* ---- video (nds_video.c) ---- */
extern UBYTE PORT_PalRGB[768]; /* current 8-bit palette (DAC-quantized) */
void PORT_PresentAll(void);    /* full-screen Log -> VRAM blit          */
extern volatile int PORT_ModeX, PORT_ModeY; /* current physical mode    */

/* engine-visible video state (defined in nds_video.c) */
extern UBYTE *Log;
extern UBYTE *MemoLog;
extern UBYTE *Phys;
extern WORD Screen_X;
extern WORD Screen_Y;
extern ULONG TabOffLine[481];
extern WORD ClipXmin, ClipYmin, ClipXmax, ClipYmax;

/* engine-visible input/timer state (defined in nds_sys.c) */
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

/* input pump, called from the VBlank IRQ (nds_sys.c) */
void PORT_ScanInput(void);

/* touch UI on the sub screen (nds_ui.c) */
void PORT_UI_Init(void);   /* after consoleDemoInit + nitroFSInit      */
void PORT_UI_VBlank(void); /* called by the video VBlank ISR           */
/* drains the language-selector mailbox. MAIN THREAD ONLY: it re-enters
 * InitDial (HQR loads + card I/O), which must never run in the ISR. Called
 * from the redraw entry points in nds_video.c. */
void PORT_PumpLang(void);

/* streaming CD-music player (nds_music.c) + shared calico lock (nds_audio.c) */
void PORT_MusicInit(void); /* creates the refill thread; called from InitWave */
void PORT_SndLock(void);   /* serialize calico sound calls across SFX+music   */
void PORT_SndUnlock(void);
int  PORT_SndReady(void);

/* card access lock (nds_sys.c): serializes every file operation, because the
 * music thread streams from the SD while the game reads the nitroFS and on a
 * flashcart both land on the same device. Take it AFTER nds_music.c's mmutex,
 * never before. */
void PORT_IoLock(void);
void PORT_IoUnlock(void);

/* diagnostics: console AND fat:/lba1/lba1ds.log, flushed every line so a
 * freeze still leaves the trail (nds_sys.c) */
void PORT_DiagInit(void);
void PORT_Diag(const char *fmt, ...);

/* memory accounting (nds_sys.c) */
extern ULONG PORT_HeapUsed;   /* live bytes through the NDS_* wrappers  */
extern ULONG PORT_HeapPeak;
void PORT_MemReport(const char *tag);
int PORT_HeapCheck(const char *tag);
void *NDS_calloc(unsigned int n, unsigned int size);

#endif /* PORT_H */
