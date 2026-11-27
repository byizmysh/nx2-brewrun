"""Adapt a build-only copy of nx2-relink 0.1.12 to select its game at runtime."""
from pathlib import Path
import shutil
import sys

upstream, output = map(Path, sys.argv[1:])
shutil.copytree(upstream / "runtime", output)


def replace(name, old, new):
    path = output / name
    source = path.read_text()
    if source.count(old) != 1:
        raise ValueError(f"{name} no longer matches nx2-relink 0.1.12: {old}")
    path.write_text(source.replace(old, new))


replace("nx2.h", "#include <stdint.h>", '#include <stdint.h>\n#include "boot.h"')
replace("nx2.h", '#ifdef __SWITCH2__\n#define NX2_TOOL "nx2-relink"\n#else\n#define NX2_TOOL "nx-relink"\n#endif\n'
        '#define NX2_HOME "/" NX2_TOOL "/" NX2_NAME  // (as the file system service is given it)\n'
        '#define NX2_DIR "sdmc:" NX2_HOME "/"\n#define NX2_F(name) (NX2_DIR name)',
        '#define NX2_TOOL "nx2-brerun"\n#define NX2_HOME iz_home\n#define NX2_DIR iz_dir\n'
        '#define NX2_NAME iz_game_name\n#define NX2_F(name) iz_path(name)')
replace("nx2.h", "extern const Nx2Image nx2_images[];", "extern Nx2Image nx2_images[];")
replace("nx2.h", "extern const int nx2_whole;", "extern int nx2_whole;")
replace("nx2.h", "extern const unsigned nx2_module_count;", "extern unsigned nx2_module_count;")
replace("whole.c", '#include "nx2.h"', '#include "nso.h"')
replace("whole.c", "    const u64 page = 0x1000, shift = page;",
        "    const u64 page = IZ_NSO_PAGE, shift = page;")
replace("whole.c", "(im->image_size + page - 1) & ~(page - 1)", "iz_nso_round_pages(im->image_size)")
replace("whole.c", "        u8 *nro = aligned_alloc(page, nro_size), *bss = aligned_alloc(page, bss_size);",
        "        u8 *nro = (u8 *)im->image - shift; /* The NSO decoder reserved the header page. */\n"
        "        u8 *bss = aligned_alloc(page, bss_size);")
replace("whole.c", "        if (!nro || !bss) {", "        if (!bss) {")
replace("whole.c", "        memcpy(nro + shift, im->image, im->image_size);\n", "")
replace("nvnwatch.c", "    return (void *)loader_watched;",
        """    if (iz_game_nvn_native && !nogpu && !nx2_nvnlock) {
        nx2_log("[nvn] native NVN enabled in loader.cfg\\n");
        return NULL;
    }
    return (void *)loader_watched;""")
replace("main.c", "u64 host = 128ull << 20;",
        "u64 host = (u64)iz_loader_room_mib() << 20; /* Includes runtime ExeFS and RomFS staging. */")
replace("main.c", "const u64 keep = heap_given ? size : 512ull << 20, most = (u64)NX2_GAME_MB << 20;",
        "const u64 room = (u64)iz_loader_room_mib() << 20;\n"
        "    const u64 keep = heap_given ? size : (room > (512ull << 20) ? room : 512ull << 20);\n"
        "    const u64 most = (u64)NX2_GAME_MB << 20;")
replace("main.c", "int main(int argc, char **argv) {",
        "void iz_restore_host_heap(void) { heap_back(); }\n\nint nx2_game_main(int argc, char **argv) {")
replace("main.c", "    nx2_log_start();",
        '    nx2_log_start();\n'
        '    nx2_log("[nx2] requested loader room: %u MiB\\n", (u64)iz_loader_room_mib());\n'
        '    if (iz_game_language_override)\n'
        '        nx2_log("[lang] requested %s\\n", (const char *)&iz_game_language_override);')
