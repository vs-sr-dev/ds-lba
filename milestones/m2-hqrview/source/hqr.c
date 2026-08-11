/* HQR resource file reader — see hqr.h.
 *
 * On-disk format (little endian, matches T_HEADER in HQ_RESS.C):
 *   u32 offset[0]            -> also gives entry count = offset[0]/4
 *   ...offset[i] per entry
 *   at offset[i]: u32 sizeFile, u32 compressedSizeFile, u16 compressMethod,
 *                 then compressedSizeFile bytes of data.
 * Method 0 = stored, 1 = LZSS.
 * Header fields are read field-by-field: the struct is 10 bytes packed on
 * disk, which GCC would pad to 12. */

#include "hqr.h"
#include <stdio.h>
#include <stdlib.h>

static FILE *hqr_open_entry(const char *path, int index,
                            uint32_t *size, uint32_t *csize, uint16_t *method)
{
	FILE *f = fopen(path, "rb");
	uint32_t first, seekindex;

	if (!f)
		return NULL;

	if (fread(&first, 4, 1, f) != 1 || (uint32_t)index >= first / 4)
		goto fail;

	if (fseek(f, index * 4L, SEEK_SET) != 0 ||
	    fread(&seekindex, 4, 1, f) != 1 ||
	    fseek(f, seekindex, SEEK_SET) != 0)
		goto fail;

	if (fread(size, 4, 1, f) != 1 ||
	    fread(csize, 4, 1, f) != 1 ||
	    fread(method, 2, 1, f) != 1)
		goto fail;

	return f;
fail:
	fclose(f);
	return NULL;
}

int HQR_NumEntries(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint32_t first;
	int n = -1;

	if (!f)
		return -1;
	if (fread(&first, 4, 1, f) == 1)
		n = (int)(first / 4);
	fclose(f);
	return n;
}

uint32_t HQR_EntrySize(const char *path, int index)
{
	uint32_t size, csize;
	uint16_t method;
	FILE *f = hqr_open_entry(path, index, &size, &csize, &method);

	if (!f)
		return 0;
	fclose(f);
	return size;
}

uint32_t HQR_LoadEntry(const char *path, int index, void *dest)
{
	uint32_t size, csize;
	uint16_t method;
	uint8_t *comp;
	FILE *f = hqr_open_entry(path, index, &size, &csize, &method);

	if (!f)
		return 0;

	switch (method) {
	case 0:
		if (fread(dest, 1, size, f) != size)
			size = 0;
		break;
	case 1:
		comp = malloc(csize);
		if (!comp || fread(comp, 1, csize, f) != csize) {
			size = 0;
		} else {
			HQR_Expand(comp, dest, (int32_t)size);
		}
		free(comp);
		break;
	default:
		size = 0;
	}

	fclose(f);
	return size;
}

void *HQR_LoadEntryAlloc(const char *path, int index, uint32_t *size_out)
{
	uint32_t size = HQR_EntrySize(path, index);
	void *buf;

	if (size_out)
		*size_out = 0;
	if (!size)
		return NULL;

	buf = malloc(size);
	if (!buf)
		return NULL;

	if (HQR_LoadEntry(path, index, buf) != size) {
		free(buf);
		return NULL;
	}
	if (size_out)
		*size_out = size;
	return buf;
}

/* LZSS: 8 flag bits per control byte; flag=1 literal, flag=0 back-reference
 * u16 with (dist << 4 | (len-2)), copied byte-by-byte (may self-overlap). */
void HQR_Expand(const uint8_t *src, uint8_t *dest, int32_t count)
{
	while (count > 0) {
		int flags = *src++;
		int bit;

		for (bit = 0; bit < 8; bit++, flags >>= 1) {
			if (flags & 1) {
				*dest++ = *src++;
				if (--count == 0)
					return;
			} else {
				uint16_t token = (uint16_t)(src[0] | (src[1] << 8));
				int32_t len = (token & 0x0F) + 2;
				const uint8_t *back = dest - (token >> 4) - 1;

				src += 2;
				count -= len;
				while (len-- > 0)
					*dest++ = *back++;
				if (count <= 0)
					return;
			}
		}
	}
}
