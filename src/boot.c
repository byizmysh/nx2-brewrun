#include <switch.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "boot.h"
#include "nso.h"
#include "nup_parts.h"
#include "virtual_romfs.h"

static const char *const module_names[] = {
    "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5",
    "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"
};

Nx2Module nx2_modules[IZ_WHOLE_CAPACITY];
Nx2Image nx2_images[IZ_WHOLE_CAPACITY];
unsigned nx2_module_count;
int nx2_whole;
IzGameMemory iz_memory;

char iz_game_name[sizeof("nup-0000000000000000")];
/* Reserve room for the SD prefix and runtime filenames appended to the folder. */
char iz_home[FS_MAX_PATH - 64];
char iz_dir[FS_MAX_PATH];
char iz_save_dir[FS_MAX_PATH];
char iz_content_home[512];

static const char *const known_paths[] = {
    ".unsupported-library-path", "astc", "bounce", "control", "crash.txt", "debug", "erpt",
    "farthunks", "faulttest", "font.ttf", "hold", "ipcbytes", "ipclog", "log.prev.txt",
    "log.txt", "native", "netwatch", "nfptry", "nogpu", "noplt",
    "nowatch", "nvnlock", "onecore", "ownapplet", "pactest", "pause", "peek", "pin",
    "refuse", "reports", "reports/list.bin", "rocount", "running",
    "said", "settings.cfg", "shaders.bin", "shaders.spv", "silentaudio",
    "skip", "slow", "stackwatch", "stderr.txt", "told", "trace", "traceat", "tracebt",
    "tracein", "try", "vulkan"
};
static char paths[sizeof(known_paths) / sizeof(*known_paths)][FS_MAX_PATH];
static Nup container;
typedef struct {
    NupParts parts;
    FsFile files[NUP_PART_LIMIT];
} SdNup;
static SdNup input;
static VirtualRomFs *rom;
static unsigned rom_busy;

const char *iz_path(const char *name) {
    for (size_t i = 0; i < sizeof(known_paths) / sizeof(*known_paths); ++i)
        if (!strcmp(name, known_paths[i])) return paths[i];
    return paths[0];
}

static bool game_paths(const char *self, uint64_t id, const char **error) {
    const char *filename = self && !strncmp(self, "sdmc:/", 6) ? strrchr(self, '/') : NULL;
    if (!filename || !filename[1]) {
        *error = "Could not find the folder containing the " IZ_NUP_TITLE " NRO.";
        return false;
    }
    const char *parent = self + 5;
    const size_t length = (size_t)(filename - parent);
    if (length + sizeof(iz_game_name) >= sizeof(iz_home)) {
        *error = "The folder containing the " IZ_NUP_TITLE " NRO is too long.";
        return false;
    }
    snprintf(iz_game_name, sizeof(iz_game_name), "nup-%016llx", (unsigned long long)id);
    snprintf(iz_home, sizeof(iz_home), "%.*s/%s", (int)length, parent, iz_game_name);
    snprintf(iz_dir, sizeof(iz_dir), "sdmc:%s", iz_home);
    struct stat status;
    if (mkdir(iz_dir, 0777) && (errno != EEXIST || stat(iz_dir, &status) || !S_ISDIR(status.st_mode))) {
        *error = "Could not create the game's data folder beside the " IZ_NUP_TITLE " NRO.";
        return false;
    }
    strcat(iz_dir, "/");
    snprintf(iz_save_dir, sizeof(iz_save_dir), "%s/save", iz_home);
    for (size_t i = 0; i < sizeof(known_paths) / sizeof(*known_paths); ++i)
        snprintf(paths[i], sizeof(paths[i]), "sdmc:%s/%s", iz_home, known_paths[i]);
    return true;
}

static bool read_part(void *context, unsigned part, uint64_t offset, void *buffer, size_t bytes) {
    SdNup *source = context;
    FsFile *file = &source->files[part];
    unsigned char *out = buffer;
    while (bytes) {
        const size_t step = bytes < (2u << 20) ? bytes : (2u << 20);
        u64 actual = 0;
        if (offset > INT64_MAX || R_FAILED(fsFileRead(file, (s64)offset, out, step, FsReadOption_None, &actual)) ||
            actual != step) return false;
        offset += step;
        out += step;
        bytes -= step;
    }
    return true;
}

static void close_nup(SdNup *source) {
    while (source->parts.count) fsFileClose(&source->files[--source->parts.count]);
}

