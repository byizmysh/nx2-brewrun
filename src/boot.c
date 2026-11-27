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
#include "rom_io.h"
#include "settings.h"
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
uint64_t iz_game_language_override;
uint64_t iz_game_title_id;
char iz_game_display_version[NUP_DISPLAY_VERSION_SIZE];
int iz_game_language_index = -1;
bool iz_game_nvn_native;

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
static NupRomSet rom;
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
    if (!iz_game_data_path(self, id, iz_dir, sizeof(iz_home) + 5)) {
        *error = "The folder containing the " IZ_NUP_TITLE " NRO is too long.";
        return false;
    }
    strcpy(iz_home, iz_dir + 5);
    strcpy(iz_game_name, strrchr(iz_dir, '/') + 1);
    struct stat status;
    if (mkdir(iz_dir, 0777) && (errno != EEXIST || stat(iz_dir, &status) || !S_ISDIR(status.st_mode))) {
        *error = "Could not create the game's data folder beside the " IZ_NUP_TITLE " NRO.";
        return false;
    }
    strcat(iz_dir, "/");
    snprintf(iz_save_dir, sizeof(iz_save_dir), "%s" IZ_SAVE_SUFFIX, iz_home);
    iz_game_title_id = id & ~UINT64_C(0xfff);
    for (size_t i = 0; i < sizeof(known_paths) / sizeof(*known_paths); ++i)
        snprintf(paths[i], sizeof(paths[i]), "sdmc:%s/%s", iz_home, known_paths[i]);
    const IzGameLanguage language = iz_game_language(self, id);
    iz_game_language_override = iz_game_language_code(language);
    iz_game_language_index = iz_game_application_language(language);
    iz_game_nvn_native = iz_game_native_nvn(self, id);
    return true;
}

static bool read_part_bytes(void *context, unsigned part, uint64_t offset, void *buffer, size_t bytes, bool game) {
    SdNup *source = context;
    FsFile *file = &source->files[part];
    unsigned char *out = buffer;
    while (bytes) {
        const size_t step = bytes < (2u << 20) ? bytes : (2u << 20);
        u64 actual = 0;
        if (offset > INT64_MAX) return false;
        const Result result = game ? iz_read_rom_file(file, (s64)offset, out, step, &actual) :
            fsFileRead(file, (s64)offset, out, step, FsReadOption_None, &actual);
        if (R_FAILED(result) || actual != step) return false;
        offset += step;
        out += step;
        bytes -= step;
    }
    return true;
}

static bool read_part(void *context, unsigned part, uint64_t offset, void *buffer, size_t bytes) {
    return read_part_bytes(context, part, offset, buffer, bytes, false);
}

static bool read_game_part(void *context, unsigned part, uint64_t offset, void *buffer, size_t bytes) {
    return read_part_bytes(context, part, offset, buffer, bytes, true);
}

static void close_nup(SdNup *source) {
    while (source->parts.count) fsFileClose(&source->files[--source->parts.count]);
}

static bool open_source(SdNup *source, const char *path, unsigned limit) {
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    char part_path[512];
    const size_t length = strlen(path);
    if (!sd || strncmp(path, "sdmc:/", 6) || length >= sizeof(part_path)) return false;
    NupPartName split;
    if (limit > 1 && !nup_part_start(path, &split)) return false;
    memcpy(part_path, path, length + 1);
    source->parts = (NupParts){.read = read_part, .user = source};
    for (unsigned i = 0; i < limit; ++i) {
        if (i && !nup_part_path(path, &split, i, part_path, sizeof(part_path))) break;
        FsFile *file = &source->files[i];
        if (R_FAILED(fsFsOpenFile(sd, part_path + 5, FsOpenMode_Read, file))) break;
        s64 size;
        if (R_FAILED(fsFileGetSize(file, &size)) || size <= 0) {
            fsFileClose(file);
            break;
        }
        if (!nup_parts_append(&source->parts, (uint64_t)size)) {
            fsFileClose(file);
            close_nup(source);
            return false;
        }
    }
    return source->parts.count != 0;
}

