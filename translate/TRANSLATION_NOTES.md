# TRANSLATION_NOTES — LIB386/LIB_SVGA ASM → C (translate/)

Session: translating the SVGA blitters (Watcom/MASM 386 flat) into portable C
for the DS port. Pristine sources: `lba1-classic-community-main/LIB386/LIB_SVGA/*.ASM`.
Signatures taken from `engine/LIB_SVGA/LIB_SVGA.H`, which is the authority on
types.

## General conventions

- Every file includes only `translate.h` (plus `<string.h>` where needed): the
  engine headers do not include cleanly under GCC (`__far`, Watcom `cdecl`), so
  the globals are redeclared locally in `translate.h` with the same types as
  `LIB_SVGA.H` (`WORD ClipXmin`, `ULONG TabOffLine`, `UBYTE *Log`,
  `WORD Screen_X`…). Once the engine headers are cleaned up, swapping the
  include is all it takes.
- `TabOffLine` is declared as a scalar in the engine header; it is accessed as
  an array through `TABOFFLINE` (`(ULONG*)&TabOffLine`), the same style as
  `engine/game/CPYMASK.C`.
- Stride: many of the original routines use `Screen_X`, others hardcode **640**
  (S_LINE, ZOOM, S_FILLV, the S_STRING rewind, the clipped paths of
  GRAPH_A/MASK_A). Kept faithfully: as long as `Screen_X == 640` it makes no
  difference, but do NOT replace 640 with Screen_X without redoing the
  pixel-perfect comparison.
- 8-bit counters (`dec bl/bh`) are rendered as `UBYTE` + do/while: a DY or
  NbBlock of 0 iterates 256 times, exactly as on x86.
- Word/dword accesses at potentially odd addresses (rep stosw/movsd,
  `mov [edi], ax`) are rendered as byte operations / `memset` / `memcpy`: no
  unaligned access on the ARM9. The single assumption is that the graphics
  banks (`((ULONG*)bank)[num]`) are 4-aligned — the same assumption the
  community translation of CPYMASK.C makes.
- x86 address parity (`test edi,1` in Copper/Bopper/Trame) is rendered as the
  parity of the screen x, assuming `Log` is evenly aligned (it was on DOS; it
  will be on the DS).

## Modules

### s_plot.c (S_PLOT.ASM) — confidence HIGH
Plot/GetPlot with inclusive clipping. GetPlot returns 0 outside the clip
region. Nothing in doubt.

### s_box.c (S_BOX.ASM) — confidence HIGH
Clipped filled rectangle, one fill per row (memset ≡ stosd/stosb). Stride
`Screen_X`.

### s_block3.c (S_BLOCK3.ASM) — confidence HIGH
`CopyBlockIncrust`: rectangle copy with colour 0 transparent (tested on the
**source**). The comments in the ASM source ("BX Delta Y") are swapped; the
code was followed, not the comments.

### s_block2.c (S_BLOCK2.ASM) — confidence HIGH
`CopyBlockOnBlack`: the whole scasb/movsd dance is per-pixel equivalent to
`if (dst==0) dst=src`; run equivalence was verified (there is no case where the
scan skips or rewrites a byte). The 16-bit CptPixl/CptLine counters are
irrelevant here (screen dimensions < 64K).

### graphmsk.c (GRAPHMSK.ASM) — confidence HIGH
`CalcGraphMsk`: converts an RLE brick (00=skip / 01=copy / 10=repeat, count+1)
into the mask format (alternating skip/draw counters; a row always starts with
a skip, with a 0 inserted if needed). Note: opcode `11xxxxxx` is treated as
repeat (bit 7 is tested first), as in the ASM — the format comment would say
otherwise. The NbData accumulator is 8-bit (it wraps past 255; faithful). The
DX/DY/HotX/HotY header is copied byte by byte.

### zoom.c (ZOOM.ASM) — confidence HIGH
`ScaleLine`/`ScaleBox`: a 16.16 DDA with a 16-bit fractional accumulator plus
carry (`add bx,dx / adc esi,ebp`), reproduced exactly. Fidelity kept on:
- ScaleLine does NOT add xs0/xe0 to the pointers (it uses the deltas only) and,
  unlike ScaleBox, does not add 1 to the source delta;
- ScaleBox advances the source rows using `TabOffLine[n]` as a relative offset
  (assuming a linear 640 layout);
- division by (xd1-xd0)==0 → crash, as in the original (no guard added).
The 16-bit ScaleSprite in the original file is dead code after `End`: not
translated.

