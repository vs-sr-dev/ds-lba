/*
 * nds_ui.c — touch UI on the SUB screen (the reason this port exists).
 *
 * SUB video layout (VRAM bank C, set up by consoleDemoInit + PORT_UI_Init):
 *   BG0 = libnds debug console (text 4bpp, map base 22 / tile base 3 —
 *         untouched, still prints all the [wave]/[MEM]/P0-P6 logs)
 *   BG2 = this UI: extended-rotation 8bpp bitmap 256x256 at map base 4
 *         (offset 64K in bank C, clear of the console's 44K..56K region).
 *   Holding SELECT for 1 second toggles UI <-> console (only DISPCNT_SUB
 *   BG2 enable bit is poked — deterministic, no libnds bg state involved).
 *   Palette: UI uses SUB palette entries 1..31; 0 and 240..255 are left to
 *   the console (backdrop / default font colors).
 *
 * Drawing: everything is drawn into a 48K shadow buffer in main RAM (byte
 * writes allowed), then dirty rectangles are blitted to VRAM as u32 writes
 * (VRAM ignores byte writes). Redraws happen only when a sampled value
 * changes, from the VBlank ISR: worst case (all 8 buttons flip state) is a
 * few KB of writes — well under the 64K/frame the MCGA present already does
 * in this very ISR. Fonts and icons are hand-made C tables (BYOA: no
 * external assets).
 *
 * Action injection (see "UI secondo schermo" in PORTING_NOTES_NDS.md):
 * the ISR never calls engine code. It posts into three one-shot mailboxes
 * consumed at a single well-defined point of the PERSO.C MainLoop
 * (edits #29-31 in platform/sdl/PORTING_NOTES.md):
 *   PORT_InjectKey          — scan code, replaces MyKey for ONE iteration:
 *                             K_1/K_2 = magic ball / sabre (same handlers,
 *                             same ListFlagGame gating as the PC keyboard)
 *   PORT_TouchComportement  — 0..3, direct SetComportement() (same call the
 *                             life-script SET_COMPORTEMENT opcode makes: no
 *                             menu flash, no full redraw)
 *   PORT_TouchInvAction     — inventory action id fed to the existing
 *                             switch(InventoryAction): 0 = holomap,
 *                             14 = meca-pingouin (exact same code path as a
 *                             real inventory selection, without opening the
 *                             inventory)
 * Unconsumed mailboxes expire after ~200 ms (MAIL_TTL vblanks) so a tap
 * can never fire late after a modal (dialog/menu) closes.
 *
 * Gameplay gating (buttons act ONLY during interactive gameplay):
 *   CmptMemoTimerRef == 0   — engine SaveTimer() depth: >0 inside every
 *                             menu / holomap / options / inventory modal
 *   FlagCredits == 0
 *   AffScene ran < 40 ticks ago (stamped by __wrap_AffScene in nds_prof.c)
 *                           — false in main menu / intro / FLA / holomap
 *   PORT_UI_PersoManual()   — Body != -1 && Move == MOVE_MANUAL (glue,
 *                             nds_ui_glue.c) — false in cutscenes
 * plus per-button inventory flags (same predicate Inventory() uses):
 *   ListFlagGame[item] == 1 && ListFlagGame[FLAG_CONSIGNE] == 0,
 *   meca-pingouin additionally NumPingouin > 0 (valid object in this cube).
 */
#include <nds.h>
#include <stdio.h>
#include <string.h>
#include "port.h"

/* ------------------------------------------------------------------ */
/* engine globals (READ ONLY — game/GLOBAL.C, LIB_3D/P_ANIM.C)         */
/* ------------------------------------------------------------------ */
extern WORD Comportement;      /* 0..3 (4 = protopack)                 */
extern WORD Weapon;            /* 0 = magic ball, 1 = sabre            */
extern WORD MagicPoint;        /* 0..MagicLevel*20                     */
extern WORD MagicLevel;        /* 0..4                                 */
extern WORD NbGoldPieces;      /* kashes                               */
extern WORD NbLittleKeys;
extern WORD NbFourLeafClover;
extern WORD NbCloverBox;
extern WORD NumPingouin;       /* -1 if no penguin object in the cube  */
extern UBYTE ListFlagGame[];   /* inventory/game flags                 */
extern LONG CmptMemoTimerRef;  /* SaveTimer() nesting depth            */
extern LONG FlagCredits;

/* game/COMMON.H flag indices (stable defines of the original source) */
#define UI_FLAG_HOLOMAP 0
#define UI_FLAG_BALLE 1
#define UI_FLAG_SABRE 2
#define UI_FLAG_TUNIQUE 4
#define UI_FLAG_PINGOUIN 14
#define UI_FLAG_CONSIGNE 70

/* LIB_SYS.H scan codes */
#define UI_K_1 2
#define UI_K_2 3

/* glue accessors compiled against the real engine headers */
WORD PORT_UI_LifePoint(void);   /* ListObjet[NUM_PERSO].LifePoint      */
WORD PORT_UI_PersoManual(void); /* Body != -1 && Move == MOVE_MANUAL   */

/* ------------------------------------------------------------------ */
/* mailboxes consumed by game/PERSO.C MainLoop (edits #29-31)          */
/* ------------------------------------------------------------------ */
volatile WORD PORT_InjectKey = 0;
volatile WORD PORT_TouchComportement = -1;
volatile WORD PORT_TouchInvAction = -1;

