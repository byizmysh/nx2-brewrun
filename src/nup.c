#include "nup.h"

#include <stdlib.h>
#include <string.h>
#include <zstd.h>

uint32_t iz_le32(const void *value) {
    const unsigned char *p = value;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint64_t iz_le64(const void *value) {
    const unsigned char *p = value;
    return iz_le32(p) | (uint64_t)iz_le32(p + 4) << 32;
}

void iz_put32(void *value, uint32_t n) {
    unsigned char *p = value;
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}

void iz_put64(void *value, uint64_t n) {
    unsigned char *p = value;
    iz_put32(p, (uint32_t)n);
    iz_put32(p + 4, (uint32_t)(n >> 32));
}

bool iz_span(uint64_t offset, uint64_t bytes, uint64_t size) {
    return offset <= size && bytes <= size - offset;
}

static bool name_at(const Nup *nup, uint32_t offset, uint32_t length, const char **name) {
    if (length > NUP_NAME_LIMIT || !iz_span(offset, length, nup->names_size)) return false;
    const char *p = nup->names + offset;
    if ((length == 1 && p[0] == '.') || (length == 2 && !memcmp(p, "..", 2))) return false;
    for (uint32_t i = 0; i < length; ++i)
        if ((unsigned char)p[i] < 32 || p[i] == '/' || p[i] == '\\') return false;
    *name = p;
    return true;
}

void nup_close(Nup *nup) {
    free(nup->table);
    memset(nup, 0, sizeof(*nup));
}

bool nup_preview(NupSource source, NupPreview *preview) {
    unsigned char header[128];
    if (!source.read || source.size < sizeof(header) ||
        !source.read(source.user, 0, header, sizeof(header)) ||
        memcmp(header, "NUP\1", 4) || iz_le64(header + 64) != source.size) return false;
    const uint64_t toc = iz_le64(header + 8), toc_size = iz_le64(header + 16), data = iz_le64(header + 56);
    const uint32_t packages = iz_le32(header + 72);
    if (!packages || packages > NUP_PACKAGE_LIMIT || toc < sizeof(header) ||
        toc_size > NUP_TOC_LIMIT || 48 + 64 * packages > toc_size ||
        !iz_span(toc, toc_size, data) || data > source.size) return false;
    unsigned char table[48 + 64 * NUP_PACKAGE_LIMIT];
    if (!source.read(source.user, toc, table, 48 + 64 * packages) ||
        memcmp(table, "TOC\1", 4) || iz_le32(table + 4) != packages) return false;

    *preview = (NupPreview){.size = source.size};
    const unsigned char *best = NULL;
    for (uint32_t i = 0; i < packages; ++i) {
        const unsigned char *pack = table + 48 + 64 * i;
        if (pack[24] > 1) continue;
        if (!best || pack[24] < best[24] ||
            (pack[24] == 1 && best[24] == 1 && iz_le32(pack + 20) > iz_le32(best + 20))) best = pack;
    }
    if (!best) return true;
    preview->title_id = iz_le64(best);
    uint64_t cursor = iz_le64(best + 32);
    const uint64_t bytes = iz_le32(best + 40);
    if (cursor < data || !iz_span(cursor, bytes, source.size)) return true;
    const uint64_t end = cursor + bytes;
    for (unsigned i = 0; i < 128 && iz_span(cursor, 8, end); ++i) {
        unsigned char record[8];
        if (!source.read(source.user, cursor, record, sizeof(record))) break;
        cursor += sizeof(record);
        const uint32_t length = iz_le32(record + 4);
        if (!iz_span(cursor, length, end)) break;
        if (record[0] == 3 && record[1] == 0 && length && length <= (1u << 20)) {
            preview->cover_offset = cursor;
            preview->cover_size = length;
        }
        if (record[0] == 5 && record[1] == 0 && length && length < sizeof(preview->title)) {
            char name[sizeof(preview->title)];
            if (!source.read(source.user, cursor, name, length)) break;
            size_t count = 0;
            while (count < length && (unsigned char)name[count] >= 32 && name[count] != 127) ++count;
            if (count) memcpy(preview->title, name, count);
        }
        if (preview->title[0] && preview->cover_size) break;
        cursor += length;
    }
    return true;
}

bool nup_open(Nup *nup, NupSource source, const char **error) {
    const char *reason = "Unreadable NUP header.";
    unsigned char header[128];
    memset(nup, 0, sizeof(*nup));
    nup->source = source;
    if (!source.read || source.size < sizeof(header) ||
        !source.read(source.user, 0, header, sizeof(header))) goto fail;
    reason = "Unsupported NUP container (expected NUP v1).";
    if (memcmp(header, "NUP\1", 4)) goto fail;

    uint64_t toc_at = iz_le64(header + 8), toc_size = iz_le64(header + 16);
    nup->data_at = iz_le64(header + 56);
    const uint64_t declared = iz_le64(header + 64);
    const uint32_t packages = iz_le32(header + 72);
    reason = "NUP table, file size or package count is invalid.";
    if (declared != source.size || !packages || packages > NUP_PACKAGE_LIMIT ||
        toc_at < sizeof(header) || toc_size < 48 || toc_size > NUP_TOC_LIMIT ||
        !iz_span(toc_at, toc_size, nup->data_at) || nup->data_at > source.size) goto fail;

    nup->table_size = (size_t)toc_size;
    nup->table = malloc(nup->table_size);
    reason = "Not enough memory for the NUP index.";
    if (!nup->table) goto fail;
    reason = "NUP table could not be read or verified.";
    if (!source.read(source.user, toc_at, nup->table, nup->table_size) ||
        memcmp(nup->table, "TOC\1", 4) || iz_le32(nup->table + 4) != packages ||
        (source.hash && !source.hash(nup->table, nup->table_size, header + 24))) goto fail;

    const uint64_t dir_count = iz_le64(nup->table + 8), file_count = iz_le64(nup->table + 16);
    const uint64_t names_at = iz_le64(nup->table + 24), names_size = iz_le64(nup->table + 32);
    const uint64_t files_at = 48 + 64 * packages + 24 * dir_count;
    const uint64_t children_at = files_at + 80 * file_count;
    reason = "NUP directory or file index exceeds supported bounds.";
    if (!dir_count || dir_count > NUP_NODE_LIMIT || file_count > NUP_NODE_LIMIT ||
        children_at > names_at || (names_at - children_at) % 4 ||
        (names_at - children_at) / 4 > 2 * NUP_NODE_LIMIT ||
        !iz_span(names_at, names_size, toc_size)) goto fail;

    nup->package_count = packages;
    nup->dir_count = (uint32_t)dir_count;
    nup->file_count = (uint32_t)file_count;
    nup->files_at = (size_t)files_at;
    nup->children_at = (size_t)children_at;
    nup->children_count = (size_t)((names_at - children_at) / 4);
    nup->names = (const char *)nup->table + names_at;
    nup->names_size = (size_t)names_size;
    reason = "A package in the NUP refers to an invalid directory.";
    for (uint32_t i = 0; i < packages; ++i) {
        const unsigned char *p = nup->table + 48 + 64 * i;
        NupPackage *pack = &nup->packages[i];
        *pack = (NupPackage){.id = iz_le64(p), .root = iz_le32(p + 8), .exefs = iz_le32(p + 12),
            .romfs = iz_le32(p + 16), .version = iz_le32(p + 20), .kind = p[24],
            .meta_at = iz_le64(p + 32), .meta_size = iz_le32(p + 40)};
        if (pack->root >= dir_count ||
            (pack->exefs != NUP_NONE && pack->exefs >= dir_count) ||
            (pack->romfs != NUP_NONE && pack->romfs >= dir_count)) goto fail;
    }
    return true;

fail:
    nup_close(nup);
    if (error) *error = reason;
    return false;
}

bool nup_dir(const Nup *nup, uint32_t index, NupDir *out) {
    if (index >= nup->dir_count) return false;
    const unsigned char *p = nup->table + 48 + nup->package_count * 64 + 24 * index;
    out->name_size = iz_le32(p + 4);
    out->first = iz_le32(p + 8);
    out->count = iz_le32(p + 12);
    return name_at(nup, iz_le32(p), out->name_size, &out->name) &&
        iz_span(out->first, out->count, nup->children_count);
}

bool nup_file(const Nup *nup, uint32_t index, NupFile *out) {
    if (index >= nup->file_count) return false;
    const unsigned char *p = nup->table + nup->files_at + 80 * index;
    *out = (NupFile){.name_size = iz_le32(p + 4), .size = iz_le64(p + 8),
        .offset = iz_le64(p + 16), .stored = iz_le64(p + 24), .hash = p + 32, .codec = p[64]};
    const bool valid_payload = out->codec == 0 ? out->stored == out->size :
        (out->codec == 1 && out->stored && out->size);
    return valid_payload && out->name_size && out->offset >= nup->data_at &&
        iz_span(out->offset, out->stored, nup->source.size) &&
        name_at(nup, iz_le32(p), out->name_size, &out->name);
}

uint32_t nup_child(const Nup *nup, uint32_t index) {
    if (index >= nup->children_count) return NUP_NONE;
    return iz_le32(nup->table + nup->children_at + 4 * index);
}

uint32_t nup_find_file(const Nup *nup, uint32_t root, const char *name) {
    NupDir dir;
    if (!nup_dir(nup, root, &dir)) return NUP_NONE;
    const size_t length = strlen(name);
    for (uint32_t i = 0; i < dir.count; ++i) {
        const uint32_t item = nup_child(nup, dir.first + i);
        if (item < nup->dir_count) continue;
        NupFile file;
        if (nup_file(nup, item - nup->dir_count, &file) &&
            file.name_size == length && !memcmp(file.name, name, length)) return item - nup->dir_count;
    }
    return NUP_NONE;
}

size_t nup_layers(const Nup *nup, const NupPackage **layers) {
    size_t count = 0;
    for (uint32_t k = 0; k < nup->package_count; ++k) {
        const NupPackage *p = &nup->packages[k];
        if (p->kind > 1) continue;
        size_t i = count;
        while (i && (layers[i - 1]->kind > p->kind ||
               (layers[i - 1]->kind == p->kind && layers[i - 1]->version > p->version))) {
            layers[i] = layers[i - 1];
            --i;
        }
        layers[i] = p;
        ++count;
    }
    return count;
}

bool nup_read_file(const Nup *nup, const NupFile *file, void *output, size_t bytes) {
    if (file->size != bytes || file->stored > SIZE_MAX) return false;
    if (!file->codec) {
        if (!nup->source.read(nup->source.user, file->offset, output, bytes)) return false;
    } else {
        void *packed = malloc((size_t)file->stored);
        if (!packed) return false;
        const bool read = nup->source.read(nup->source.user, file->offset, packed, (size_t)file->stored);
        const size_t decoded = read ? ZSTD_decompress(output, bytes, packed, (size_t)file->stored) : 0;
        free(packed);
        if (!read || ZSTD_isError(decoded) || decoded != bytes) return false;
    }
    return !file->hash || !nup->source.hash || nup->source.hash(output, bytes, file->hash);
}
