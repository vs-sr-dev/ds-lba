# LBA1 — Nintendo DS build (devkitARM, ARM9 + nitroFS)
#
# Build from PowerShell via Docker:
#   docker run --rm -v ${PWD}:/work -w /work/platform/nds devkitpro/devkitarm:latest make -j8
#
# Same engine build conventions as platform/sdl/Makefile:
# - engine .C forced to C (-x c), gnu89, watcom_compat.h force-included
#   (the NDS variant also maps stricmp->strcasecmp, fopen->NDS_fopen,
#   malloc->NDS_malloc for the RAM budget log)
# - -fsigned-char (1994 x86 code assumes signed char; ARM defaults unsigned)
# - -fcommon (K&R duplicate definitions)
# - compat/inc resolves the "../LIB386/LIB_SYS/*.H" includes
# - compat_nds provides <process.h> / "direct.h" that newlib lacks

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
LIBNDS    ?= $(DEVKITPRO)/libnds
CALICO    ?= $(DEVKITPRO)/calico

CC      := $(DEVKITARM)/bin/arm-none-eabi-gcc
NDSTOOL := $(DEVKITPRO)/tools/bin/ndstool

TOP       := ../..
ENGINE    := $(TOP)/engine
TRANSLATE := $(TOP)/translate

ARCH := -march=armv5te -mtune=arm946e-s -mthumb -mthumb-interwork

CFLAGS := -O2 -g -pipe -MMD -MP -ffunction-sections -fdata-sections -fomit-frame-pointer \
          -fsigned-char -fcommon -Wall -Wno-unused -Wno-parentheses \
          $(ARCH) -DARM9 -D__NDS__ -DCDROM -DPORT_NDS \
          -I$(ENGINE) -I$(ENGINE)/game -I. \
          -I$(LIBNDS)/include -I$(CALICO)/include

ENGINE_CFLAGS := $(CFLAGS) -std=gnu89 -include watcom_compat.h \
          -Wno-implicit-function-declaration -Wno-implicit-int \
          -Wno-int-conversion -Wno-incompatible-pointer-types \
          -Wno-pointer-sign -Wno-discarded-qualifiers -Wno-format \
          -I$(ENGINE)/LIB_SYS -I$(ENGINE)/LIB_CD -I$(ENGINE)/LIB_MENU \
          -I$(ENGINE)/LIB_SVGA -Icompat/inc -Icompat_nds

PLAT_CFLAGS := $(CFLAGS)

# make PROFFULL=1 — diagnostic ROM: AffScene forced to full redraw every
# frame (worst case "redraw pieno"); see nds_prof.c. Do not ship.
ifdef PROFFULL
PLAT_CFLAGS += -DPROF_FORCE_FULL
endif

LDFLAGS := -specs=$(CALICO)/share/ds9.specs -g $(ARCH) -Wl,--gc-sections \
           -Wl,-Map,build/lba1ds.map

# frame-phase profiler (nds_prof.c): intercept the hot engine entry points
# with ld --wrap — zero engine edits, see nds_prof.c for the phase map
WRAPS := AffScene Cls CopyScreen ClsBoxes AffGrille SetInterAnimObjet2 \
         AffObjetIso DrawOverBrick DrawOverBrick3
comma := ,
LDFLAGS += $(foreach w,$(WRAPS),-Wl$(comma)--wrap=$(w))
LIBS    := -L$(LIBNDS)/lib -L$(CALICO)/lib -lfilesystem -lfat -lnds9 -lcalico_ds9 -lm

OBJDIR := build

