/* HeapDef table selection and formats follow nx2-relink's tools/heapdef.py (MIT, Olmectron). */
#include "game_memory.h"
#include "virtual_romfs.h"

#include <stdlib.h>
#include <string.h>
#include <zstd.h>

enum { METADATA_LIMIT = 8 << 20, CONSOLE_MIB = 8992 };

typedef struct {
    unsigned char *data;
    size_t size;
} Blob;

typedef struct {
    const unsigned char *data;
    size_t size;
    bool little;
} View;

static uint32_t number(View view, size_t at, unsigned width) {
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value = (value << 8) | view.data[at + (view.little ? width - 1 - i : i)];
    return value;
}

static bool text_at(View view, uint64_t at, const char *text) {
    const size_t size = strlen(text) + 1;
    return iz_span(at, size, view.size) && !memcmp(view.data + at, text, size);
}

static bool byml_map(View view, uint32_t dict, uint32_t *count) {
    if (!iz_span(dict, 4, view.size) || view.data[dict] != 0xc1) return false;
    *count = number(view, dict + 1, 3);
    return iz_span((uint64_t)dict + 4, 8ull * *count, view.size);
}

static unsigned byml_value(View view, uint32_t keys, uint32_t dict, const char *name, uint32_t *out) {
    uint32_t count;
    if (!byml_map(view, dict, &count)) return 0;
    const uint32_t key_count = number(view, keys + 1, 3);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t entry = (size_t)dict + 4 + 8 * i;
        const uint32_t key = number(view, entry, 3);
        if (key >= key_count) continue;
        const uint64_t offset = (uint64_t)keys + 4 + 4ull * key;
        if (!iz_span(offset, 4, view.size) ||
            !text_at(view, (uint64_t)keys + number(view, offset, 4), name)) continue;
        *out = number(view, entry + 4, 4);
        return view.data[entry + 3];
    }
    return 0;
}

static uint64_t byml_size_kib(View view, uint32_t keys, uint32_t heap) {
    uint32_t value;
    const unsigned kind = byml_value(view, keys, heap, "Size", &value);
    if (kind == 0xd1 || kind == 0xd3) return value;
    /* Negative sizes denote automatic heaps and exceed the usable range as unsigned values. */
    if ((kind == 0xd4 || kind == 0xd5) && iz_span(value, 8, view.size)) {
        const size_t low = view.little ? value : (size_t)value + 4;
        const size_t high = view.little ? (size_t)value + 4 : value;
        return number(view, low, 4) | (uint64_t)number(view, high, 4) << 32;
    }
    return 0;
}

static unsigned byml_fixed_apart(View view, uint32_t keys, uint32_t heaps) {
    uint32_t count;
    if (!byml_map(view, heaps, &count)) return 0;
    const uint32_t key_count = number(view, keys + 1, 3);
    if (!iz_span((uint64_t)keys + 4, 4ull * key_count, view.size)) return 0;

    const uint64_t console_kib = (uint64_t)CONSOLE_MIB * 1024;
    uint64_t fixed_kib = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t entry = (size_t)heaps + 4 + 8 * i;
        if (number(view, entry, 3) >= key_count) return 0;
        if (view.data[entry + 3] != 0xc1) continue;
        const uint32_t heap = number(view, entry + 4, 4);
        uint32_t fields, parent;
        if (!byml_map(view, heap, &fields)) return 0;
        if (byml_value(view, keys, heap, "Parent", &parent)) continue;
        const uint64_t size = byml_size_kib(view, keys, heap);
        if (size <= console_kib) fixed_kib += size;
        if (fixed_kib > console_kib) return 0;
    }
    const unsigned fixed_mib = (unsigned)(fixed_kib / 1024);
    if (fixed_mib < 5120) return 0;
    /* Native fixed-heap titles may keep address space aside in code without declaring it in HeapDef. */
    const int remaining = CONSOLE_MIB - (int)fixed_mib - 900;
    return remaining < 1024 ? 1024 : remaining > 1500 ? 1500 : (unsigned)remaining;
}

