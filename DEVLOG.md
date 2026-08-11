# DEVLOG — what a 1994 DOS engine does when you move it to an ARM

This is the interesting half of the port: the bugs.

The premise is worth stating, because it shapes everything below. The source
is good code. It shipped a finished game in 1994, it is careful about memory,
and it was written by people who knew their machine intimately. That last part
is the problem. It knows it is running on a 486 in 32-bit flat mode, where an
unaligned read is free, a store to a misaligned address is exact, `char` is
signed, `sizeof` follows Watcom's packing, and address 0 is memory like any
other.

The DS is a 67 MHz ARM946E-S with 4 MB of RAM. It disagrees with every one of
those assumptions — and, crucially, it disagrees *quietly* with some of them.

Almost every crash in this log was already present in the original. The DS
just refused to keep the secret.

Two things had to happen before any of this could bite. The engine's sixteen
x86 assembly modules — the polygon filler, the 3D model renderer, the
trigonometry tables, the texture mapper — were translated to C (`translate/`);
and the DOS driver layer was replaced wholesale with DS hardware. Everything
below happened afterwards, in code that was already "working".

---

## 1. The unaligned read that does not fault — it rotates

**Symptom:** garbage pixels in decompressed graphics, non-deterministic.

The HQR archives are LZSS-compressed. The decompressor reads its control
tokens with a 16-bit load at whatever offset the stream happens to be at.

On x86 that is legal and slightly slow. On ARMv5 it is legal, fast, and
**wrong**: `LDR` with the low address bits set does not fault and does not
read across the boundary — it reads the aligned word and **rotates** it. You
get real data, in the wrong order, silently.

This is the single nastiest class in the whole port, because there is no
signal. It cost a campaign across `GERELIFE`, `GERETRAK`, `FICHE`, `GRILLE`,
`GRILLE_A`, `P_ANIM` and `DISKFUNC` — every place the engine walks a byte
stream and pulls a word or a long out of it.

> If you port anything from x86, assume every multi-byte read from a parsed
> byte stream is unaligned until proven otherwise. The compiler will not warn
> you and the hardware will not stop you.

## 2. The store that missed by two bits

**Symptom:** freezes with no exception at all, only outside the first area,
only sometimes, mostly near guards and animals.

The engine stores a random-movement timer by punning a 32-bit write over two
adjacent 16-bit fields:

```c
*(ULONG *)(&ptrobj->Info) = value;
```

`offsetof(T_OBJET, Info)` is 2 mod 4. On x86, the write lands exactly where
the programmer meant. On ARM, `STR` to a misaligned address **ignores the low
two bits** — the write lands two bytes earlier, on top of `OffsetLife`, the
actor's script program counter.

So the actor kept running, from a wild offset into its own life script. No
fault, no guru, just a machine executing nonsense — which is why it presented
as a freeze rather than a crash. Every `MOVE_RANDOM` actor was a candidate,
and those start appearing right after the opening area.

The fix is unremarkable. What was worth adding is the static assert on the
offset, so the next person to pun a field there hears about it at compile
time.

## 3. Twelve bytes where DOS had ten

**Symptom:** the magic ball, after a bounce, freezes in mid-air and blinks
until you leave the area.

The homing-ball code overlays a `T_REAL_VALUE` (an interpolator: three words
plus a long) on part of an extra-object struct:

```c
InitRealValue(..., (T_REAL_VALUE *)&ptrextra->OrgX);
```

Under Watcom's packing that overlay is 10 bytes and lands on fields the ball
code does not otherwise use. On ARM the `ULONG` must be 4-aligned, so the
struct is **12** bytes — and the last field falls straight onto `Vz`, which
happened to be where the ball's homing velocity lived. The interpolator's
init zeroed it. Velocity 0, ball stops.

Note the flavour: nothing was corrupted in the "wild pointer" sense. Two
perfectly valid pieces of code disagreed about a struct's size by two bytes.
This one is latent on any modern PC build too.

> Type-punned overlays on packed structs are a portability trap that survives
> every warning flag. If the code does it, measure both layouts.

## 4. `free()` on a string literal

**Symptom:** guru on opening the pause menu. The faulting address, decoded as
ASCII, spelled fragments of a text string.

A text lookup returns the literal `""` when it finds nothing, and the caller
dutifully frees the result. Under msvcrt, `free()` on a `.rodata` pointer was
quietly ignored for years. newlib's allocator instead read the bytes in front
of that literal as a chunk header — hence a guru whose address looked like
text, because it *was* text.

The trigger was a supply-chain detail rather than a coding one: the community
source added text IDs 950/951 for "Load game"/"Save game", which do not exist
in the retail `TEXT.HQR`.

## 5. The memory query that always answered zero

This one produced the most spectacular symptom list of the whole project, and
it is the most instructive.

**Symptoms:** 3D objects that fail to render, animations that never play,
dialogue text advancing one letter per button press, gurus in three unrelated
functions, and a `memmove` fault at the very top of RAM.

