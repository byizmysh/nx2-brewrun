#ifndef IZ_GAME_MEMORY_H
#define IZ_GAME_MEMORY_H

#include "nup.h"

typedef struct {
    unsigned heap_mib, apart_mib, slack_mib, remote_mib, system_kib;
} IzGameMemory;

IzGameMemory iz_game_memory(const Nup *, const NupPackage *const *, size_t, const NupPackage *);
IzGameMemory iz_image_game_memory(NupSource, const void *, size_t);

#endif
