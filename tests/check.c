#define _FILE_OFFSET_BITS 64
#include "nup.h"
#include "nup_parts.h"
#include "nso.h"
#include "settings.h"
#include "virtual_romfs.h"
#include "game_memory.h"

#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

static bool read_at(void *context, uint64_t at, void *data, size_t size) {
    FILE *file = context;
    return at <= INT64_MAX && !fseeko(file, (off_t)at, SEEK_SET) &&
        fread(data, 1, size, file) == size;
}

static bool hash_matches(const void *data, size_t bytes, const unsigned char *expected) {
    unsigned char digest[32];
    return SHA256(data, bytes, digest) && !memcmp(digest, expected, sizeof(digest));
}

static void expect(bool ok, const char *description) {
    if (!ok) {
        fprintf(stderr, "%s\n", description);
        exit(1);
    }
}

static void part_name_checks(void) {
    const struct { const char *first, *later; unsigned part; bool bare; } cases[] = {
        {"sdmc:/Games/Example.nup.001", "sdmc:/Games/Example.nup.002", 1, false},
        {"sdmc:/Games/Example.nup.00", "sdmc:/Games/Example.nup.01", 1, false},
        {"dump.001", "dump.003", 2, false},
        {"Example.nup.1", "Example.nup.10", 9, false},
        {"Example.nup.part1", "Example.nup.part10", 9, false},
        {"Example.nup.aa", "Example.nup.ba", 26, false},
        {"Example.NUP.AAA", "Example.NUP.AAC", 2, false},
        {"Example.nu0", "Example.nu9", 9, false},
        {"Example.part1.nup", "Example.part2.nup", 1, false},
        {"Example.0001.NUP", "Example.0002.NUP", 1, false},
        {"Games/Example/00", "Games/Example/02", 2, true},
        {"Games/Example/001", "Games/Example/002", 1, true},
    };
    NupPartName name;
    char path[256];
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        expect(nup_part_start(cases[i].first, &name) && name.bare == cases[i].bare, cases[i].first);
        expect(nup_part_path(cases[i].first, &name, cases[i].part, path, sizeof(path)) &&
               !strcmp(path, cases[i].later), cases[i].later);
    }
    const char *invalid[] = {"Game.nup", "Game.nup.002", "Game.nup.ab", "Game.02.nup", "Game.nu2", "Game.gif", ".00"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
        expect(!nup_part_start(invalid[i], &name), invalid[i]);
    expect(nup_part_start("Example.nu1", &name) &&
           !nup_part_path("Example.nu1", &name, 9, path, sizeof(path)), ".nu9 is the last single-digit part");
    expect(!nup_part_path("Example.nu1", &name, 1, path, 4), "reject a truncated path");
    puts("Validated split filename formats and consecutive numbering.");
}

static bool read_slice(void *context, unsigned part, uint64_t offset, void *data, size_t bytes) {
    const uint64_t boundaries[] = {0, 31, 131, 232};
    return read_at(context, boundaries[part] + offset, data, bytes);
}

static bool read_memory(void *context, uint64_t offset, void *out, size_t size) {
    if (size > 256 * 1024) return false;
    memcpy(out, (const unsigned char *)context + offset, size);
    return true;
}

static void compressed_file_checks(void) {
    enum { BLOCK = 512 * 1024, DECODED = 2 * BLOCK };
    unsigned char *plain = malloc(DECODED), *out = malloc(DECODED);
    const size_t bound = ZSTD_compressBound(BLOCK);
    unsigned char *packed = malloc(2 * bound);
    expect(plain && out && packed, "allocate streaming fixture");
    uint32_t random = 1;
    for (size_t i = 0; i < DECODED; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        plain[i] = (unsigned char)random;
    }
    const size_t first = ZSTD_compress(packed, bound, plain, BLOCK, 1);
    expect(!ZSTD_isError(first) && first > 256 * 1024, "compress first large frame");
    const size_t second = ZSTD_compress(packed + first, bound, plain + BLOCK, BLOCK, 1);
    expect(!ZSTD_isError(second), "compress second large frame");
    const Nup nup = {.source = {read_memory, NULL, packed, first + second}};
    NupFile file = {.codec = 1, .size = DECODED, .stored = first + second};
    expect(nup_read_file(&nup, &file, out, DECODED) && !memcmp(plain, out, DECODED),
           "stream concatenated frames through bounded reads");
    --file.stored;
    expect(!nup_read_file(&nup, &file, out, DECODED), "reject truncated compressed frame");
    file.stored = first + second;
    file.size = DECODED - 1;
    expect(!nup_read_file(&nup, &file, out, DECODED - 1), "reject undersized output");
    free(packed);
    free(out);
    free(plain);
    puts("Validated bounded Zstandard reads, concatenated frames and truncated data.");
}