static bool open_nup(SdNup *source, const char *path) {
    if (!open_source(source, path, 1)) return false;
    NupPartName split;
    if (!nup_part_start(path, &split)) return true;
    unsigned char header[128];
    const uint64_t size = nup_parts_size(&source->parts);
    const size_t head = size < sizeof(header) ? (size_t)size : sizeof(header);
    if (!nup_parts_read(&source->parts, 0, header, head) ||
        memcmp(header, "NUP\1", head < 4 ? head : 4) ||
        (head == sizeof(header) && iz_le64(header + 64) <= size)) return true;
    close_nup(source);
    return open_source(source, path, NUP_PART_LIMIT);
}

static bool sha256_matches(const void *bytes, size_t size, const unsigned char *expected);

bool iz_probe_nup(const char *path, NupPreview *preview) {
    SdNup source;
    if (!open_nup(&source, path)) return false;
    const NupSource archive = {nup_parts_read, sha256_matches, &source.parts, nup_parts_size(&source.parts)};
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

static bool open_romfs(SdNup *source, const char *folder, char (*first)[512]) {
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
    else if (first) strcpy(*first, path);
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
    if (!open_romfs(&source, folder, NULL)) return false;
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

bool iz_delete_game_image(const char *path, bool loose, uint64_t size, uint64_t title, unsigned *removed) {
    *removed = 0;
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd || strncmp(path, "sdmc:/", 6) || strlen(path) >= 512) return false;

    SdNup source;
    char first[512];
    if (loose) {
        if (!open_romfs(&source, path, &first)) return false;
    } else {
        if (!open_nup(&source, path)) return false;
        strcpy(first, path);
    }
    const unsigned count = source.parts.count;
    const uint64_t actual = nup_parts_size(&source.parts);
    bool valid = actual == size;
    if (valid && loose && title) {
        unsigned char header[128];
        valid = read_npdm(path, header) == title;
    } else if (valid && !loose) {
        const NupSource archive = {nup_parts_read, sha256_matches, &source.parts, actual};
        NupPreview preview;
        valid = nup_preview(archive, &preview) && preview.title_id == title;
    }
    close_nup(&source);

    NupPartName split;
    if (!valid || (count > 1 && !nup_part_start(first, &split))) return false;
    char name[512];
    strcpy(name, first);
    for (unsigned i = 0; i < count; ++i) {
        if (i && !nup_part_path(first, &split, i, name, sizeof(name))) return false;
        FsDirEntryType type;
        if (R_FAILED(fsFsGetEntryType(sd, name + 5, &type)) || type != FsDirEntryType_File) return false;
    }
    /* Keep the identifying header in place until all later parts are removed. */
    for (unsigned i = count; i-- > 0;) {
        if (!i) strcpy(name, first);
        else if (!nup_part_path(first, &split, i, name, sizeof(name))) return false;
        if (R_FAILED(fsFsDeleteFile(sd, name + 5))) return false;
        ++*removed;
    }
    return true;
}

static bool sha256_matches(const void *bytes, size_t size, const unsigned char *expected) {
    unsigned char digest[32];
    sha256CalculateHash(digest, bytes, size);
    return !memcmp(digest, expected, sizeof(digest));
}

bool iz_check_game_image(const char *path, bool loose, const char **error) {
    if (loose) {
        uint64_t size, title;
        if (!iz_probe_loose(path, &size, &title)) *error = "RomFS is incomplete or unreadable.";
        else if (!title) *error = "The original ExeFS main or main.npdm is missing, invalid or unreadable.";
        else return true;
        return false;
    }

    SdNup source;
    if (!open_nup(&source, path)) {
        *error = "Could not open the NUP or its split parts.";
        return false;
    }
    Nup nup;
    const NupSource archive = {nup_parts_read, sha256_matches, &source.parts, nup_parts_size(&source.parts)};
    bool ok = nup_open(&nup, archive, error) && nup_check_index(&nup, error);
    if (ok) {
        const NupPackage *layers[NUP_PACKAGE_LIMIT];
        const size_t count = nup_layers(&nup, layers);
        uint64_t title;
        ok = nup_program(&nup, layers, count, &title) && title;
        if (!ok) *error = "No executable entry with a Switch 2 title ID was found.";
    }
    nup_close(&nup);
    close_nup(&source);
    return ok;
}

static void clear_game(void) {
    memset(iz_game_display_version, 0, sizeof(iz_game_display_version));
    iz_game_language_override = 0;
    iz_game_language_index = -1;
    iz_game_nvn_native = false;
    nup_rom_set_close(&rom);
    for (unsigned i = 0; i < nx2_module_count; ++i) iz_nso_close(&nx2_images[i]);
    nx2_module_count = nx2_whole = 0;
    nup_close(&container);
    close_nup(&input);
}

typedef struct {
    uint64_t budget, initialized_limit;
} ExefsLimits;

static ExefsLimits exefs_limits(const char *self, uint64_t id, IzLaunchError *report) {
    const unsigned available = iz_loader_room_mib() - IZ_ROOM_OVERHEAD_MIB;
    const unsigned budget = iz_game_exefs_budget(self, id);
    report->stage = "Loader settings";
    report->title = id;
    report->room = iz_loader_room_mib();
    report->saved_budget = budget;
    if (budget > available) {
        report->reason = "The configured ExeFS budget exceeds the space available in Loader room.";
        report->exefs = (IzExefsFailure){IZ_EXEFS_CHECK_SETTINGS, (uint64_t)budget << 20, (uint64_t)available << 20};
        return (ExefsLimits){0};
    }
    report->total_limit = budget ? budget : available;
    report->initialized_limit = budget ? budget : IZ_EXEFS_MIN_MIB;
    return (ExefsLimits){
        .budget = (uint64_t)report->total_limit << 20,
        .initialized_limit = (uint64_t)report->initialized_limit << 20,
    };
}

static bool add_module(const Nup *nup, const NupFile *file, const char *name,
                       ExefsLimits limits, uint64_t *footprint, IzLaunchError *report) {
    report->module = name;
    report->file_bytes = file->size;
    report->stored_bytes = file->stored;
    if (nx2_module_count >= IZ_WHOLE_CAPACITY) {
        report->reason = "The game needs more whole module slots than this build supports.";
        return false;
    }
    const unsigned j = nx2_module_count;
    if (!iz_nso_prepare(nup, file, name, limits.budget, limits.initialized_limit,
                        &nx2_modules[j], &nx2_images[j], &report->reason, &report->exefs)) return false;
    ++nx2_module_count;
    *footprint += iz_nso_mapping_bytes(&nx2_modules[j], &nx2_images[j]);
    if (*footprint > limits.budget) {
        report->reason = IZ_EXEFS_BUDGET_ERROR;
        report->exefs = (IzExefsFailure){IZ_EXEFS_CHECK_TOTAL, *footprint, limits.budget};
        return false;
    }
    report->module = NULL;
    report->file_bytes = report->stored_bytes = 0;
    return true;
}

static bool prepare_loose(const char *self, const char *path, IzLaunchError *report) {
    unsigned char header[128];
    SdNup source;
    report->stage = "ExeFS metadata";
    const uint64_t id = read_npdm(path, header);
    if (!id) {
        report->module = "main.npdm";
        report->reason = "The original exefs/main.npdm is missing, invalid or unreadable.";
        return false;
    }
    const ExefsLimits limits = exefs_limits(self, id, report);
    if (!limits.budget) return false;
    report->stage = "RomFS";
    if (!open_romfs(&source, path, NULL)) {
        report->reason = "The game's RomFS image is incomplete or unreadable.";
        return false;
    }
    const NupSource image = {nup_parts_read, NULL, &source.parts, nup_parts_size(&source.parts)};
    iz_memory = iz_image_game_memory(image, header, sizeof(header));
    close_nup(&source);
    uint64_t footprint = 0;
    report->stage = "ExeFS";
    for (size_t i = 0; i < sizeof(module_names) / sizeof(*module_names); ++i) {
        if (!open_exefs(&source, path, module_names[i])) {
            if (i) continue;
            report->module = module_names[i];
            report->reason = "The original exefs/main is missing or unreadable.";
            return false;
        }
        const uint64_t length = nup_parts_size(&source.parts);
        const Nup view = {.source = {nup_parts_read, sha256_matches, &source.parts, length}};
        const NupFile file = {.size = length, .stored = length};
        const bool loaded = add_module(&view, &file, module_names[i], limits, &footprint, report);
        close_nup(&source);
        if (!loaded) {
            clear_game();
            return false;
        }
    }
    report->stage = "Game data folder";
    if (!game_paths(self, id, &report->reason)) {
        clear_game();
        return false;
    }
    nx2_whole = (int)nx2_module_count;
    snprintf(iz_content_home, sizeof(iz_content_home), "%.*s", (int)sizeof(iz_content_home) - 1, path + 5);
    if (!strcmp(iz_content_home, "/")) iz_content_home[0] = 0;
    return true;
}

static bool prepare_game(const char *self, IzLaunchError *report) {
    const char *path = report->path;
    report->stage = "Homebrew session";
    if (smGetServiceOverride(smEncodeName("ldr:ro")) == INVALID_HANDLE) {
        report->reason = "This homebrew session did not provide the module loader (ldr:ro).";
        return false;
    }
    struct stat info;
    if (!strncmp(path, "sdmc:/", 6) && !lstat(path, &info) && S_ISDIR(info.st_mode))
        return prepare_loose(self, path, report);
    report->stage = "Open NUP";
    if (!open_nup(&input, path)) {
        report->reason = "Could not open this NUP or its split parts.";
        return false;
    }
    report->stage = "Read NUP index";
    const uint64_t size = nup_parts_size(&input.parts);
    if (!nup_open(&container, (NupSource){nup_parts_read, sha256_matches, &input.parts, size}, &report->reason))
        goto fail;
    const NupPackage *layers[NUP_PACKAGE_LIMIT];
    const size_t layers_count = nup_layers(&container, layers);
    uint64_t title;
    const NupPackage *code = nup_program(&container, layers, layers_count, &title);
    if (!code || !title) {
        report->reason = "No Switch 2 program and title ID were found in this NUP.";
        goto fail;
    }
    const ExefsLimits limits = exefs_limits(self, title, report);
    if (!limits.budget) goto fail;
    iz_memory = iz_game_memory(&container, layers, layers_count, code);
    report->stage = "ExeFS";
    uint64_t footprint = 0;
    for (size_t i = 0; i < sizeof(module_names) / sizeof(*module_names); ++i) {
        const uint32_t index = nup_find_file(&container, code->exefs, module_names[i]);
        if (index == NUP_NONE) continue;
        NupFile file;
        if (!nup_file(&container, index, &file)) {
            report->module = module_names[i];
            report->reason = "The module's ExeFS entry has conflicting metadata.";
            goto fail;
        }
        if (!add_module(&container, &file, module_names[i], limits, &footprint, report)) goto fail;
    }
    if (!nx2_module_count) {
        report->reason = "No supported executable modules were found.";
        goto fail;
    }
    report->stage = "RomFS";
    if (!nup_rom_set_open(&rom, &container, layers, layers_count,
                          iz_game_dlc(self, title), &report->reason)) goto fail;
    report->stage = "Game data folder";
    if (!game_paths(self, report->title, &report->reason)) goto fail;
    nup_display_version(&container, layers, layers_count, iz_game_display_version);
    input.parts.read = read_game_part;
    nx2_whole = (int)nx2_module_count;
    return true;

fail:
    clear_game();
    return false;
}

unsigned iz_rom_addon_count(void) { return rom.addons; }
unsigned iz_rom_addon_index(unsigned addon) { return addon < rom.addons ? rom.indices[addon] : 0; }
bool iz_rom_addons_disabled(void) { return rom.addons_disabled; }

uint64_t iz_rom_size(unsigned image) {
    return image <= rom.addons ? virtual_romfs_size(rom.images[image]) : 0;
}

bool iz_rom_read(unsigned image, uint64_t offset, void *out, uint64_t bytes) {
    if (image > rom.addons || bytes > SIZE_MAX) return false;
    while (__atomic_exchange_n(&rom_busy, 1u, __ATOMIC_ACQUIRE)) svcSleepThread(20000);
    const bool ok = virtual_romfs_read(rom.images[image], offset, out, (size_t)bytes);
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

unsigned iz_loader_room_mib(void) {
    // libounce provides the raw argument string before it allocates the libc heap.
    static unsigned room;
    if (!room) room = iz_room_from_args((const char *)envGetArgv());
    return room;
}

bool iz_queue_game(const char *self, const char *path, uint64_t title, IzLaunchError *report) {
    static const char alphabet[] = "0123456789abcdef";
    char args[1792];
    report->stage = "Launch request";
    if (!envHasNextLoad() || !self || strncmp(self, "sdmc:/", 6) || strchr(self, '"')) {
        report->reason = "This session cannot start the game. Reopen BreRun from IZ2S or Homebrew Menu.";
        return false;
    }
    if (smGetServiceOverride(smEncodeName("ldr:ro")) == INVALID_HANDLE) {
        report->stage = "Homebrew session";
        report->reason = "This homebrew session did not provide the module loader (ldr:ro).";
        return false;
    }
    const int prefix = snprintf(args, sizeof(args), "\"%s\" --nup=", self);
    const size_t length = strlen(path);
    if (prefix < 0 || (size_t)prefix + 2 * length >= sizeof(args)) {
        report->reason = "The selected path is too long.";
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char b = (unsigned char)path[i];
        args[prefix + 2 * i] = alphabet[b >> 4];
        args[prefix + 2 * i + 1] = alphabet[b & 15];
    }
    const size_t used = (size_t)prefix + 2 * length;
    if (!title) {
        NupPreview preview;
        if (iz_probe_nup(path, &preview)) title = preview.title_id;
    }
    const int suffix = snprintf(args + used, sizeof(args) - used, " room=%u", iz_game_room(self, title));
    if (suffix < 0 || (size_t)suffix >= sizeof(args) - used) {
        report->reason = "The selected path is too long.";
        return false;
    }
    const Result result = envSetNextLoad(self, args);
    if (R_FAILED(result)) {
        report->reason = "The homebrew loader could not schedule the selected game.";
        report->result = result;
        return false;
    }
    return true;
}

extern int nx2_game_main(int, char **);

int main(int argc, char **argv) {
    const char *self = argc > 0 ? argv[0] : NULL;
    if (!fsdevGetDeviceFileSystem("sdmc") && R_FAILED(fsdevMountSdmc())) {
        iz_restore_host_heap();
        return 1;
    }
    IzLaunchError report = {.stage = "Launch request"};
    if (argc > 1) {
        if (!iz_decode_path(argv[1], report.path, sizeof(report.path))) {
            report.path[0] = 0;
            report.reason = "The launch request contains an invalid path.";
        } else if (prepare_game(self, &report)) {
            const int result = nx2_game_main(argc, argv);
            if (result) {
                report.stage = "Game runtime";
                report.reason = "The runtime returned an error. See the game's log for details.";
                report.runtime_exit = result;
                iz_log_launch_error(self, &report);
            }
            return result;
        }
        iz_log_launch_error(self, &report);
    }
    const int result = iz_browser(self, argc > 1 ? &report : NULL);
    iz_restore_host_heap();
    return result;
}