### s_line.c (S_LINE.ASM) — confidence HIGH (details MEDIUM)
`Line`/`Line_A` (Line_A is exported: P_OB_ISO calls it). Iterative
Cohen-Sutherland clipping plus Bresenham. The intersections use 16-bit
`imul si` / `idiv di` with a `movsx` of the quotient: reproduced with a cast to
`WORD` (identical as long as the deltas fit in 16 bits — always true on
screen). The vertical branch uses `adc edi,esi` after the carry: the carry was
proved to be always 1 at that point (`err+2dy-2dx > 0`), hence
`pDest += stride+1`. Stride hardcoded to ±640. Still to verify pixel-perfect:
exactly diagonal lines, and the multiple-clip cases (the order of the c0..c4
branches was kept identical).

### s_block.c (S_BLOCK.ASM) — confidence HIGH
`CopyBlock`/`SaveBlock`/`RestoreBlock`: rectangle copies; the ASM's two-row
unrolling is pure optimisation (a memcpy per row is byte-identical). Note: in
the last two parameters of Save/RestoreBlock the header says dx/dy but the code
uses them as x1/y1 (`width = dx - x + 1`) — the code's behaviour was kept.

### mask_a.c (MASK_A.ASM) — confidence HIGH (clipped path MEDIUM)
`CoulMask`/`AffMask`/`GetDxDyMask`. Mask format: alternating skip/draw counters.
Unclipped path: writes `ColMask` directly. Clipped path: expands the row into
`BufferClip[512]` (0=skip, ColMask=draw) and blits skipping the zeros → a
**faithful quirk**: with `ColMask==0` the clipped path draws nothing while the
unclipped one writes zeros. Stride 640 is hardcoded in the clipped path. The
end-of-row bookkeeping (EndBlock `inc esi` / `dec esi;inc esi`) was verified
instruction by instruction. AffMask_Asm is not exported (no external callers;
FONT uses the C AffMask).

### graph_a.c (GRAPH_A.ASM) — confidence HIGH (clipped path MEDIUM)
`AffGraph`/`GetDxDyGraph`. RLE bricks as in graphmsk. Faithful quirks:
- unclipped: Copy/Repeat blocks also write colour-0 pixels;
  clipped: a colour 0 expanded into BufferClip becomes transparent at blit time;
- skipping the rows above ClipYmin: repeat consumes 1 data byte, copy consumes
  `count+1`;
- 8-bit row counter (`inc al` / `dec bh`).
Still to verify pixel-perfect: bricks crossing the left/right edge
(OffsetBegin/NbPix), and bricks whose blocks contain colour 0.

### s_string.c (S_STRING.ASM) — confidence HIGH
`AffString`/`CoulText`. 8x8 font: the ASM's `db` data already exists in C in
`engine/LIB_SVGA/FONT8X8.C` (spot-checked against the first rows) → used via
extern rather than duplicated. `Text_Paper == 0xFF` means a transparent
background. Row advance uses `Screen_X`, but the cell rewind hardcodes
`(640*8)-8`, as the ASM does. No clipping. The Font6X6 variant (AffString1) is
inside a `comment #`: dead code, not translated.

### s_fillv.c (S_FILLV.ASM) — confidence MEDIUM/HIGH
`FillVertic`/`FillVertic_A` (exported: P_OB_ISO calls it)/`SetFillDetails`, plus
nine fillers. Key points:
- **TabVerticD/TabCoulD**: the ASM addresses these as `TabVerticG+960` /
  `TabCoulG+960`; here they are separate arrays (each ≥480 WORDs). Whatever
  fills the tables (S_POLY, later) must use the same pairs of arrays.
- Jump-table index: 0 Triste, 1 Tele, 2 Copper, 3 Bopper, 4 Marbre, 5 Trans,
  6 Trame, 7 Gouraud, 8 Dith. The `POLY_*` equates in svga.ash do NOT match —
  the `dd` table is the authority. SetFillDetails clamps unsigned to 2 and
  swaps the table.
- 8.8 arithmetic in 16 bits, reproduced bit-exact (`UWORD` plus explicit
  carry): Marbre uses the byte-swapped step with an offset carry (`adc ax,dx`);
  Dith uses `rol dl,cl` with the current counter as dither noise (rotation
  mod 8); Tele accumulates `ax` seeded with xG and `bx=17371`, evolved with
  `rol 2`/`inc` across the whole polygon.