/* stamped by __wrap_AffScene (nds_prof.c): "MainLoop is rendering" */
volatile ULONG PORT_UI_SceneStamp = 0;
volatile int PORT_UI_SceneSeen = 0;

#define MAIL_TTL 12 /* vblanks (~200 ms) before an unconsumed tap expires */
static int MailAge = 0;

/* ------------------------------------------------------------------ */
/* palette (SUB entries 1..31)                                         */
/* ------------------------------------------------------------------ */
enum
{
	C_TRANSP = 0,   /* never drawn: bitmap 0 = backdrop            */
	C_BG = 1,       /* page background, very dark blue             */
	C_PANEL = 2,    /* disabled button face                        */
	C_FACE = 3,     /* enabled button face                         */
	C_FACEHI = 4,   /* active button face                          */
	C_GOLD = 5,
	C_GOLDDK = 6,
	C_WHITE = 7,
	C_GRAY = 8,
	C_DIM = 9,      /* disabled icon/label                         */
	C_LIFE = 10,
	C_LIFEDK = 11,
	C_MAGIC = 12,
	C_MAGICDK = 13,
	C_RED = 14,
	C_EDGEDK = 15,  /* dark bevel                                  */
	C_PRESS = 16,   /* pressed button face                         */
	C_GREEN = 17,   /* clover                                      */
	C_BAREMPTY = 18 /* empty part of the bars                      */
};

static const u16 UiPal[] = {
	/* 1  */ RGB15(2, 2, 5),
	/* 2  */ RGB15(4, 4, 8),
	/* 3  */ RGB15(6, 6, 12),
	/* 4  */ RGB15(9, 8, 16),
	/* 5  */ RGB15(29, 23, 9),
	/* 6  */ RGB15(17, 13, 4),
	/* 7  */ RGB15(30, 30, 30),
	/* 8  */ RGB15(19, 19, 21),
	/* 9  */ RGB15(9, 9, 11),
	/* 10 */ RGB15(8, 25, 10),
	/* 11 */ RGB15(3, 11, 4),
	/* 12 */ RGB15(9, 12, 28),
	/* 13 */ RGB15(4, 5, 13),
	/* 14 */ RGB15(26, 8, 8),
	/* 15 */ RGB15(1, 1, 2),
	/* 16 */ RGB15(13, 12, 22),
	/* 17 */ RGB15(10, 26, 12),
	/* 18 */ RGB15(3, 3, 6),
};