The engine sizes its resource caches at startup as fractions of the free
memory:

```c
memory   = Malloc(-1);           /* "how much is left?" */
SpriteMem = memory / 8;
AnimMem   = (memory / 8) * 2;
SampleMem = (memory / 8) * 4;
```

In the community source, `Malloc(-1)` is a stub: `return 0; /* Query memory
not supported */`. So every pool fell to its clamped minimum — 50 KB of
sprites for a 286 KB archive, 100 KB of animations for 431 KB — and every
cache thrashed continuously.

Thrashing alone would only have been slow. The damage came from what eviction
does: `HQR_Del_Bloc` **compacts** the pool with a `memmove`, sliding every
block above the victim downwards. Any pointer a caller was still holding now
points at different data. And callers do hold them — across a whole menu loop,
in the holomap, in the "found an object" sequence.

That is why the symptoms had no common shape. The corruption was not in one
subsystem; it was wherever a pointer happened to be held when a cache made
room. The one-letter-per-keypress text was the same bug: the loop advances the
animation and the text together, so whatever stalls one stalls the other. We
spent a while suspecting the font.

The fix is to measure the archives and size the pools from real numbers
instead of a query that cannot answer. Roughly 650 KB of heap bought total
residency for sprites, animations and inventory objects.

> Two lessons. A *compacting* cache is a completely different risk from a
> reloading one — it can corrupt code that never touches it. And when a
> platform stubs out a query, find every caller that does arithmetic on the
> answer.

## 6. The cache that ate itself

Found while fixing the previous one, in the same file.

```c
while ((size > FreeSize) OR (NbIndex >= MaxIndex))
    HQR_Del_Bloc(header, 0);
```

With an empty table this evicts from nothing: `NbIndex--` wraps 0 to 65535,
`FreeSize` accumulates garbage, and from then on `Buffer + MaxSize - FreeSize`
lands outside the buffer entirely. `HQR_Get` starts returning wild pointers
that the caller writes through.

Two further defects sat next to it: the LRU scan compared `ptrbloc[oldest]`
where it meant `ptrbloc[n]`, so it evicted the wrong block; and an entry
larger than the whole pool spins the loop forever.

Because this family kept producing "impossible" pointers, the port now has
`HQR_Sane()`: every pointer the cache hands out — cache hit or fresh load — is
checked against the buffer bounds, and a violation logs the pool's whole state
instead of returning. A cheap check at the exact boundary where "impossible"
becomes "someone else's memory".

## 7. The accented character

**Symptom:** guru in the Temple of Bù, reproducible, right as the dialogue box
opens.

The text is "the Book of **Bù**". In the game's font that `ù` is byte `0x97`.
The engine is compiled with `-fsigned-char`, which is *required* — the 1994
code assumes signed `char` in a hundred places, and ARM defaults to unsigned.
So `0x97` reached the glyph renderer as **-105**, indexed 420 bytes *below*
the font's offset table, read whatever was there, and used it as a pointer.

The dump made it obvious once you knew what to look for: `r0` was
`FFFFFF97` — the byte, sign-extended, sitting in a register.

Fixed where the character is read, by making the text pointer unsigned, so
every byte from 0x80 up indexes correctly. The consequence is much larger than
one string: **every accented character was doing this**, which means French,
German, Spanish and Italian dialogue was a minefield — several crashes per
conversation. An English-only playthrough met it about once a game.

The glyph renderer now also refuses an out-of-range character and logs it. A
missing letter and a log line beat a guru three frames later.

## 8. The guard that three call sites had

**Symptom:** guru on sustained contact with a mecha penguin, shortly after the
Temple of Bù.

Animations can carry a list of scripted actions — a hit frame, a sound, a
thrown object. Most do not, and the engine stores a null pointer for those.
That is normal, and three of the four places that call the action interpreter
test for it.

The fourth is the "hit an actor that is already reeling" path: rather than
restarting the stagger animation, it replays frame 1's actions in place. Walk
into an enemy and hold the contact, and you hit it again inside the same
stagger — so if that actor's stagger animation has no action list, the null
goes straight through.

On DOS, reading address 0 returned whatever was in low memory and the loop
usually ran zero times. The DS has nothing mapped there.

This one is a good example of the general shape: not a wrong computation, but
an invariant that three quarters of the code knows about and one quarter does
not.

---

## Interlude: reading a guru screen

A DS guru is more informative than it looks. Recovering the exact source line
from a photograph became routine; the technique is worth writing down.

**Keep the `.elf` and `.map` of every build you hand to anyone.** Without them
the `pc` is a number. With them:

```sh
arm-none-eabi-addr2line -f -i -e lba1ds.elf <pc>
```

**Trust `pc` and `addr`; distrust the register file.** The dump is not a
reliable pre-instruction snapshot — registers have been observed holding
values the faulting instruction could not have used. Use them for
corroboration, not deduction.

**`lr` is very often not the return address.** GCC's Thumb prologue for a
function with high-register spills looks like this:

