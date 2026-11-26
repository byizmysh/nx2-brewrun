#ifndef IZ_NUP_PARTS_H
#define IZ_NUP_PARTS_H

#include "nup.h"

enum { NUP_PART_LIMIT = 100 };

typedef bool (*NupPartRead)(void *, unsigned, uint64_t, void *, size_t);
typedef struct {
    uint64_t ends[NUP_PART_LIMIT];
    unsigned count;
    NupPartRead read;
    void *user;
} NupParts;

typedef struct {
    size_t offset, width;
    unsigned first, limit;
    bool alphabetic, bare;
} NupPartName;

bool nup_part_start(const char *path, NupPartName *);
bool nup_part_path(const char *path, const NupPartName *, unsigned part, char *out, size_t capacity);
bool nup_parts_append(NupParts *, uint64_t);
uint64_t nup_parts_size(const NupParts *);
bool nup_parts_read(void *, uint64_t, void *, size_t);

#endif