static bool byml_apart(Blob blob, bool native, unsigned *apart) {
    View view = {blob.data, blob.size, false};
    if (view.size < 16) return false;
    view.little = !memcmp(view.data, "YB", 2);
    if (!view.little && memcmp(view.data, "BY", 2)) return false;
    const uint32_t keys = number(view, 4, 4), root = number(view, 12, 4);
    uint32_t heaps, entry;
    if (!iz_span(keys, 4, view.size) || view.data[keys] != 0xc2 ||
        byml_value(view, keys, root, "Heaps", &heaps) != 0xc1) return false;
    const unsigned virtual_type = byml_value(view, keys, heaps, "VirtualAddressMemory", &entry);
    if (virtual_type == 0xc1) {
        const uint64_t kib = byml_size_kib(view, keys, entry);
        if (kib >= 1024 && kib <= (uint64_t)CONSOLE_MIB * 1024) *apart = (unsigned)(kib / 1024);
    } else if (native && !virtual_type) *apart = byml_fixed_apart(view, keys, heaps);
    return true;
}

static Blob read_blob(const Nup *nup, const NupFile *file) {
    if (!file->size || file->size > METADATA_LIMIT || file->stored > METADATA_LIMIT) return (Blob){0};
    Blob blob = {malloc((size_t)file->size), (size_t)file->size};
    if (blob.data && nup_read_file(nup, file, blob.data, blob.size)) return blob;
    free(blob.data);
    return (Blob){0};
}

static Blob decompress(Blob packed, Blob dict) {
    if (!packed.data) return (Blob){0};
    const unsigned long long size = ZSTD_getFrameContentSize(packed.data, packed.size);
    if (size == ZSTD_CONTENTSIZE_ERROR || (size != ZSTD_CONTENTSIZE_UNKNOWN && size > METADATA_LIMIT))
        return (Blob){0};
    const size_t capacity = size == ZSTD_CONTENTSIZE_UNKNOWN ? METADATA_LIMIT : (size_t)size;
    void *data = malloc(capacity ? capacity : 1);
    if (!data) return (Blob){0};
    size_t decoded;
    if (dict.size) {
        ZSTD_DCtx *ctx = ZSTD_createDCtx();
        if (!ctx) {
            free(data);
            return (Blob){0};
        }
        decoded = ZSTD_decompress_usingDict(ctx, data, capacity, packed.data, packed.size, dict.data, dict.size);
        ZSTD_freeDCtx(ctx);
    } else decoded = ZSTD_decompress(data, capacity, packed.data, packed.size);
    if (!ZSTD_isError(decoded)) return (Blob){data, decoded};
    free(data);
    return (Blob){0};
}

static Blob unpack_table(Blob packed, Blob dictionaries) {
    Blob decoded = decompress(packed, (Blob){0});
    if (decoded.data || dictionaries.size < 0x20 || memcmp(dictionaries.data, "SARC", 4)) return decoded;
    View view = {dictionaries.data, dictionaries.size, dictionaries.data[6] == 0xff};
    const uint32_t data_at = number(view, 0x0c, 4), count = number(view, 0x1a, 2);
    if (!iz_span(0x20, 16ull * count, view.size) || memcmp(view.data + 0x14, "SFAT", 4)) return decoded;
    const unsigned dict_id = ZSTD_getDictID_fromFrame(packed.data, packed.size);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t node = 0x20 + 16 * i;
        const uint32_t first = number(view, node + 8, 4), last = number(view, node + 12, 4);
        const uint64_t start = (uint64_t)data_at + first;
        if (last <= first || !iz_span(start, last - first, view.size)) continue;
        Blob dict = {dictionaries.data + start, last - first};
        if (dict_id && ZSTD_getDictID_fromDict(dict.data, dict.size) != dict_id) continue;
        decoded = decompress(packed, dict);
        if (decoded.data) return decoded;
    }
    return decoded;
}

static uint32_t find_dir(const Nup *nup, uint32_t root, const char *name) {
    NupDir dir;
    if (!nup_dir(nup, root, &dir)) return NUP_NONE;
    const size_t length = strlen(name);
    for (uint32_t i = 0; i < dir.count; ++i) {
        const uint32_t index = nup_child(nup, dir.first + i);
        NupDir child;
        if (nup_dir(nup, index, &child) && child.name_size == length &&
            !memcmp(child.name, name, length)) return index;
    }
    return NUP_NONE;
}

typedef struct {
    NupFile file;
    unsigned layer, priority;
    bool packed;
} HeapTable;

