#ifndef IZ_NUP_BOOT_H
#define IZ_NUP_BOOT_H

#include <stdbool.h>
#include <stdint.h>

#include "game_memory.h"
#include "virtual_romfs.h"

extern char iz_game_name[], iz_home[], iz_dir[], iz_save_dir[];
extern char iz_content_home[512];
extern IzGameMemory iz_memory;
extern uint64_t iz_game_language_override;
extern int iz_game_language_index;
const char *iz_path(const char *);
uint64_t iz_rom_size(unsigned image);
bool iz_rom_read(unsigned image, uint64_t offset, void *, uint64_t);
unsigned iz_rom_addon_count(void);
unsigned iz_rom_addon_index(unsigned addon);
bool iz_rom_addons_disabled(void);
bool iz_probe_nup(const char *, NupPreview *);
bool iz_read_nup_cover(const char *, uint64_t, void *, size_t);
bool iz_probe_loose(const char *, uint64_t *, uint64_t *);
bool iz_decode_path(const char *, char *, unsigned);
bool iz_queue_game(const char *, const char *, uint64_t, const char **);
unsigned iz_loader_room_mib(void);
void iz_restore_host_heap(void);
int iz_browser(const char *, const char *);

#endif
