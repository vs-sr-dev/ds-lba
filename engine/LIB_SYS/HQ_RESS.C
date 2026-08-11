#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

#include "ADELINE.H"
#include "LIB_SYS.H"
#include "LIB_SAMP/LIB_WAVE.H"

#define RECOVER_AREA 500

/*══════════════════════════════════════════════════════════════════════════*
	█   █ █▀▀▀█       █▀▀▀█ █▀▀▀▀ ██▀▀▀ ██▀▀▀ █▀▀▀█ █   █ █▀▀▀█ █▀▀▀▀ █▀▀▀▀
	██▀▀█ ██ ▄█       ██▀█▀ ██▀▀  ▀▀▀▀█ ▀▀▀▀█ ██  █ ██  █ ██▀█▀ ██    ██▀▀
	▀▀  ▀ ▀▀▀▀  ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

WORD HQR_Flag = FALSE;

#pragma pack(push, 1) /* PORT: on-disk struct, Watcom packed to 10 bytes; GCC would pad to 12 */
typedef struct
{
	ULONG SizeFile;
	ULONG CompressedSizeFile;
	WORD CompressMethod; /* 0 stored */
						 /* 1 LZS */
} T_HEADER;
#pragma pack(pop)

void Expand(void *ptrsourcecomp, void *ptrblocdest, ULONG sizefile);
void PORT_Diag(const char *fmt, ...); /* PORT: nds_sys.c, tees to the SD log */

/*══════════════════════════════════════════════════════════════════════════*
		█   █ █▀▀▀█ █▀▀▀█       █     █▀▀▀█ █▀▀▀█ █▀▀▀▄
		██▀▀█ ██ ▄█ ██▀█▀       ██    ██  █ ██▀▀█ ██  █
		▀▀  ▀ ▀▀▀▀  ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

void *LoadMalloc_HQR(UBYTE *name, UWORD index)
{
	FILE *handle;
	UWORD nbbloc;
	ULONG buffer;
	ULONG seekindex;
	UBYTE *ptrbloc;
	UBYTE *ptrdecomp;
	T_HEADER header;

	handle = OpenRead(name);
	if (!handle)
		return 0L;

	Read(handle, &buffer, 4L);
	nbbloc = (UWORD)(buffer / 4L);

	if (index >= nbbloc)
	{
		Close(handle);
		return 0L;
	}

	Seek(handle, index * 4L, SEEK_START);
	Read(handle, &seekindex, 4L);

	Seek(handle, seekindex, SEEK_START);
	Read(handle, &header, sizeof(header));

	ptrbloc = Malloc(header.SizeFile + RECOVER_AREA);
	if (!ptrbloc)
	{
		Close(handle);
		return 0L;
	}

	switch (header.CompressMethod)
	{
	case 0: /* Stored */
		Read(handle, ptrbloc, header.SizeFile);
		break;

	case 1: /* LZS */
		ptrdecomp = ptrbloc + header.SizeFile - header.CompressedSizeFile + RECOVER_AREA;
		Read(handle, ptrdecomp, header.CompressedSizeFile);
		Expand(ptrdecomp, ptrbloc, header.SizeFile);
		break;

	default:
		Free(ptrbloc);
		Close(handle);
		return 0L; /* UnKnown version */
	}

	Mshrink(ptrbloc, header.SizeFile);
	Close(handle);

	return ptrbloc;
}

/*──────────────────────────────────────────────────────────────────────────*/