static bool heap_table(HeapTable *table, NupFile file, unsigned layer) {
    char name[NUP_NAME_LIMIT + 1];
    memcpy(name, file.name, file.name_size);
    name[file.name_size] = 0;
    if (!strstr(name, ".HeapDef.byml")) return false;
    const unsigned priority = (strstr(name, "Ounce") ? 0 : 2) + (strstr(name, "Product") ? 0 : 1);
    const bool packed = file.name_size >= 3 && !strcmp(name + file.name_size - 3, ".zs");
    *table = (HeapTable){file, layer, priority, packed};
    return true;
}

static int compare_tables(const void *pa, const void *pb) {
    const HeapTable *a = pa, *b = pb;
    if (a->priority != b->priority) return (int)a->priority - (int)b->priority;
    const uint32_t x = a->file.name_size, y = b->file.name_size;
    const int order = memcmp(a->file.name, b->file.name, x < y ? x : y);
    if (order) return order;
    if (x != y) return (int)x - (int)y;
    return (int)b->layer - (int)a->layer;
}

static unsigned heap_apart(const Nup *nup, HeapTable *tables, size_t used, const NupFile *dict_file) {
    if (!used) return 0;
    qsort(tables, used, sizeof(*tables), compare_tables);
    bool read_dictionaries = false;
    Blob dictionaries = {0};
    unsigned apart = 0;
    const NupFile *previous = NULL;
    for (size_t i = 0; i < used; ++i) {
        const HeapTable *table = &tables[i];
        if (table->priority >= 2 && tables[0].priority < 2) break;
        if (previous && previous->name_size == table->file.name_size &&
            !memcmp(previous->name, table->file.name, previous->name_size)) continue;
        previous = &table->file;
        Blob raw = read_blob(nup, &table->file), byml = raw;
        if (table->packed && raw.data) {
            if (!read_dictionaries) {
                read_dictionaries = true;
                Blob packed = read_blob(nup, dict_file);
                dictionaries = decompress(packed, (Blob){0});
                free(packed.data);
            }
            byml = unpack_table(raw, dictionaries);
            free(raw.data);
        }
        const bool found = byml.data && byml_apart(byml, table->priority < 2, &apart);
        free(byml.data);
        if (found) break;
    }
    free(dictionaries.data);
    return apart;
}

static unsigned nup_heap_apart(const Nup *nup, const NupPackage *const *layers, size_t count) {
    HeapTable tables[64];
    size_t used = 0;
    NupFile dict_file = {0};
    for (size_t i = 0; i < count; ++i) {
        const uint32_t pack = find_dir(nup, layers[i]->romfs, "Pack");
        const uint32_t dict = nup_find_file(nup, pack, "ZsDic.pack.zs");
        if (dict != NUP_NONE) nup_file(nup, dict, &dict_file);
        const uint32_t system = find_dir(nup, layers[i]->romfs, "System");
        const uint32_t heap = find_dir(nup, system, "HeapSize");
        NupDir dir;
        if (!nup_dir(nup, heap, &dir)) continue;
        for (uint32_t j = 0; j < dir.count; ++j) {
            const uint32_t index = nup_child(nup, dir.first + j);
            NupFile file;
            HeapTable table;
            if (index < nup->dir_count || !nup_file(nup, index - nup->dir_count, &file) ||
                !heap_table(&table, file, (unsigned)i)) continue;
            if (used == sizeof(tables) / sizeof(*tables)) return 0;
            tables[used++] = table;
        }
    }
    return heap_apart(nup, tables, used, &dict_file);
}

typedef struct {
    NupSource source;
    uint64_t dir, dir_size, files, files_size, data;
} ImageIndex;

static bool image_record(const ImageIndex *image, bool directory, uint32_t slot,
                         unsigned char *record, char *name) {
    const uint64_t table = directory ? image->dir : image->files;
    const uint64_t size = directory ? image->dir_size : image->files_size;
    const size_t header = directory ? 24 : 32;
    if ((slot & 3) || !iz_span(slot, header, size) ||
        !image->source.read(image->source.user, table + slot, record, header)) return false;
    if (!name) return true;
    const uint32_t length = iz_le32(record + header - 4);
    if (length > NUP_NAME_LIMIT || !iz_span(slot, header + length, size) ||
        !image->source.read(image->source.user, table + slot + header, name, length)) return false;
    name[length] = 0;
    return true;
}

