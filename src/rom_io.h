#ifndef IZ_ROM_IO_H
#define IZ_ROM_IO_H

#include <switch.h>

Result iz_read_rom_file(const FsFile *, s64 offset, void *, size_t size, u64 *read);

#endif