ULONG Load_HQR(UBYTE *name, void *ptrdest, UWORD index)
{
	FILE *handle;
	UWORD nbbloc;
	ULONG buffer;
	ULONG seekindex;
	UBYTE *ptrdecomp;
	T_HEADER header;

	handle = OpenRead(name);
	if (!handle)
		return 0L;

	Read(handle, &buffer, 4L);
	nbbloc = (UWORD)(buffer / 4L);

	if (index >= nbbloc)
	{
		Close(handle);
		return 0L;
	}

	Seek(handle, index * 4L, SEEK_START);
	Read(handle, &seekindex, 4L);

	Seek(handle, seekindex, SEEK_START);
	Read(handle, &header, sizeof(header));

	switch (header.CompressMethod)
	{
	case 0: /* Stored */
		Read(handle, ptrdest, header.SizeFile);
		break;

	case 1: /* LZS */
		ptrdecomp = (UBYTE *)ptrdest + header.SizeFile - header.CompressedSizeFile + RECOVER_AREA;
		Read(handle, ptrdecomp, header.CompressedSizeFile);
		Expand(ptrdecomp, ptrdest, header.SizeFile);
		break;

	default:
		Close(handle);
		return 0L; /* UnKnown version */
	}

	Close(handle);

	return header.SizeFile;
}

/*──────────────────────────────────────────────────────────────────────────*/

ULONG Size_HQR(char *name, UWORD index)
{
	FILE *handle;
	UWORD nbbloc;
	ULONG buffer;
	ULONG seekindex;
	UBYTE *ptrdecomp;
	T_HEADER header;

	handle = OpenRead(name);
	if (!handle)
		return 0;

	Read(handle, &buffer, 4);
	nbbloc = (UWORD)(buffer / 4);

	if (index >= nbbloc)
	{
		Close(handle);
		return 0;
	}

	Seek(handle, index * 4, SEEK_START);
	Read(handle, &seekindex, 4);

	if (seekindex)
	{
		Seek(handle, seekindex, SEEK_START);
		Read(handle, &header, sizeof(T_HEADER));

		Close(handle);

		return header.SizeFile;
	}
	else
	{
		Close(handle);
		return 0;
	}
}

/*══════════════════════════════════════════════════════════════════════════*
	  █   █ █▀▀▀█ █▀▀▀█       █▄ ▄█ █▀▀▀█ █     █     █▀▀▀█ █▀▀▀▀
	  ██▀▀█ ██ ▄█ ██▀█▀       ██▀ █ ██▀▀█ ██    ██    ██  █ ██
	  ▀▀  ▀ ▀▀▀▀  ▀▀  ▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

T_HQR_HEADER *HQR_Init_Ressource(char *hqrname,
								 ULONG maxsize,
								 UWORD maxindex)
{
	T_HQR_HEADER *header;
	void *buffer;

	if (!FileSize(hqrname))
		return 0; // fichier ok ?

	header = Malloc(sizeof(T_HQR_HEADER) +
					sizeof(T_HQR_BLOC) * maxindex);

	if (!header)
		return 0; // mem ok ?

	buffer = Malloc(maxsize + RECOVER_AREA);
	if (!buffer)
		return 0; // mem ok ?

	strcpy(header->Name, hqrname);
	header->MaxSize =
		header->FreeSize = maxsize;
	header->MaxIndex = maxindex;
	header->NbIndex = 0;
	header->Buffer = buffer;

	return header; // header
}

/*──────────────────────────────────────────────────────────────────────────*/

void HQR_Reset_Ressource(T_HQR_HEADER *header)
{
	header->FreeSize = header->MaxSize;
	header->NbIndex = 0;
}

/*──────────────────────────────────────────────────────────────────────────*/

LONG HQR_Change_Ressource(T_HQR_HEADER *header, char *newhqrname)
{
	if (!FileSize(newhqrname))
		return FALSE; // fichier ok ?

	strcpy(header->Name, newhqrname);

	header->FreeSize = header->MaxSize;
	header->NbIndex = 0;

	return TRUE;
}

/*──────────────────────────────────────────────────────────────────────────*/

void HQR_Free_Ressource(T_HQR_HEADER *header)
{
	if (header)
	{
		Free(header->Buffer);
		Free(header);
	}
}

/*──────────────────────────────────────────────────────────────────────────*/