static uint32_t image_child_dir(const ImageIndex *image, uint32_t parent, const char *name) {
    unsigned char record[24];
    char entry[NUP_NAME_LIMIT + 1];
    if (!image_record(image, true, parent, record, NULL)) return NUP_NONE;
    uint32_t slot = iz_le32(record + 8);
    for (unsigned i = 0; i < 4096 && image_record(image, true, slot, record, entry); ++i) {
        if (iz_le32(record) != parent) break;
        if (!strcmp(entry, name)) return slot;
        slot = iz_le32(record + 4);
    }
    return NUP_NONE;
}

static uint32_t image_file_list(const ImageIndex *image, uint32_t dir) {
    unsigned char record[24];
    return image_record(image, true, dir, record, NULL) ? iz_le32(record + 12) : NUP_NONE;
}

static bool image_file(const ImageIndex *image, uint32_t *slot, uint32_t parent,
                       NupFile *file, char name[NUP_NAME_LIMIT + 1]) {
    unsigned char record[32];
    if (!image_record(image, false, *slot, record, name) || iz_le32(record) != parent) return false;
    const uint64_t offset = iz_le64(record + 8), bytes = iz_le64(record + 16);
    if (!iz_span(offset, bytes, image->source.size - image->data)) return false;
    *file = (NupFile){.name = name, .name_size = iz_le32(record + 28),
        .offset = image->data + offset, .stored = bytes, .size = bytes};
    *slot = iz_le32(record + 4);
    return true;
}

static unsigned image_heap_apart(NupSource source) {
    unsigned char header[80];
    if (source.size < sizeof(header) || !source.read(source.user, 0, header, sizeof(header)) ||
        !romfs_image_fits(header, source.size)) return 0;
    const ImageIndex image = {source, iz_le64(header + 24), iz_le64(header + 32),
        iz_le64(header + 56), iz_le64(header + 64), iz_le64(header + 72)};
    const uint32_t system = image_child_dir(&image, 0, "System");
    const uint32_t heap = image_child_dir(&image, system, "HeapSize");
    uint32_t slot = image_file_list(&image, heap);
    HeapTable tables[64];
    char names[64][NUP_NAME_LIMIT + 1];
    size_t used = 0;
    for (unsigned i = 0; i < 4096 && used < sizeof(tables) / sizeof(*tables); ++i) {
        NupFile file;
        if (!image_file(&image, &slot, heap, &file, names[used])) break;
        if (heap_table(&tables[used], file, 0)) ++used;
    }
    if (!used) return 0;
    const uint32_t pack = image_child_dir(&image, 0, "Pack");
    slot = image_file_list(&image, pack);
    NupFile dict = {0};
    char name[NUP_NAME_LIMIT + 1];
    for (unsigned i = 0; i < 4096; ++i) {
        NupFile file;
        if (!image_file(&image, &slot, pack, &file, name)) break;
        if (!strcmp(name, "ZsDic.pack.zs")) { dict = file; break; }
    }
    const Nup view = {.source = source};
    return heap_apart(&view, tables, used, &dict);
}

static IzGameMemory memory_config(unsigned apart, const unsigned char *npdm, size_t size) {
    IzGameMemory memory = {apart ? CONSOLE_MIB : 8192, apart, apart ? 64 : 400, 0, 0};
    if (size >= 24 && !memcmp(npdm, "META", 4)) {
        memory.remote_mib = iz_le32(npdm + 16) >> 20;
        const uint32_t system = iz_le32(npdm + 20);
        memory.system_kib = system >> 10;
        if (size >= 0x80 && !apart && !system) {
            memory.heap_mib = CONSOLE_MIB;
            memory.slack_mib = 64;
        }
    }
    return memory;
}

IzGameMemory iz_game_memory(const Nup *nup, const NupPackage *const *layers, size_t count,
                            const NupPackage *code) {
    enum { NPDM_LIMIT = 1 << 20 };
    const unsigned apart = nup_heap_apart(nup, layers, count);
    const uint32_t index = nup_find_file(nup, code->exefs, "main.npdm");
    NupFile file;
    Blob header = {0};
    if (nup_file(nup, index, &file) && file.size <= NPDM_LIMIT && file.stored <= NPDM_LIMIT)
        header = read_blob(nup, &file);
    const IzGameMemory memory = memory_config(apart, header.data, header.size);
    free(header.data);
    return memory;
}

IzGameMemory iz_image_game_memory(NupSource source, const void *npdm, size_t size) {
    return memory_config(image_heap_apart(source), npdm, size);
}