replace("main.c", """#ifndef NX2_GAME_APART_MB
#define NX2_GAME_APART_MB 0
#endif
#ifdef NX2_GAME_ROOM_MB  // (no figure was given for the game: this much of what there is stays outside what it is told of)
            const u64 slack = (u64)NX2_GAME_ROOM_MB << 20, apart = (u64)NX2_GAME_APART_MB << 20;
#else
            const u64 slack = 64ull << 20, apart = (u64)NX2_GAME_APART_MB << 20;
#endif
            u64 most = (u64)NX2_GAME_MB << 20;""",
        """            const u64 slack = (u64)iz_memory.slack_mib << 20;
            const u64 apart = (u64)iz_memory.apart_mib << 20;
            u64 most = (u64)iz_memory.heap_mib << 20;""")
replace("main.c", "#ifdef NX2_GAME_REMOTE_MB", "")
replace("main.c", "            {\n                u64 there = 0, held = 0;",
        "            if (iz_memory.remote_mib) {\n                u64 there = 0, held = 0;")
replace("main.c", "(u64)NX2_GAME_REMOTE_MB << 20", "(u64)iz_memory.remote_mib << 20")
replace("main.c", "#endif\n#ifdef NX2_GAME_SYSRES_KB", "")
replace("main.c", "            {\n                u64 there = 0;",
        "            if (iz_memory.system_kib) {\n                u64 there = 0;")
replace("main.c", "(u64)NX2_GAME_SYSRES_KB << 10", "(u64)iz_memory.system_kib << 10")
replace("main.c", "#endif\n        }\n#endif\n    }\n", "        }\n#endif\n    }\n")
replace("main.c", "    // the game's heap: half of what's free, 1 GB at most, in 2 MB steps as the kernel gives it",
        '    if (total && total < (8000ull << 20))\n'
        '        nx2_log("[nx2] The homebrew host has only %u MiB. A Switch 1 host can leave too little memory for a Switch 2 game.\\n",\n'
        '                total >> 20);\n\n'
        "    // the game's heap: half of what's free, 1 GB at most, in 2 MB steps as the kernel gives it")
replace("log.c", '    mkdir("sdmc:/" NX2_TOOL, 0777);\n    mkdir("sdmc:" NX2_HOME, 0777);\n', "")
replace("gl2.c", "static const char mine[] = NX2_NAME;", "const char *mine = NX2_NAME;")
replace("menu2.c", '"Juego: " NX2_NAME "   (en pausa)"', "NX2_NAME")

for source, indent, pattern in (
    ("control.c", "        ", "control-%016llx.bin"),
    ("erpt.c", "            ", "reports/%02u.bin"),
    ("nvn2.c", "            ", "missing-%u-%c.bin"),
    ("nvn2.c", "        ", "shader/%016llx.glsl"),
):
    replace(source, f'char name[64];\n{indent}snprintf(name, sizeof name, NX2_F("{pattern}"),',
            f'char name[FS_MAX_PATH];\n{indent}snprintf(name, sizeof name, "%s{pattern}", iz_dir,')

replace("rom.c", "#define ROM_IMAGES 41", "#define LEGACY_ROM_IMAGES 41\n#define ROM_IMAGES (1 + NUP_ADDON_LIMIT)")
replace("rom.c", "index < ROM_IMAGES && image_count < ROM_IMAGES",
        "index < LEGACY_ROM_IMAGES && image_count < ROM_IMAGES")
replace("rom.c", 'static char path[FS_MAX_PATH] = NX2_HOME "/romfs.bin";\n    image_open(&images[0], path);',
        'char path[FS_MAX_PATH];\n    snprintf(path, sizeof path, "%s/romfs.bin", iz_content_home);\n'
        '    const u64 virtual_size = iz_rom_size(0);\n'
        '    if (virtual_size) images[0] = (Image){.size = (s64)virtual_size, .ok = 1};\n'
        '    else image_open(&images[0], path);')
