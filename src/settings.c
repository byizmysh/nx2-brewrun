#include "settings.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool decimal(const char *start, const char *end, unsigned max, unsigned *out) {
    if (start == end) return false;
    unsigned value = 0;
    while (start != end) {
        const unsigned digit = (unsigned)(*start++ - '0');
        if (digit > 9 || value > (max - digit) / 10) return false;
        value = 10 * value + digit;
    }
    *out = value;
    return true;
}

bool iz_sibling_path(const char *self, const char *leaf, char *out, size_t capacity) {
    const char *slash = self ? strrchr(self, '/') : NULL;
    if (!slash || !slash[1]) return false;
    const size_t parent = (size_t)(slash - self) + 1;
    const size_t length = strlen(leaf);
    if (parent >= capacity || length >= capacity - parent) return false;
    memcpy(out, self, parent);
    memcpy(out + parent, leaf, length + 1);
    return true;
}

bool iz_game_data_path(const char *self, uint64_t title, char *out, size_t capacity) {
    char folder[sizeof("nup-0000000000000000")];
    title &= ~UINT64_C(0xfff);
    if (!title) return false;
    snprintf(folder, sizeof(folder), "nup-%016llx", (unsigned long long)title);
    return iz_sibling_path(self, folder, out, capacity);
}

bool iz_game_save_path(const char *self, uint64_t title, char *out, size_t capacity) {
    if (capacity < sizeof(IZ_SAVE_SUFFIX) ||
        !iz_game_data_path(self, title, out, capacity - sizeof(IZ_SAVE_SUFFIX) + 1)) return false;
    strcat(out, IZ_SAVE_SUFFIX);
    return true;
}

static bool save_component(const char *name, size_t length) {
    if (!length || (length == 1 && name[0] == '.') || (length == 2 && !memcmp(name, "..", 2)))
        return false;
    return !memchr(name, '/', length) && !memchr(name, '\\', length) && !memchr(name, ':', length);
}

bool iz_save_child_path(const char *root, const char *parent, const char *name, char *out, size_t capacity) {
    const size_t root_size = strlen(root);
    if (root_size < sizeof("sdmc:" IZ_SAVE_SUFFIX) - 1 || strncmp(root, "sdmc:/", 6) ||
        strcmp(root + root_size - sizeof(IZ_SAVE_SUFFIX) + 1, IZ_SAVE_SUFFIX) ||
        strncmp(parent, root, root_size) || (parent[root_size] && parent[root_size] != '/') ||
        !save_component(name, strlen(name))) return false;
    for (const char *part = parent + 6;;) {
        const char *slash = strchr(part, '/');
        if (!save_component(part, slash ? (size_t)(slash - part) : strlen(part))) return false;
        if (!slash) break;
        part = slash + 1;
    }
    const int length = snprintf(out, capacity, "%s/%s", parent, name);
    return length > 0 && (size_t)length < capacity;
}

enum { CONFIG_LINE = 128 };

static const IzGameSettings default_game_settings = {
    .room = IZ_ROOM_DEFAULT_MIB, .language = IZ_LANGUAGE_SYSTEM, .dlc_enabled = true,
};

static char *next_value(FILE *file, const char *key, char line[CONFIG_LINE]) {
    const size_t key_size = strlen(key);
    while (fgets(line, CONFIG_LINE, file)) {
        char *start = line;
        while (isspace((unsigned char)*start)) ++start;
        if (strncmp(start, key, key_size)) continue;
        start += key_size;
        while (isspace((unsigned char)*start)) ++start;
        if (*start++ != '=') continue;
        while (isspace((unsigned char)*start)) ++start;
        char *end = strchr(start, '#');
        if (!end) end = start + strlen(start);
        while (end > start && isspace((unsigned char)end[-1])) --end;
        *end = 0;
        return start;
    }
    return NULL;
}

static unsigned read_number(FILE *file, const char *key, unsigned min, unsigned max, unsigned fallback) {
    if (!file) return fallback;
    char line[CONFIG_LINE];
    for (char *start; (start = next_value(file, key, line));) {
        unsigned value;
        if (decimal(start, start + strlen(start), max, &value) && value >= min)
            fallback = value;
    }
    fclose(file);
    return fallback;
}

static FILE *open_game_config(const char *self, uint64_t title) {
    char folder[704], path[736];
    if (!iz_game_data_path(self, title, folder, sizeof(folder))) return NULL;
    snprintf(path, sizeof(path), "%s/loader.cfg", folder);
    FILE *file = fopen(path, "r");
    if (file || errno != ENOENT) return file;
    snprintf(path, sizeof(path), "%s/loader.cfg.previous", folder);
    return fopen(path, "r");
}

unsigned iz_game_room(const char *self, uint64_t title) {
    return read_number(open_game_config(self, title), "room", IZ_ROOM_DEFAULT_MIB,
                       IZ_ROOM_MAX_MIB, default_game_settings.room);
}

