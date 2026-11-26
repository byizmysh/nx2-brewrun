#ifndef IZ_COVER_H
#define IZ_COVER_H

#include <stdbool.h>
#include <stddef.h>

enum { COVER_BYTES_LIMIT = 1 << 20 };

typedef struct {
    unsigned char *pixels;
    int width, height;
} Cover;

bool cover_decode(Cover *, const void *, size_t);
bool cover_read_file(Cover *, const char *);
void cover_clear(Cover *);

#endif