UWORD HQR_Del_Bloc(T_HQR_HEADER *header, WORD index)
{
	UWORD n;
	T_HQR_BLOC *ptrbloc;
	ULONG delsize;
	UBYTE *ptr, *ptrs, *ptrd;

	ptr = (UBYTE *)(header) + sizeof(T_HQR_HEADER);
	ptrbloc = (T_HQR_BLOC *)(ptr);
	delsize = ptrbloc[index].Size;

	if (index < (header->NbIndex - 1))
	{
		// shift buffer
		ptrd = header->Buffer + ptrbloc[index].Offset;
		ptrs = ptrd + delsize;
		memmove(ptrd,
				ptrs,
				((header->Buffer + header->MaxSize) - ptrs));

		// shift index table

		ptrd = (UBYTE *)&ptrbloc[index];
		ptrs = ptrd + sizeof(T_HQR_BLOC);
		memmove(ptrd,
				ptrs,
				(header->MaxIndex - (index + 1)) * sizeof(T_HQR_BLOC));

		// shift index value

		for (n = index; n < (header->NbIndex - 1); n++)
		{
			ptrbloc[n].Offset -= delsize;
		}
	}

	header->NbIndex--;
	header->FreeSize += delsize;

	return delsize;
}

/*──────────────────────────────────────────────────────────────────────────*/

/* PORT: a cache can only ever hand back a pointer inside its own buffer.
 * When one does not, the header's own bookkeeping has been corrupted, and
 * the caller dereferences the result immediately — that is where the guru
 * meditations landed (an animation "pointer" of 9). Catch it at the source
 * instead: name the resource, dump the header, and fail the call, which
 * every caller already handles. The check is two compares. */
static void *HQR_Sane(T_HQR_HEADER *header, void *p, WORD index, const char *how)
{
	UBYTE *b = (UBYTE *)p;

	if (b >= header->Buffer && b < header->Buffer + header->MaxSize)
		return p;

	PORT_Diag("[HQR] %s: %s returned %p OUTSIDE buffer for index %d\n",
		   header->Name, how, p, (int)index);
	PORT_Diag("[HQR]   Buffer=%p MaxSize=%lu FreeSize=%lu NbIndex=%u MaxIndex=%u\n",
		   (void *)header->Buffer, (unsigned long)header->MaxSize,
		   (unsigned long)header->FreeSize, (unsigned)header->NbIndex,
		   (unsigned)header->MaxIndex);
	return 0;
}

