#ifndef IZ_VIRTUAL_ROMFS_H
#define IZ_VIRTUAL_ROMFS_H

#include "nup.h"

typedef struct VirtualRomFs VirtualRomFs;
typedef struct VirtualRomReader VirtualRomReader;
enum { NUP_ADDON_LIMIT = 64 };

typedef struct {
    VirtualRomReader *reader;
    VirtualRomFs *images[1 + NUP_ADDON_LIMIT];
    unsigned addons, indices[NUP_ADDON_LIMIT];
    bool addons_disabled;
} NupRomSet;

bool nup_rom_set_open(NupRomSet *, const Nup *, const NupPackage *const *, size_t,
                      bool addons_enabled, const char **);
void nup_rom_set_close(NupRomSet *);
uint64_t virtual_romfs_size(const VirtualRomFs *);
bool virtual_romfs_read(VirtualRomFs *, uint64_t, void *, size_t);
bool romfs_image_fits(const unsigned char header[80], uint64_t);

#endif