static void fixture_checks(VirtualRomFs *rom, const unsigned char header[80]) {
    const uint64_t table_at = iz_le64(header + 56), table_size = iz_le64(header + 64);
    const uint64_t payload_at = iz_le64(header + 72);
    unsigned char table[160];
    expect(table_size <= sizeof(table), "fixture file table too big");
    expect(virtual_romfs_read(rom, table_at, table, (size_t)table_size), "read file table");
    bool base = false, replaced = false, empty = false;
    for (size_t at = 0; at < table_size;) {
        const size_t names = iz_le32(table + at + 28);
        const uint64_t offset = iz_le64(table + at + 8) + payload_at;
        if (names == 8 && !memcmp(table + at + 32, "keep.bin", names)) {
            char out[18];
            expect(iz_le64(table + at + 16) == sizeof(out), "base size");
            expect(virtual_romfs_read(rom, offset, out, sizeof(out)), "read base payload");
            expect(!memcmp(out, "keep the base file", sizeof(out)), "base content");
            base = true;
        } else if (names == 10 && !memcmp(table + at + 32, "shared.bin", names)) {
            expect(iz_le64(table + at + 16) == 900000, "update must replace base entry");
            const size_t positions[] = {510000, 780000, 1023, 262140, 0};
            for (size_t k = 0; k < sizeof(positions) / sizeof(*positions); ++k) {
                unsigned char out[64];
                const size_t start = positions[k];
                expect(virtual_romfs_read(rom, offset + start, out, sizeof(out)), "zstd seek read");
                for (size_t i = 0; i < sizeof(out); ++i) {
                    const size_t pos = start + i;
                    expect(out[i] == (unsigned char)((pos * 13 + pos / 251) % 256), "zstd seek bytes");
                }
            }
            replaced = true;
        } else if (names == 9 && !memcmp(table + at + 32, "empty.csv", names)) {
            expect(iz_le64(table + at + 16) == 0, "preserve empty RomFS file");
            empty = true;
        }
        at += 32 + ((names + 3) & ~3u);
    }
    expect(base && replaced && empty, "fixture file inventory");
    unsigned char outside;
    expect(!virtual_romfs_read(rom, virtual_romfs_size(rom), &outside, 1), "out-of-range read");
    puts("Validated base/update overlay, raw reads, cross-block and backwards Zstandard reads.");
}

static void addon_checks(NupRomSet *roms, const Nup *nup, const NupPackage *const *layers, size_t count) {
    expect(roms->addons == 2 && roms->indices[0] == 1 && roms->indices[1] == 3,
           "enumerate unique sorted DLC indices and skip a zero index");
    const IzGameMemory memory = iz_game_memory(nup, layers, count, layers[count - 1]);
    expect(memory.remote_mib == 1536 && memory.system_kib == 8192, "read a packed 96 KiB main.npdm");

    unsigned char header[80], data[64];
    expect(virtual_romfs_read(roms->images[0], 0, header, sizeof(header)), "read base header");
    const uint64_t base_payload = iz_le64(header + 72) + 32;
    expect(virtual_romfs_read(roms->images[1], 0, header, sizeof(header)), "read DLC header");
    const uint64_t payload = iz_le64(header + 72);
    const uint64_t positions[] = {279901, 512, 262140};
    for (size_t i = 0; i < sizeof(positions) / sizeof(*positions); ++i) {
        const uint64_t position = positions[i];
        expect(virtual_romfs_read(roms->images[1], payload + position, data, sizeof(data)), "read compressed DLC");
        for (size_t j = 0; j < sizeof(data); ++j)
            expect(data[j] == (unsigned char)(((position + j) * 7 + 31) % 251), "new DLC layer takes precedence");
        // Alternate two compressed images on the same decode workspace.
        expect(virtual_romfs_read(roms->images[0], base_payload + position, data, sizeof(data)),
               "read base image after DLC");
        for (size_t j = 0; j < sizeof(data); ++j) {
            const uint64_t at = position + j;
            expect(data[j] == (unsigned char)((at * 13 + at / 251) % 256), "base decoder state stays isolated");
        }
    }
    expect(virtual_romfs_read(roms->images[2], 0, header, sizeof(header)) && iz_le64(header + 64) == 0,
           "valid empty DLC image");

    NupRomSet disabled = {0};
    const char *error = NULL;
    expect(nup_rom_set_open(&disabled, nup, layers, count, false, &error), error);
    expect(!disabled.addons && disabled.addons_disabled && !disabled.images[1] &&
           virtual_romfs_size(disabled.images[0]) == virtual_romfs_size(roms->images[0]),
           "disable DLC without changing the base image");
    nup_rom_set_close(&disabled);
    puts("Validated DLC overlay, empty DLC, the shared reader, DLC off, and packed NPDM metadata.");
}