void *HQR_Get(T_HQR_HEADER *header, WORD index)
{
	UWORD n, oldest;
	ULONG time, testtime;
	UBYTE *ptr;
	T_HQR_BLOC *ptrbloc;
	ULONG size;
	ULONG offset;

	// ressources
	FILE *handle;
	UWORD nbbloc;
	ULONG buffer;
	ULONG seekindex;
	UBYTE *ptrdecomp;
	T_HEADER lzssheader;

	if (index < 0)
		return 0;

	if ((ptrbloc = HQR_GiveIndex(
			 index,
			 header->NbIndex,
			 (UBYTE *)header + sizeof(T_HQR_HEADER))) != 0)
	{ // existing index
		ptrbloc->Time = TimerRef;

		HQR_Flag = FALSE; // marque non chargement ressource

		return HQR_Sane(header, header->Buffer + ptrbloc->Offset, index,
						"cache hit");
	}
	else // need load
	{
		//		SaveTimer() ;
		size = Size_HQR(header->Name, index);

		// load and expand hqr bloc

		handle = OpenRead(header->Name);
		if (!handle)
			return 0;

		Read(handle, &buffer, 4);
		nbbloc = (UWORD)(buffer / 4);

		if (index >= nbbloc)
		{
			Close(handle);
			return 0;
		}

		Seek(handle, index * 4, SEEK_START);
		Read(handle, &seekindex, 4);

		if (!seekindex)
		{
			Close(handle);
			return 0;
		}

		Seek(handle, seekindex, SEEK_START);
		Read(handle, &lzssheader, sizeof(T_HEADER));

		// taille decompacte
		size = lzssheader.SizeFile;

		if (!size)
		{
			Close(handle);
			return 0;
		}

		size = (size + 3) & ~3UL; /* PORT: keep pool entries 4-aligned (ARM reads anim/body data as WORDs) */

		// gestion mémoire

		time = TimerRef;

		ptr = (UBYTE *)(header) + sizeof(T_HQR_HEADER);
		ptrbloc = (T_HQR_BLOC *)(ptr);

		// check if enough space for bloc or index

		while ((size > header->FreeSize) OR(header->NbIndex >= header->MaxIndex))
		{
			/* PORT: nothing left to evict and it still does not fit — this
			 * entry is bigger than the whole pool. The original spun on and
			 * called HQR_Del_Bloc(header, 0) against an EMPTY table: that
			 * read a garbage Size, skipped the shift (index < NbIndex-1 is
			 * false), then did NbIndex-- from 0 -> 65535 and added the
			 * garbage to FreeSize. Every later HQR_Get then computed
			 * `ptr = Buffer + MaxSize - FreeSize` outside the buffer and
			 * handed out wild pointers while scribbling over the heap.
			 * Fail the load cleanly instead; callers already handle 0. */
			if (header->NbIndex == 0)
			{
				Close(handle);
				return 0;
			}

			// delete oldest bloc
			oldest = 0;
			testtime = 0;

			for (n = 0; n < header->NbIndex; n++)
			{
				if ((time - ptrbloc[n].Time) > testtime)
				{
					/* PORT: was ptrbloc[oldest].Time — the running maximum
					 * lagged one candidate behind, so the victim was not
					 * the least recently used and the pool thrashed. */
					testtime = time - ptrbloc[n].Time;
					oldest = n;
				}
			}
			HQR_Del_Bloc(header, oldest);
		}

		// space size ok, load it

		ptr = header->Buffer + header->MaxSize - header->FreeSize;

		//		Load_HQR( header->Name, ptr, index ) ;

		switch (lzssheader.CompressMethod)
		{
		case 0: /* Stored */
			Read(handle, ptr, lzssheader.SizeFile);
			break;

		case 1: /* LZS */
			ptrdecomp = (UBYTE *)ptr + lzssheader.SizeFile - lzssheader.CompressedSizeFile + RECOVER_AREA;
			Read(handle, ptrdecomp, lzssheader.CompressedSizeFile);
			Expand(ptrdecomp, ptr, lzssheader.SizeFile);
			break;

		default:
			Close(handle);
			return 0; /* UnKnown version */
		}

		Close(handle);

		HQR_Flag = TRUE; // indicate loaded

		ptrbloc[header->NbIndex].Index = index;
		ptrbloc[header->NbIndex].Time = TimerRef;
		ptrbloc[header->NbIndex].Offset = header->MaxSize - header->FreeSize;
		ptrbloc[header->NbIndex].Size = size;

		header->NbIndex++;
		header->FreeSize -= size;

		//		RestoreTimer() ;

		return HQR_Sane(header, ptr, index, "fresh load");
	}
}

/*──────────────────────────────────────────────────────────────────────────*/

UWORD HQR_Del_Bloc_Sample(T_HQR_HEADER *header, WORD index)
{
	UWORD n;
	T_HQR_BLOC *ptrbloc;
	ULONG delsize;
	UBYTE *ptr, *ptrs, *ptrd;

	ptr = (UBYTE *)(header) + sizeof(T_HQR_HEADER);
	ptrbloc = (T_HQR_BLOC *)(ptr);
	delsize = ptrbloc[index].Size;

	if (index < (header->NbIndex - 1))
	{
		// shift buffer
		ptrd = header->Buffer + ptrbloc[index].Offset;
		ptrs = ptrd + delsize;
		/*		memmove(ptrd,
					ptrs,
					((header->Buffer+header->MaxSize) - ptrs) ) ;
		*/
		WaveMove(ptrd,
				 ptrs,
				 ((header->Buffer + header->MaxSize) - ptrs));

		// shift index table

		ptrd = (UBYTE *)&ptrbloc[index];
		ptrs = ptrd + sizeof(T_HQR_BLOC);
		memmove(ptrd,
				ptrs,
				(header->MaxIndex - (index + 1)) * sizeof(T_HQR_BLOC));

		// shift index value

		for (n = index; n < (header->NbIndex - 1); n++)
		{
			ptrbloc[n].Offset -= delsize;
		}
	}

	header->NbIndex--;
	header->FreeSize += delsize;

	return delsize;
}