- Faithful quirks: Copper does NOT write the last
  `(len&1 ? 1 : 0) + ((len&3)==3 ? 1 : 0)` bytes of the span (`and cl,2` instead
  of `and cl,3`); in "up" mode Copper/Bopper decrement the colour even on empty
  rows; Triche/Gouraud/Dith do NOT advance the intensity pointer on empty rows
  (a deliberate desync, faithfully reproduced); Gouraud discards the division
  remainder; a single pixel takes the average (l0), and in Marbre it takes the
  END colour.
- `sar ax,1` (the Dith optimisation) is rendered as `>>` on a negative int: fine
  with GCC/devkitARM (arithmetic shift), not strictly conforming C. Noted.
Still to verify pixel-perfect, in priority order: Dith (rol/add ordering),
the Gouraud opt/iopt branches (2-3 pixels), Copper up/down at multiple-of-16
boundaries.

### p_trigo.c (LIB_3D/P_TRIGO.ASM) — confidence HIGH
Fixed-point trigonometry (P_SinTab 1.15, matrices >>14 — the ASM comments say
">>15" but the code does `sar 14`). No data tables to extract: P_SinTab is
already in `engine/LIB_3D/P_SINTAB.C`. Data faithful to the ASM layout:
XCentre/YCentre are **LONG** (dd) even though LIB_3D.H declares them WORD
(little-endian, so reading the low word works; no engine C touches them
directly).
- The register-based entry points used by P_OB_ISO are exposed in `lib3d_p.h`:
  `Rot/WorldRot(WORD,WORD,WORD)`, `LongWorldRot/LongInverseRot(LONG…)` (64-bit,
  like imul/adc/shrd), `RotMatIndex2(src,dst)` (with the intermediate
  LMatriceDummy and the same aliases/copies of the alpha→gamma→beta flow),
  `Proj_3D` (entirely 16-bit, including the bp=32767 saturation and the 32/16
  idiv with no divide-by-zero guard), `Proj_ISO` (32-bit input, final
  `neg bx / add bx,[YCentre]` in 16 bits), `RotList/TransRotList` (16-bit adds
  of X0/Y0/Z0, WORD `compteur`: 0 → 65536 iterations, faithful).