ENGINE_SRC := $(wildcard $(ENGINE)/game/*.C) \
              $(wildcard $(ENGINE)/LIB_3D/*.C) \
              $(wildcard $(ENGINE)/LIB_CD/*.C) \
              $(wildcard $(ENGINE)/LIB_MENU/*.C) \
              $(wildcard $(ENGINE)/LIB_MIDI/*.C) \
              $(wildcard $(ENGINE)/LIB_MIX/*.C) \
              $(wildcard $(ENGINE)/LIB_SVGA/*.C) \
              $(wildcard $(ENGINE)/LIB_SYS/*.C)

PLAT_SRC      := $(wildcard *.c)
TRANSLATE_SRC := $(wildcard $(TRANSLATE)/*.c)
ENGINE_OBJ    := $(patsubst $(ENGINE)/%.C,$(OBJDIR)/%.o,$(ENGINE_SRC))

# Hot engine objects promoted whole into ITCM with zero source edits:
# calico's ds9.ld places the .text/.rodata of any object named *.itcm.* in
# ITCM. Compiled -marm (ARM32 pays off in ITCM; in main RAM thumb is better).
# CPYMASK.C = CopyMask(), the brick re-masking blitter (DrawOverBrick).
# (GRILLE.C was tried and reverted: -1.6ms on a 252ms AffGrille — the cost
# is inside the AffGraph RLE runs, not the cube walk.)
ENGINE_ITCM   := game/CPYMASK
ENGINE_OBJ    := $(foreach o,$(ENGINE_OBJ),$(if $(filter $(patsubst $(OBJDIR)/%.o,%,$(o)),$(ENGINE_ITCM)),$(patsubst %.o,%.itcm.o,$(o)),$(o)))
PLAT_OBJ      := $(patsubst %.c,$(OBJDIR)/nds/%.o,$(PLAT_SRC))
TRANSLATE_OBJ := $(patsubst $(TRANSLATE)/%.c,$(OBJDIR)/translate/%.o,$(TRANSLATE_SRC))

# nitroFS content stamp: mtime-based deps miss files copied in with old
# timestamps (seen: vox/ refresh didn't trigger a repack) — hash the actual
# content at every make and touch the stamp only when it changes.
NITROSTAMP := $(OBJDIR)/nitrofs.stamp
_nitro_check := $(shell mkdir -p $(OBJDIR); \
	new=$$(find nitrofiles -type f 2>/dev/null | LC_ALL=C sort | xargs md5sum 2>/dev/null | md5sum); \
	old=$$(cat $(NITROSTAMP) 2>/dev/null); \
	[ "$$new" = "$$old" ] || echo "$$new" > $(NITROSTAMP))

# input-harness hooks (read from nitro:/ at boot): never in the SHIPPING ROM.
# "make diag" builds lba1ds_diag.nds with them staged in (see below).
HARNESS := autoenter autowalk autopause autosave autoload uitest

all: lba1ds.nds

lba1ds.nds: $(OBJDIR)/lba1ds.elf $(NITROSTAMP)
	@[ -f nitrofiles/ress.hqr ] || { \
		echo "ERROR: nitrofiles/ has no game data - none ships with this repository."; \
		echo "       Stage it from your own copy of LBA1, from the repo root:"; \
		echo "         tools/make_nitro_nds.sh <path-to-your-LBA1-install>"; \
		echo "       See README.md, 'Bring your own assets'."; \
		exit 1; \
	}
	@bad="$(wildcard $(addprefix nitrofiles/,$(HARNESS)))"; \
	if [ -n "$$bad" ]; then \
		echo "ERROR: harness file(s) in nitrofiles/: $$bad"; \
		echo "       the shipping ROM must be clean - remove them (use 'make diag' for a harness ROM)"; \
		exit 1; \
	fi
	$(NDSTOOL) -c $@ -9 $(OBJDIR)/lba1ds.elf -7 $(CALICO)/bin/ds7_maine.elf \
		-d nitrofiles -b $(CALICO)/share/nds-icon.bmp "LBA1;DS port M4;DS-LBA"

# Diagnostic ROM: same binary, nitroFS staged with the harness hooks.
# Values (seconds) via make vars, empty = hook absent:
#   make diag DIAG_AUTOENTER=60 DIAG_AUTOSAVE=85   (scripted in-game save)
#   make diag DIAG_AUTOENTER= DIAG_AUTOLOAD=45     (scripted load from menu)
DIAG_AUTOENTER ?= 60
DIAG_AUTOWALK  ?=
DIAG_AUTOPAUSE ?=
DIAG_AUTOSAVE  ?=
DIAG_AUTOLOAD  ?=

diag: lba1ds_diag.nds

lba1ds_diag.nds: $(OBJDIR)/lba1ds.elf $(NITROSTAMP) FORCE
	rm -rf $(OBJDIR)/nitrofs_diag
	cp -r nitrofiles $(OBJDIR)/nitrofs_diag
	@[ -z "$(DIAG_AUTOENTER)" ] || { echo "$(DIAG_AUTOENTER)" > $(OBJDIR)/nitrofs_diag/autoenter; echo "diag: autoenter=$(DIAG_AUTOENTER)"; }
	@[ -z "$(DIAG_AUTOWALK)" ]  || { echo "$(DIAG_AUTOWALK)"  > $(OBJDIR)/nitrofs_diag/autowalk;  echo "diag: autowalk=$(DIAG_AUTOWALK)"; }
	@[ -z "$(DIAG_AUTOPAUSE)" ] || { echo "$(DIAG_AUTOPAUSE)" > $(OBJDIR)/nitrofs_diag/autopause; echo "diag: autopause=$(DIAG_AUTOPAUSE)"; }
	@[ -z "$(DIAG_AUTOSAVE)" ]  || { echo "$(DIAG_AUTOSAVE)"  > $(OBJDIR)/nitrofs_diag/autosave;  echo "diag: autosave=$(DIAG_AUTOSAVE)"; }
	@[ -z "$(DIAG_AUTOLOAD)" ]  || { echo "$(DIAG_AUTOLOAD)"  > $(OBJDIR)/nitrofs_diag/autoload;  echo "diag: autoload=$(DIAG_AUTOLOAD)"; }
	$(NDSTOOL) -c $@ -9 $(OBJDIR)/lba1ds.elf -7 $(CALICO)/bin/ds7_maine.elf \
		-d $(OBJDIR)/nitrofs_diag -b $(CALICO)/share/nds-icon.bmp "LBA1 DIAG;harness build;DS-LBA"

FORCE:

$(OBJDIR)/lba1ds.elf: $(ENGINE_OBJ) $(PLAT_OBJ) $(TRANSLATE_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# NB: -O3 on the hot translate files was tried and reverted: no measurable
# gain over -O2 once the hot code sits in ITCM (see PORTING_NOTES_NDS.md,
# "Performance"), and it costs ~1.1KB of ITCM.
$(OBJDIR)/translate/%.o: $(TRANSLATE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.itcm.o: $(ENGINE)/%.C
	@mkdir -p $(dir $@)
	$(CC) $(ENGINE_CFLAGS) -marm -x c -c $< -o $@

# libc override: never let GCC pattern-match these loops back into
# memcpy/memset calls (self-recursion)
$(OBJDIR)/nds/nds_fastmem.o: nds_fastmem.c
	@mkdir -p $(dir $@)
	$(CC) $(PLAT_CFLAGS) -ffreestanding -fno-builtin -c $< -o $@

# touch-UI glue: compiled like an ENGINE TU (real engine headers) so the UI
# can read T_OBJET fields without mirroring the struct (see nds_ui_glue.c)
$(OBJDIR)/nds/nds_ui_glue.o: nds_ui_glue.c
	@mkdir -p $(dir $@)
	$(CC) $(ENGINE_CFLAGS) -x c -c $< -o $@

$(OBJDIR)/%.o: $(ENGINE)/%.C
	@mkdir -p $(dir $@)
	$(CC) $(ENGINE_CFLAGS) -x c -c $< -o $@

$(OBJDIR)/nds/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(PLAT_CFLAGS) -c $< -o $@

# The log banner stamps __DATE__/__TIME__, which is the compile time of THIS
# TU — not of the link. A build that changed only engine sources therefore
# reused a stale stamp and two different ROMs reported the same one, which
# has already sent one crash report to the wrong symbol table. Rebuild it
# every time: one file, and the stamp then always identifies the ROM.
$(OBJDIR)/nds/nds_sys.o: FORCE

clean:
	rm -rf $(OBJDIR) lba1ds.nds lba1ds_diag.nds

-include $(ENGINE_OBJ:.o=.d) $(PLAT_OBJ:.o=.d) $(TRANSLATE_OBJ:.o=.d)

.PHONY: all clean diag FORCE
