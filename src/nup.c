#include "nup.h"

#include <stdlib.h>
#include <string.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

/* The console and full host builds link the existing stb_image decoder. */
extern int stbi_zlib_decode_noheader_buffer(char *, int, const char *, int) __attribute__((weak));

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

typedef enum { FACT_NPDM, FACT_NACP, FACT_ICON, FACT_VERSION, FACT_TITLE, FACT_PUBLISHER, FACT_COUNT } FactKind;
typedef struct { uint64_t at; uint32_t size; } Fact;

static NupPackage read_package(const unsigned char *p) {
    return (NupPackage){.id = iz_le64(p), .root = iz_le32(p + 8), .exefs = iz_le32(p + 12),
        .romfs = iz_le32(p + 16), .version = iz_le32(p + 20), .kind = p[24],
        .meta_at = iz_le64(p + 32), .meta_size = iz_le32(p + 40)};
}

static bool fact_has_magic(NupSource source, Fact fact, const char *magic, size_t length) {
    unsigned char bytes[8];
    return fact.size >= length && source.read(source.user, fact.at, bytes, length) &&
        !memcmp(bytes, magic, length);
}

static bool fact_is_icon(NupSource source, Fact fact) {
    return fact.size <= (1u << 20) &&
        (fact_has_magic(source, fact, "\xff\xd8\xff", 3) ||
         fact_has_magic(source, fact, "\x89PNG\r\n\x1a\n", 8));
}

static bool fact_is_nacp(Fact fact) {
    return fact.size >= 0x3070 && fact.size <= 0x4000;
}

static unsigned package_facts(const Nup *nup, const NupPackage *pack, Fact facts[FACT_COUNT + 1]) {
    const NupSource source = nup->source;
    if (pack->meta_at >= nup->data_at && iz_span(pack->meta_at, pack->meta_size, source.size)) {
        uint64_t at = pack->meta_at;
        const uint64_t end = at + pack->meta_size;
        for (unsigned i = 0; i < 128 && iz_span(at, 8, end); ++i) {
            unsigned char record[8];
            if (!source.read(source.user, at, record, sizeof(record))) break;
            const unsigned kind = record[0] | (unsigned)record[1] << 8;
            const uint32_t size = iz_le32(record + 4);
            at += sizeof(record);
            if (!iz_span(at, size, end)) break;
            if (size && kind <= FACT_COUNT) facts[kind] = (Fact){at, size};
            at += size;
        }
    }
    if (fact_has_magic(source, facts[0], "META", 4)) return 0;
    if (fact_has_magic(source, facts[1], "META", 4)) return 1;
    if (fact_is_nacp(facts[1]) || fact_is_icon(source, facts[2])) return 0;
    if (fact_is_nacp(facts[2]) || fact_is_icon(source, facts[3]) || facts[FACT_COUNT].size) return 1;
    /* Older BreRun builds used one-based title-only facts. A short kind-3 fact is a zero-based version. */
    if (facts[3].size && facts[3].size <= NUP_DISPLAY_VERSION_SIZE) return 0;
    return facts[5].size != 0;
}

static void copy_metadata_text(char *out, size_t capacity, const unsigned char *text, size_t size) {
    size_t length = 0;
    while (length < size && length < capacity - 1 && text[length] >= 32 && text[length] != 127) ++length;
    while (length && length < size && (text[length] & 0xc0) == 0x80) --length;
    if (length) {
        memcpy(out, text, length);
        out[length] = 0;
    }
}

static void read_fact_text(NupSource source, Fact fact, char *out, size_t capacity) {
    unsigned char bytes[sizeof(((NupPreview *)0)->title)];
    const size_t size = fact.size < capacity ? fact.size : capacity;
    if (size && source.read(source.user, fact.at, bytes, size))
        copy_metadata_text(out, capacity, bytes, size);
}