/* ------------------------------------------------------------------ */
/* 5x7 font, column format (bit 0 = top), chars: A-Z 0-9 / : - =       */
/* ------------------------------------------------------------------ */
static const UBYTE Font5[][5] = {
	{0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
	{0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
	{0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
	{0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
	{0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
	{0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
	{0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
	{0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
	{0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
	{0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
	{0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
	{0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
	{0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
	{0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
	{0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
	{0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
	{0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
	{0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
	{0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
	{0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
	{0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
	{0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
	{0x7F, 0x20, 0x18, 0x20, 0x7F}, /* W */
	{0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
	{0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
	{0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
	{0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
	{0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
	{0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
	{0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
	{0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
	{0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
	{0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
	{0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
	{0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
	{0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
	{0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
	{0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
	{0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
	{0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
};

static int GlyphIndex(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a';
	if (c >= '0' && c <= '9')
		return 26 + (c - '0');
	if (c == '/')
		return 36;
	if (c == ':')
		return 37;
	if (c == '-')
		return 38;
	if (c == '=')
		return 39;
	return -1; /* space / unknown */
}

/* ------------------------------------------------------------------ */
/* 16x16 icons, one u16 per row, bit 15 = leftmost pixel               */
/* ------------------------------------------------------------------ */
static const u16 IcoHeart[16] = {
	0x0000, 0x0000, 0x1C70, 0x3EF8, 0x7FFC, 0x7FFC, 0x7FFC, 0x3FF8,
	0x1FF0, 0x0FE0, 0x07C0, 0x0380, 0x0100, 0x0000, 0x0000, 0x0000};

static const u16 IcoStar[16] = {
	0x0100, 0x0100, 0x0380, 0x0380, 0x07C0, 0x1FF0, 0x7FFC, 0xFFFE,
	0x7FFC, 0x1FF0, 0x07C0, 0x0380, 0x0380, 0x0100, 0x0100, 0x0000};

static const u16 IcoCoin[16] = {
	0x07E0, 0x1FF8, 0x3FFC, 0x7FFE, 0x7FFE, 0x7FFE, 0x781E, 0x781E,
	0x7FFE, 0x7FFE, 0x7FFE, 0x3FFC, 0x1FF8, 0x07E0, 0x0000, 0x0000};

static const u16 IcoKey[16] = {
	0x0000, 0x0000, 0x0000, 0x7800, 0x8400, 0x8400, 0x87FE, 0x87FE,
	0x780A, 0x000A, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};

static const u16 IcoClover[16] = {
	0x0000, 0x3180, 0x7BC0, 0x7FC0, 0x3F80, 0x7FC0, 0x7BC0, 0x3180,
	0x0400, 0x0200, 0x0100, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};

/* behaviours */
static const u16 IcoNormal[16] = {
	0x01C0, 0x01C0, 0x0080, 0x1FF8, 0x1188, 0x1188, 0x0180, 0x0180,
	0x0180, 0x0180, 0x0240, 0x0240, 0x0420, 0x0C30, 0x0000, 0x0000};

static const u16 IcoSporty[16] = {
	0x0070, 0x0070, 0x0020, 0x03E0, 0x0FC0, 0x1B80, 0x0380, 0x0700,
	0x0F00, 0x1B00, 0x3180, 0x60C0, 0x4060, 0x0000, 0x0000, 0x0000};

static const u16 IcoAggro[16] = {
	0x0000, 0x0000, 0x6DB0, 0x7FF0, 0x7FF0, 0x7FFC, 0x7FFC, 0x7FF8,
	0x3FF0, 0x3FF0, 0x0FC0, 0x0FC0, 0x0000, 0x0000, 0x0000, 0x0000};

static const u16 IcoSneak[16] = {
	0x0000, 0x0000, 0x00E0, 0x00E0, 0x0040, 0x03C0, 0x0F80, 0x1F00,
	0x3F00, 0x3300, 0x2100, 0x6180, 0x0000, 0x0000, 0x0000, 0x0000};

/* actions */
static const u16 IcoBall[16] = {
	0x0000, 0x0000, 0x07C0, 0x0FE0, 0x1FF0, 0x1FF0, 0x1FF3, 0x1FF3,
	0x1FF0, 0x0FE0, 0x07C0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};

static const u16 IcoSaber[16] = {
	0x0003, 0x0006, 0x000C, 0x0018, 0x0030, 0x0060, 0x00C0, 0x0180,
	0x0300, 0x0FC0, 0x1C00, 0x3800, 0x3000, 0x0000, 0x0000, 0x0000};

static const u16 IcoHolo[16] = {
	0x0000, 0x0000, 0x03C0, 0x0C30, 0x1008, 0x100B, 0x2016, 0x202C,
	0x3458, 0x1AB0, 0x0D60, 0x03C0, 0x6000, 0xC000, 0x0000, 0x0000};

static const u16 IcoPengu[16] = {
	0x03C0, 0x03C0, 0x0180, 0x07E0, 0x0FF0, 0x2FF4, 0x2FF4, 0x1FF8,
	0x1FF8, 0x1FF8, 0x0FF0, 0x0FF0, 0x0C30, 0x0000, 0x0000, 0x0000};

/* ------------------------------------------------------------------ */
/* shadow buffer + blit                                                */
/* ------------------------------------------------------------------ */
#define UI_W 256
#define UI_H 192

static UBYTE Ui[UI_W * UI_H]; /* main RAM shadow (bss, 48K) */
static u16 *UiGfx = NULL;     /* BG2 bitmap in VRAM C       */
static int UiBg = -1;
static int UiInited = 0;
static int UiAutoShown = 0; /* set at first gameplay        */
static int ConsoleMode = 0; /* user toggled console back on */

static void SetUiVisible(int on)
{
	if (on)
		REG_DISPCNT_SUB |= DISPLAY_BG2_ACTIVE;
	else
		REG_DISPCNT_SUB &= ~DISPLAY_BG2_ACTIVE;
}

/* copy shadow -> VRAM, u32 stores only (VRAM ignores byte writes) */
static void BlitRect(int x, int y, int w, int h)
{
	int x0 = x & ~3;
	int x1 = (x + w + 3) & ~3;
	int row;

	if (!UiGfx)
		return;
	if (x1 > UI_W)
		x1 = UI_W;
	if (y + h > UI_H)
		h = UI_H - y;

	for (row = y; row < y + h; row++)
	{
		const ULONG *s = (const ULONG *)(Ui + row * UI_W + x0);
		ULONG *d = (ULONG *)((UBYTE *)UiGfx + row * UI_W + x0);
		int n = (x1 - x0) >> 2;

		while (n--)
			*d++ = *s++;
	}
}

/* ------------------------------------------------------------------ */
/* shadow-buffer primitives (byte writes, main RAM)                    */
/* ------------------------------------------------------------------ */
static void FillRect(int x, int y, int w, int h, UBYTE c)
{
	int row;

	for (row = 0; row < h; row++)
		memset(Ui + (y + row) * UI_W + x, c, w);
}

static void HLine(int x, int y, int w, UBYTE c)
{
	memset(Ui + y * UI_W + x, c, w);
}

static void VLine(int x, int y, int h, UBYTE c)
{
	while (h--)
		Ui[(y++) * UI_W + x] = c;
}

static void Bevel(int x, int y, int w, int h, UBYTE lt, UBYTE dk)
{
	HLine(x, y, w, lt);
	VLine(x, y, h, lt);
	HLine(x, y + h - 1, w, dk);
	VLine(x + w - 1, y, h, dk);
}

static void DrawChar(int x, int y, char ch, UBYTE c)
{
	int g = GlyphIndex(ch);
	int col, row;

	if (g < 0)
		return;
	for (col = 0; col < 5; col++)
	{
		UBYTE bits = Font5[g][col];

		for (row = 0; row < 7; row++)
			if (bits & (1 << row))
				Ui[(y + row) * UI_W + x + col] = c;
	}
}

static void DrawText(int x, int y, const char *s, UBYTE c)
{
	for (; *s; s++, x += 6)
		DrawChar(x, y, *s, c);
}

static int TextWidth(const char *s)
{
	return strlen(s) * 6;
}

static void DrawIcon(int x, int y, const u16 *rows, UBYTE c, int scale)
{
	int row, col;

	for (row = 0; row < 16; row++)
	{
		u16 bits = rows[row];

		if (!bits)
			continue;
		for (col = 0; col < 16; col++)
			if (bits & (0x8000 >> col))
			{
				if (scale == 1)
					Ui[(y + row) * UI_W + x + col] = c;
				else
				{
					UBYTE *p = Ui + (y + row * 2) * UI_W + x + col * 2;

					p[0] = c;
					p[1] = c;
					p[UI_W] = c;
					p[UI_W + 1] = c;
				}
			}
	}
}

static void FmtNum(char *dst, int v)
{
	char tmp[6];
	int n = 0;

	if (v < 0)
		v = 0;
	if (v > 9999)
		v = 9999;
	do
	{
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n)
		*dst++ = tmp[--n];
	*dst = 0;
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */
#define ST_H 46 /* status strip height */

#define BTN_W 59
#define BTN_H 62
#define BTN_X(col) (4 + (col) * 63)
#define ROW1_Y 50
#define ROW2_Y 116

/* button state bits */
#define BS_ENABLED 1
#define BS_ACTIVE 2
#define BS_PRESSED 4

typedef struct
{
	UBYTE col, row;      /* grid position                        */
	const u16 *icon;
	const char *label;
	const char *padtag;  /* pad shortcut legend (corner), or "" */
} BtnDef;

static const BtnDef Btn[8] = {
	{0, 0, IcoNormal, "NORMAL", "X"},
	{1, 0, IcoSporty, "SPORTY", ""},
	{2, 0, IcoAggro, "AGGRO", ""},
	{3, 0, IcoSneak, "SNEAK", ""},
	{0, 1, IcoBall, "BALL", "R"},
	{1, 1, IcoSaber, "SABER", ""},
	{2, 1, IcoHolo, "HOLOMAP", ""},
	{3, 1, IcoPengu, "PENGUIN", ""},
};

static UBYTE BtnState[8];    /* current, drawn            */
static UBYTE BtnWanted[8];   /* recomputed every vblank   */

static void BtnRect(int i, int *x, int *y)
{
	*x = BTN_X(Btn[i].col);
	*y = Btn[i].row ? ROW2_Y : ROW1_Y;
}

static void DrawButton(int i)
{
	int x, y;
	UBYTE st = BtnState[i];
	UBYTE face, edge_lt, edge_dk, ink, label;

	BtnRect(i, &x, &y);

	if (!(st & BS_ENABLED))
	{
		face = C_PANEL;
		edge_lt = C_EDGEDK;
		edge_dk = C_EDGEDK;
		ink = C_DIM;
		label = C_DIM;
	}
	else if (st & BS_PRESSED)
	{
		face = C_PRESS;
		edge_lt = C_EDGEDK;
		edge_dk = C_GOLD;
		ink = C_WHITE;
		label = C_WHITE;
	}
	else if (st & BS_ACTIVE)
	{
		face = C_FACEHI;
		edge_lt = C_GOLD;
		edge_dk = C_GOLDDK;
		ink = C_GOLD;
		label = C_GOLD;
	}
	else
	{
		face = C_FACE;
		edge_lt = C_GRAY;
		edge_dk = C_EDGEDK;
		ink = C_WHITE;
		label = C_GRAY;
	}

	FillRect(x, y, BTN_W, BTN_H, face);
	Bevel(x, y, BTN_W, BTN_H, edge_lt, edge_dk);
	if (st & BS_ACTIVE)
		Bevel(x + 1, y + 1, BTN_W - 2, BTN_H - 2, edge_lt, edge_dk);

	DrawIcon(x + (BTN_W - 32) / 2, y + 7, Btn[i].icon, ink, 2);
	DrawText(x + (BTN_W - TextWidth(Btn[i].label)) / 2, y + 47,
			 Btn[i].label, label);
	if (Btn[i].padtag[0]) /* pad shortcut legend, top-left corner */
		DrawText(x + 4, y + 4, Btn[i].padtag,
				 (st & BS_ENABLED) ? C_GOLDDK : C_DIM);

	BlitRect(x, y, BTN_W, BTN_H);
}

/* ---- status strip ------------------------------------------------ */
typedef struct
{
	WORD life, magic, magiclevel, magicvis;
	WORD gold, keys, clover, cloverbox, fps;
} Status;

static Status StCur = {-1, -1, -1, -1, -1, -1, -1, -1, -1};

static void DrawBar(int x, int y, int wmax, int w, int fill, int fillmax,
					UBYTE cfill, UBYTE cdark)
{
	int fw;

	/* wmax = widest possible frame (cleared), w = frame for current max */
	FillRect(x, y, wmax, 13, C_BG);
	if (w < 8)
		return;
	Bevel(x, y, w, 13, C_GRAY, C_EDGEDK);
	fw = (fillmax > 0) ? (fill * (w - 2)) / fillmax : 0;
	if (fw > w - 2)
		fw = w - 2;
	if (fw > 0)
	{
		FillRect(x + 1, y + 1, fw, 11, cfill);
		HLine(x + 1, y + 1, fw, cdark); /* top shading line */
	}
	if (fw < w - 2)
		FillRect(x + 1 + fw, y + 1, w - 2 - fw, 11, C_BAREMPTY);
}

static void DrawStatus(const Status *st)
{
	char buf[8];
	char buf2[12];

	FillRect(0, 0, UI_W, ST_H, C_BG);
	HLine(0, ST_H - 1, UI_W, C_GOLDDK);

	/* life */
	DrawIcon(4, 2, IcoHeart, C_RED, 1);
	DrawBar(24, 4, 128, 128, st->life, 50, C_LIFE, C_LIFEDK);

	/* magic (hidden until the tunic + a magic level exist, like DOS) */
	DrawIcon(4, 24, IcoStar, C_MAGIC, 1);
	if (st->magicvis && st->magiclevel > 0)
		DrawBar(24, 26, 128, 32 * st->magiclevel, st->magic,
				st->magiclevel * 20, C_MAGIC, C_MAGICDK);
	else
		FillRect(24, 26, 128, 13, C_BG);

	/* kashes */
	DrawIcon(160, 2, IcoCoin, C_GOLD, 1);
	FmtNum(buf, st->gold);
	DrawText(178, 6, buf, C_WHITE);

	/* keys */
	DrawIcon(160, 24, IcoKey, C_GRAY, 1);
	FmtNum(buf, st->keys);
	DrawText(178, 28, buf, C_WHITE);

	/* clovers n/m */
	DrawIcon(206, 2, IcoClover, C_GREEN, 1);
	FmtNum(buf, st->clover);
	strcpy(buf2, buf);
	strcat(buf2, "/");
	FmtNum(buf, st->cloverbox);
	strcat(buf2, buf);
	DrawText(224, 6, buf2, C_WHITE);

	/* fps (NbFramePerSecond, updated by the 50 Hz tick ISR) */
	DrawText(206, 28, "FPS", C_DIM);
	FmtNum(buf, st->fps);
	DrawText(228, 28, buf, C_GRAY);

	BlitRect(0, 0, UI_W, ST_H);
}

static void DrawFrame(void)
{
	FillRect(0, 0, UI_W, UI_H, C_BG);
	HLine(0, 182, UI_W, C_GOLDDK);
	DrawText((UI_W - TextWidth("L: CTRL PANEL - HOLD SELECT: CONSOLE")) / 2,
			 184, "L: CTRL PANEL - HOLD SELECT: CONSOLE", C_DIM);
	BlitRect(0, 0, UI_W, UI_H);
}

/* ------------------------------------------------------------------ */
/* state sampling                                                      */
/* ------------------------------------------------------------------ */
static int GameplayActive(void)
{
	if (!PORT_UI_SceneSeen || CmptMemoTimerRef != 0 || FlagCredits)
		return 0;
	/* 1.5 s: a camera-recenter full redraw takes ~235 ms and sustained
	   full-redraw spam (autoenter smoke) spaces AffScene ~0.7 s apart —
	   the window must sit well above that or the gate flaps */
	if ((ULONG)(TimerSystem - PORT_UI_SceneStamp) > 75)
		return 0;
	return PORT_UI_PersoManual() ? 1 : 0;
}

/* Debounced gate: the raw predicate flaps for sub-frame instants (the
 * engine brackets SaveTimer/RestoreTimer around loads inside a frame, and
 * the VBlank ISR can sample mid-bracket) — repainting 8 buttons on every
 * flap made the behaviour row blink and burned ISR time. Only accept a
 * state that held for 10 vblanks (~170 ms). */
static int PlayStable = 0;

static int GameplayStable(void)
{
	static int cnt;
	int now = GameplayActive();

	if (now != PlayStable)
	{
		if (++cnt >= 10)
		{
			PlayStable = now;
			cnt = 0;
		}
	}
	else
		cnt = 0;
	return PlayStable;
}

static int ItemUsable(int flag)
{
	return ListFlagGame[flag] == 1 && ListFlagGame[UI_FLAG_CONSIGNE] == 0;
}

static void ComputeWanted(void)
{
	int play = GameplayStable();
	int i;

	for (i = 0; i < 4; i++)
		BtnWanted[i] = (play ? BS_ENABLED : 0) |
					   ((Comportement == i) ? BS_ACTIVE : 0);

	BtnWanted[4] = ((play && ItemUsable(UI_FLAG_BALLE)) ? BS_ENABLED : 0) |
				   ((ListFlagGame[UI_FLAG_BALLE] && Weapon == 0) ? BS_ACTIVE : 0);
	BtnWanted[5] = ((play && ItemUsable(UI_FLAG_SABRE)) ? BS_ENABLED : 0) |
				   ((ListFlagGame[UI_FLAG_SABRE] && Weapon == 1) ? BS_ACTIVE : 0);
	BtnWanted[6] = (play && ItemUsable(UI_FLAG_HOLOMAP)) ? BS_ENABLED : 0;
	BtnWanted[7] = (play && ItemUsable(UI_FLAG_PINGOUIN) && NumPingouin > 0)
					   ? BS_ENABLED : 0;
}

/* ------------------------------------------------------------------ */
/* touch                                                               */
/* ------------------------------------------------------------------ */
static int PressedBtn = -1; /* button the stylus went down on */

static int HitTest(int px, int py)
{
	int i, x, y;

	for (i = 0; i < 8; i++)
	{
		BtnRect(i, &x, &y);
		if (px >= x && px < x + BTN_W && py >= y && py < y + BTN_H)
			return i;
	}
	return -1;
}

static void FireButton(int i)
{
	switch (i)
	{
	case 0:
	case 1:
	case 2:
	case 3:
		/* no-op taps must not reach the engine: SetComportement reloads
		   the body even for the same value (InitBody churn = slowdown) */
		if (Comportement != (WORD)i)
			PORT_TouchComportement = (WORD)i;
		break;
	case 4:
		PORT_InjectKey = UI_K_1; /* magic ball, PERSO.C K_1 handler */
		break;
	case 5:
		PORT_InjectKey = UI_K_2; /* sabre, PERSO.C K_2 handler      */
		break;
	case 6:
		PORT_TouchInvAction = 0; /* holomap (InventoryAction 0)     */
		break;
	case 7:
		PORT_TouchInvAction = 14; /* meca-pingouin (InventoryAction) */
		break;
	}
	MailAge = 0;
}

/* Hardened against phantom touches (TSC noise / emu quirks): a press needs
 * TWO consecutive vblanks of pen-down with in-range coordinates (a single
 * spurious sample can never fire), fires ONCE per pen contact (re-armed
 * only after two clean pen-up frames), and actions are rate limited. */
static void HandleTouch(void)
{
	TouchData tp;
	static int penheld = 0;    /* consecutive valid pen-down frames */
	static int armed = 0;      /* starts DISARMED: a boot-time phantom
	                              "pen held since power-on" (seen on
	                              melonDS before the first real click)
	                              can never fire — the first accepted
	                              contact requires a clean release first */
	static int cooldown = 0;   /* vblanks until the next action     */
	int valid;

	valid = (keysHeld() & KEY_TOUCH) != 0 &&
			touchRead(&tp) &&
			tp.px < UI_W && tp.py < UI_H;

	if (cooldown > 0)
		cooldown--;

	if (valid)
	{
		if (penheld < 1000)
			penheld++;
		if (armed && penheld == 2)
		{
			armed = 0;
			if (cooldown == 0)
			{
				int hit = HitTest(tp.px, tp.py);

				if (hit != -1 && (BtnWanted[hit] & BS_ENABLED))
				{
					PressedBtn = hit;
					FireButton(hit);
					cooldown = 15; /* >= 250 ms between actions */
				}
			}
		}
	}
	else
	{
		if (penheld == 0)
			armed = 1; /* second consecutive clean frame re-arms */
		penheld = 0;
		PressedBtn = -1;
	}

	if (PressedBtn != -1)
		BtnWanted[PressedBtn] |= BS_PRESSED;
}

/* ================================================================== */
/* Language page                                                       */
/* ================================================================== */
/* LBA1 has no in-game language option: InitLanguage() (MESSAGE.C) reads
 * LBA.CFG once at boot and that is that. The touch screen is free
 * whenever gameplay is not running (the buttons below are all disabled
 * then anyway), so the language selector lives there.
 *
 * Two independent settings, exactly as the original release worked:
 *   Language   — subtitles; TEXT.HQR carries all five, no extra assets
 *   LanguageCD — speech; GOG only ships EN/FR/DE voice banks, so Spanish
 *                and Italian get subtitles with English speech
 *
 * Applying it is one call: reset InitDial()'s LastFileInit cache guard and
 * re-enter it — it reloads the text for `Language` and, via InitSpeak(),
 * reopens the voice bank for `LanguageCD`. That must NOT happen here: this
 * file runs inside the VBlank ISR and InitDial does HQR loads and card I/O.
 * So the tap only posts to a mailbox, drained by PORT_PumpLang() from the
 * main thread (nds_video.c calls it from the redraw entry points). */

extern LONG Language;     /* subtitle language, 0..4 (MESSAGE.C)       */
extern LONG LanguageCD;   /* spoken language, 0..4                     */
extern LONG LastFileInit; /* InitDial() cache guard, -1 = "reload"     */
extern void InitDial(long file);
extern void StopSpeak(void);

#define LANG_N 5
#define LANG_FILE "fat:/lba1/save/lang.txt"

static const char *LangLabel[LANG_N] = {
	"ENGLISH", "FRANCAIS", "DEUTSCH", "ESPANOL", "ITALIANO"};
static const char *LangTag[LANG_N] = {"EN", "FR", "DE", "SP", "IT"};
/* GOG ships EN/FR/DE voice banks only — SP and IT were subtitle-only */
static const UBYTE LangVoice[LANG_N] = {1, 1, 1, 0, 0};

static volatile int LangMail = -1; /* tap -> PORT_PumpLang()           */
static int LangPending = -1;       /* saved choice, applied once ready */
static int LangShown = 0;          /* the page is currently on screen  */
static int LangPressed = -1;

#define LNG_W 76
#define LNG_H 40

static void LangRect(int i, int *x, int *y)
{
	if (i < 3)
	{
		*x = 8 + i * 82;
		*y = 62;
	}
	else
	{
		*x = 49 + (i - 3) * 82; /* bottom row of two, centred */
		*y = 110;
	}
}

static int LangHitTest(int px, int py)
{
	int i, x, y;

	for (i = 0; i < LANG_N; i++)
	{
		LangRect(i, &x, &y);
		if (px >= x && px < x + LNG_W && py >= y && py < y + LNG_H)
			return i;
	}
	return -1;
}

static void DrawLangButton(int i)
{
	int x, y, sel = (Language == i);
	UBYTE face, lt, dk, ink;

	LangRect(i, &x, &y);

	if (LangPressed == i)
	{
		face = C_PRESS;
		lt = C_EDGEDK;
		dk = C_GOLD;
		ink = C_WHITE;
	}
	else if (sel)
	{
		face = C_FACEHI;
		lt = C_GOLD;
		dk = C_GOLDDK;
		ink = C_GOLD;
	}
	else
	{
		face = C_FACE;
		lt = C_GRAY;
		dk = C_EDGEDK;
		ink = C_WHITE;
	}

	FillRect(x, y, LNG_W, LNG_H, face);
	Bevel(x, y, LNG_W, LNG_H, lt, dk);
	if (sel)
		Bevel(x + 1, y + 1, LNG_W - 2, LNG_H - 2, lt, dk);

	DrawText(x + (LNG_W - TextWidth(LangTag[i])) / 2, y + 9, LangTag[i], ink);
	DrawText(x + (LNG_W - TextWidth(LangLabel[i])) / 2, y + 24, LangLabel[i],
			 sel ? C_GOLD : C_GRAY);
	BlitRect(x, y, LNG_W, LNG_H);
}

/* the "TEXT xx  VOICE xx" footer */
static void DrawLangFooter(void)
{
	char buf[32];
	int v = (Language >= 0 && Language < LANG_N) ? (int)Language : 0;

	strcpy(buf, "TEXT ");
	strcat(buf, LangTag[v]);
	strcat(buf, "   VOICE ");
	strcat(buf, LangTag[LangVoice[v] ? v : 0]);

	FillRect(0, 156, UI_W, 14, C_BG);
	DrawText((UI_W - TextWidth(buf)) / 2, 158, buf,
			 LangVoice[v] ? C_GRAY : C_GOLDDK);
	BlitRect(0, 156, UI_W, 14);
}

static void DrawLangPage(void)
{
	static const char *title = "LANGUAGE";
	int i;

	FillRect(0, ST_H, UI_W, UI_H - ST_H, C_BG);
	DrawText((UI_W - TextWidth(title)) / 2, 50, title, C_WHITE);
	BlitRect(0, ST_H, UI_W, UI_H - ST_H);

	for (i = 0; i < LANG_N; i++)
		DrawLangButton(i);
	DrawLangFooter();
}

/* same phantom-touch hardening as HandleTouch() */
static void HandleLangTouch(void)
{
	TouchData tp;
	static int penheld = 0;
	static int armed = 0;
	static int cooldown = 0;
	int valid, prev = LangPressed;

	valid = (keysHeld() & KEY_TOUCH) != 0 &&
			touchRead(&tp) &&
			tp.px < UI_W && tp.py < UI_H;

	if (cooldown > 0)
		cooldown--;

	if (valid)
	{
		if (penheld < 1000)
			penheld++;
		if (armed && penheld == 2)
		{
			armed = 0;
			if (cooldown == 0)
			{
				int hit = LangHitTest(tp.px, tp.py);

				if (hit != -1 && hit != Language)
				{
					LangPressed = hit;
					LangMail = hit;
					cooldown = 15;
				}
			}
		}
	}
	else
	{
		if (penheld == 0)
			armed = 1;
		penheld = 0;
		LangPressed = -1;
	}

	if (prev != LangPressed)
	{
		if (prev != -1)
			DrawLangButton(prev);
		if (LangPressed != -1)
			DrawLangButton(LangPressed);
	}
}

/* ---- main-thread side -------------------------------------------- */

static void LangSave(int n)
{
	FILE *f;

	PORT_IoLock();
	f = fopen(LANG_FILE, "w");
	if (f)
	{
		fprintf(f, "%d\n", n);
		fclose(f);
	}
	PORT_IoUnlock();
}

/* read at UI init; applied later, once the engine has run InitLanguage() */
static void LangLoad(void)
{
	FILE *f;
	int n = -1;

	PORT_IoLock();
	f = fopen(LANG_FILE, "r");
	if (f)
	{
		if (fscanf(f, "%d", &n) != 1)
			n = -1;
		fclose(f);
	}
	PORT_IoUnlock();

	if (n >= 0 && n < LANG_N)
		LangPending = n;
}

static void LangApply(int n)
{
	LONG cur = LastFileInit;

	StopSpeak(); /* a line from the old bank may still be playing */

	Language = n;
	LanguageCD = LangVoice[n] ? n : 0; /* SP/IT: subtitles, English speech */
	LastFileInit = -1;                 /* force InitDial to reload         */

	if (cur >= 0)
		InitDial(cur); /* reloads the text AND reopens the voice bank */

	printf("[lang] %s (text) / %s (voice)\n",
		   LangTag[n], LangTag[LangVoice[n] ? n : 0]);
}

/* Drained from the main thread (nds_video.c redraw entry points): never
 * call InitDial from the VBlank ISR. */
void PORT_PumpLang(void)
{
	static int busy = 0;
	int n;

	/* the redraw entry points that call us are common enough that InitDial
	 * reaching one of them (an error Message(), say) would recurse */
	if (busy)
		return;
	busy = 1;

	/* a saved choice is applied as soon as the engine has done its first
	 * InitDial — that is strictly after InitLanguage() has read LBA.CFG,
	 * so we overwrite the config default rather than race it */
	if (LangPending >= 0 && LastFileInit >= 0)
	{
		n = LangPending;
		LangPending = -1;
		if (n != Language)
			LangApply(n);
	}

	n = LangMail;
	if (n >= 0)
	{
		LangMail = -1;
		if (n < LANG_N)
		{
			LangApply(n);
			LangSave(n);
		}
	}
	busy = 0;
}

/* ------------------------------------------------------------------ */
/* SELECT hold: console <-> UI                                         */
/* ------------------------------------------------------------------ */
static void HandleConsoleToggle(void)
{
	static int hold = 0;

	if (keysHeld() & KEY_SELECT)
	{
		if (++hold == 60) /* ~1 s */
		{
			ConsoleMode = !ConsoleMode;
			if (!ConsoleMode)
			{
				/* returning from console: repaint everything */
				DrawFrame();
				DrawStatus(&StCur);
				{
					int i;

					for (i = 0; i < 8; i++)
						DrawButton(i);
				}
			}
			SetUiVisible(UiAutoShown && !ConsoleMode);
		}
	}
	else
		hold = 0;
}

/* ------------------------------------------------------------------ */
/* VBlank hook (called by nds_video.c VBlankISR after PORT_ScanInput)  */
/* ------------------------------------------------------------------ */
void PORT_UI_VBlank(void)
{
	Status st;
	int i;

	if (!UiInited)
		return;

	HandleConsoleToggle();

	/* expire unconsumed taps (e.g. posted right as a dialog opened) */
	if (PORT_InjectKey || PORT_TouchComportement != -1 ||
		PORT_TouchInvAction != -1)
	{
		if (++MailAge > MAIL_TTL)
		{
			PORT_InjectKey = 0;
			PORT_TouchComportement = -1;
			PORT_TouchInvAction = -1;
		}
	}
	else
		MailAge = 0;

	ComputeWanted();

	/* first gameplay: bring the UI up (console stays on SELECT-hold) */
	if (!UiAutoShown && PlayStable)
	{
		UiAutoShown = 1;
		SetUiVisible(!ConsoleMode);
	}

	if (!UiAutoShown || ConsoleMode)
		return; /* console visible: no touch, no redraw */

	/* Outside gameplay the eight buttons are all disabled anyway, so the
	 * panel area becomes the language selector instead. */
	if ((!PlayStable) != LangShown)
	{
		LangShown = !PlayStable;
		if (LangShown)
		{
			LangPressed = -1;
			DrawLangPage();
		}
		else
		{
			DrawFrame(); /* repaint the panel chrome under the buttons */
			for (i = 0; i < 8; i++)
				BtnState[i] = 0xFF; /* != anything wanted: force redraw */
		}
	}

	if (LangShown)
	{
		HandleLangTouch();
	}
	else
	{
		HandleTouch();

		for (i = 0; i < 8; i++)
			if (BtnWanted[i] != BtnState[i])
			{
				BtnState[i] = BtnWanted[i];
				DrawButton(i);
			}
	}

	st.life = PORT_UI_LifePoint();
	st.magic = MagicPoint;
	st.magiclevel = MagicLevel;
	st.magicvis = (ListFlagGame[UI_FLAG_TUNIQUE] &&
				   !ListFlagGame[UI_FLAG_CONSIGNE]) ? 1 : 0;
	st.gold = NbGoldPieces;
	st.keys = NbLittleKeys;
	st.clover = NbFourLeafClover;
	st.cloverbox = NbCloverBox;
	st.fps = (WORD)NbFramePerSecond;

	if (memcmp(&st, &StCur, sizeof(st)) != 0)
	{
		StCur = st;
		DrawStatus(&StCur);
	}
}

/* ------------------------------------------------------------------ */
/* init (main_nds.c, after consoleDemoInit + nitroFSInit)              */
/* ------------------------------------------------------------------ */
void PORT_UI_Init(void)
{
	int i;

	LangLoad(); /* remembered language, applied on the first PORT_PumpLang */

	/* keep the console (BG0, text) alive and add BG2 as an 8bpp bitmap.
	   Map base 4 = offset 64K in VRAM C: the console demo layout uses
	   map base 22 (44K) / tile base 3 (48K..56K), so 64K.. is free. */
	videoSetModeSub(MODE_5_2D | DISPLAY_BG0_ACTIVE);
	UiBg = bgInitSub(2, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
	UiGfx = bgGetGfxPtr(UiBg);
	bgSetPriority(UiBg, 0);
	/* console text (BG0) must never bleed through the UI: BG0 loses ties
	   against BG2 only with an explicitly worse priority */
	REG_BG0CNT_SUB = (REG_BG0CNT_SUB & ~3) | 3;
	bgUpdate();

	for (i = 0; i < (int)(sizeof(UiPal) / sizeof(UiPal[0])); i++)
		BG_PALETTE_SUB[1 + i] = UiPal[i];

	DrawFrame();
	for (i = 0; i < 8; i++)
	{
		BtnState[i] = 0;
		DrawButton(i);
	}

	SetUiVisible(0); /* console first; auto-shown at first gameplay */

	UiInited = 1;
}
