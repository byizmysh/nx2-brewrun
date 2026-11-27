#include "nso.h"
#include "settings.h"
#include "zbic.h"

#include <stdlib.h>
#include <string.h>

enum {
    HEADER_SIZE = 256,
    INITIALIZED_LIMIT = 96 << 20,
    MAPPING_LIMIT = (IZ_ROOM_MAX_MIB - IZ_ROOM_OVERHEAD_MIB) << 20
};

typedef struct {
    uint32_t source, offset, bytes, stored;
} Segment;

static bool lz4_length(const unsigned char **it, const unsigned char *end, size_t *length) {
    if (*length != 15) return true;
    unsigned char extra;
    do {
        if (*it == end) return false;
        extra = *(*it)++;
        *length += extra;
    } while (extra == 255);
    return true;
}

static bool lz4_decode(const void *encoded, size_t encoded_size, void *decoded, size_t size) {
    const unsigned char *it = encoded, *end = it + encoded_size;
    unsigned char *out = decoded;
    size_t at = 0;
    while (it < end) {
        const unsigned char token = *it++;
        size_t literal = token >> 4;
        if (!lz4_length(&it, end, &literal) || literal > (size_t)(end - it) || literal > size - at)
            return false;
        memcpy(out + at, it, literal);
        at += literal;
        it += literal;
        if (it == end) return at == size;
        if (end - it < 2) return false;
        const size_t distance = (unsigned)it[0] | (unsigned)it[1] << 8;
        it += 2;
        size_t match = token & 15;
        if (!distance || distance > at || !lz4_length(&it, end, &match) || match + 4 > size - at)
            return false;
        match += 4;
        for (size_t i = 0; i < match; ++i, ++at) out[at] = out[at - distance];
    }
    return at == size;
}

void iz_nso_close(Nx2Image *image) {
    if (image->image) free((unsigned char *)image->image - IZ_NSO_PAGE);
    free((void *)image->svcs);
    memset(image, 0, sizeof(*image));
}

static bool describe(const unsigned char *h, const unsigned char *bytes, uint64_t image_size,
                     const Segment seg[3], Nx2Module *module) {
    const uint32_t mod0 = iz_le32(bytes + 4);
    if (!iz_span(mod0, 28, image_size) || memcmp(bytes + mod0, "MOD0", 4)) return false;
    const int64_t dynamic_at = (int64_t)mod0 + (int32_t)iz_le32(bytes + mod0 + 4);
    const int64_t bss_end = (int64_t)mod0 + (int32_t)iz_le32(bytes + mod0 + 12);
    const int64_t object = (int64_t)mod0 + (int32_t)iz_le32(bytes + mod0 + 24);
    uint64_t end = image_size + iz_le32(h + 60);
    if (bss_end > 0 && (uint64_t)bss_end > end) end = (uint64_t)bss_end;
    end = iz_nso_round_pages(end);
    if (dynamic_at < 0 || object < 0 || end > MAPPING_LIMIT ||
        (uint64_t)object >= end || !iz_span((uint64_t)dynamic_at, 16, image_size)) return false;

    uint64_t sym = (uint64_t)seg[1].offset + iz_le32(h + 152);
    uint64_t strs = (uint64_t)seg[1].offset + iz_le32(h + 144);
    uint64_t count = iz_le32(h + 156) / 24, strings = iz_le32(h + 148);
    if (!count) {
        uint64_t hash = 0;
        bool terminated = false;
        for (uint64_t at = (uint64_t)dynamic_at; iz_span(at, 16, image_size); at += 16) {
            const uint64_t tag = iz_le64(bytes + at), val = iz_le64(bytes + at + 8);
            if (!tag) { terminated = true; break; }
            if (tag == 4) hash = val;
            else if (tag == 5) strs = val;
            else if (tag == 6) sym = val;
            else if (tag == 10) strings = val;
        }
        if (!terminated) return false;
        if (hash) {
            if (!iz_span(hash, 8, image_size)) return false;
            count = iz_le32(bytes + hash + 4);
        } else if (strs > sym) count = (strs - sym) / 24;
    }
    if (count > 1000000 || !iz_span(sym, count * 24, image_size) ||
        !iz_span(strs, strings, image_size)) return false;

    module->data_off = seg[2].offset;
    module->end = end;
    module->mod0 = mod0;
    module->dynamic = (uint64_t)dynamic_at;
    module->dynsym = sym;
    module->nsyms = count;
    module->dynstr = strs;
    module->dynstr_size = strings;
    module->module_object = (uint64_t)object;
    return true;
}

static bool uses_dynamic_modules(const Nx2Module *m, const unsigned char *data) {
    static const char symbol[] = "_ZN2nn2ro10LoadModule";
    for (uint64_t i = 0; i < m->nsyms; ++i) {
        const unsigned char *entry = data + m->dynsym + i * 24;
        const uint32_t name = iz_le32(entry);
        if (!iz_le64(entry + 8) && iz_span(name, sizeof(symbol) - 1, m->dynstr_size) &&
            !memcmp(data + m->dynstr + name, symbol, sizeof(symbol) - 1)) return true;
    }
    return false;
}

