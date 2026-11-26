#include "virtual_romfs.h"

#include <stdlib.h>
#include <string.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

enum { DATA_START = 512, MAX_DEPTH = 64, IO_BLOCK = 256 * 1024, MAX_WINDOW = 16 * 1024 * 1024 };

typedef struct {
    const char *name;
    uint32_t length, parent, child, file, sibling, table, blob;
    bool directory;
} Node;

typedef struct {
    const Nup *nup;
    Node *nodes;
    uint32_t *map;
    uint32_t count, capacity, map_mask, visited;
} Tree;

typedef struct {
    uint64_t offset;
    NupFile file;
} Span;

struct VirtualRomFs {
    const Nup *nup;
    unsigned char header[DATA_START], *tables, *input, *block, *raw;
    void *workspace;
    ZSTD_DStream *decoder;
    ZSTD_inBuffer in;
    Span *spans;
    size_t count, cached_bytes;
    uint64_t tables_at, size, compressed_at, decoded, cached_at;
    const Span *current;
};

static uint32_t name_hash(uint32_t parent, const char *name, uint32_t length) {
    uint32_t hash = parent ^ 123456789u;
    for (uint32_t i = 0; i < length; ++i)
        hash = ((hash >> 5) | (hash << 27)) ^ (unsigned char)name[i];
    return hash;
}

static uint32_t bucket_count(uint32_t size) {
    if (size < 3) return 3;
    if (size < 19) return size | 1;
    const unsigned primes[] = {2, 3, 5, 7, 11, 13, 17};
    for (;; ++size) {
        size_t i = 0;
        while (i < sizeof(primes) / sizeof(*primes) && size % primes[i]) ++i;
        if (i == sizeof(primes) / sizeof(*primes)) return size;
    }
}

static uint32_t get_node(Tree *tree, uint32_t parent, const char *name, uint32_t n, bool directory) {
    uint32_t slot = name_hash(parent, name, n) & tree->map_mask;
    while (tree->map[slot]) {
        const uint32_t index = tree->map[slot];
        const Node *node = &tree->nodes[index];
        if (parent == node->parent && n == node->length && !memcmp(name, node->name, n))
            return directory == node->directory ? index : NUP_NONE;
        slot = (slot + 1) & tree->map_mask;
    }
    if (tree->count == tree->capacity) return NUP_NONE;
    uint32_t *list = directory ? &tree->nodes[parent].child : &tree->nodes[parent].file;
    const uint32_t index = tree->count++;
    tree->nodes[index] = (Node){.name = name, .length = n, .parent = parent, .sibling = *list,
        .child = NUP_NONE, .file = NUP_NONE, .blob = NUP_NONE, .directory = directory};
    *list = index;
    tree->map[slot] = index;
    return index;
}

static bool import_dir(Tree *tree, uint32_t source, uint32_t dest, unsigned depth) {
    NupDir dir;
    if (depth > MAX_DEPTH || !nup_dir(tree->nup, source, &dir)) return false;
    for (uint32_t k = 0; k < dir.count; ++k) {
        if (++tree->visited > tree->capacity) return false;
        uint32_t item = nup_child(tree->nup, dir.first + k);
        if (item < tree->nup->dir_count) {
            NupDir child;
            if (!nup_dir(tree->nup, item, &child) || !child.name_size) return false;
            const uint32_t node = get_node(tree, dest, child.name, child.name_size, true);
            if (node == NUP_NONE || !import_dir(tree, item, node, depth + 1)) return false;
        } else {
            item -= tree->nup->dir_count;
            NupFile file;
            if (!nup_file(tree->nup, item, &file)) return false;
            const uint32_t node = get_node(tree, dest, file.name, file.name_size, false);
            if (node == NUP_NONE) return false;
            tree->nodes[node].blob = item;
        }
    }
    return true;
}

static uint32_t table_at(const Tree *tree, uint32_t node) {
    return node == NUP_NONE ? NUP_NONE : tree->nodes[node].table;
}

