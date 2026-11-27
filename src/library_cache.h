#ifndef IZ_LIBRARY_CACHE_H
#define IZ_LIBRARY_CACHE_H

#include "cover.h"
#include <stdint.h>

typedef struct {
    char name[512];
    char title[128];
    uint64_t bytes, title_id, cover_offset, source_key, art_key;
    uint32_t cover_size;
    bool directory, loose, needs_exefs, nup, split, incomplete;
} IzLibraryEntry;

extern const char *const iz_library_roots[];

bool iz_library_cache_open(const char *self, char path[512]);
bool iz_library_cache_read(const char *, IzLibraryEntry **, size_t *, uint64_t *);
bool iz_library_cache_write(const char *, const IzLibraryEntry *, size_t, uint64_t *);
bool iz_library_cover_read(const char *, uint64_t key, Cover *);
bool iz_library_cover_write(const char *, uint64_t key, const Cover *);
bool iz_library_cover_exists(const char *, uint64_t key);
void iz_library_cover_forget(const char *, uint64_t key);

#endif
