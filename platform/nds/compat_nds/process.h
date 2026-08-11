/* process.h — devkitARM has no <process.h>; ADELINE.C only uses spawnl()
 * to launch external DOS driver setup EXEs (MidiExec/WaveExec). No such
 * thing on the DS: report failure, the engine just skips the exec. */
#ifndef NDS_COMPAT_PROCESS_H
#define NDS_COMPAT_PROCESS_H

#define P_WAIT 0
#define spawnl(...) (-1)

#endif