static void memory_profile_checks(void) {
    const NupSource source = {0};
    unsigned char npdm[0x80] = "META";
    IzGameMemory profile = iz_image_game_memory(source, npdm, sizeof(npdm));
    expect(profile.heap_mib == 8992 && profile.slack_mib == 64 && !profile.system_kib,
           "use the higher heap profile for a verified zero system resource");
    profile = iz_image_game_memory(source, npdm, 24);
    expect(profile.heap_mib == 8192 && profile.slack_mib == 400,
           "keep the default heap profile for an incomplete NPDM header");
    profile = iz_image_game_memory(source, NULL, 0);
    expect(profile.heap_mib == 8192 && profile.slack_mib == 400,
           "keep the default heap profile for missing NPDM metadata");
    iz_put32(npdm + 20, 512);
    profile = iz_image_game_memory(source, npdm, sizeof(npdm));
    expect(profile.heap_mib == 8192 && profile.slack_mib == 400 && !profile.system_kib,
           "a nonzero byte count must not use the zero-system-resource profile");
}

static void invalid_index_checks(Nup *nup) {
    const char *error = NULL;
    for (uint32_t i = 0; i < nup->dir_count; ++i) {
        NupDir dir;
        expect(nup_dir(nup, i, &dir), "valid directory metadata");
        if (!dir.count) continue;
        unsigned char *child = nup->table + nup->children_at + 4 * dir.first;
        const uint32_t previous = iz_le32(child);
        iz_put32(child, i);
        expect(!nup_check_index(nup, &error) && strstr(error, "cycle"), "reject an index with a directory cycle");
        iz_put32(child, NUP_NONE);
        expect(!nup_check_index(nup, &error) && strstr(error, "folder entry"), "reject an invalid child index");
        iz_put32(child, previous);
        break;
    }
    expect(nup->file_count, "fixture has indexed files");
    unsigned char *codec = nup->table + nup->files_at + 64;
    const unsigned char previous = *codec;
    *codec = 255;
    expect(!nup_check_index(nup, &error) && strstr(error, "file or compression"), "reject invalid file metadata");
    *codec = previous;
    unsigned char *offset = nup->table + nup->files_at + 16;
    const uint64_t stored_offset = iz_le64(offset);
    iz_put64(offset, 0);
    expect(!nup_check_index(nup, &error) && strstr(error, "file or compression"),
           "reject a nonempty file with no data offset");
    iz_put64(offset, stored_offset);
}

