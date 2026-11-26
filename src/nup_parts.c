#include "nup_parts.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

bool nup_part_start(const char *path, NupPartName *part) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (!*name || *name == '.') return false;
    const char *dot = strrchr(name, '.');
    const char *end = name + strlen(name);
    const char *number = dot ? dot + 1 : name;
    unsigned limit = 10000;
    const bool bare = dot == NULL;
    bool alphabetic = false;

    if (dot && !strcasecmp(dot, ".nup")) {
        end = dot;
        while (dot > name && dot[-1] != '.') --dot;
        if (dot <= name + 1) return false;
        number = dot;
    } else if (dot && end - number == 3 && !strncasecmp(number, "nu", 2)) {
        number += 2;
        limit = 10;
    } else if (dot && dot - name >= 4 && !strncasecmp(dot - 4, ".nup", 4) &&
               (end - number == 2 || end - number == 3) && (*number == 'a' || *number == 'A')) {
        alphabetic = true;
        limit = end - number == 2 ? 26 * 26 : 26 * 26 * 26;
    }
    if (!bare && end - number > 4 && !strncasecmp(number, "part", 4)) number += 4;
    const size_t width = (size_t)(end - number);
    if (!width || width > 4) return false;

    unsigned first = 0;
    for (size_t i = 0; i < width; ++i) {
        if (alphabetic) {
            if (number[i] != 'a' && number[i] != 'A') return false;
        } else {
            if (number[i] < '0' || number[i] > '9') return false;
            first = first * 10 + (unsigned)(number[i] - '0');
        }
    }
    if (first > 1) return false;
    *part = (NupPartName){.offset = (size_t)(number - path), .width = width, .first = first,
                          .limit = limit, .alphabetic = alphabetic, .bare = bare};
    return true;
}

bool nup_part_path(const char *path, const NupPartName *name, unsigned part, char *out, size_t capacity) {
    if (part >= name->limit - name->first) return false;
    unsigned number = name->first + part;
    char token[5];
    if (name->alphabetic) {
        for (size_t i = name->width; i; --i) {
            const int base = path[name->offset + i - 1] == 'A' ? 'A' : 'a';
            token[i - 1] = (char)(base + number % 26);
            number /= 26;
        }
        token[name->width] = 0;
    } else {
        snprintf(token, sizeof(token), "%0*u", (int)name->width, number);
    }
    const int length = snprintf(out, capacity, "%.*s%s%s", (int)name->offset, path, token,
                                path + name->offset + name->width);
    return length >= 0 && (size_t)length < capacity;
}

uint64_t nup_parts_size(const NupParts *parts) {
    return parts->count ? parts->ends[parts->count - 1] : 0;
}

bool nup_parts_append(NupParts *parts, uint64_t bytes) {
    const uint64_t current = nup_parts_size(parts);
    if (!bytes || parts->count >= NUP_PART_LIMIT || !iz_span(current, bytes, INT64_MAX)) return false;
    parts->ends[parts->count++] = current + bytes;
    return true;
}

bool nup_parts_read(void *context, uint64_t offset, void *buffer, size_t bytes) {
    const NupParts *parts = context;
    if (!parts->read || !iz_span(offset, bytes, nup_parts_size(parts))) return false;
    unsigned lo = 0, hi = parts->count;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        if (offset < parts->ends[mid]) hi = mid;
        else lo = mid + 1;
    }
    unsigned char *out = buffer;
    for (unsigned i = lo; bytes; ++i) {
        const uint64_t start = i ? parts->ends[i - 1] : 0;
        const uint64_t left = parts->ends[i] - offset;
        const size_t take = left < bytes ? (size_t)left : bytes;
        if (!parts->read(parts->user, i, offset - start, out, take)) return false;
        offset += take;
        out += take;
        bytes -= take;
    }
    return true;
}
