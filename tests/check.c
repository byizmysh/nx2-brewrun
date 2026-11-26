#define _FILE_OFFSET_BITS 64
#include "nup.h"
#include "nso.h"
#include "virtual_romfs.h"
#include "game_memory.h"

#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void fixture_checks(VirtualRomFs *rom, const unsigned char header[80]) {
    const uint64_t table_at = iz_le64(header + 56), table_size = iz_le64(header + 64);
    const uint64_t payload_at = iz_le64(header + 72);
    unsigned char table[128];
    expect(table_size <= sizeof(table), "fixture file table too big");
    expect(virtual_romfs_read(rom, table_at, table, (size_t)table_size), "read file table");
    bool base = false, replaced = false;
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
        }
        at += 32 + ((names + 3) & ~3u);
    }
    expect(base && replaced, "fixture file inventory");
    unsigned char outside;
    expect(!virtual_romfs_read(rom, virtual_romfs_size(rom), &outside, 1), "out-of-range read");
    puts("Validated base/update overlay, raw reads, cross-block and backwards Zstandard reads.");
}

int main(int argc, char **argv) {
    expect(argc == 2 || (argc == 3 && !strcmp(argv[2], "fixture")), "usage: check FILE.nup [fixture]");
    FILE *file = fopen(argv[1], "rb");
    expect(file && !fseeko(file, 0, SEEK_END), "cannot open test NUP");
    const off_t size = ftello(file);
    expect(size > 0, "invalid NUP size");
    const char *error = NULL;
    Nup nup;
    const bool opened = nup_open(&nup, (NupSource){read_at, hash_matches, file, (uint64_t)size}, &error);
    expect(opened, error);
    const NupPackage *layers[NUP_PACKAGE_LIMIT];
    const size_t count = nup_layers(&nup, layers);
    VirtualRomFs *rom = virtual_romfs_open(&nup, layers, count, &error);
    expect(rom, error);
    unsigned char header[80];
    expect(virtual_romfs_read(rom, 0, header, sizeof(header)) && iz_le64(header) == 80, "invalid RomFS header");
    printf("%u packages, %u archive entries, virtual RomFS: %llu bytes\n",
           nup.package_count, nup.file_count, (unsigned long long)virtual_romfs_size(rom));
    if (argc == 3 && !strcmp(argv[2], "fixture")) {
        fixture_checks(rom, header);
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
            const bool ready = iz_nso_prepare(&nup, &module_file, names[i], &module, &image, &error);
            expect(ready, error);
            footprint += module.end + image.image_size;
            printf("%s: %llu decoded bytes, %llu mapped bytes, %u calls\n", names[i],
                   (unsigned long long)image.image_size, (unsigned long long)module.end, image.nsvcs);
            iz_nso_close(&image);
        }
        printf("Staging: %llu bytes\n", (unsigned long long)footprint);
    }
    virtual_romfs_close(rom);
    nup_close(&nup);
    fclose(file);
    return 0;
}
