/*
 * nds_fastmem.c — ITCM/ARM32 memcpy + memset overriding newlib's.
 *
 * Why: the brick blitter (translate/graph_a.c AffGraph) issues one
 * memcpy/memset per RLE run — hundreds of thousands of TINY (2..20 byte)
 * calls per full grid redraw. newlib's routines live in main RAM (slow
 * fetch) and have a heavy prologue for small n; the profiler showed
 * AffGrille at ~250ms/frame dominated by exactly this. Linking these
 * definitions from an object file wins over the libc archive members —
 * no --wrap needed.
 *
 * Kept deliberately simple and obviously correct:
 *   - n < 16: straight byte loop (the hot brick-run case);
 *   - same 4-alignment: u32 x8 unrolled blocks (CopyScreen 300K, Cls...);
 *   - anything else: byte loop.
 *
 * Compiled with -ffreestanding -fno-builtin (see Makefile rule): stops GCC
 * from recognizing the loops as memcpy/memset patterns and emitting a call
 * to ourselves.
 */
#include <stddef.h>

#define FASTMEM __attribute__((section(".itcm"), long_call, target("arm")))

FASTMEM void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (n >= 16 && ((((unsigned)d ^ (unsigned)s) & 3u) == 0))
	{
		while ((unsigned)d & 3u)
		{
			*d++ = *s++;
			n--;
		}
		{
			unsigned long *dw = (void *)d;
			const unsigned long *sw = (const void *)s;

			while (n >= 32)
			{
				dw[0] = sw[0];
				dw[1] = sw[1];
				dw[2] = sw[2];
				dw[3] = sw[3];
				dw[4] = sw[4];
				dw[5] = sw[5];
				dw[6] = sw[6];
				dw[7] = sw[7];
				dw += 8;
				sw += 8;
				n -= 32;
			}
			while (n >= 4)
			{
				*dw++ = *sw++;
				n -= 4;
			}
			d = (void *)dw;
			s = (const void *)sw;
		}
	}
	while (n--)
		*d++ = *s++;
	return dst;
}

FASTMEM void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;
	unsigned char b = (unsigned char)c;

	if (n >= 16)
	{
		unsigned long w = b;

		w |= w << 8;
		w |= w << 16;
		while ((unsigned)d & 3u)
		{
			*d++ = b;
			n--;
		}
		{
			unsigned long *dw = (void *)d;

			while (n >= 32)
			{
				dw[0] = w;
				dw[1] = w;
				dw[2] = w;
				dw[3] = w;
				dw[4] = w;
				dw[5] = w;
				dw[6] = w;
				dw[7] = w;
				dw += 8;
				n -= 32;
			}
			while (n >= 4)
			{
				*dw++ = w;
				n -= 4;
			}
			d = (void *)dw;
		}
	}
	while (n--)
		*d++ = b;
	return dst;
}
