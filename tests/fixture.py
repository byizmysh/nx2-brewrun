from hashlib import sha256
from pathlib import Path
import struct
import sys
import zstandard

output = Path(sys.argv[1])
title = 0x0100001000100000
none = 0xffffffff
zstd = zstandard.ZstdCompressor()

npdm = bytearray(96 * 1024)
npdm[:4] = b"META"
struct.pack_into("<II", npdm, 16, 1536 << 20, 8192 << 10)
files = [
    (b"keep.bin", b"keep the base file", 0),
    (b"shared.bin", b"replaced", 0),
    (b"shared.bin", bytes((i * 13 + i // 251) % 256 for i in range(900_000)), 1),
    (b"main.npdm", npdm, 1),
    (b"addon.bin", b"old package", 0),
    (b"addon.bin", bytes((i * 7 + 31) % 251 for i in range(310_000)), 1),
]
dirs = [
    (b"", [1], []),
    (b"romfs", [], [0, 1]),
    (b"", [3, 4], []),
    (b"romfs", [], [2]),
    (b"exefs", [], [3]),
    (b"", [6], []),
    (b"romfs", [], [4]),
    (b"", [8], []),
    (b"romfs", [], [5]),
    (b"", [10], []),
    (b"romfs", [], []),
]
packages = [
    (title, 0, none, 1, 0, 0),
    (title + 0x800, 2, 4, 3, 1, 1),
    # Unsorted versions and indices exercise ordering and overlay.
    (title + 0x1003, 9, none, 10, 0, 2),
    (title + 0x1001, 7, none, 8, 20, 2),
    (title + 0x1000, 9, none, 10, 0, 2),
    (title + 0x1001, 5, none, 6, 10, 2),
]

names = bytearray()
def name_ref(name):
    offset = len(names)
    names.extend(name)
    return offset, len(name)

package_table = b"".join(struct.pack("<QIIIIBB", *package, 0).ljust(64, b"\0") for package in packages)
dir_table, children = bytearray(), []
for name, child_dirs, child_files in dirs:
    entries = child_dirs + [len(dirs) + index for index in child_files]
    dir_table += struct.pack("<IIIIQ", *name_ref(name), len(children), len(entries), 0)
    children.extend(entries)
file_names = [name_ref(name) for name, _, _ in files]

names_at = 48 + len(package_table) + len(dir_table) + 80 * len(files) + 4 * len(children)
toc_size = names_at + len(names)
data_at = 128 + toc_size
data, toc = bytearray(), bytearray(toc_size)
struct.pack_into("<4sIQQQQ", toc, 0, b"TOC\x01", len(packages), len(dirs), len(files), names_at, len(names))
at = 48
toc[at : at + len(package_table)] = package_table
at += len(package_table)
toc[at : at + len(dir_table)] = dir_table
at += len(dir_table)
for name, (_, decoded, codec) in zip(file_names, files):
    stored = zstd.compress(decoded) if codec else decoded
    struct.pack_into("<IIQQQ", toc, at, *name, len(decoded), data_at + len(data), len(stored))
    toc[at + 32 : at + 64] = sha256(decoded).digest()
    toc[at + 64] = codec
    data += stored
    at += 80
struct.pack_into(f"<{len(children)}I", toc, at, *children)
toc[names_at:] = names

header = bytearray(128)
struct.pack_into("<4sIQQ", header, 0, b"NUP\x01", 0, 128, toc_size)
header[24:56] = sha256(toc).digest()
struct.pack_into("<QQI", header, 56, data_at, data_at + len(data), len(packages))
output.write_bytes(header + toc + data)
