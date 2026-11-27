#ifndef IZ_SETTINGS_H
#define IZ_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    IZ_ROOM_DEFAULT_MIB = 384,
    IZ_ROOM_MAX_MIB = 2048,
    IZ_ROOM_OVERHEAD_MIB = 168,
    IZ_EXEFS_MIN_MIB = 96,
    IZ_EXEFS_MAX_MIB = IZ_ROOM_MAX_MIB - IZ_ROOM_OVERHEAD_MIB,
    IZ_SPLIT_MAX_MIB = 4000
};

#define IZ_SAVE_SUFFIX "/save"

typedef enum {
    IZ_LANGUAGE_SYSTEM,
    IZ_LANGUAGE_JAPANESE,
    IZ_LANGUAGE_ENGLISH_US,
    IZ_LANGUAGE_ENGLISH_UK,
    IZ_LANGUAGE_FRENCH,
    IZ_LANGUAGE_FRENCH_CA,
    IZ_LANGUAGE_GERMAN,
    IZ_LANGUAGE_ITALIAN,
    IZ_LANGUAGE_SPANISH,
    IZ_LANGUAGE_SPANISH_LATAM,
    IZ_LANGUAGE_DUTCH,
    IZ_LANGUAGE_PORTUGUESE,
    IZ_LANGUAGE_PORTUGUESE_BR,
    IZ_LANGUAGE_RUSSIAN,
    IZ_LANGUAGE_KOREAN,
    IZ_LANGUAGE_CHINESE_SIMPLIFIED,
    IZ_LANGUAGE_CHINESE_TRADITIONAL,
    IZ_LANGUAGE_POLISH,
    IZ_LANGUAGE_THAI,
    IZ_LANGUAGE_COUNT
} IzGameLanguage;

bool iz_game_data_path(const char *self, uint64_t title, char *, size_t);
bool iz_game_save_path(const char *self, uint64_t title, char *, size_t);
bool iz_save_child_path(const char *root, const char *parent, const char *name, char *, size_t);
unsigned iz_game_room(const char *self, uint64_t title);
bool iz_save_game_room(const char *self, uint64_t title, unsigned mib);
unsigned iz_game_exefs_budget(const char *self, uint64_t title);
bool iz_save_game_exefs_budget(const char *self, uint64_t title, unsigned mib);
bool iz_game_dlc(const char *self, uint64_t title);
bool iz_save_game_dlc(const char *self, uint64_t title, bool enabled);
bool iz_game_native_nvn(const char *self, uint64_t title);
bool iz_save_game_native_nvn(const char *self, uint64_t title, bool enabled);
IzGameLanguage iz_game_language(const char *self, uint64_t title);
bool iz_save_game_language(const char *self, uint64_t title, IzGameLanguage);
const char *iz_game_language_label(IzGameLanguage);
uint64_t iz_game_language_code(IzGameLanguage);
int iz_game_application_language(IzGameLanguage);
unsigned iz_transfer_split(const char *self);
unsigned iz_room_from_args(const char *args);

#endif