static void nacp_title(NupSource source, Fact fact, NupPreview *preview) {
    enum { TITLE_DATA_SIZE = 0x3000, ENTRY_SIZE = 0x300, FORMAT = 0x3215 };
    unsigned char format = 0, titles[TITLE_DATA_SIZE];
    if (!fact_is_nacp(fact) ||
        (fact.size > FORMAT && !source.read(source.user, fact.at + FORMAT, &format, 1)) ||
        format > 1 || !source.read(source.user, fact.at, titles, sizeof(titles))) return;
    const unsigned char *entries = titles;
    size_t size = sizeof(titles);
    char *decoded = NULL;
    if (format == 1) {
        const unsigned compressed = titles[0] | (unsigned)titles[1] << 8;
        if (!compressed || compressed > sizeof(titles) - 2 || !stbi_zlib_decode_noheader_buffer) return;
        decoded = malloc(32 * ENTRY_SIZE);
        if (!decoded) return;
        const int length = stbi_zlib_decode_noheader_buffer(decoded, 32 * ENTRY_SIZE,
                                                            (const char *)titles + 2, (int)compressed);
        entries = (const unsigned char *)decoded;
        size = length > 0 && length <= 32 * ENTRY_SIZE && !(length % ENTRY_SIZE) ? (size_t)length : 0;
    }
    for (size_t at = 0; at + ENTRY_SIZE <= size && !preview->title[0]; at += ENTRY_SIZE)
        copy_metadata_text(preview->title, sizeof(preview->title), entries + at, 0x200);
    free(decoded);
}

static void read_metadata(const Nup *nup, const NupPackage *const *layers, size_t count,
                          char version[NUP_DISPLAY_VERSION_SIZE], NupPreview *preview) {
    for (size_t i = count; i; --i) {
        Fact facts[FACT_COUNT + 1] = {0};
        const unsigned first = package_facts(nup, layers[i - 1], facts);
        const NupSource source = nup->source;
        const Fact nacp = facts[first + FACT_NACP], icon = facts[first + FACT_ICON];
        if (!version[0]) {
            read_fact_text(source, facts[first + FACT_VERSION], version, NUP_DISPLAY_VERSION_SIZE);
            if (!version[0] && fact_is_nacp(nacp))
                read_fact_text(source, (Fact){nacp.at + 0x3060, NUP_DISPLAY_VERSION_SIZE},
                               version, NUP_DISPLAY_VERSION_SIZE);
        }
        if (!preview) {
            if (version[0]) break;
            continue;
        }
        if (!preview->title[0]) {
            read_fact_text(source, facts[first + FACT_TITLE], preview->title, sizeof(preview->title));
            if (!preview->title[0]) nacp_title(source, nacp, preview);
        }
        if (!preview->cover_size && fact_is_icon(source, icon)) {
            preview->cover_offset = icon.at;
            preview->cover_size = icon.size;
        }
        if (version[0] && preview->title[0] && preview->cover_size) break;
    }
}

void nup_display_version(const Nup *nup, const NupPackage *const *layers, size_t count,
                         char version[NUP_DISPLAY_VERSION_SIZE]) {
    memset(version, 0, NUP_DISPLAY_VERSION_SIZE);
    read_metadata(nup, layers, count, version, NULL);
}

static uint64_t package_title(const NupPackage *const *layers, size_t count) {
    uint64_t update = 0;
    for (size_t i = 0; i < count; ++i) {
        const NupPackage *pack = layers[i];
        if (!(pack->id & ~UINT64_C(0xfff))) continue;
        if (!pack->kind) return pack->id;
        update = pack->id;
    }
    return update & ~UINT64_C(0xfff);
}