/*──────────────────────────────────────────────────────────────────────────*/

void *HQR_GetSample(T_HQR_HEADER *header, WORD index)
{
	UWORD n, oldest;
	ULONG time, testtime;
	UBYTE *ptr;
	T_HQR_BLOC *ptrbloc;
	ULONG size;
	ULONG offset;

	// ressources
	FILE *handle;
	UWORD nbbloc;
	ULONG buffer;
	ULONG seekindex;
	UBYTE *ptrdecomp;
	T_HEADER lzssheader;

	if (index < 0)
		return 0;

	if ((ptrbloc = HQR_GiveIndex(
			 index,
			 header->NbIndex,
			 (UBYTE *)header + sizeof(T_HQR_HEADER))) != 0)
	{ // existing index
		ptrbloc->Time = TimerRef;

		HQR_Flag = FALSE; // marque non chargement ressource

		return HQR_Sane(header, header->Buffer + ptrbloc->Offset, index,
						"cache hit");
	}
	else // need load
	{
		//		SaveTimer() ;
		size = Size_HQR(header->Name, index);

		// load and expand hqr bloc

		handle = OpenRead(header->Name);
		if (!handle)
			return 0;

		Read(handle, &buffer, 4);
		nbbloc = (UWORD)(buffer / 4);

		if (index >= nbbloc)
		{
			Close(handle);
			return 0;
		}

		Seek(handle, index * 4, SEEK_START);
		Read(handle, &seekindex, 4);

		if (!seekindex)
		{
			Close(handle);
			return 0;
		}

		Seek(handle, seekindex, SEEK_START);
		Read(handle, &lzssheader, sizeof(T_HEADER));

		// taille decompacte
		size = lzssheader.SizeFile;

		if (!size)
		{
			Close(handle);
			return 0;
		}

		size = (size + 3) & ~3UL; /* PORT: keep pool entries 4-aligned (see HQR_Get) */

		// gestion mémoire

		time = TimerRef;

		ptr = (UBYTE *)(header) + sizeof(T_HQR_HEADER);
		ptrbloc = (T_HQR_BLOC *)(ptr);

		// check if enough space for bloc or index

		while ((size > header->FreeSize) OR(header->NbIndex >= header->MaxIndex))
		{
			// delete oldest bloc
			oldest = 0;
			testtime = 0;

			for (n = 0; n < header->NbIndex; n++)
			{
				if ((time - ptrbloc[n].Time) > testtime)
				{
					testtime = time - ptrbloc[oldest].Time;
					oldest = n;
				}
			}
			// méthode violente (attendre réflexions désagréables...)
			WaveStopOne(ptrbloc[oldest].Index);

			HQR_Del_Bloc_Sample(header, oldest);
		}

		// space size ok, load it

		ptr = header->Buffer + header->MaxSize - header->FreeSize;

		//		Load_HQR( header->Name, ptr, index ) ;

		switch (lzssheader.CompressMethod)
		{
		case 0: /* Stored */
			Read(handle, ptr, lzssheader.SizeFile);
			break;

		case 1: /* LZS */
			ptrdecomp = (UBYTE *)ptr + lzssheader.SizeFile - lzssheader.CompressedSizeFile + RECOVER_AREA;
			Read(handle, ptrdecomp, lzssheader.CompressedSizeFile);
			Expand(ptrdecomp, ptr, lzssheader.SizeFile);
			break;

		default:
			Close(handle);
			return 0; /* UnKnown version */
		}

		Close(handle);

		HQR_Flag = TRUE; // indicate loaded

		ptrbloc[header->NbIndex].Index = index;
		ptrbloc[header->NbIndex].Time = TimerRef;
		ptrbloc[header->NbIndex].Offset = header->MaxSize - header->FreeSize;
		ptrbloc[header->NbIndex].Size = size;

		header->NbIndex++;
		header->FreeSize -= size;

		//		RestoreTimer() ;

		return HQR_Sane(header, ptr, index, "fresh load");
	}
}