static bool open_source(SdNup *source, const char *path, unsigned limit) {
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    char part_path[512];
    const size_t length = strlen(path);
    if (!sd || strncmp(path, "sdmc:/", 6) || length >= sizeof(part_path)) return false;
    unsigned first = 0;
    const unsigned digits = limit > 1 ? nup_part_number(path, &first) : 0;
    if (limit > 1 && !digits) return false;
    if (digits == 1 && limit > 10 - first) limit = 10 - first;
    if (digits == 2 && limit > 100 - first) limit = 100 - first;
    memcpy(part_path, path, length + 1);
    source->parts = (NupParts){.read = read_part, .user = source};
    for (unsigned i = 0; i < limit; ++i) {
        if (digits) {
            snprintf(part_path + length - digits, digits + 1, "%0*u", (int)digits, first + i);
        }
        FsFile *file = &source->files[i];
        if (R_FAILED(fsFsOpenFile(sd, part_path + 5, FsOpenMode_Read, file))) break;
        s64 size;
        if (R_FAILED(fsFileGetSize(file, &size)) || size <= 0 ||
            !nup_parts_append(&source->parts, (uint64_t)size)) {
            fsFileClose(file);
            close_nup(source);
            return false;
        }
    }
    return source->parts.count != 0;
}

static bool open_nup(SdNup *source, const char *path) {
    if (!open_source(source, path, 1)) return false;
    if (!nup_part_digits(path)) return true;
    unsigned char header[128];
    const uint64_t size = nup_parts_size(&source->parts);
    const size_t head = size < sizeof(header) ? (size_t)size : sizeof(header);
    if (!nup_parts_read(&source->parts, 0, header, head) ||
        memcmp(header, "NUP\1", head < 4 ? head : 4) ||
        (head == sizeof(header) && iz_le64(header + 64) <= size)) return true;
    close_nup(source);
    return open_source(source, path, NUP_PART_LIMIT);
}

bool iz_probe_nup(const char *path, NupPreview *preview) {
    SdNup source;
    if (!open_nup(&source, path)) return false;
    const NupSource archive = {nup_parts_read, NULL, &source.parts, nup_parts_size(&source.parts)};
    const bool valid = nup_preview(archive, preview);
    if (valid) preview->split = source.parts.count > 1;
    close_nup(&source);
    return valid;
}

bool iz_read_nup_cover(const char *path, uint64_t offset, void *buffer, size_t bytes) {
    SdNup source;
    if (!open_nup(&source, path)) return false;
    const bool read = nup_parts_read(&source.parts, offset, buffer, bytes);
    close_nup(&source);
    return read;
}

static bool open_romfs(SdNup *source, const char *folder) {
    char path[512];
    const size_t n = strlen(folder);
    const int length = snprintf(path, sizeof(path), "%s%sromfs.bin", folder,
                                n && folder[n - 1] == '/' ? "" : "/");
    if (length < 0 || (size_t)length + sizeof(".00") > sizeof(path)) return false;
    if (!open_source(source, path, 1)) {
        memcpy(path + length, ".00", sizeof(".00"));
        if (!open_source(source, path, 16)) return false;
    }
    const uint64_t total = nup_parts_size(&source->parts);
    unsigned char header[80];
    bool valid = nup_parts_read(&source->parts, 0, header, sizeof(header)) &&
        romfs_image_fits(header, total);
    const uint64_t width = source->parts.ends[0];
    for (unsigned i = 1; valid && i < source->parts.count; ++i) {
        const uint64_t part = source->parts.ends[i] - source->parts.ends[i - 1];
        valid = part <= width && (i + 1 == source->parts.count || part == width);
    }
    if (!valid) close_nup(source);
    return valid;
}

static bool open_exefs(SdNup *source, const char *folder, const char *name) {
    char path[512];
    const size_t n = strlen(folder);
    const int length = snprintf(path, sizeof(path), "%s%sexefs/%s", folder,
                                n && folder[n - 1] == '/' ? "" : "/", name);
    return length > 0 && (size_t)length < sizeof(path) && open_source(source, path, 1);
}

static uint64_t read_npdm(const char *folder, unsigned char header[128]) {
    SdNup source;
    if (!open_exefs(&source, folder, "main.npdm")) return 0;
    unsigned char aci[24];
    const bool valid = nup_parts_read(&source.parts, 0, header, 128) &&
        !memcmp(header, "META", 4) &&
        nup_parts_read(&source.parts, iz_le32(header + 0x70), aci, sizeof(aci)) &&
        !memcmp(aci, "ACI0", 4);
    close_nup(&source);
    return valid ? iz_le64(aci + 16) & ~UINT64_C(0xfff) : 0;
}

