from hashlib import sha256
from pathlib import Path
import struct
import sys
import zstandard

output = Path(sys.argv[1])
names = b"romfskeep.binshared.bin"
raw = b"keep the base file"
old = b"replaced"
new = bytes((i * 13 + i // 251) % 256 for i in range(900_000))
files = [(5, 8, raw, raw, 0), (13, 10, old, old, 0),
         (13, 10, new, zstandard.ZstdCompressor().compress(new), 1)]
header_bytes = 128
dirs = [(0, 0, 0, 1), (0, 5, 1, 2), (0, 0, 3, 1), (0, 5, 4, 1)]
children = [1, 4, 5, 3, 6]
names_at = 48 + 128 + 24 * len(dirs) + 80 * len(files) + 4 * len(children)
toc_size = names_at + len(names)
data_at = header_bytes + toc_size
data = bytearray()
toc = bytearray(toc_size)
struct.pack_into("<4sIQQQQ", toc, 0, b"TOC\x01", 2, len(dirs), len(files), names_at, len(names))
for idx, (root, romfs, version) in enumerate([(0, 1, 0), (2, 3, 1)]):
    struct.pack_into("<QIIIIBB", toc, 48 + 64 * idx, 0x100001000100000, root, 0xffffffff, romfs, version, idx, 0)
at = 48 + 128
for directory in dirs:
    struct.pack_into("<IIII", toc, at, *directory)
    at += 24
for name, length, decoded, stored, codec in files:
    struct.pack_into("<IIQQQ", toc, at, name, length, len(decoded), data_at + len(data), len(stored))
    toc[at + 32 : at + 64] = sha256(decoded).digest()
    toc[at + 64] = codec
    data += stored
    at += 80
struct.pack_into("<5I", toc, at, *children)
toc[names_at:] = names
header = bytearray(128)
struct.pack_into("<4sIQQ", header, 0, b"NUP\x01", 0, header_bytes, toc_size)
header[24:56] = sha256(toc).digest()
struct.pack_into("<QQI", header, 56, data_at, data_at + len(data), 2)
output.write_bytes(header + toc + data)
