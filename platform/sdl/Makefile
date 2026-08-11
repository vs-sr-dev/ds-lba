# LBA1 — PC/SDL2 shim build (MinGW i686, 32-bit: same ILP32 model as the DS)
#
# Build from PowerShell:
#   $env:PATH = 'C:\msys64\mingw32\bin;C:\msys64\usr\bin;' + $env:PATH
#   make -C <repo>\platform\sdl
#
# Notes:
# - Engine sources use uppercase .C; gcc would treat them as C++, so we force -x c.
# - -fsigned-char: the 1994 x86 code assumes signed char (ARM defaults unsigned,
#   the DS build will need the same flag).

TOP     := ../..
ENGINE  := $(TOP)/engine

# Some shells hand gcc a bogus TMP (C:\WINDOWS) -> collect2/as can't create
# temp files. Point gcc's temp dir at the build dir (Windows-style path).
export TMP  := $(shell cygpath -m $(CURDIR))/build
export TEMP := $(TMP)

CC      := gcc
CFLAGS  := -O1 -g -pipe -fsigned-char -fcommon -Wall -Wno-unused -Wno-parentheses \
           -DCDROM -DPORT_SDL \
           -I$(ENGINE) -I$(ENGINE)/game -I.

# Engine-only flags: 1994 K&R/Watcom code. gnu89 + downgrade the checks that
# GCC>=14 promoted to hard errors; watcom_compat.h kills Watcom keywords.
# Extra -I dirs cover bare includes ("ADELINE.H", "LIB_CD.H", "lib_menu.h")
# and compat/inc makes "../LIB386/LIB_SYS/*.H" resolve to compat/LIB386/.
ENGINE_CFLAGS := $(CFLAGS) -std=gnu89 -include watcom_compat.h \
           -Wno-implicit-function-declaration -Wno-implicit-int \
           -Wno-int-conversion -Wno-incompatible-pointer-types \
           -Wno-pointer-sign -Wno-discarded-qualifiers -Wno-format \
           -I$(ENGINE)/LIB_SYS -I$(ENGINE)/LIB_CD -I$(ENGINE)/LIB_MENU \
           -I$(ENGINE)/LIB_SVGA -Icompat/inc

# Shim-only flags: modern C + SDL2 headers (-Dmain=SDL_main comes from
# sdl2-config and only affects main_sdl.c, which is what SDL2main expects).
PLAT_CFLAGS := $(CFLAGS) $(shell sdl2-config --cflags)

LIBS    := -lmingw32 -lSDL2main -lSDL2

OBJDIR  := build

ENGINE_SRC := $(wildcard $(ENGINE)/game/*.C) \
              $(wildcard $(ENGINE)/LIB_3D/*.C) \
              $(wildcard $(ENGINE)/LIB_CD/*.C) \
              $(wildcard $(ENGINE)/LIB_MENU/*.C) \
              $(wildcard $(ENGINE)/LIB_MIDI/*.C) \
              $(wildcard $(ENGINE)/LIB_MIX/*.C) \
              $(wildcard $(ENGINE)/LIB_SVGA/*.C) \
              $(wildcard $(ENGINE)/LIB_SYS/*.C)

PLAT_SRC   := $(wildcard *.c)

# C translations of the LIB386 ASM modules (portable, plain C90)
TRANSLATE     := $(TOP)/translate
TRANSLATE_SRC := $(wildcard $(TRANSLATE)/*.c)

ENGINE_OBJ    := $(patsubst $(ENGINE)/%.C,$(OBJDIR)/%.o,$(ENGINE_SRC))
PLAT_OBJ      := $(patsubst %.c,$(OBJDIR)/sdl/%.o,$(PLAT_SRC))
TRANSLATE_OBJ := $(patsubst $(TRANSLATE)/%.c,$(OBJDIR)/translate/%.o,$(TRANSLATE_SRC))

all: lba1sdl.exe

lba1sdl.exe: $(ENGINE_OBJ) $(PLAT_OBJ) $(TRANSLATE_OBJ)
	$(CC) -o $@ $^ $(LIBS)

$(OBJDIR)/translate/%.o: $(TRANSLATE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: $(ENGINE)/%.C
	@mkdir -p $(dir $@)
	$(CC) $(ENGINE_CFLAGS) -x c -c $< -o $@

$(OBJDIR)/sdl/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(PLAT_CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) lba1sdl.exe

.PHONY: all clean
