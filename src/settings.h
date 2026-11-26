#ifndef IZ_SETTINGS_H
#define IZ_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    IZ_ROOM_DEFAULT_MIB = 384,
    IZ_ROOM_MAX_MIB = 2048,
    IZ_ROOM_OVERHEAD_MIB = 168,
    IZ_SPLIT_MAX_MIB = 4000
};

#define IZ_SAVE_SUFFIX "/save"

bool iz_game_data_path(const char *self, uint64_t title, char *, size_t);
bool iz_game_save_path(const char *self, uint64_t title, char *, size_t);
bool iz_save_child_path(const char *root, const char *parent, const char *name, char *, size_t);
unsigned iz_game_room(const char *self, uint64_t title);
bool iz_save_game_room(const char *self, uint64_t title, unsigned mib);
bool iz_game_dlc(const char *self, uint64_t title);
bool iz_save_game_dlc(const char *self, uint64_t title, bool enabled);
unsigned iz_transfer_split(const char *self);
unsigned iz_room_from_args(const char *args);

#endif
