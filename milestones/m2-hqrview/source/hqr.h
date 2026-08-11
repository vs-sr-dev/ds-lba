/* HQR resource file reader — portable C adaptation of LIB_SYS/HQ_RESS.C + EXPAND.C
 * (lba1-classic-community, GPL). First portable engine module of the DS port. */
#ifndef HQR_H
#define HQR_H

#include <stdint.h>

/* Number of entries in an HQR archive, or -1 on error. */
int HQR_NumEntries(const char *path);

/* Decompressed size of an entry, 0 on error. */
uint32_t HQR_EntrySize(const char *path, int index);

/* Load and decompress entry into dest (must hold HQR_EntrySize bytes).
 * Returns decompressed size, 0 on error. */
uint32_t HQR_LoadEntry(const char *path, int index, void *dest);

/* Convenience: allocate + load. Caller frees. NULL on error. */
void *HQR_LoadEntryAlloc(const char *path, int index, uint32_t *size_out);

/* LZSS decompressor (port of LIB_SYS/EXPAND.C). */
void HQR_Expand(const uint8_t *src, uint8_t *dest, int32_t count);

#endif