static bool build_tables(VirtualRomFs *rom, Tree *tree) {
    uint32_t dir_bytes = 0, file_bytes = 0, dirs = 0, files = 0;
    for (uint32_t i = 0; i < tree->count; ++i) {
        Node *n = &tree->nodes[i];
        uint32_t *bytes = n->directory ? &dir_bytes : &file_bytes;
        n->table = *bytes;
        *bytes += (n->directory ? 24 : 32) + ((n->length + 3) & ~3u);
        if (n->directory) ++dirs; else ++files;
    }
    if (!files) return false;
    const uint32_t dir_buckets = bucket_count(dirs);
    const uint32_t file_buckets = bucket_count(files);
    const size_t table_size = (size_t)(dir_buckets + file_buckets) * 4 + dir_bytes + file_bytes;
    if (table_size > NUP_TOC_LIMIT) return false;
    rom->tables = calloc(1, table_size);
    rom->spans = calloc(files, sizeof(*rom->spans));
    if (!rom->tables || !rom->spans) return false;
    unsigned char *dir_hash = rom->tables;
    unsigned char *dir_table = dir_hash + dir_buckets * 4;
    unsigned char *file_hash = dir_table + dir_bytes;
    unsigned char *file_table = file_hash + file_buckets * 4;
    memset(dir_hash, 0xff, dir_buckets * 4);
    memset(file_hash, 0xff, file_buckets * 4);
    uint64_t data_size = 0;
    for (uint32_t i = 0; i < tree->count; ++i) {
        const Node *n = &tree->nodes[i];
        const uint32_t parent = table_at(tree, n->parent);
        const uint32_t hash = name_hash(parent, n->name, n->length);
        unsigned char *entry, *bucket;
        if (n->directory) {
            entry = dir_table + n->table;
            bucket = dir_hash + (hash % dir_buckets) * 4;
            iz_put32(entry + 8, table_at(tree, n->child));
            iz_put32(entry + 12, table_at(tree, n->file));
            iz_put32(entry + 16, iz_le32(bucket));
            iz_put32(entry + 20, n->length);
            memcpy(entry + 24, n->name, n->length);
        } else {
            NupFile file;
            if (!nup_file(tree->nup, n->blob, &file) || file.size > INT64_MAX) return false;
            data_size = (data_size + 15) & ~UINT64_C(15);
            if (!iz_span(data_size, file.size, INT64_MAX - DATA_START - table_size - 32)) return false;
            entry = file_table + n->table;
            bucket = file_hash + (hash % file_buckets) * 4;
            iz_put64(entry + 8, data_size);
            iz_put64(entry + 16, file.size);
            iz_put32(entry + 24, iz_le32(bucket));
            iz_put32(entry + 28, n->length);
            memcpy(entry + 32, n->name, n->length);
            if (file.size) rom->spans[rom->count++] = (Span){DATA_START + data_size, file};
            data_size += file.size;
        }
        iz_put32(entry, parent);
        iz_put32(entry + 4, table_at(tree, n->sibling));
        iz_put32(bucket, n->table);
    }
    rom->tables_at = DATA_START + ((data_size + 15) & ~UINT64_C(15));
    rom->size = rom->tables_at + table_size;
    const uint64_t header[] = {80, rom->tables_at, dir_buckets * 4,
        rom->tables_at + (uint64_t)(dir_table - rom->tables), dir_bytes,
        rom->tables_at + (uint64_t)(file_hash - rom->tables), file_buckets * 4,
        rom->tables_at + (uint64_t)(file_table - rom->tables), file_bytes, DATA_START};
    for (size_t i = 0; i < sizeof(header) / sizeof(*header); ++i) iz_put64(rom->header + i * 8, header[i]);
    return true;
}

void virtual_romfs_close(VirtualRomFs *rom) {
    if (!rom) return;
    free(rom->tables);
    free(rom->spans);
    free(rom->input);
    free(rom->block);
    free(rom->raw);
    free(rom->workspace);
    free(rom);
}

VirtualRomFs *virtual_romfs_open(const Nup *nup, const NupPackage *const *layers,
                                size_t count, const char **error) {
    const uint32_t capacity = nup->dir_count + nup->file_count + 1;
    uint32_t map_size = 2;
    while (map_size < 2 * capacity) map_size *= 2;
    Tree tree = {.nup = nup, .capacity = capacity, .map_mask = map_size - 1, .count = 1};
    tree.nodes = calloc(capacity, sizeof(*tree.nodes));
    tree.map = calloc(map_size, sizeof(*tree.map));
    VirtualRomFs *rom = calloc(1, sizeof(*rom));
    const char *reason = "Not enough memory to index the game files.";
    if (!rom || !tree.nodes || !tree.map) goto fail;
    rom->nup = nup;
    tree.nodes[0] = (Node){.name = "", .child = NUP_NONE, .file = NUP_NONE,
        .sibling = NUP_NONE, .directory = true};
    reason = "Invalid or unsupported RomFS tree in the NUP.";
    for (size_t i = 0; i < count; ++i)
        if (layers[i]->romfs != NUP_NONE && !import_dir(&tree, layers[i]->romfs, 0, 0)) goto fail;
    if (!build_tables(rom, &tree)) goto fail;
    free(tree.nodes);
    free(tree.map);
    tree.nodes = NULL;
    tree.map = NULL;

    reason = "Not enough memory for the RomFS read buffers.";
    rom->input = malloc(IO_BLOCK);
    rom->block = malloc(IO_BLOCK);
    rom->raw = malloc(IO_BLOCK);
    if (!rom->input || !rom->block || !rom->raw) goto fail;
    for (size_t k = 0; k < rom->count; ++k) {
        if (!rom->spans[k].file.codec) continue;
        const size_t size = ZSTD_estimateDStreamSize(MAX_WINDOW);
        if (ZSTD_isError(size) || !(rom->workspace = malloc(size))) goto fail;
        rom->decoder = ZSTD_initStaticDStream(rom->workspace, size);
        if (!rom->decoder) goto fail;
        break;
    }
    return rom;

fail:
    free(tree.nodes);
    free(tree.map);
    virtual_romfs_close(rom);
    if (error) *error = reason;
    return NULL;
}

