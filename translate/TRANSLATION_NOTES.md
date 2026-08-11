# TRANSLATION_NOTES — LIB386/LIB_SVGA ASM → C (translate/)

Sessione: traduzione dei blitter SVGA (Watcom/MASM 386 flat) in C portabile per il
port DS. Sorgenti pristini: `lba1-classic-community-main/LIB386/LIB_SVGA/*.ASM`.
Firme prese da `engine/LIB_SVGA/LIB_SVGA.H` (che è la verità per i tipi).

## Convenzioni generali

- Tutti i file includono solo `translate.h` (+ `<string.h>` dove serve): gli header
  dell'engine non sono includibili puliti con GCC (`__far`, `cdecl` Watcom), quindi
  i globals sono ridichiarati localmente in `translate.h` con gli stessi tipi di
  `LIB_SVGA.H` (`WORD ClipXmin`, `ULONG TabOffLine`, `UBYTE *Log`, `WORD Screen_X`…).
  Quando gli header engine saranno sistemati, basterà sostituire l'include.
- `TabOffLine` è dichiarata scalare nell'header engine; vi si accede come array via
  `TABOFFLINE` (`(ULONG*)&TabOffLine`), stesso stile di `engine/game/CPYMASK.C`.
- Stride: molte routine originali usano `Screen_X`, altre hardcodano **640**
  (S_LINE, ZOOM, S_FILLV, S_STRING rewind, path clippati di GRAPH_A/MASK_A).
  Mantenuto fedelmente: finché `Screen_X == 640` è indifferente, ma NON sostituire
  640 con Screen_X senza rifare il confronto pixel-perfect.
- Contatori a 8 bit (`dec bl/bh`) resi con `UBYTE` + do/while: un DY o NbBlock pari
  a 0 itera 256 volte, come su x86.
- Accessi word/dword su indirizzi potenzialmente dispari (rep stosw/movsd, `mov [edi], ax`)
  resi con operazioni byte/`memset`/`memcpy`: nessun accesso disallineato per ARM9.
  Unica assunzione: i bank grafici (`((ULONG*)bank)[num]`) sono allineati a 4
  (stessa assunzione della traduzione community CPYMASK.C).
- Parità di indirizzo x86 (`test edi,1` in Copper/Bopper/Trame): resa come parità
  della x di schermo, assumendo `Log` allineato pari (su DOS era allineato; su DS lo sarà).

## Moduli

### s_plot.c (S_PLOT.ASM) — confidenza ALTA
Plot/GetPlot con clipping inclusivo. GetPlot ritorna 0 fuori clip. Nessun dubbio.

### s_box.c (S_BOX.ASM) — confidenza ALTA
Rettangolo pieno clippato, riempimento per riga (memset ≡ stosd/stosb). Stride `Screen_X`.

### s_block3.c (S_BLOCK3.ASM) — confidenza ALTA
`CopyBlockIncrust`: copia rettangolo con colore 0 trasparente (test sul **sorgente**).
I commenti del sorgente ASM ("BX Delta Y") sono invertiti; seguito il codice, non i commenti.

### s_block2.c (S_BLOCK2.ASM) — confidenza ALTA
`CopyBlockOnBlack`: l'intero balletto scasb/movsd equivale per-pixel a
`if (dst==0) dst=src`; verificata l'equivalenza dei run (nessun caso in cui la
scansione salti o riscriva un byte). Contatori CptPixl/CptLine a 16 bit irrilevanti
(dimensioni schermo < 64K).

### graphmsk.c (GRAPHMSK.ASM) — confidenza ALTA
`CalcGraphMsk`: converte brick RLE (00=salto/01=copy/10=repeat, count+1) nel formato
mask (contatori alternati skip/draw, la riga inizia sempre con uno skip, 0 inserito
se serve). NB: opcode `11xxxxxx` trattato come repeat (bit7 testato per primo), come
nell'ASM (il commento di formato direbbe altro). Accumulatore NbData a 8 bit (wrappa
oltre 255, fedele). Header DX/DY/HotX/HotY copiato byte a byte.