```
push {r4, r5, r6, r7, lr}
mov  r5, r8
mov  r7, sl
mov  r6, r9
mov  lr, fp          <-- lr is now the frame pointer
push {r5, r6, r7, lr}
```

If the fault happens early in the function, the `lr` in the dump is `fp`. The
real return address is in the stack dump, at **`sp+32`**: four words of
high-register spills, then `r4`–`r7`, then the saved `lr`. Feeding that value
to `addr2line` turns "somewhere in this function" into an exact line — that is
how §8 was pinned to its call site from a photograph, with no log at all.

**Corroborate with arithmetic.** In the same crash, `r0` minus the address of
the actor array, divided by `sizeof(T_OBJET)`, gave 5 — the actor the player
was touching. Two independent derivations of the same fact is what makes a
diagnosis safe to act on.

---

## Bugs that were mine, not 1994's

Worth their own section, in the interest of not looking cleverer than the run
of play deserves.

**`Read()` returns an item count, not a byte count.** The wrapper is
`fread(buffer, lenread, 1, handle)` — size and count swapped relative to
instinct — so success is **1**, always. A safety guard I added compared it
against the expected byte count, which meant it fired on every healthy voice
bank: no voices anywhere in the game. The engine's own check has the same
confusion and is harmless only because the function it calls on failure has
its entire body commented out. Which is the real lesson: in this codebase,
never assume a wrapper's return convention.

**`fflush()` does not create a file on libfat.** The log was written with a
flush per line, and did not exist on the card. libfat caches the data sectors
*and the directory entry* and commits on `fclose` — so a file held open is
invisible to the host, and a freeze loses everything. The log now opens,
writes and closes on every line. It costs a card write per line; the lines are
rare and the last one before a hang is worth more than the throughput.

**Two root causes announced and retracted**, both from the same mistake:
reasoning convincingly about code without first checking that it *runs*. One
was a genuine bug in a function reached only from a code path this port never
takes; the other a mechanism that could not exist because the value it
depended on was always zero. Now: before declaring a cause, prove the code
executes.

## Things that looked like bugs and were not

**The playtester with no music.** One tester crashed constantly and heard no
music; the developer, same build, had neither problem. "No music" turned out
not to be a symptom at all — it was the *signature* of an unmounted SD card,
and from that single fact everything followed: no log could exist on his
machine, voice banks fell back to a nearly empty set, and the music request
took a different branch entirely. We had spent a day comparing notes about two
different programs.

His crashes were still real, and still ours to fix — the port is meant to
degrade gracefully without a card. But the useful habit is: find one
observable that separates the two configurations, and check it before
debugging anything else.

**"SELECT doesn't work."** Every keyboard binding in that melonDS
configuration was set to -1. The emulator was taking input from a gamepad
only. Check the input map before suspecting the input code.

**The diagnostic ROMs.** Test builds carrying an input harness were named
exactly like the clean ROM, and a playtester spent an evening reporting the
harness as bugs — behaviour icons flickering, mysterious slowdown. The
shipping target now refuses to build if a harness file is staged, and the
diagnostic ROM has its own name.

## Timing, which is its own country

**The main loop has no frame limiter** — the VGA card was the limiter. Two
lines that do the tick-lock sit in the original source, commented out by the
developers. Restoring them gives the 50 Hz lock the engine expects. Without
it, sprites whose delta-tick rounds to zero fall into a fallback path the
original authors labelled `tanpis machine trop speed` and move ±1 unit per
*frame* — so a hover-bed's speed was proportional to your frame rate.

**The timer must only ever increment.** `RestoreTimer()` rewinds the engine's
tick counter after menus. A platform tick derived from wall-clock time fights
that; the DS tick does nothing but `++`, from a hardware timer at 50.005 Hz,
exactly like the DOS IRQ0 handler.

**And it must be the right hardware timer.** The game tick was initially on
TM2, which belongs to the libnds/calico runtime on the ARM9. It worked, in the
sense that both pieces of software kept running while quietly corrupting each
other's idea of time.

## Comments that lie

The matrix code shifts right by 14. The assembly comments alongside say 15.
They are wrong, they have been wrong since 1994, and believing them produces a
renderer that is subtly, unfixably off. Verify against behaviour, not prose.

---

## What generalises

1. **Prove the code runs before naming it as the cause.** Two retractions came
   from skipping this, and it is the cheapest check available.
2. **Old wrappers do not follow libc conventions.** Read them.
3. **Alignment and struct size are the two silent killers.** Neither warns.
   Static-assert the offsets that matter.
4. **A pointer held across an allocation is a bug waiting for a compacting
   allocator** — and this engine has one.
5. **Ship symbols with every build, and stamp the build into the log.** A
   crash report you cannot tie to a binary costs an evening. (One did: a stamp
   taken from a single file's compile time reported two different ROMs as the
   same build.)
6. **Make the failure loud and early.** Most fixes here added a check at the
   boundary where a value stops making sense — a range check on a glyph, a
   bounds check on a cache pointer — because the alternative is a guru several
   frames later, nowhere near the cause.