bool nup_preview(NupSource source, NupPreview *preview) {
    unsigned char header[128];
    if (!source.read || source.size < sizeof(header) ||
        !source.read(source.user, 0, header, sizeof(header)) ||
        memcmp(header, "NUP\1", 4)) return false;
    const uint64_t toc = iz_le64(header + 8), toc_size = iz_le64(header + 16), data = iz_le64(header + 56);
    const uint64_t declared = iz_le64(header + 64);
    const uint32_t packages = iz_le32(header + 72);
    if (!packages || packages > NUP_PACKAGE_LIMIT || toc < sizeof(header) ||
        toc_size > NUP_TOC_LIMIT || 48 + 64 * packages > toc_size ||
        !iz_span(toc, toc_size, data) || data > declared || declared < source.size) return false;
    *preview = (NupPreview){.size = source.size, .incomplete = declared > source.size};
    if (!iz_span(toc, 48 + 64 * packages, source.size)) return preview->incomplete;
    unsigned char table[48 + 64 * NUP_PACKAGE_LIMIT];
    if (!source.read(source.user, toc, table, 48 + 64 * packages) ||
        memcmp(table, "TOC\1", 4) || iz_le32(table + 4) != packages) return false;

    Nup nup = {.source = source, .data_at = data, .package_count = packages};
    for (uint32_t i = 0; i < packages; ++i) nup.packages[i] = read_package(table + 48 + 64 * i);
    const NupPackage *layers[NUP_PACKAGE_LIMIT];
    size_t count = nup_layers(&nup, layers);
    if (!count) return true;
    preview->title_id = package_title(layers, count);
    preview->package_version = layers[count - 1]->version;
    read_metadata(&nup, layers, count, preview->display_version, preview);
    if (!preview->title_id && !preview->incomplete && nup_open(&nup, source, NULL)) {
        count = nup_layers(&nup, layers);
        nup_program(&nup, layers, count, &preview->title_id);
        nup_close(&nup);
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
    if (declared < source.size || !packages || packages > NUP_PACKAGE_LIMIT ||
        toc_at < sizeof(header) || toc_size < 48 || toc_size > NUP_TOC_LIMIT ||
        !iz_span(toc_at, toc_size, nup->data_at) || nup->data_at > declared) goto fail;
    reason = NUP_INCOMPLETE_MESSAGE;
    if (declared > source.size) goto fail;

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
        *pack = read_package(p);
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

static bool nup_read_zstd(const NupSource *source, const NupFile *file, void *output, size_t bytes) {
    enum { INPUT_BLOCK = 256 * 1024 };
    const size_t capacity = file->stored < INPUT_BLOCK ? (size_t)file->stored : INPUT_BLOCK;
    if (!capacity) return false;
    unsigned char *buffer = malloc(capacity);
    ZSTD_DCtx *decoder = ZSTD_createDCtx();
    /* The caller owns the entire output, so zstd can use it directly as its window. */
    bool ok = buffer && decoder &&
        !ZSTD_isError(ZSTD_DCtx_setParameter(decoder, ZSTD_d_stableOutBuffer, 1)) &&
        !ZSTD_isError(ZSTD_DCtx_setParameter(decoder, ZSTD_d_windowLogMax, ZSTD_WINDOWLOG_MAX));
    ZSTD_outBuffer out = {output, bytes, 0};
    size_t status = 1;
    for (uint64_t at = 0; ok && at < file->stored;) {
        const uint64_t remaining = file->stored - at;
        const size_t take = remaining < capacity ? (size_t)remaining : capacity;
        ok = source->read(source->user, file->offset + at, buffer, take);
        ZSTD_inBuffer in = {buffer, take, 0};
        while (ok && in.pos < in.size) {
            const size_t in_before = in.pos, out_before = out.pos;
            status = ZSTD_decompressStream(decoder, &out, &in);
            ok = !ZSTD_isError(status) && (in.pos != in_before || out.pos != out_before);
        }
        at += take;
    }
    ZSTD_freeDCtx(decoder);
    free(buffer);
    return ok && status == 0 && out.pos == out.size;
}

bool nup_read_file(const Nup *nup, const NupFile *file, void *output, size_t bytes) {
    if (file->size != bytes || file->stored > SIZE_MAX ||
        !iz_span(file->offset, file->stored, nup->source.size)) return false;
    if (!file->codec) {
        if (!nup->source.read(nup->source.user, file->offset, output, bytes)) return false;
    } else {
        if (!nup_read_zstd(&nup->source, file, output, bytes)) return false;
    }
    return !file->hash || !nup->source.hash || nup->source.hash(output, bytes, file->hash);
}

static uint32_t aci_offset(const unsigned char header[0x80], uint64_t size) {
    if (memcmp(header, "META", 4)) return 0;
    const uint32_t offset = iz_le32(header + 0x70), length = iz_le32(header + 0x74);
    return offset >= 0x80 && length >= 24 && iz_span(offset, length, size) ? offset : 0;
}

static uint64_t aci_program(const unsigned char aci[24]) {
    return memcmp(aci, "ACI0", 4) ? 0 : iz_le64(aci + 16);
}

static uint64_t exefs_program(const Nup *nup, const NupPackage *code) {
    NupFile file;
    if (!nup_file(nup, nup_find_file(nup, code->exefs, "main.npdm"), &file) ||
        file.size < 0x80 || file.size > (1u << 20) || file.stored > (1u << 20)) return 0;
    unsigned char *npdm = malloc((size_t)file.size);
    if (!npdm) return 0;
    uint64_t id = 0;
    if (nup_read_file(nup, &file, npdm, (size_t)file.size)) {
        const uint32_t offset = aci_offset(npdm, file.size);
        if (offset) id = aci_program(npdm + offset);
    }
    free(npdm);
    return id;
}

static uint64_t metadata_program(const Nup *nup, const NupPackage *pack) {
    Fact facts[FACT_COUNT + 1] = {0};
    const unsigned first = package_facts(nup, pack, facts);
    const Fact npdm = facts[first + FACT_NPDM];
    unsigned char header[0x80], aci[24];
    const NupSource source = nup->source;
    if (npdm.size < sizeof(header) || !source.read(source.user, npdm.at, header, sizeof(header))) return 0;
    const uint32_t offset = aci_offset(header, npdm.size);
    return offset && source.read(source.user, npdm.at + offset, aci, sizeof(aci)) ? aci_program(aci) : 0;
}

const NupPackage *nup_program(const Nup *nup, const NupPackage *const *layers, size_t count, uint64_t *title) {
    const NupPackage *code = NULL;
    for (size_t i = 0; i < count; ++i)
        if (nup_find_file(nup, layers[i]->exefs, "main") != NUP_NONE) code = layers[i];
    *title = package_title(layers, count);
    if (!*title && code) {
        uint64_t id = exefs_program(nup, code);
        if (id & ~UINT64_C(0xfff)) *title = id;
        for (size_t i = count; i && !*title; --i) {
            id = metadata_program(nup, layers[i - 1]);
            if (id & ~UINT64_C(0xfff)) *title = id;
        }
    }
    return code;
}

bool nup_check_index(const Nup *nup, const char **error) {
    uint32_t *incoming = calloc(nup->dir_count, sizeof(*incoming));
    uint32_t *queue = malloc(nup->dir_count * sizeof(*queue));
    const char *reason = "Not enough memory to check the NUP index.";
    if (!incoming || !queue) goto fail;

    size_t edges_left = nup->children_count;
    for (uint32_t i = 0; i < nup->dir_count; ++i) {
        NupDir dir;
        reason = "The NUP index contains an invalid directory or child count.";
        if (!nup_dir(nup, i, &dir) || dir.count > edges_left) goto fail;
        edges_left -= dir.count;
        for (uint32_t j = 0; j < dir.count; ++j) {
            const uint32_t child = nup_child(nup, dir.first + j);
            reason = "The NUP index contains an invalid folder entry.";
            if (child >= nup->dir_count + nup->file_count) goto fail;
            if (child < nup->dir_count) ++incoming[child];
        }
    }

    uint32_t queued = 0;
    for (uint32_t i = 0; i < nup->dir_count; ++i)
        if (!incoming[i]) queue[queued++] = i;
    reason = "The NUP index contains an invalid directory.";
    for (uint32_t i = 0; i < queued; ++i) {
        NupDir dir;
        if (!nup_dir(nup, queue[i], &dir)) goto fail;
        for (uint32_t j = 0; j < dir.count; ++j) {
            const uint32_t child = nup_child(nup, dir.first + j);
            if (child < nup->dir_count && !--incoming[child]) queue[queued++] = child;
        }
    }
    reason = "The NUP index contains a directory cycle.";
    if (queued != nup->dir_count) goto fail;
    reason = "The NUP index contains invalid file or compression metadata.";
    for (uint32_t i = 0; i < nup->file_count; ++i) {
        NupFile file;
        if (!nup_file(nup, i, &file)) goto fail;
    }

    free(queue);
    free(incoming);
    return true;

fail:
    free(queue);
    free(incoming);
    if (error) *error = reason;
    return false;
}
