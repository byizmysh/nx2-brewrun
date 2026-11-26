#ifndef IZ_NUP_PARTS_H
#define IZ_NUP_PARTS_H

#include "nup.h"
#include <string.h>
#include <strings.h>

enum { NUP_PART_LIMIT = 100 };

typedef bool (*NupPartRead)(void *, unsigned, uint64_t, void *, size_t);
typedef struct {
    uint64_t ends[NUP_PART_LIMIT];
    unsigned count;
    NupPartRead read;
    void *user;
} NupParts;

/* Split streams can begin at part zero or one. */
static inline unsigned nup_part_number(const char *path, unsigned *first) {
    const char *suffix = strrchr(path, '.');
    if (!suffix) return 0;
    const size_t digits = strlen(++suffix);
    if (!digits || digits > 3) return 0;
    unsigned value = 0;
    for (size_t i = 0; i < digits; ++i) {
        if (suffix[i] < '0' || suffix[i] > '9') return 0;
        value = 10 * value + suffix[i] - '0';
    }
    if (value > 1) return 0;
    *first = value;
    return (unsigned)digits;
}

static inline unsigned nup_part_digits(const char *path) {
    unsigned first;
    return nup_part_number(path, &first);
}
bool nup_parts_append(NupParts *, uint64_t);
uint64_t nup_parts_size(const NupParts *);
bool nup_parts_read(void *, uint64_t, void *, size_t);

#endif
