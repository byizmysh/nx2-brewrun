#include "library_cache.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *const iz_library_roots[] = {"Games", "nx2-relink", "iz2s/modules/nx2-brerun", NULL};

enum { NUP = 1, LOOSE = 2, NEEDS_EXEFS = 4, SPLIT = 8, INCOMPLETE = 16, ALL_FLAGS = 31 };
typedef struct { char magic[8]; uint64_t generation, count; } IndexHeader;
typedef struct {
    char name[512], title[128];
    uint64_t bytes, title_id, cover_offset, source_key, art_key;
    uint32_t cover_size, flags;
} Record;
typedef struct { char magic[8]; uint64_t key; uint32_t width, height; } ArtHeader;

bool iz_library_cache_open(const char *self, char path[512]) {
    path[0] = 0;
    if (!self || strncmp(self, "sdmc:/", 6)) return false;
    const char *slash = strrchr(self, '/');
    if (!slash || slash - self >= 512 || !slash[1]) return false;
    const int length = snprintf(path, 512, "%.*s/.library-cache", (int)(slash - self), self);
    struct stat st;
    if (length > 0 && length < 512 && (!mkdir(path, 0777) ||
        (errno == EEXIST && !stat(path, &st) && S_ISDIR(st.st_mode)))) return true;
    path[0] = 0;
    return false;
}

static bool game_path(const char *name) {
    bool root = false;
    for (size_t i = 0; iz_library_roots[i]; ++i) {
        const size_t n = strlen(iz_library_roots[i]);
        if (!strncmp(name, iz_library_roots[i], n) && (!name[n] || name[n] == '/')) root = true;
    }
    if (!root || strcspn(name, "\\:") != strlen(name)) return false;
    while (*name) {
        const size_t n = strcspn(name, "/");
        if (!n || name[0] == '.') return false;
        name += n;
        if (*name && !*++name) return false;
    }
    return true;
}

static bool read_index(const char *path, IzLibraryEntry **entries, size_t *count, uint64_t *generation) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    IndexHeader head;
    IzLibraryEntry *list = NULL;
    bool ok = false;
    if (fread(&head, sizeof(head), 1, file) != 1 || memcmp(head.magic, "BRERIDX1", 8) ||
        head.count > SIZE_MAX / sizeof(*list) || head.count > (LONG_MAX - sizeof(head)) / sizeof(Record) ||
        !head.generation || fseek(file, 0, SEEK_END) ||
        ftell(file) != (long)(sizeof(head) + head.count * sizeof(Record)) || fseek(file, sizeof(head), SEEK_SET)) goto done;
    if (head.count && !(list = calloc((size_t)head.count, sizeof(*list)))) goto done;
    for (size_t i = 0; i < head.count; ++i) {
        Record record;
        if (fread(&record, sizeof(record), 1, file) != 1 ||
            !memchr(record.name, 0, sizeof(record.name)) || !memchr(record.title, 0, sizeof(record.title)) ||
            !game_path(record.name) || (record.flags & ~ALL_FLAGS) ||
            !!(record.flags & NUP) == !!(record.flags & LOOSE) || !record.bytes) goto done;
        list[i] = (IzLibraryEntry){.bytes = record.bytes, .title_id = record.title_id, .cover_offset = record.cover_offset,
            .source_key = record.source_key,
            .art_key = record.art_key, .cover_size = record.cover_size,
            .nup = record.flags & NUP, .loose = record.flags & LOOSE, .needs_exefs = record.flags & NEEDS_EXEFS,
            .split = record.flags & SPLIT, .incomplete = record.flags & INCOMPLETE};
        strcpy(list[i].name, record.name);
        strcpy(list[i].title, record.title);
    }
    *entries = list;
    *count = (size_t)head.count;
    *generation = head.generation;
    ok = true;
done:
    fclose(file);
    if (!ok) free(list);
    return ok;
}

bool iz_library_cache_read(const char *dir, IzLibraryEntry **entries, size_t *count, uint64_t *generation) {
    if (!*dir) return false;
    char path[544];
    bool loaded = false;
    for (int i = 0; i < 2; ++i) {
        snprintf(path, sizeof(path), "%s/index.%d", dir, i);
        IzLibraryEntry *list;
        size_t length;
        uint64_t revision;
        if (!read_index(path, &list, &length, &revision)) continue;
        if (loaded && revision <= *generation) { free(list); continue; }
        if (loaded) free(*entries);
        *entries = list;
        *count = length;
        *generation = revision;
        loaded = true;
    }
    return loaded;
}

