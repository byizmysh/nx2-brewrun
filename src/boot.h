#ifndef IZ_NUP_BOOT_H
#define IZ_NUP_BOOT_H

#include <stdbool.h>
#include <stdint.h>

#include "game_memory.h"

extern char iz_game_name[], iz_home[], iz_dir[], iz_save_dir[];
extern char iz_content_home[512];
extern IzGameMemory iz_memory;
const char *iz_path(const char *);
uint64_t iz_rom_size(void);
bool iz_rom_read(uint64_t, void *, uint64_t);
bool iz_probe_nup(const char *, NupPreview *);
bool iz_read_nup_cover(const char *, uint64_t, void *, size_t);
bool iz_probe_loose(const char *, uint64_t *, uint64_t *);
bool iz_decode_path(const char *, char *, unsigned);
bool iz_queue_game(const char *, const char *, const char **);
void iz_restore_host_heap(void);
int iz_browser(const char *, const char *);

#endif