unsigned iz_game_exefs_budget(const char *self, uint64_t title) {
    FILE *file = open_game_config(self, title);
    if (!file) return default_game_settings.exefs_budget;
    unsigned budget = default_game_settings.exefs_budget;
    char line[CONFIG_LINE];
    for (const char *value; (value = next_value(file, "exefs", line));) {
        unsigned mib;
        if (!strcmp(value, "auto")) budget = 0;
        else if (decimal(value, value + strlen(value), IZ_EXEFS_MAX_MIB, &mib) && mib >= IZ_EXEFS_MIN_MIB)
            budget = mib;
    }
    fclose(file);
    return budget;
}

static bool read_game_toggle(const char *self, uint64_t title, const char *key, bool enabled) {
    FILE *file = open_game_config(self, title);
    if (!file) return enabled;
    char line[CONFIG_LINE];
    for (const char *value; (value = next_value(file, key, line));) {
        if (!strcmp(value, "off")) enabled = false;
        else if (!strcmp(value, "on")) enabled = true;
    }
    fclose(file);
    return enabled;
}

bool iz_game_dlc(const char *self, uint64_t title) {
    return read_game_toggle(self, title, "dlc", default_game_settings.dlc_enabled);
}

bool iz_game_native_nvn(const char *self, uint64_t title) {
    return read_game_toggle(self, title, "nvnnative", default_game_settings.native_nvn);
}

/* The tag is returned by AM and SET. NS uses the separate NACP language index. */
static const struct {
    const char *tag;
    const char *label;
    int application_index;
} game_languages[IZ_LANGUAGE_COUNT] = {
    [IZ_LANGUAGE_SYSTEM] = {"system", "System", -1},
    [IZ_LANGUAGE_JAPANESE] = {"ja", "Japanese", 2},
    [IZ_LANGUAGE_ENGLISH_US] = {"en-US", "English (US)", 0},
    [IZ_LANGUAGE_ENGLISH_UK] = {"en-GB", "English (UK)", 1},
    [IZ_LANGUAGE_FRENCH] = {"fr", "French", 3},
    [IZ_LANGUAGE_FRENCH_CA] = {"fr-CA", "French (Canada)", 9},
    [IZ_LANGUAGE_GERMAN] = {"de", "German", 4},
    [IZ_LANGUAGE_ITALIAN] = {"it", "Italian", 7},
    [IZ_LANGUAGE_SPANISH] = {"es", "Spanish (Spain)", 6},
    [IZ_LANGUAGE_SPANISH_LATAM] = {"es-419", "Spanish (Latin America)", 5},
    [IZ_LANGUAGE_DUTCH] = {"nl", "Dutch", 8},
    [IZ_LANGUAGE_PORTUGUESE] = {"pt", "Portuguese (Portugal)", 10},
    [IZ_LANGUAGE_PORTUGUESE_BR] = {"pt-BR", "Portuguese (Brazil)", 15},
    [IZ_LANGUAGE_RUSSIAN] = {"ru", "Russian", 11},
    [IZ_LANGUAGE_KOREAN] = {"ko", "Korean", 12},
    [IZ_LANGUAGE_CHINESE_SIMPLIFIED] = {"zh-Hans", "Chinese (Simplified)", 14},
    [IZ_LANGUAGE_CHINESE_TRADITIONAL] = {"zh-Hant", "Chinese (Traditional)", 13},
    [IZ_LANGUAGE_POLISH] = {"pl", "Polish", 16},
    [IZ_LANGUAGE_THAI] = {"th", "Thai", 17},
};

IzGameLanguage iz_game_language(const char *self, uint64_t title) {
    FILE *file = open_game_config(self, title);
    if (!file) return default_game_settings.language;
    IzGameLanguage selected = default_game_settings.language;
    char line[CONFIG_LINE];
    for (const char *value; (value = next_value(file, "language", line));) {
        for (unsigned i = 0; i < IZ_LANGUAGE_COUNT; ++i)
            if (!strcmp(value, game_languages[i].tag)) { selected = (IzGameLanguage)i; break; }
    }
    fclose(file);
    return selected;
}

IzGameSettings iz_default_game_settings(void) { return default_game_settings; }

IzGameSettings iz_load_game_settings(const char *self, uint64_t title) {
    return (IzGameSettings){
        .room = iz_game_room(self, title),
        .exefs_budget = iz_game_exefs_budget(self, title),
        .language = iz_game_language(self, title),
        .native_nvn = iz_game_native_nvn(self, title),
        .dlc_enabled = iz_game_dlc(self, title),
    };
}

unsigned iz_room_for_exefs(unsigned room, unsigned budget) {
    const unsigned minimum = budget + IZ_ROOM_OVERHEAD_MIB;
    return room < minimum ? minimum : room;
}