### zoom.c (ZOOM.ASM) — confidenza ALTA
`ScaleLine`/`ScaleBox`: DDA 16.16 con accumulatore frazionario 16 bit + carry
(`add bx,dx / adc esi,ebp`) riprodotto esatto. Fedeltà mantenuta su:
- ScaleLine NON somma xs0/xe0 ai puntatori (usa solo i delta) e a differenza di
  ScaleBox non fa +1 sul delta sorgente;
- ScaleBox avanza le righe sorgente con `TabOffLine[n]` come offset relativo
  (assume layout lineare 640);
- divisione per (xd1-xd0)==0 → crash come l'originale (nessuna guardia aggiunta).
ScaleSprite 16-bit nel file originale è dead code dopo `End`: non tradotto.

### s_line.c (S_LINE.ASM) — confidenza ALTA (dettagli MEDIA)
`Line`/`Line_A` (Line_A esportata: la chiama P_OB_ISO). Clipping Cohen-Sutherland
iterativo + Bresenham. Le intersezioni usano `imul si/idiv di` a 16 bit con
`movsx` del quoziente: riprodotto con cast a `WORD` (identico finché i delta stanno
in 16 bit — sempre vero a schermo). Il ramo verticale usa `adc edi,esi` dopo il
riporto: dimostrato che il carry è sempre 1 in quel punto (`err+2dy-2dx > 0`),
quindi `pDest += stride+1`. Stride hardcoded ±640. Da verificare pixel-perfect:
linee esattamente diagonali e i casi di clip multiplo (l'ordine dei rami c0..c4 è
stato mantenuto identico).

### s_block.c (S_BLOCK.ASM) — confidenza ALTA
`CopyBlock`/`SaveBlock`/`RestoreBlock`: copie rettangolari; l'unrolling 2-righe
dell'ASM è pura ottimizzazione (memcpy per riga è byte-identico). NB: negli ultimi
due parametri di Save/RestoreBlock l'header dice dx/dy ma il codice li usa come
x1/y1 (`width = dx - x + 1`): mantenuto il comportamento del codice.

### mask_a.c (MASK_A.ASM) — confidenza ALTA (path clippato MEDIA)
`CoulMask`/`AffMask`/`GetDxDyMask`. Formato mask: contatori alternati skip/draw.
Path non clippato: scrive `ColMask` direttamente. Path clippato: espande la riga in
`BufferClip[512]` (0=skip, ColMask=draw) e blitta saltando gli zeri → **quirk
fedele**: con `ColMask==0` il path clippato non disegna nulla mentre quello non
clippato scrive zeri. Stride 640 hardcoded nel path clippato. Bookkeeping di fine
riga (EndBlock `inc esi` / `dec esi;inc esi`) verificato istruzione per istruzione.
AffMask_Asm non esportata (nessun chiamante esterno; FONT usa AffMask C).

### graph_a.c (GRAPH_A.ASM) — confidenza ALTA (path clippato MEDIA)
`AffGraph`/`GetDxDyGraph`. RLE brick come graphmsk. Quirk fedeli:
- non clippato: i blocchi Copy/Repeat scrivono anche pixel di colore 0;
  clippato: il colore 0 espanso in BufferClip diventa trasparente al blit;
- skip delle righe sopra ClipYmin: repeat consuma 1 byte dato, copy `count+1`;
- contatore righe a 8 bit (`inc al` / `dec bh`).
Da verificare pixel-perfect: brick che intersecano il bordo sinistro/destro
(OffsetBegin/NbPix) e brick con blocchi contenenti colore 0.