replace("rom.c", "    mods_open(&images[0]);", """    if (virtual_size) {
        if (iz_rom_addons_disabled()) nx2_log("[rom] add-on content disabled in loader.cfg\\n");
        for (unsigned i = 0; i < iz_rom_addon_count(); ++i) {
            Image *image = &images[image_count++];
            *image = (Image){.ok = 1, .size = (s64)iz_rom_size(i + 1), .index = iz_rom_addon_index(i)};
            nx2_log("[rom] add-on content %u from NUP: %u KiB\\n", (u64)image->index, (u64)image->size >> 10);
        }
        return;
    }
    mods_open(&images[0]);""")
replace("rom.c", 'const char *head = NX2_HOME "/aoc";', 'char head[FS_MAX_PATH];\n        snprintf(head, sizeof head, "%s/aoc", iz_content_home);')
replace("rom.c", """static u32 rom_read(Image *im, u64 offset, u8 *to, u64 size, u64 *got) {
    *got = 0;""", """static u32 rom_read(Image *im, u64 offset, u8 *to, u64 size, u64 *got) {
    *got = 0;
    if (iz_rom_size(0)) {
        if (!iz_rom_read((unsigned)(im - images), offset, to, size)) return 0x2EE202;
        *got = size;
        return 0;
    }""")
replace("rom.c", "if (id >> 12 == 0x0100000000000ull)",
        "if (id >> 12 == 0x0100000000000ull || id >> 12 == 0x0400000000000ull)")
replace("rom.c", 'if (image_count < 2 || (type != 4 && type != 6)) return 0;',
        'if ((image_count < 2 && !iz_rom_addons_disabled()) || (type != 4 && type != 6)) return 0;')
replace("rom.c", "        const u32 from = rd32(q + 16), want = rd32(q + 20);",
        "        if (q + 24 > m + 0x100) return 0;\n"
        "        const u32 from = rd32(q + 16), want = rd32(q + 20);")
replace("rom.c",
        'nx2_log("[rom] the SDK asks for data %x: add-on content %u, given the file on the card\\n", id, (u64)images[k].index);',
        'nx2_log("[rom] the SDK asks for data %x: add-on content %u, served from %s\\n", id, '
        '(u64)images[k].index, iz_rom_size(0) ? "the NUP" : "the card");')
replace("rom.c", 'static const char folder[] = NX2_HOME "/mods";',
        'char folder[FS_MAX_PATH];\n    snprintf(folder, sizeof folder, "%s/mods", NX2_HOME);')
replace("rom.c", 'static const char home[] = NX2_HOME "/mods/";',
        'char home[FS_MAX_PATH];\n    snprintf(home, sizeof home, "%s/mods/", NX2_HOME);')
replace("rom.c", '#define SAVE_DIR NX2_HOME "/save"', '#define SAVE_DIR iz_save_dir')

replace("rom.c", "static u32 save_objects[SAVE_OBJECTS];",
        "static u64 save_objects[SAVE_OBJECTS];  /* Filesystem kind above the domain object ID. */")
replace("rom.c", "    u8 fs;     // the file system itself: its paths get the folder, and closing it closes nothing of the host's",
        "    u32 fs;    /* Zero for a child object, otherwise the save or indexed cache filesystem. */")
replace("rom.c", "static u32 lend(u32 real, int fs)", "static u32 lend(u32 real, u32 fs)")
replace("rom.c", "lent[k].fs = (u8)fs", "lent[k].fs = fs")
replace("rom.c", "void nx2_save_open(void) {\n    FsFileSystem *sd = fsdevGetDeviceFileSystem(\"sdmc\");\n    if (!sd) return;",
        '#include "save_storage.inc"\n\nvoid nx2_save_open(void) {\n'
        '    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");\n'
        '    if (!sd) return;\n    cache_setup(sd);')
replace("rom.c", "static int in_folder(u8 *m, u32 cmd, int plain, char (*paths)[0x301]) {",
        """static int in_folder(u8 *m, u32 cmd, int plain, u32 fs, char (*paths)[0x301]) {
    if (cmd == 16) return plain && inline_save_input_size(m) == 16;  /* GetFileSystemAttribute. */
    char cache[FS_MAX_PATH];
    if (fs >= SdCacheFilesystem && !cache_folder(cache, (u16)(fs - SdCacheFilesystem))) return 0;
    const char *folder = fs >= SdCacheFilesystem ? cache : SAVE_DIR;""")