uint64_t virtual_romfs_size(const VirtualRomFs *rom) {
    return rom ? rom->size : 0;
}

bool romfs_image_fits(const unsigned char header[80], uint64_t size) {
    const uint64_t data = iz_le64(header + 72);
    if (iz_le64(header) != 80 || data < 80 || data > size) return false;
    for (unsigned i = 8; i < 72; i += 16) {
        const uint64_t offset = iz_le64(header + i);
        if (offset < data || !iz_span(offset, iz_le64(header + i + 8), size)) return false;
    }
    return true;
}

static size_t min_size(uint64_t a, size_t b) { return a < b ? (size_t)a : b; }

static bool decode_block(VirtualRomFs *rom, const Span *span, uint64_t offset) {
    if (rom->current == span && offset >= rom->cached_at && offset - rom->cached_at < rom->cached_bytes)
        return true;
    if (rom->current != span || offset < rom->decoded) {
        rom->current = span;
        rom->compressed_at = rom->decoded = rom->cached_bytes = 0;
        rom->in = (ZSTD_inBuffer){0};
        if (ZSTD_isError(ZSTD_initDStream(rom->decoder)) ||
            ZSTD_isError(ZSTD_DCtx_setMaxWindowSize(rom->decoder, MAX_WINDOW))) return false;
    }
    const NupFile *file = &span->file;
    while (rom->decoded <= offset) {
        ZSTD_outBuffer out = {rom->block, min_size(file->size - rom->decoded, IO_BLOCK), 0};
        while (out.pos < out.size) {
            if (rom->in.pos == rom->in.size && rom->compressed_at < file->stored) {
                const size_t bytes = min_size(file->stored - rom->compressed_at, IO_BLOCK);
                if (!rom->nup->source.read(rom->nup->source.user,
                        file->offset + rom->compressed_at, rom->input, bytes)) return false;
                rom->compressed_at += bytes;
                rom->in = (ZSTD_inBuffer){rom->input, bytes, 0};
            }
            const size_t old_in = rom->in.pos, old_out = out.pos;
            const size_t status = ZSTD_decompressStream(rom->decoder, &out, &rom->in);
            if (ZSTD_isError(status) || (old_in == rom->in.pos && old_out == out.pos)) return false;
        }
        rom->cached_at = rom->decoded;
        rom->cached_bytes = out.pos;
        rom->decoded += out.pos;
    }
    return true;
}

static bool payload(VirtualRomFs *rom, const Span *span, uint64_t offset,
                    unsigned char *out, size_t bytes) {
    const NupFile *file = &span->file;
    if (!file->codec) {
        while (bytes) {
            const size_t take = min_size(bytes, IO_BLOCK);
            if (!rom->nup->source.read(rom->nup->source.user, file->offset + offset, rom->raw, take))
                return false;
            memcpy(out, rom->raw, take);
            out += take;
            offset += take;
            bytes -= take;
        }
        return true;
    }
    while (bytes) {
        if (!decode_block(rom, span, offset)) {
            rom->current = NULL;
            return false;
        }
        const size_t skip = (size_t)(offset - rom->cached_at);
        const size_t take = min_size(rom->cached_bytes - skip, bytes);
        memcpy(out, rom->block + skip, take);
        out += take;
        offset += take;
        bytes -= take;
    }
    return true;
}

bool virtual_romfs_read(VirtualRomFs *rom, uint64_t offset, void *output, size_t length) {
    if (!rom || !iz_span(offset, length, rom->size)) return false;
    unsigned char *out = output;
    while (length) {
        size_t take;
        if (offset < DATA_START) {
            take = min_size(DATA_START - offset, length);
            memcpy(out, rom->header + offset, take);
        } else if (offset >= rom->tables_at) {
            take = min_size(rom->size - offset, length);
            memcpy(out, rom->tables + (offset - rom->tables_at), take);
        } else {
            size_t lo = 0, hi = rom->count;
            while (lo < hi) {
                const size_t mid = lo + (hi - lo) / 2;
                if (rom->spans[mid].offset + rom->spans[mid].file.size <= offset) lo = mid + 1;
                else hi = mid;
            }
            const Span *span = lo < rom->count ? &rom->spans[lo] : NULL;
            if (!span || offset < span->offset) {
                take = min_size((span ? span->offset : rom->tables_at) - offset, length);
                memset(out, 0, take);
            } else {
                const uint64_t within = offset - span->offset;
                take = min_size(span->file.size - within, length);
                if (!payload(rom, span, within, out, take)) return false;
            }
        }
        offset += take;
        out += take;
        length -= take;
    }
    return true;
}
