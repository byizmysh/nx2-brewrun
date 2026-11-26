#include "nup_parts.h"

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