replace("rom.c", "        for (const char *p = SAVE_DIR; *p; p++) to[n++] = *p;",
        "        for (const char *p = folder; ok && *p; p++) {\n"
        "            if (n >= 0x300) ok = 0;\n"
        "            else to[n++] = *p;\n        }")
replace("rom.c", "if (from[i] == '.' && from[i + 1] == '.')",
        "if (from[i] == '.' && i + 1 < size && from[i + 1] == '.')")
replace("rom.c", "int nx2_save_before(u8 *m, char (*paths)[0x301], u32 *handle) {",
        "int nx2_save_before(u8 *m, char (*paths)[0x301], u32 *handle, u32 *opening_fs) {")
replace("rom.c", "if (!in_folder(m, cmd, kind == 1 && has_cmd, paths))",
        "if (!in_folder(m, cmd, kind == 1 && has_cmd, lent[k].fs, paths))")
replace("rom.c", "    int mine = -1;\n    for (int k = 0; k < SAVE_OBJECTS; k++)\n"
        "        if (object && save_objects[k] == object) mine = k;",
        """    int mine = -1;
    u64 mounted = 0;
    for (int k = 0; object && k < SAVE_OBJECTS; ++k) {
        const u64 entry = __atomic_load_n(&save_objects[k], __ATOMIC_RELAXED);
        if ((u32)entry == object) { mine = k; mounted = entry; break; }
    }""")
replace("rom.c", "(command 51, below: it is given the same folder)", "(command 51, below)")
replace("rom.c",
        'nx2_log("[save] the SDK asks how large the game\'s cache storage is (command 34): told of 2 GB; '
        'it is kept with the saved data, in %s" "\\n", SAVE_DIR);',
        'nx2_log("[save] the SDK asks how large the game\'s cache storage is (command 34): told of 2 GB\\n");')
replace("rom.c", "        if (host_fs) {\n            const u32 id = lend(host_fs->object_id, 1);",
        """        u32 fs = SdSaveFilesystem;
        const u8 *attr = d + 40;  /* Domain and CMIF headers, then the aligned save-data space ID. */
        if (inline_save_input_size(m) >= 24 + sizeof(FsSaveDataAttribute) &&
            attr[offsetof(FsSaveDataAttribute, save_data_type)] == FsSaveDataType_Cache) {
            const u16 index = (u16)rd32(attr + offsetof(FsSaveDataAttribute, save_data_index));
            const u32 rc = ensure_cache_directory(index);
            if (rc) {
                answer(m, rc, NULL, 0, 0);
                return 3;
            }
            fs = SdCacheFilesystem + index;
            nx2_log("[save] mounting SD-backed cache index %u\\n", (u64)index);
        }
        if (host_fs) {
            const u32 id = lend(host_fs->object_id, fs);""")
replace("rom.c", '            nx2_log("[save] the SDK asks for the program\'s save data (command %u): '
        'given the host\'s file system of the card, in %s\\n", (u64)cmd, SAVE_DIR);',
        '            if (fs == SdSaveFilesystem)\n'
        '                nx2_log("[save] the SDK asks for the program\'s save data (command %u): '
        'given the host\'s file system of the card, in %s\\n", (u64)cmd, SAVE_DIR);')
replace("rom.c", '        nx2_log("[save] the SDK asks for the program\'s save data (command %u): '
        'given the card\'s file system, in %s\\n", (u64)cmd, SAVE_DIR);',
        '        *opening_fs = fs;\n'
        '        if (fs == SdSaveFilesystem)\n'
        '            nx2_log("[save] the SDK asks for the program\'s save data (command %u): '
        'given the card\'s file system, in %s\\n", (u64)cmd, SAVE_DIR);')
replace("rom.c", "        save_objects[mine] = 0;",
        "        __atomic_compare_exchange_n(&save_objects[mine], &mounted, 0, false,\n"
        "                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED);")
