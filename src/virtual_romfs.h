#ifndef IZ_VIRTUAL_ROMFS_H
#define IZ_VIRTUAL_ROMFS_H

#include "nup.h"

typedef struct VirtualRomFs VirtualRomFs;
VirtualRomFs *virtual_romfs_open(const Nup *, const NupPackage *const *, size_t, const char **);
void virtual_romfs_close(VirtualRomFs *);
uint64_t virtual_romfs_size(const VirtualRomFs *);
bool virtual_romfs_read(VirtualRomFs *, uint64_t, void *, size_t);
bool romfs_image_fits(const unsigned char header[80], uint64_t);

#endif