static bool publish(FILE *file, bool ready, const char *temporary, const char *target) {
    if (fclose(file)) ready = false;
    if (ready) {
        remove(target);
        if (!rename(temporary, target)) return true;
    }
    remove(temporary);
    return false;
}

bool iz_library_cache_write(const char *dir, const IzLibraryEntry *entries, size_t count, uint64_t *generation) {
    if (!*dir || *generation == UINT64_MAX) return false;
    char path[544], temporary[544];
    const IndexHeader head = {{'B','R','E','R','I','D','X','1'}, *generation + 1, count};
    snprintf(path, sizeof(path), "%s/index.%u", dir, (unsigned)(head.generation % 2));
    snprintf(temporary, sizeof(temporary), "%s/index.tmp", dir);
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    bool ok = fwrite(&head, sizeof(head), 1, file) == 1;
    for (size_t i = 0; ok && i < count; ++i) {
        const IzLibraryEntry *entry = &entries[i];
        Record record = {.bytes = entry->bytes, .title_id = entry->title_id, .cover_offset = entry->cover_offset,
            .source_key = entry->source_key, .art_key = entry->art_key, .cover_size = entry->cover_size,
            .flags = (entry->nup ? NUP : 0) | (entry->loose ? LOOSE : 0) | (entry->needs_exefs ? NEEDS_EXEFS : 0) |
                     (entry->split ? SPLIT : 0) | (entry->incomplete ? INCOMPLETE : 0)};
        strcpy(record.name, entry->name);
        strcpy(record.title, entry->title);
        ok = fwrite(&record, sizeof(record), 1, file) == 1;
    }
    if (!publish(file, ok, temporary, path)) return false;
    *generation = head.generation;
    return true;
}

static void art_path(char path[544], const char *dir, uint64_t key) {
    snprintf(path, 544, "%s/%016" PRIx64 ".rgba", dir, key);
}

bool iz_library_cover_exists(const char *dir, uint64_t key) {
    if (!*dir || !key) return false;
    char path[544];
    struct stat st;
    art_path(path, dir, key);
    return !stat(path, &st) && S_ISREG(st.st_mode) && st.st_size > (long)sizeof(ArtHeader);
}

bool iz_library_cover_read(const char *dir, uint64_t key, Cover *cover) {
    if (!*dir || !key) return false;
    char path[544];
    art_path(path, dir, key);
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    ArtHeader head;
    bool ok = fread(&head, sizeof(head), 1, file) == 1 && !memcmp(head.magic, "BRERART1", 8) && head.key == key &&
              head.width && head.width <= 512 && head.height && head.height <= 512;
    const size_t bytes = ok ? (size_t)head.width * head.height * 4 : 0;
    unsigned char *pixels = ok ? malloc(bytes) : NULL;
    ok = pixels && fread(pixels, 1, bytes, file) == bytes && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!ok) { free(pixels); return false; }
    cover_clear(cover);
    *cover = (Cover){pixels, (int)head.width, (int)head.height};
    return true;
}

bool iz_library_cover_write(const char *dir, uint64_t key, const Cover *cover) {
    if (!*dir || !key || !cover->pixels || cover->width <= 0 || cover->width > 512 ||
        cover->height <= 0 || cover->height > 512) return false;
    char path[544], temporary[544];
    art_path(path, dir, key);
    snprintf(temporary, sizeof(temporary), "%s/art.tmp", dir);
    const ArtHeader head = {{'B','R','E','R','A','R','T','1'}, key, (uint32_t)cover->width, (uint32_t)cover->height};
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    const size_t bytes = (size_t)head.width * head.height * 4;
    const bool ok = fwrite(&head, sizeof(head), 1, file) == 1 && fwrite(cover->pixels, 1, bytes, file) == bytes;
    return publish(file, ok, temporary, path);
}

void iz_library_cover_forget(const char *dir, uint64_t key) {
    if (!*dir || !key) return;
    char path[544];
    art_path(path, dir, key);
    remove(path);
}
