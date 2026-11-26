from pathlib import Path
import struct
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

def charset(path):
    encoded = subprocess.check_output(
        ["fc-query", "-i", "0", "-f", "%{charset}", str(path)], text=True
    ).split()
    ranges = [tuple(int(part, 16) for part in value.split("-")) for value in encoded]
    return {point for bounds in ranges for point in range(bounds[0], bounds[-1] + 1)}


def glyph(font, point):
    character = chr(point)
    left, top, right, bottom = font.getbbox(character, anchor="ls")
    width, height = right - left, bottom - top
    mask = Image.new("L", (width, height))
    if width and height:
        ImageDraw.Draw(mask).text((-left, -top), character, font=font, fill=255, anchor="ls")
    return left, top, width, height, round(font.getlength(character)), mask.tobytes()


main_path, asian_path, regular_path, header, atlas = map(Path, sys.argv[1:])
main_chars, asian_chars, regular_chars = map(charset, (main_path, asian_path, regular_path))
latin_cyrillic = (
    set(range(32, 0x0250))
    | set(range(0x0300, 0x0530))
    | set(range(0x1E00, 0x1F00))
    | set(range(0x2000, 0x2070))
    | {0x20AC, 0x2212, 0x25A1, 0xFFFD}
)
cjk = (
    set(range(0x3000, 0x3130))
    | set(range(0x31F0, 0x3200))
    | set(range(0x3400, 0xA000))
    | set(range(0xAC00, 0xD7A4))
    | set(range(0xFF00, 0xFFF0))
)
tables, image = [], bytearray()
for size, points, face, characters in (
    (25, latin_cyrillic | cjk, main_path, main_chars),
    (46, latin_cyrillic, main_path, main_chars),
    (25, set(range(32, 127)) | {0x2026, 0x2212}, regular_path, regular_chars),
):
    fonts = [ImageFont.truetype(str(font), size, layout_engine=ImageFont.Layout.BASIC) for font in (face, asian_path)]
    table = []
    for point in sorted(points):
        if point in characters:
            font = fonts[0]
        elif point in asian_chars:
            font = fonts[1]
        else:
            continue
        left, top, width, height, advance, mask = glyph(font, point)
        table.append((point, len(image), left, top, width, height, advance))
        # Four bits per pixel are enough for antialiasing at the console's scale.
        opacity = [(value + 8) // 17 for value in mask]
        if len(opacity) & 1:
            opacity.append(0)
        image.extend(low | high << 4 for low, high in zip(opacity[::2], opacity[1::2]))
    tables.append(table)
assert set(map(ord, "Покемон — Café 新しい冒険.")) <= {entry[0] for entry in tables[0]}
record = struct.Struct("<IIbbBBB3x")
data_at = sum(len(table) * record.size for table in tables)
with atlas.open("wb") as output:
    for table in tables:
        for point, offset, left, top, width, height, advance in table:
            output.write(record.pack(point, data_at + offset, left, top, width, height, advance))
    output.write(image)
header.write_text(
    f"enum {{ FONT_SMALL_COUNT = {len(tables[0])}, FONT_BIG_COUNT = {len(tables[1])}, "
    f"FONT_FOOTER_COUNT = {len(tables[2])}, FONT_RECORD = {record.size}, "
    f"FONT_BIG_OFFSET = {len(tables[0]) * record.size}, "
    f"FONT_FOOTER_OFFSET = {(len(tables[0]) + len(tables[1])) * record.size} }};\n"
    "extern const unsigned char iz_font[];\n",
    encoding="ascii",
)