bool iz_probe_loose(const char *folder, uint64_t *size, uint64_t *title_id) {
    SdNup source;
    if (!open_romfs(&source, folder)) return false;
    *size = nup_parts_size(&source.parts);
    close_nup(&source);
    *title_id = 0;
    unsigned char header[128], nso[4];
    const uint64_t id = read_npdm(folder, header);
    if (id && open_exefs(&source, folder, "main")) {
        if (nup_parts_read(&source.parts, 0, nso, sizeof(nso)) && !memcmp(nso, "NSO0", 4))
            *title_id = id;
        close_nup(&source);
    }
    return true;
}

static bool sha256_matches(const void *bytes, size_t size, const unsigned char *expected) {
    unsigned char digest[32];
    sha256CalculateHash(digest, bytes, size);
    return !memcmp(digest, expected, sizeof(digest));
}

static void clear_game(void) {
    virtual_romfs_close(rom);
    rom = NULL;
    for (unsigned i = 0; i < nx2_module_count; ++i) iz_nso_close(&nx2_images[i]);
    nx2_module_count = nx2_whole = 0;
    nup_close(&container);
    close_nup(&input);
}

static bool add_module(const Nup *nup, const NupFile *file, const char *name,
                       uint64_t *footprint, const char **error) {
    if (nx2_module_count >= IZ_WHOLE_CAPACITY) {
        *error = "The game needs more whole module slots than this build supports.";
        return false;
    }
    const unsigned j = nx2_module_count;
    if (!iz_nso_prepare(nup, file, name, &nx2_modules[j], &nx2_images[j], error)) return false;
    ++nx2_module_count;
    *footprint += nx2_modules[j].end + nx2_images[j].image_size;
    if (*footprint > (216u << 20)) {
        *error = "The game's modules exceed this build's staging memory budget.";
        return false;
    }
    return true;
}

static bool prepare_loose(const char *self, const char *path, const char **error) {
    unsigned char header[128];
    SdNup source;
    const uint64_t id = read_npdm(path, header);
    if (!id) {
        *error = "This game folder needs its original exefs/main.npdm.";
        return false;
    }
    if (!open_romfs(&source, path)) {
        *error = "The game's RomFS image is incomplete or unreadable.";
        return false;
    }
    const NupSource image = {nup_parts_read, NULL, &source.parts, nup_parts_size(&source.parts)};
    iz_memory = iz_image_game_memory(image, header, sizeof(header));
    close_nup(&source);
    uint64_t footprint = 0;
    for (size_t i = 0; i < sizeof(module_names) / sizeof(*module_names); ++i) {
        if (!open_exefs(&source, path, module_names[i])) {
            if (i) continue;
            *error = "This game folder needs its original exefs/main.";
            return false;
        }
        const uint64_t length = nup_parts_size(&source.parts);
        const Nup view = {.source = {nup_parts_read, sha256_matches, &source.parts, length}};
        const NupFile file = {.size = length, .stored = length};
        const bool loaded = add_module(&view, &file, module_names[i], &footprint, error);
        close_nup(&source);
        if (!loaded) {
            clear_game();
            return false;
        }
    }
    if (!game_paths(self, id, error)) {
        clear_game();
        return false;
    }
    nx2_whole = (int)nx2_module_count;
    snprintf(iz_content_home, sizeof(iz_content_home), "%s", path + 5);
    if (!strcmp(iz_content_home, "/")) iz_content_home[0] = 0;
    return true;
}