### s_string.c (S_STRING.ASM) — confidenza ALTA
`AffString`/`CoulText`. Font 8x8: i dati `db` dell'ASM esistono già in C in
`engine/LIB_SVGA/FONT8X8.C` (spot-check ok sulle prime righe) → usati via extern,
non duplicati. `Text_Paper == 0xFF` = sfondo trasparente. Avanzamento riga con
`Screen_X` ma rewind cella con `(640*8)-8` hardcoded, come l'ASM. Nessun clipping.
La variante Font6X6 (AffString1) è in `comment #`: dead code, non tradotta.

### s_fillv.c (S_FILLV.ASM) — confidenza MEDIA/ALTA
`FillVertic`/`FillVertic_A` (esportata: la chiama P_OB_ISO)/`SetFillDetails` +
9 filler. Punti chiave:
- **TabVerticD/TabCoulD**: l'ASM li indirizza come `TabVerticG+960`/`TabCoulG+960`;
  qui array separati (ognuno ≥480 WORD). Chi riempie le tabelle (S_POLY futuro)
  dovrà usare le stesse coppie di array.
- Indice tabella salti: 0 Triste, 1 Tele, 2 Copper, 3 Bopper, 4 Marbre, 5 Trans,
  6 Trame, 7 Gouraud, 8 Dith (le `POLY_*` equ di svga.ash NON corrispondono: fa
  fede la tabella dd). SetFillDetails clampa unsigned a 2 e scambia la tabella.
- Aritmetica 8.8 a 16 bit riprodotta bit-exact (`UWORD` + carry esplicito):
  Marbre usa lo step byte-swappato con carry sfalsato (`adc ax,dx`), Dith usa
  `rol dl,cl` col contatore corrente come rumore di dithering (rotazione mod 8),
  Tele accumula `ax` seminato con xG e `bx=17371` evoluto `rol 2/inc` sull'intero
  poligono.
- Quirk fedeli: Copper NON scrive gli ultimi (len&1 ? 1 : 0)+((len&3)==3 ? 1 : 0)
  byte della campata (`and cl,2` invece di `and cl,3`); in modalità "up"
  Copper/Bopper decrementano il colore anche su righe vuote; Triche/Gouraud/Dith
  NON avanzano il puntatore intensità sulle righe vuote (desync voluto/fedele);
  Gouraud scarta il resto della divisione; il pixel singolo vale la media (l0),
  in Marbre vale il colore END.
- `sar ax,1` (Dith opt) reso con `>>` su int negativo: ok con GCC/devkitARM
  (shift aritmetico), non-C-strict. Annotato.
Da verificare pixel-perfect in priorità: Dith (ordine rol/add), Gouraud rami
opt/iopt (2-3 pixel), Copper up/down ai bordi dei multipli di 16.

