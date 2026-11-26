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
        '#define NX2_TOOL "nup-library"\n#define NX2_HOME iz_home\n#define NX2_DIR iz_dir\n'
        '#define NX2_NAME iz_game_name\n#define NX2_F(name) iz_path(name)')
replace("nx2.h", "extern const Nx2Image nx2_images[];", "extern Nx2Image nx2_images[];")
replace("nx2.h", "extern const int nx2_whole;", "extern int nx2_whole;")
replace("nx2.h", "extern const unsigned nx2_module_count;", "extern unsigned nx2_module_count;")
replace("main.c", "u64 host = 128ull << 20;", "u64 host = 384ull << 20; /* Includes runtime ExeFS and RomFS staging. */")
replace("main.c", "int main(int argc, char **argv) {",
        "void iz_restore_host_heap(void) { heap_back(); }\n\nint nx2_game_main(int argc, char **argv) {")
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

replace("rom.c", 'static char path[FS_MAX_PATH] = NX2_HOME "/romfs.bin";\n    image_open(&images[0], path);',
        'char path[FS_MAX_PATH];\n    snprintf(path, sizeof path, "%s/romfs.bin", iz_content_home);\n'
        '    const u64 virtual_size = iz_rom_size();\n'
        '    if (virtual_size) images[0] = (Image){.size = (s64)virtual_size, .ok = 1};\n'
        '    else image_open(&images[0], path);')
replace("rom.c", "    mods_open(&images[0]);", "    if (virtual_size) return;\n    mods_open(&images[0]);")
replace("rom.c", 'const char *head = NX2_HOME "/aoc";', 'char head[FS_MAX_PATH];\n        snprintf(head, sizeof head, "%s/aoc", iz_content_home);')
replace("rom.c", """static u32 rom_read(Image *im, u64 offset, u8 *to, u64 size, u64 *got) {
    *got = 0;""", """static u32 rom_read(Image *im, u64 offset, u8 *to, u64 size, u64 *got) {
    *got = 0;
    if (im == &images[0] && iz_rom_size()) {
        if (!iz_rom_read(offset, to, size)) return 0x2EE202;
        *got = size;
        return 0;
    }""")
replace("rom.c", 'static const char folder[] = NX2_HOME "/mods";',
        'char folder[FS_MAX_PATH];\n    snprintf(folder, sizeof folder, "%s/mods", NX2_HOME);')
replace("rom.c", 'static const char home[] = NX2_HOME "/mods/";',
        'char home[FS_MAX_PATH];\n    snprintf(home, sizeof home, "%s/mods/", NX2_HOME);')
replace("rom.c", '#define SAVE_DIR NX2_HOME "/save"', '#define SAVE_DIR iz_save_dir')