replace("rom.c", "if (in_folder(m, cmd, kind == 1 && has_cmd, paths)) return 2;",
        "if (in_folder(m, cmd, kind == 1 && has_cmd, (u32)(mounted >> 32), paths)) return 2;")
replace("rom.c", "void nx2_save_after(u8 *m, int how) {", "void nx2_save_after(u8 *m, int how, u32 opening_fs) {")
replace("rom.c", "    for (int k = 0; k < SAVE_OBJECTS; k++)\n        if (!save_objects[k]) {\n"
        "            save_objects[k] = object;\n            return;\n        }",
        """    const u64 mounted = ((u64)opening_fs << 32) | object;
    for (int k = 0; object && k < SAVE_OBJECTS; ++k) {
        u64 empty = 0;
        if (__atomic_compare_exchange_n(&save_objects[k], &empty, mounted, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    }""")
replace("nx2.h", "int nx2_save_before(u8 *message, char (*paths)[0x301], u32 *handle);\n"
        "void nx2_save_after(u8 *message, int how);",
        "int nx2_save_before(u8 *message, char (*paths)[0x301], u32 *handle, u32 *opening_fs);\n"
        "void nx2_save_after(u8 *message, int how, u32 opening_fs);\n"
        "bool iz_has_sd_cache_storage(void);\nu32 iz_create_cache_storage(s32 index, s64 data_size, s64 journal_size);")
replace("svc.c", "    u32 save_handle = 0;", "    u32 save_handle = 0, opening_fs = 0;")
replace("svc.c", "nx2_save_before(m, save_paths, &save_handle)",
        "nx2_save_before(m, save_paths, &save_handle, &opening_fs)")
replace("svc.c", "nx2_save_after(m, save);", "nx2_save_after(m, save, opening_fs);")
replace("gl2.c", "        if (watch) return watch;",
        "        if (watch) return watch;\n"
        '        if (!strcmp(name, "_ZN2nn2fs18CreateCacheStorageEill") && iz_has_sd_cache_storage())\n'
        "            return iz_create_cache_storage;")

replace("svc.c", "static u32 applet_sdk, applet_host, applet_proxy;",
        'static u32 applet_sdk, applet_host, applet_proxy;\n#include "language_ipc.inc"')
replace("svc.c", "static void session_forget(u32 h) {",
        "static void language_ns_forget(u32 handle, u32 object);\n"
        "static void session_forget(u32 h) {\n"
        "    if (iz_game_language_override) language_ns_forget(h, 0);")
replace("svc.c", "    applet_host = p->session, applet_proxy = p->object_id, applet_sdk = sdk_handle;",
        """    applet_host = p->session, applet_proxy = p->object_id, applet_sdk = sdk_handle;
    if (iz_game_language_override) {
        Service *functions = appletGetServiceSession_Functions();
        language_host_functions = serviceIsActive(functions) && functions->session == applet_host ?
            functions->object_id : 0;
        __atomic_store_n(&language_functions, 0, __ATOMIC_RELAXED);
    }""")
replace("svc.c", "    const u32 rc = forced ? 0 : svcSendSyncRequest(h);",
        """    const LanguageRequest language = language_request(m, q, (u32)r[0], type, cmd, who);
    const u32 rc = forced ? 0 : svcSendSyncRequest(h);
    language_reply(m, language, (u32)r[0], rc);""")
replace("svc.c", '    const u8 *q = payload(m, "SFCI");\n    const u32 type = rd32(m) & 0xFFFF;',
        '    const u32 type = rd32(m) & 0xFFFF;\n'
        '    const u8 *q = type >= 16 ? NULL : payload(m, "SFCI");')
replace("svc.c", '    u32 result = a ? rd32(a + 8) : 0, got = rc ? 0 : answer_handle(m);',
        '    /* TIPC can leave a stale CMIF header beyond its actual response. */\n'
        '    u32 result = is_sm && type >= 16 && !rc ? short_result(m) : a ? rd32(a + 8) : 0;\n'
        '    u32 got = rc ? 0 : answer_handle(m);')