static bool prepare_game(const char *self, const char *path, const char **error) {
    if (smGetServiceOverride(smEncodeName("ldr:ro")) == INVALID_HANDLE) {
        *error = "This IZ2S session did not provide the module loader (ldr:ro).";
        return false;
    }
    struct stat info;
    if (!strncmp(path, "sdmc:/", 6) && !lstat(path, &info) && S_ISDIR(info.st_mode))
        return prepare_loose(self, path, error);
    if (!open_nup(&input, path)) {
        *error = "Could not open this NUP or its numbered parts.";
        return false;
    }
    const uint64_t size = nup_parts_size(&input.parts);
    unsigned char header[128];
    if (nup_part_digits(path) && nup_parts_read(&input.parts, 0, header, sizeof(header)) &&
        !memcmp(header, "NUP\1", 4) && iz_le64(header + 64) != size) {
        *error = "Split NUP size mismatch. Check all its numbered parts.";
        goto fail;
    }
    if (!nup_open(&container, (NupSource){nup_parts_read, sha256_matches, &input.parts, size}, error))
        goto fail;
    const NupPackage *layers[NUP_PACKAGE_LIMIT];
    const size_t layers_count = nup_layers(&container, layers);
    const NupPackage *code = NULL;
    for (size_t i = 0; i < layers_count; ++i)
        if (nup_find_file(&container, layers[i]->exefs, "main") != NUP_NONE) code = layers[i];
    if (!code || !(code->id & ~UINT64_C(0xfff))) {
        *error = "No Switch 2 program and title ID were found in this NUP.";
        goto fail;
    }
    iz_memory = iz_game_memory(&container, layers, layers_count, code);
    uint64_t footprint = 0;
    for (size_t i = 0; i < sizeof(module_names) / sizeof(*module_names); ++i) {
        const uint32_t index = nup_find_file(&container, code->exefs, module_names[i]);
        if (index == NUP_NONE) continue;
        NupFile file;
        if (!nup_file(&container, index, &file)) {
            *error = "An ExeFS entry has conflicting metadata.";
            goto fail;
        }
        if (!add_module(&container, &file, module_names[i], &footprint, error)) goto fail;
    }
    if (!nx2_module_count) {
        *error = "No supported executable modules were found.";
        goto fail;
    }
    rom = virtual_romfs_open(&container, layers, layers_count, error);
    if (!rom) goto fail;
    if (!game_paths(self, code->id & ~UINT64_C(0xfff), error)) goto fail;
    nx2_whole = (int)nx2_module_count;
    return true;

fail:
    clear_game();
    return false;
}

uint64_t iz_rom_size(void) { return virtual_romfs_size(rom); }

bool iz_rom_read(uint64_t offset, void *out, uint64_t bytes) {
    if (bytes > SIZE_MAX) return false;
    while (__atomic_exchange_n(&rom_busy, 1u, __ATOMIC_ACQUIRE)) svcSleepThread(20000);
    const bool ok = virtual_romfs_read(rom, offset, out, (size_t)bytes);
    __atomic_store_n(&rom_busy, 0u, __ATOMIC_RELEASE);
    return ok;
}

static int digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool iz_decode_path(const char *arg, char *path, unsigned capacity) {
    const char prefix[] = "--nup=";
    if (!arg || strncmp(arg, prefix, sizeof(prefix) - 1)) return false;
    arg += sizeof(prefix) - 1;
    const size_t length = strlen(arg);
    if (!length || (length & 1) || length / 2 >= capacity || length < 12) return false;
    for (size_t i = 0; i < length / 2; ++i) {
        const int a = digit(arg[2 * i]), b = digit(arg[2 * i + 1]);
        if (a < 0 || b < 0 || !(a || b)) return false;
        path[i] = (char)(a * 16 + b);
    }
    path[length / 2] = 0;
    return !strncmp(path, "sdmc:/", 6);
}

bool iz_queue_game(const char *self, const char *path, const char **error) {
    static const char alphabet[] = "0123456789abcdef";
    char args[1792];
    if (!envHasNextLoad() || !self || strncmp(self, "sdmc:/", 6) || strchr(self, '"')) {
        *error = "Start this application as an NRO from the IZ2S library.";
        return false;
    }
    if (smGetServiceOverride(smEncodeName("ldr:ro")) == INVALID_HANDLE) {
        *error = "This IZ2S session did not provide the module loader (ldr:ro).";
        return false;
    }
    const int prefix = snprintf(args, sizeof(args), "\"%s\" --nup=", self);
    const size_t length = strlen(path);
    if (prefix < 0 || (size_t)prefix + 2 * length >= sizeof(args)) {
        *error = "The selected path is too long.";
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char b = (unsigned char)path[i];
        args[prefix + 2 * i] = alphabet[b >> 4];
        args[prefix + 2 * i + 1] = alphabet[b & 15];
    }
    args[prefix + 2 * length] = '\0';
    if (R_FAILED(envSetNextLoad(self, args))) {
        *error = "IZ2S could not schedule the selected game.";
        return false;
    }
    return true;
}

extern int nx2_game_main(int, char **);

int main(int argc, char **argv) {
    const char *error = NULL;
    const char *self = argc > 0 ? argv[0] : NULL;
    if (!fsdevGetDeviceFileSystem("sdmc") && R_FAILED(fsdevMountSdmc())) {
        iz_restore_host_heap();
        return 1;
    }
    if (argc > 1) {
        char path[512];
        if (!iz_decode_path(argv[1], path, sizeof(path))) {
            error = "The launch request contains an invalid path.";
        } else if (prepare_game(self, path, &error)) {
            return nx2_game_main(argc, argv);
        }
    }
    const int result = iz_browser(self, error);
    iz_restore_host_heap();
    return result;
}