int main(int argc, char **argv) {
    expect(argc == 2 || (argc == 3 && !strcmp(argv[2], "fixture")), "usage: check FILE.nup [fixture]");
    memory_profile_checks();
    expect(iz_room_for_exefs(384, 344) == 512 && iz_room_for_exefs(512, 344) == 512 &&
           iz_room_for_exefs(512, 352) == 520 && iz_room_for_exefs(768, 344) == 768 &&
           iz_room_for_exefs(384, 0) == 384 && iz_room_for_exefs(384, IZ_EXEFS_MAX_MIB) == IZ_ROOM_MAX_MIB,
           "increase Loader room only as needed for the ExeFS budget");
    expect(iz_bytes_to_mib_ceil(UINT64_C(337952768)) == 323, "round the needed budget up");
    const NupFile oversized = {.size = UINT64_C(97) << 20, .stored = UINT64_C(32) << 20};
    const uint64_t auto_limit = (uint64_t)IZ_EXEFS_MIN_MIB << 20;
    IzExefsFailure diagnostic = {0};
    const char *diagnostic_reason = NULL;
    Nx2Module ignored_module;
    Nx2Image ignored_image;
    expect(!iz_nso_prepare(NULL, &oversized, "main", auto_limit, auto_limit,
                           &ignored_module, &ignored_image, &diagnostic_reason, &diagnostic) &&
           diagnostic.check == IZ_EXEFS_CHECK_FILE && diagnostic.required == oversized.size &&
           diagnostic.available == auto_limit && !strcmp(diagnostic_reason, IZ_EXEFS_BUDGET_ERROR),
           "report the exact file-size ExeFS limit");
    FILE *file = fopen(argv[1], "rb");
    expect(file && !fseeko(file, 0, SEEK_END), "cannot open test NUP");
    const off_t size = ftello(file);
    expect(size > 0, "invalid NUP size");
    const char *error = NULL;
    Nup nup;
    NupSource source = {read_at, hash_matches, file, (uint64_t)size};
    NupParts parts = {.read = read_slice, .user = file};
    if (argc == 3) {
        part_name_checks();
        compressed_file_checks();
        NupPreview preview;
        NupSource incomplete = source;
        incomplete.size = 128;
        expect(nup_preview(incomplete, &preview) && preview.incomplete, "preview a NUP with missing parts");
        expect(!nup_open(&nup, incomplete, &error) && !strcmp(error, NUP_INCOMPLETE_MESSAGE),
               "report incomplete NUP before opening its table");
        const uint64_t part_sizes[] = {31, 100, 101, (uint64_t)size - 232};
        expect(size > 232, "split fixture size");
        for (size_t i = 0; i < sizeof(part_sizes) / sizeof(*part_sizes); ++i)
            expect(nup_parts_append(&parts, part_sizes[i]), "append fixture part");
        source = (NupSource){nup_parts_read, hash_matches, &parts, nup_parts_size(&parts)};
        expect(nup_preview(source, &preview) && !preview.incomplete, "read NUP header and table across parts");
    }
    const bool opened = nup_open(&nup, source, &error);
    expect(opened, error);
    const bool index_ok = nup_check_index(&nup, &error);
    expect(index_ok, error);
    if (argc == 3) invalid_index_checks(&nup);
    const NupPackage *layers[NUP_PACKAGE_LIMIT];
    const size_t count = nup_layers(&nup, layers);
    NupRomSet roms = {0};
    expect(nup_rom_set_open(&roms, &nup, layers, count, true, &error), error);
    VirtualRomFs *rom = roms.images[0];
    unsigned char header[80];
    expect(virtual_romfs_read(rom, 0, header, sizeof(header)) && iz_le64(header) == 80, "invalid RomFS header");
    printf("%u packages, %u archive entries, virtual RomFS: %llu bytes\n",
           nup.package_count, nup.file_count, (unsigned long long)virtual_romfs_size(rom));
    if (argc == 3 && !strcmp(argv[2], "fixture")) {
        fixture_checks(rom, header);
        addon_checks(&roms, &nup, layers, count);
    } else {
        const char *names[] = {"main","subsdk0","subsdk1","subsdk2","subsdk3",
            "subsdk4","subsdk5","subsdk6","subsdk7","subsdk8","subsdk9","sdk"};
        const NupPackage *package = NULL;
        for (size_t i = 0; i < count; ++i)
            if (nup_find_file(&nup, layers[i]->exefs, "main") != NUP_NONE) package = layers[i];
        expect(package, "no program in archive");
        const IzGameMemory memory = iz_game_memory(&nup, layers, count, package);
        printf("Memory: %u MiB reported, %u MiB apart, %u MiB slack; %u MiB graphics, %u KiB system\n",
               memory.heap_mib, memory.apart_mib, memory.slack_mib, memory.remote_mib, memory.system_kib);
        uint64_t footprint = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
            const uint32_t index = nup_find_file(&nup, package->exefs, names[i]);
            if (index == NUP_NONE) continue;
            NupFile module_file;
            Nx2Image image;
            Nx2Module module;
            expect(nup_file(&nup, index, &module_file), "invalid module entry");
            const uint64_t budget = (uint64_t)IZ_EXEFS_MAX_MIB << 20;
            const bool ready = iz_nso_prepare(&nup, &module_file, names[i], budget, budget,
                                              &module, &image, &error, NULL);
            expect(ready, error);
            footprint += iz_nso_mapping_bytes(&module, &image);
            printf("%s: %llu decoded bytes, %llu mapped bytes, %u calls\n", names[i],
                   (unsigned long long)image.image_size, (unsigned long long)module.end, image.nsvcs);
            iz_nso_close(&image);
        }
        printf("Staging: %llu bytes\n", (unsigned long long)footprint);
    }
    nup_rom_set_close(&roms);
    nup_close(&nup);
    fclose(file);
    return 0;
}