bool iz_nso_prepare(const Nup *nup, const NupFile *file, const char *name, Nx2Module *module,
                    Nx2Image *image, const char **error) {
    const char *reason = "ExeFS module is missing, too large, or unreadable.";
    unsigned char *source = NULL, *mapping = NULL;
    uint32_t *svcs = NULL;
    if (file->size < HEADER_SIZE || file->size > INITIALIZED_LIMIT || file->stored > INITIALIZED_LIMIT ||
        !(source = malloc((size_t)file->size)) || !nup_read_file(nup, file, source, (size_t)file->size))
        goto fail;

    reason = "Unsupported ExeFS module format.";
    if (memcmp(source, "NSO0", 4)) goto fail;
    const uint32_t flags = iz_le32(source + 12);
    if (flags & ~0xffu) goto fail;
    Segment seg[3];
    uint64_t end = 0;
    for (unsigned k = 0; k < 3; ++k) {
        const unsigned char *h = source + 16 + k * 16;
        seg[k] = (Segment){iz_le32(h), iz_le32(h + 4), iz_le32(h + 8), iz_le32(source + 96 + k * 4)};
        if (seg[k].offset < end || (seg[k].offset & (IZ_NSO_PAGE - 1)) ||
            !iz_span(seg[k].offset, seg[k].bytes, INITIALIZED_LIMIT) ||
            !iz_span(seg[k].source, seg[k].stored, file->size)) goto fail;
        end = (uint64_t)seg[k].offset + seg[k].bytes;
    }
    if (seg[0].offset || seg[0].bytes < 12 || (seg[0].bytes & 3)) goto fail;
    reason = "Not enough memory for an ExeFS module.";
    /* Decode into the pages the loader will map. The first page holds the NRO header and thunks. */
    mapping = aligned_alloc(IZ_NSO_PAGE, (size_t)iz_nso_round_pages(end) + IZ_NSO_PAGE);
    if (!mapping) goto fail;
    unsigned char *bytes = mapping + IZ_NSO_PAGE;
    memset(bytes, 0, (size_t)end);

    reason = "A segment of an ExeFS module is damaged.";
    for (unsigned k = 0; k < 3; ++k) {
        const Segment s = seg[k];
        if (!(flags & (1u << k))) {
            if (s.stored < s.bytes) goto fail;
            memcpy(bytes + s.offset, source + s.source, s.bytes);
        } else if ((flags & 0x80) && s.stored >= 4 && !memcmp(source + s.source, "ZBIC", 4)) {
            if (!iz_zbic_decode(source + s.source, s.stored, bytes + s.offset, s.bytes)) {
                reason = "A ZBIC segment of an ExeFS module could not be decoded.";
                goto fail;
            }
        } else if (!lz4_decode(source + s.source, s.stored, bytes + s.offset, s.bytes)) goto fail;
        if ((flags & (8u << k)) && nup->source.hash &&
            !nup->source.hash(bytes + s.offset, s.bytes, source + 160 + k * 32)) goto fail;
    }
    if (!strcmp(name, "main") && iz_le32(bytes) != 1) {
        reason = "This is not a native Switch 2 program.";
        goto fail;
    }
    reason = "Invalid dynamic tables in an ExeFS module.";
    *module = (Nx2Module){.name = name};
    if (!describe(source, bytes, end, seg, module)) goto fail;
    if (uses_dynamic_modules(module, bytes)) {
        reason = "This title uses dynamic modules. This build supports whole modules only.";
        goto fail;
    }
    size_t count = 0;
    for (size_t at = 0; at < seg[0].bytes; at += 4)
        if ((iz_le32(bytes + at) & 0xffe0001fu) == 0xd4000001u) ++count;
    reason = "The module exceeds the current whole-image layout limit.";
    if (count > 165) goto fail;
    if (count && !(svcs = malloc(count * 2 * sizeof(*svcs)))) goto fail;
    size_t i = 0;
    for (size_t at = 0; at < seg[0].bytes; at += 4) {
        const uint32_t instruction = iz_le32(bytes + at);
        if ((instruction & 0xffe0001fu) != 0xd4000001u) continue;
        svcs[i++] = (uint32_t)at;
        svcs[i++] = (instruction >> 5) & 0xffffu;
    }
    *image = (Nx2Image){.image = bytes, .image_size = end, .text_size = seg[0].bytes,
        .ro_off = seg[1].offset, .ro_size = seg[1].bytes, .svcs = svcs, .nsvcs = (uint32_t)count};
    free(source);
    return true;

fail:
    free(svcs);
    free(mapping);
    free(source);
    if (error) *error = reason;
    return false;
}