- `ProjettePoint`: the 3D Z-clip test is `or cx,cx` (16-bit) — kept.
- `LongProjettePoint`: the `shl/mov/adc` saturation → 0x7FFF/0x8000 reproduced.
- `SetInverseAngleCamera`: FlipMatrice(World→Dummy) + Copy(Dummy→World)
  (Watcom's right-to-left push order verified).
Still to verify pixel-perfect: nothing specific; the only risk is the 32-bit
wraps of the matrix products (done through unsigned MUL32/ADD32).

### s_poly.c (LIB_SVGA/S_POLY.ASM) — confidence MEDIUM/HIGH
`ComputePoly/_A`, `ComputeSphere/_A` plus Sutherland-Hodgman clipping. Key
points:
- **TabPoly and TabPolyClip are contiguous** in a single array (97+96 WORDs):
  the closing point (the `movsw/movsd` "transitivité") of a 32-point poly
  overruns 2 words into TabPolyClip, exactly as it does on DOS.
- The four ClipGauche/Droit/Haut/Bas are a single
  `ClipPolyEdge(primIdx, bound, outsideIsGreater)` — in the ASM they are
  identical bar the axis, bound and direction. The normalisation of the edge
  direction before the idiv ("clip 2 poly collés") is kept; colour interpolation
  happens only if `TypePoly >= 7` (POLY_GOURAUD of svga.ash: here the indices DO
  match the ASM comparison, which is against the TypePoly already translated by
  P_OB_ISO).
- Edge DDA (EdgeGauche/EdgeDroite): the two add/adc (left) and sub/sbb (right)
  loops are reproduced bit-exact, including the "init carry"
  (add/rcl/sub/shr), the fractional seed `rem/2 + 7FFFh` (right:
  `-(rem/2) + 7FFFh`), the loop entry depending on the parity of deltaY
  (deltaY+1 entries written), and the store direction from DF (std on the
  swapped branches). Intensity is 8.8 with the seed `rem_low_byte/2 ± 7Fh`.
- The global Xmin/Xmax are NOT recomputed after clipping (only Ymin/Ymax, via
  "rencadre"); horizontal edges write nothing (flat polys → FillVertic reads
  stale tables, a faithful DOS quirk).
- Sphere: midpoint over a double pair of rows, with the carry of `add ebp,edx`
  marking the crossing to a sum ≥ 0; the clipped path has Ymin/Ymax narrowing
  at runtime and 16-bit row comparisons. `Ymin>=Ymax` → no sphere.
Still to verify pixel-perfect, in priority order: EdgeDroite (the sbb chain),
the fractional seeds on swapped rows, ClipPolyEdge gouraud (discarded idiv
remainders).

### p_ob_iso.c (LIB_3D/P_OB_ISO.ASM) — confidence MEDIUM/HIGH
`AffObjetIso`/`PatchObjet`. The flow and the buffers are documented at the head
of the file (summarised for DS tuning at the end of these notes). Fidelity:
- List_Entity records are handled ONLY as WORDs (the vertex blocks are 2-aligned,
  not 4: no unaligned dword access on ARM); object data is read through 16-bit
  memcpy helpers.
- Polys: `sub cl,7` (flat→Triste/Tele) and `sub cl,2` (gouraud→7/8) on the
  material byte ONLY; per-vertex gouraud colour `add dl,ch` (low byte of the
  intensity plus coul1); backface cull with the 16×16→32 cross product and the
  exact sub/sbb comparison (64-bit in C); ZMax = the maximum of the vertices'
  Zrot.
- Lines: the colour is **byte-swapped** (`xchg al,ah`) before Line_A; Z = the
  maximum of the two points.
- Spheres: coul is read with `mov ax,[esi+1]` (an odd offset! = byte1|byte2<<8,
  recomposed from the two WORDs); the ISO radius is `*34>>9`, the 3D one is a
  16-bit `imul/idiv`; the Screen box is updated even for spheres that are
  subsequently rejected (faithful).
- The "SergeSort" is transliterated 1:1 (a quicksort with an explicit stack,
  plus a selection sort for ≤8 and a two-element case): the ordering of equal-Z
  records is part of the visual result. Record = struct {WORD z; WORD type;
  WORD *ptr;} (8 bytes on ILP32, as on x86/ARM32).
- ISO projection: Xp=((x+zrot)*24>>9)+XCentre, Yp=((12(x-zrot)-30y)>>9)+YC,
  Zsort=(zrot-x)-y in 16 bits; 3D: 64/32 imul/idiv with the
  overX/overY/overZ saturations (`shr 16 | 7FFF`) and the ebp≤0 overflow →
  0x7FFFFFFF.
- **RotateNuage (static, non-ANIM objects)**: the ASM passes Proj_ISO registers
  whose upper halves are "dirty" (eax: the upper half of Y0, ebx: that of Z0,
  ebp: that of x*LMat20 — the source itself says "passer les param en long !!!").
  Reproduced EXACTLY, by recomposing the 32 bits. In LBA1 almost every body is
  INFO_ANIM, so this path is rare.
- List_Normal stays at 500 WORDs ("surement plus" in the source): models with
  more normals would overrun on DOS too.
Still to verify pixel-perfect: the sort order for equal Z (already 1:1, but it
is the most sensitive point), the static path, and clipped spheres.

### texture.c (TEXTURE.ASM) — confidence HIGH (DDA seed MEDIUM/HIGH)
An **affine** textured-triangle rasteriser, used only by the holomap
(HOLOMAP.C: `AsmTexturedTriangleNoClip()` + `FillTextPolyNoClip(LYmin,LYmax,
PtrMap)`). Everything that follows the `END` directive in the ASM
(M_FillTextPoly, M_AsmFillProp, disptexture, a second
AsmTexturedTriangleNoClip) is dead code and was not translated;
AsmFillProp/FillTextPoly/FillTextPolyShade/AsmGouraudTriangleNoClip are live
but have no C callers — translated faithfully for completeness. Key points:
- **Edge tables**: in the ASM they are one contiguous block addressed as
  `TabGauche + 960*n`; here they map onto the s_poly.c arrays with the original
  layout's aliasing: TabGauche=TabVerticG, TabDroite=TabVerticD,
  TabX0=TabCoulG, TabY0=TabCoulD, plus TabX1/TabY1 (already provided for in
  lib3d_p.h).
- **Edge DDA (A_FillPropNoClip)**: a 16.16 quotient `(delta16<<16)/deltay`
  rotated (`rol edx,16`), with a single 32-bit `adc/sbb eax,edx` per entry: the
  fraction lives in the high word and its overflow reaches X only through CF on
  the following iteration (and a wrap of X loads the fraction) — the chain is
  reproduced exactly with `unsigned long long`. The fractional seed is
  `rem/2 + 7FFFh` in the NoClip entries and **the bare remainder** in
  AsmFillProp (the shr/add are commented out in the ASM). deltay+1 entries are
  written; deltay==0 in the FillProps is a division by zero, as on DOS (the
  triangle builders only ever call with strictly different Ys).
- **Triangle builder**: coordinates are read with **movzx** (negative Xs would
  become 32000+); LYmin/LYmax are updated with <=/>= and only from
  non-horizontal edges → an entirely horizontal triangle leaves 32000/-32000
  (TestVuePoly in HOLOMAP.C rejects it first). It does not call the filler: it
  is HOLOMAP.C that chains FillTextPolyNoClip.
- **Filler**: per row, `xstep=(u1-u0+1)/len`, `ystep=(v1-v0+1)/len` (+1 on both
  numerators, truncating idiv); the u/v accumulators are **8.8 in 16 bits** with
  the steps truncated to the low word; texel = `map[(v&FF00h)|(u>>8)]` → 256x256
  wrapping for free. Quirk: NoClip draws `xd-xg` pixels while the clipped
  variants draw `min(xd,ClipXmax)-max(xg,ClipXmin)+1` (one pixel MORE for the
  same span); the left clip rewrites TabX0/TabY0 (TabCoulG/D) **in place** with
  16-bit imul products; the row/pixel counters are 16-bit (0 → 65536
  iterations); stride 640 is hardcoded. FillTextPolyShade: the low nibble is
  darkened with a clamp at the base of the ramp (a palette of 16 ramps of 16),
  preserving the high bits.
- AsmFillProp compares the bounds with 32-bit loads (on DOS ClipYmin/max are
  dd): here the port's WORDs are widened — identical for values 0..479.
Verified: clean SDL build, in-game holomap with the planet Twinsun textured
(recognisable continents and oceans, correct wrapping at the poles, rotation
with the arrow keys). Still to verify pixel-perfect, in priority order: the
DDA's fractional seed on the right-hand edges (sbb), and rows with len==1.

## Notes for the DS port (P_OB_ISO)

Hot data (DTCM candidates): List_Anim_Point/List_Point (3 KB each),
List_Normal (1 KB), TabMat (1.1 KB), List_Tri (4 KB), TabVerticG/D+TabCoulG/D
(3.8 KB); List_Entity (10 KB) is probably too large → main RAM.
Hot code (ITCM candidates): RotList/TransRotList, the two projection loops,
EdgeGauche/EdgeDroite, the s_fillv.c fillers, SergeSort.

## Verification performed

`gcc -fsyntax-only -std=c99 -Wall -Wextra -fsigned-char` and
`-std=c90 -pedantic`: all twelve files clean (mingw32). No execution test yet:
that needs the pixel-perfect comparison harness (640x480 framebuffer plus a
linear TabOffLine).

P_TRIGO/S_POLY/P_OB_ISO session: clean mingw32 build, in-game smoke test (the
first LBA1 scene): Twinsen and the other actors (the doctors, the sweeping
clone, the robot) render correctly — proportions, gouraud, lines (the
microphone cable), shadows. Not yet compared pixel-perfect against DOS.

## Useful patterns for S_POLY / TEXTURE / P_OB_ISO (future sessions)

(Historical section: TEXTURE has since been translated — the campaign is
complete, 16/16.)

- Convention: `proc`s with stack parameters (`.model SYSCALL`) map 1:1 onto the
  C declaration in LIB_SVGA.H; the "register-based" entry points (`Line_A`:
  eax,ebx,ecx,edx,ebp; `FillVertic_A`: ecx=type, edi=colour) were exposed as
  ordinary C functions taking those parameters in that order — P_OB_ISO will
  call them that way.
- Recurring globals: `Log`, `TabOffLine` (an array, via `&`), `Screen_X`,
  `ClipXmin/Ymin/Xmax/Ymax` (inclusive clipping), and the poly tables
  `Ymin/Ymax/TabVerticG/D/TabCoulG/D` (16-bit, intensity 8.8).
- 640 is hardcoded throughout the poly/line/zoom path; `Screen_X` is used in the
  rectangle blitters.
- Loop counters are often 8- or 16-bit: mind the wraps (fidelity!).
- `rep stosw` with `jnc/stosb` = filling in pairs plus an odd byte (no real
  alignment effect); the initial `test edi,1`, on the other hand, depends on the
  parity of the address → the parity of x, if Log is even.