bool iz_save_game_settings(const char *self, uint64_t title, const IzGameSettings *settings) {
    const unsigned room = settings->room, budget = settings->exefs_budget;
    if (room < IZ_ROOM_DEFAULT_MIB || room > IZ_ROOM_MAX_MIB ||
        (budget && (budget < IZ_EXEFS_MIN_MIB || budget > room - IZ_ROOM_OVERHEAD_MIB)) ||
        (unsigned)settings->language >= IZ_LANGUAGE_COUNT) return false;

    char folder[704], path[736], previous[736], temporary[736];
    if (!iz_game_data_path(self, title, folder, sizeof(folder))) return false;
    struct stat status;
    if (mkdir(folder, 0777) && (errno != EEXIST || stat(folder, &status) || !S_ISDIR(status.st_mode))) return false;
    snprintf(path, sizeof(path), "%s/loader.cfg", folder);
    snprintf(previous, sizeof(previous), "%s/loader.cfg.previous", folder);
    snprintf(temporary, sizeof(temporary), "%s/loader.cfg.tmp", folder);
    FILE *existing = fopen(path, "rb");
    const bool active = existing != NULL;
    if (!existing) {
        if (errno != ENOENT) return false;
        existing = fopen(previous, "rb");
        if (!existing && errno != ENOENT) return false;
    }

    FILE *file = fopen(temporary, "wb");
    bool ok = file != NULL;
    if (existing) {
        unsigned char chunk[512];
        size_t length;
        while (ok && (length = fread(chunk, 1, sizeof(chunk), existing)))
            ok = fwrite(chunk, 1, length, file) == length;
        if (ferror(existing)) ok = false;
        if (fclose(existing)) ok = false;
    }
    if (file) {
        char exefs[sizeof("4294967295")] = "auto";
        if (budget) snprintf(exefs, sizeof(exefs), "%u", budget);
        if (ok) ok = fprintf(file, "\nroom=%u\nexefs=%s\nlanguage=%s\nnvnnative=%s\ndlc=%s\n", room, exefs,
                             game_languages[settings->language].tag, settings->native_nvn ? "on" : "off",
                             settings->dlc_enabled ? "on" : "off") >= 0 && !fflush(file) && !fsync(fileno(file));
        if (fclose(file)) ok = false;
    }
    /* FAT does not replace the destination on rename. Readers can recover the old file between renames. */
    if (ok && active) ok = (!unlink(previous) || errno == ENOENT) && !rename(path, previous);
    if (!ok) {
        unlink(temporary);
        return false;
    }
    if (rename(temporary, path)) {
        if (active) rename(previous, path);
        unlink(temporary);
        return false;
    }
    unlink(previous);
    return true;
}

const char *iz_game_language_label(IzGameLanguage language) {
    if ((unsigned)language >= IZ_LANGUAGE_COUNT) language = IZ_LANGUAGE_SYSTEM;
    return game_languages[language].label;
}

uint64_t iz_game_language_code(IzGameLanguage language) {
    if (language == IZ_LANGUAGE_SYSTEM || (unsigned)language >= IZ_LANGUAGE_COUNT) return 0;
    const unsigned char *tag = (const unsigned char *)game_languages[language].tag;
    uint64_t code = 0;
    for (unsigned i = 0; tag[i]; ++i) code |= (uint64_t)tag[i] << (i * 8);
    return code;
}

int iz_game_application_language(IzGameLanguage language) {
    return (unsigned)language < IZ_LANGUAGE_COUNT ? game_languages[language].application_index : -1;
}

unsigned iz_transfer_split(const char *self) {
    char path[704];
    if (!iz_sibling_path(self, "menu.cfg", path, sizeof(path))) return 0;
    return read_number(fopen(path, "r"), "split", 0, IZ_SPLIT_MAX_MIB, 0);
}

unsigned iz_room_from_args(const char *args) {
    if (!args) return IZ_ROOM_DEFAULT_MIB;
    unsigned result = IZ_ROOM_DEFAULT_MIB;
    bool game = false;
    while (*args) {
        while (*args == ' ' || *args == '\t') ++args;
        const bool quoted = *args == '"';
        if (quoted) ++args;
        const char *end = args;
        while (*end && (quoted ? *end != '"' : *end != ' ' && *end != '\t')) ++end;
        const size_t length = (size_t)(end - args);
        if (length > 6 && !memcmp(args, "--nup=", 6)) game = true;
        if (length > 5 && !memcmp(args, "room=", 5)) {
            unsigned value;
            if (decimal(args + 5, end, IZ_ROOM_MAX_MIB, &value) && value >= IZ_ROOM_DEFAULT_MIB)
                result = value;
        }
        args = end + (quoted && *end == '"');
    }
    return game ? result : IZ_ROOM_DEFAULT_MIB;
}