### p_trigo.c (LIB_3D/P_TRIGO.ASM) — confidenza ALTA
Trigonometria fixed point (P_SinTab 1.15, matrici >>14 — i commenti ASM dicono
">>15" ma il codice fa `sar 14`). Nessuna tabella dati da estrarre: P_SinTab è
già in `engine/LIB_3D/P_SINTAB.C`. Dati fedeli al layout ASM: XCentre/YCentre
sono **LONG** (dd) anche se LIB_3D.H li dichiara WORD (little-endian: la
lettura del low word funziona; nessun C dell'engine li tocca direttamente).
- Le entry register-based usate da P_OB_ISO sono esposte in `lib3d_p.h`:
  `Rot/WorldRot(WORD,WORD,WORD)`, `LongWorldRot/LongInverseRot(LONG…)` (64 bit
  come imul/adc/shrd), `RotMatIndex2(src,dst)` (con LMatriceDummy intermedia e
  gli stessi alias/copy del flusso alpha→gamma→beta), `Proj_3D` (tutto a 16
  bit, inclusa la saturazione bp=32767 e l'idiv 32/16 senza guardia div-by-0),
  `Proj_ISO` (input 32 bit, `neg bx/add bx,[YCentre]` finali a 16 bit),
  `RotList/TransRotList` (add di X0/Y0/Z0 a 16 bit, contatore `compteur` WORD:
  0 → 65536 iterazioni, fedele).
- `ProjettePoint`: il test di Z-clip 3D è `or cx,cx` (16 bit) — mantenuto.
- `LongProjettePoint`: saturazione `shl/mov/adc` → 0x7FFF/0x8000 riprodotta.
- `SetInverseAngleCamera`: FlipMatrice(World→Dummy) + Copy(Dummy→World)
  (ordine push Watcom right-to-left verificato).
Da verificare pixel-perfect: niente di specifico; unico rischio i wrap a 32
bit dei prodotti matrice (fatti via unsigned MUL32/ADD32).

### s_poly.c (LIB_SVGA/S_POLY.ASM) — confidenza MEDIA/ALTA
`ComputePoly/_A`, `ComputeSphere/_A` + clip S-H. Punti chiave:
- **TabPoly e TabPolyClip contigui** in un solo array (97+96 WORD): il closing
  point (`movsw/movsd` "transitivité") di un poly a 32 punti sborda di 2 word
  in TabPolyClip esattamente come su DOS.
- I 4 ClipGauche/Droit/Haut/Bas sono un'unica `ClipPolyEdge(primIdx, bound,
  outsideIsGreater)` — sono identici a meno di asse/bound/verso nell'ASM.
  Normalizzazione del verso dell'edge prima dell'idiv ("clip 2 poly collés")
  mantenuta; interpolazione colore solo se `TypePoly >= 7` (POLY_GOURAUD di
  svga.ash: qui gli indici COINCIDONO col confronto ASM, che è sul TypePoly
  già tradotto da P_OB_ISO).
- Edge DDA (EdgeGauche/EdgeDroite): riprodotti bit-exact i due loop
  add/adc (sinistra) e sub/sbb (destra) inclusi l'"init carry"
  (add/rcl/sub/shr), il seme frazionario `rem/2 + 7FFFh` (destra:
  `-(rem/2) + 7FFFh`), l'entrata nel loop in base alla parità di deltaY
  (deltaY+1 entry scritte), la direzione di store da DF (std sui rami
  swappati). Intensità 8.8 con seme `rem_low_byte/2 ± 7Fh`.
- Xmin/Xmax globali NON ricalcolati dopo il clip (solo Ymin/Ymax via
  "rencadre"); edge orizzontali non scrivono nulla (poly piatti → FillVertic
  legge tabelle stantie, quirk DOS fedele).
- Sphere: midpoint a doppia coppia di righe, carry di `add ebp,edx` = il
  passaggio a somma ≥ 0; path clippato con Ymin/Ymax che si restringono a
  runtime e confronti di riga a 16 bit. `Ymin>=Ymax` → niente sfera.
Da verificare pixel-perfect in priorità: EdgeDroite (catena sbb), i semi
frazionari sulle righe swappate, ClipPolyEdge gouraud (resti idiv scartati).

### p_ob_iso.c (LIB_3D/P_OB_ISO.ASM) — confidenza MEDIA/ALTA
`AffObjetIso`/`PatchObjet`. Flusso e buffer documentati in testa al file
(riassunto per il tuning DS in fondo a queste note). Fedeltà:
- record List_Entity gestiti SOLO a WORD (i vertex block sono allineati a 2,
  non a 4: niente accessi dword disallineati su ARM); dati oggetto letti con
  helper memcpy a 16 bit.
- Poly: `sub cl,7` (flat→Triste/Tele) e `sub cl,2` (gouraud→7/8) sul SOLO
  byte materia; colore gouraud per-vertice `add dl,ch` (byte basso della
  intensità + coul1); backface cull col cross product 16×16→32 e confronto
  sub/sbb esatto (64 bit in C); ZMax = max degli Zrot dei vertici.
- Linee: colore **byte-swappato** (`xchg al,ah`) prima di Line_A; Z = max dei
  due punti.
- Sfere: coul letto con `mov ax,[esi+1]` (offset dispari! = byte1|byte2<<8,
  ricomposto dai due WORD); raggio ISO `*34>>9`, 3D `imul/idiv` a 16 bit;
  la box Screen si aggiorna anche per sfere poi respinte (fedele).
- Sort "SergeSort" trasliterato 1:1 (quicksort con stack esplicito +
  selection sort ≤8 + caso 2 elementi): l'ordine dei pari-Z è parte del
  risultato visivo. Record = struct {WORD z; WORD type; WORD *ptr;} (8 byte
  su ILP32 come su x86/ARM32).
- Proiezione ISO: Xp=((x+zrot)*24>>9)+XCentre, Yp=((12(x-zrot)-30y)>>9)+YC,
  Zsort=(zrot-x)-y su 16 bit; 3D: imul/idiv 64/32 con saturazioni
  overX/overY/overZ (`shr 16 | 7FFF`) e overflow ebp≤0 → 0x7FFFFFFF.
- **RotateNuage (oggetti statici, non-ANIM)**: l'ASM passa a Proj_ISO
  registri con metà alte "sporche" (eax: upper di Y0, ebx: upper di Z0,
  ebp: upper di x*LMat20 — il sorgente stesso dice "passer les param en
  long !!!"). Riprodotto ESATTAMENTE ricomponendo i 32 bit. In LBA1 quasi
  tutti i body sono INFO_ANIM, quindi il path è raro.
- List_Normal resta 500 WORD ("surement plus" nel sorgente): modelli con
  più normali sfonderebbero anche su DOS.
Da verificare pixel-perfect: ordine di sort con Z uguali (già 1:1 ma è il
punto più sensibile), il path statico, le sfere clippate.

### texture.c (TEXTURE.ASM) — confidenza ALTA (seed DDA MEDIA/ALTA)
Rasterizzatore di triangoli texturati **affine** usato solo dall'holomap
(HOLOMAP.C: `AsmTexturedTriangleNoClip()` + `FillTextPolyNoClip(LYmin,LYmax,
PtrMap)`). Tutto ciò che nell'ASM segue la direttiva `END` (M_FillTextPoly,
M_AsmFillProp, disptexture, un secondo AsmTexturedTriangleNoClip) è dead code
non tradotto; AsmFillProp/FillTextPoly/FillTextPolyShade/
AsmGouraudTriangleNoClip sono vivi ma senza chiamanti C: tradotti fedeli per
completezza. Punti chiave:
- **Tabelle edge**: nell'ASM sono un blocco contiguo indirizzato da
  `TabGauche + 960*n`; qui si mappano sugli array di s_poly.c con l'aliasing
  del layout originale: TabGauche=TabVerticG, TabDroite=TabVerticD,
  TabX0=TabCoulG, TabY0=TabCoulD, più TabX1/TabY1 (già previsti in lib3d_p.h).
- **Edge DDA (A_FillPropNoClip)**: quoziente 16.16 `(delta16<<16)/deltay`
  ruotato (`rol edx,16`), un solo `adc/sbb eax,edx` a 32 bit per entry: la
  frazione vive nella high word e il suo overflow raggiunge la X solo via CF
  all'iterazione successiva (e un wrap della X carica la frazione) — catena
  riprodotta esatta con `unsigned long long`. Seme frazionario
  `rem/2 + 7FFFh` nelle entry NoClip, **rem nudo** in AsmFillProp (shr/add
  commentati nell'ASM). deltay+1 entry scritte; deltay==0 nei FillProp =
  divisione per zero come su DOS (i triangle builder chiamano solo con Y
  strettamente diversi).
- **Triangle builder**: coordinate lette con **movzx** (X negative
  diventerebbero 32000+); LYmin/LYmax aggiornati con <=/>= e solo dagli edge
  non orizzontali → triangolo tutto orizzontale lascia 32000/-32000
  (TestVuePoly in HOLOMAP.C rigetta prima). Non chiama il filler: è
  HOLOMAP.C a concatenare FillTextPolyNoClip.
- **Filler**: per riga `xstep=(u1-u0+1)/len`, `ystep=(v1-v0+1)/len` (+1 su
  entrambi i numeratori, idiv troncato); accumulatori u/v **8.8 a 16 bit**
  con step troncati alla low word; texel = `map[(v&FF00h)|(u>>8)]` → wrap
  256x256 gratuito. Quirk: NoClip disegna `xd-xg` pixel, le varianti clippate
  `min(xd,ClipXmax)-max(xg,ClipXmin)+1` (un pixel in PIÙ a parità di span);
  il clip sinistro riscrive TabX0/TabY0 (TabCoulG/D) **in place** con
  prodotti imul a 16 bit; contatori riga/pixel a 16 bit (0 → 65536 iter);
  stride 640 hardcoded. FillTextPolyShade: nibble basso −dark con clamp alla
  base della ramp (palette a 16 rampe di 16), bit alti preservati.
- AsmFillProp confronta i bound con load a 32 bit (su DOS ClipYmin/max sono
  dd): qui i WORD del port sono allargati — identico per i valori 0..479.
Verificato: build SDL pulita, holomap in-game col pianeta Twinsun texturato
(continenti/oceani riconoscibili, wrapping corretto ai poli, rotazione con
frecce). Da verificare pixel-perfect in priorità: seme frazionario del DDA
sugli edge destri (sbb) e le righe con len==1.

## Note per il porting DS (P_OB_ISO)

Dati caldi (candidati DTCM): List_Anim_Point/List_Point (3KB ciascuno),
List_Normal (1KB), TabMat (1.1KB), List_Tri (4KB), TabVerticG/D+TabCoulG/D
(3.8KB); List_Entity (10KB) probabilmente troppo grande → main RAM.
Codice caldo (candidati ITCM): RotList/TransRotList, i due loop di
proiezione, EdgeGauche/EdgeDroite, i filler di s_fillv.c, SergeSort.

## Verifica effettuata

`gcc -fsyntax-only -std=c99 -Wall -Wextra -fsigned-char` e `-std=c90 -pedantic`:
tutti i 12 file puliti (mingw32). Nessun test di esecuzione ancora: serve il
harness di confronto pixel-perfect (framebuffer 640x480 + TabOffLine lineare).

Sessione P_TRIGO/S_POLY/P_OB_ISO: build mingw32 pulita, smoke test in-game
(prima scena LBA1): Twinsen e gli altri attori (dottori, clone che spazza,
robot) renderizzati correttamente — proporzioni, gouraud, linee (cavo del
microfono), ombre. Non ancora confrontati pixel-perfect col DOS.

## Pattern utili per S_POLY / TEXTURE / P_OB_ISO (sessioni future)

(Sezione storica: TEXTURE è stato tradotto — campagna completa 16/16.)

- Convenzione: i `proc` con parametri stack (`.model SYSCALL`) mappano 1:1 sulla
  dichiarazione C in LIB_SVGA.H; gli entry "register-based" (`Line_A`: eax,ebx,
  ecx,edx,ebp; `FillVertic_A`: ecx=tipo, edi=colore) sono stati esposti come
  normali funzioni C con quei parametri in ordine: P_OB_ISO li chiamerà così.
- Globals ricorrenti: `Log`, `TabOffLine` (array via `&`), `Screen_X`,
  `ClipXmin/Ymin/Xmax/Ymax` (clip inclusivo), tabelle poly
  `Ymin/Ymax/TabVerticG/D/TabCoulG/D` (16 bit, intensità 8.8).
- 640 hardcoded ovunque nel path poly/line/zoom; `Screen_X` nei blitter rect.
- I contatori loop sono spesso a 8/16 bit: attenzione ai wrap (fedeltà!).
- `rep stosw` con `jnc/stosb` = riempimento a coppie + byte dispari (nessun
  effetto di allineamento reale); `test edi,1` iniziale invece dipende dalla
  parità dell'indirizzo → parità di x se Log è pari.
