#!/usr/bin/env bash
set -euo pipefail

project=$(cd -- "$(dirname -- "$0")" && pwd)
workspace=$(cd -- "$project/../.." && pwd)
name=nx2-brerun
title='NX2 BreRun'
version=0.6.0
app_flags=("-DIZ_NUP_TITLE=\"$title\"" "-DIZ_NUP_VERSION=\"$version\"")
upstream=${NX2_RELINK_SOURCE:-"$HOME/.local/opt/nx2-relink-0.1.12"}
platform=${IZ2S_PLATFORM_SOURCE:-"$workspace/iz2s-panel/RUN33"}
release=false
default_output="$workspace/outputs/$name"
if [[ ${1:-} == --release ]]; then
    release=true
    default_output="$project/dist"
    shift
fi
output=${1:-"$default_output"}
sdk=${DEVKITPRO:-"$upstream/devkitpro"}
export DEVKITPRO="$sdk" DEVKITA64="$sdk/devkitA64"
zstd=${IZ2S_ZSTD_ROOT:-"$workspace/modules/kaihen-sd-loader"}
zstd_include=${IZ2S_ZSTD_INCLUDE:-"$zstd/vendor/zstd/lib"}
zstd_lib=${IZ2S_ZSTD_LIB:-"$zstd/build/libzstd.a"}
stb=${IZ2S_STB_INCLUDE:-"$workspace/modules/kaihen-sd-loader/vendor/seedswap/seedswap/source/vendor"}
python=${IZ2S_PYTHON:-"$upstream/.venv/bin/python"}
font=${IZ2S_UI_FONT:-/usr/share/fonts/noto/NotoSans-Bold.ttf}
font_cjk=${IZ2S_CJK_FONT:-/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc}
font_regular=${IZ2S_REGULAR_FONT:-/usr/share/fonts/noto/NotoSans-Regular.ttf}
compiler="$sdk/devkitA64/bin/aarch64-none-elf-gcc"
tools="$sdk/tools/bin"
mkdir -p "$output"
output=$(cd "$output" && pwd)
build=$(mktemp -d "$output/.build.XXXXXXXX")
trap 'rm -r -- "$build"' EXIT

"$python" "$project/adapt_runtime.py" "$upstream" "$build/runtime"
atlas="$build/$name-atlas.bin"
brand="$build/$name-icon.jpg"
header_brand="$build/$name-icon-70.png"
compact_brand="$build/$name-icon-47.png"
nro="$build/$name.nro"
"$python" "$project/font.py" "$font" "$font_cjk" "$font_regular" "$build/font.h" "$atlas"
"$python" "$project/icon.py" "$project/assets/icon.png" "$brand" "$header_brand" "$compact_brand"
common=(-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE)
compile=(-O2 -g -Wall -ffunction-sections -D__SWITCH__ -D__SWITCH2__ -DNX2_GAME_MB=8192
  '-DNX2_VERSION="0.1.12-iz2s"' "${app_flags[@]}"
  "-ffile-prefix-map=$project=." "-ffile-prefix-map=$build=." "-ffile-prefix-map=$stb=stb"
  "-ffile-prefix-map=$zstd_include=zstd" "-ffile-prefix-map=$sdk=devkitpro"
  -I"$sdk/libounce/include" -I"$sdk/portlibs/switch2/include"
  -I"$project/src" -I"$build/runtime" -I"$build" -I"$zstd_include" -isystem "$stb")

objects=()
for source in "$project"/src/*.{c,s} "$build"/runtime/*.{c,s}; do
    object="$build/$(basename "$source").o"
    "$compiler" "${common[@]}" "${compile[@]}" -Wa,-I"$build" -c "$source" -o "$object"
    objects+=("$object")
done
"$compiler" "${common[@]}" -g -o "$build/library.elf" "${objects[@]}" \
    -specs="$sdk/libounce/switch.specs" -L"$sdk/libounce/lib" -L"$sdk/portlibs/switch2/lib" \
    "$zstd_lib" -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx -lm \
    -Wl,--wrap=__libnx_exception_entry,--wrap=svcBreak,--wrap=svcExitProcess,--wrap=svcReturnFromException,--wrap=abort,--wrap=svcExitThread

"$tools/nacptool" --create "$title" IZ2S "$version" "$build/library.nacp"
"$tools/elf2nro" "$build/library.elf" "$nro" \
    "--icon=$brand" "--nacp=$build/library.nacp"
"$python" - "$nro" "$atlas" "$brand" "$header_brand" "$compact_brand" <<'PY'
from pathlib import Path
import struct
import sys

program, atlas, brand, *header_icons = (Path(path).read_bytes() for path in sys.argv[1:])
ro_offset, ro_size = struct.unpack_from("<II", program, 0x28)
if program.find(atlas, ro_offset, ro_offset + ro_size) < 0:
    sys.exit("The generated font atlas is missing from the NRO read-only segment.")
if any(program.find(icon, ro_offset, ro_offset + ro_size) < 0 for icon in header_icons):
    sys.exit("A generated header icon is missing from the NRO read-only segment.")
image_end = struct.unpack_from("<I", program, 0x18)[0]
if program[image_end:image_end + 4] != b"ASET":
    sys.exit("The NRO has no asset section.")
offset, size = struct.unpack_from("<QQ", program, image_end + 8)
if program[image_end + offset:image_end + offset + size] != brand:
    sys.exit("The NRO launcher icon differs from the generated icon.")
PY
if "$release"; then
    release_name="$name-v$version"
    zip="$build/$release_name.zip"
    "$python" - "$zip" "$nro" "$project/LICENSE" "$name" <<'PY'
from zipfile import ZIP_DEFLATED, ZipFile
import sys

output, program, license_file, name = sys.argv[1:]
folder = f"iz2s/modules/{name}"
with ZipFile(output, "w", compression=ZIP_DEFLATED) as bundle:
    bundle.write(program, f"{folder}/{name}.nro")
    bundle.write(license_file, f"{folder}/LICENSE")
PY
    install -m 0644 "$nro" "$output/$release_name.nro"
    install -m 0644 "$zip" "$output"
    exit 0
fi
iza="$build/$name.iza"
node "$project/package.cjs" "$platform" "$nro" "$iza" "$build/verify" "$version"

cc -O2 -Wall -Wextra -DIZ_PREVIEW "${app_flags[@]}" -I"$build" -isystem "$stb" "$project/src/browser.c" \
    "$project/src/cover.c" "$project/src/settings.c" "$project/src/font_atlas.s" -Wa,-I"$build" -lm -o "$build/preview"
preview_args=()
if [[ -n ${IZ2S_PREVIEW_COVERS:-} ]]; then
    preview_args=("$IZ2S_PREVIEW_COVERS/1.jpg" "$IZ2S_PREVIEW_COVERS/2.jpg" "$IZ2S_PREVIEW_COVERS/3.jpg")
fi
"$build/preview" "${preview_args[@]}" > "$build/preview.ppm"
"$python" - "$build/preview.ppm" "$build/preview.png" <<'PY'
from PIL import Image
import sys
with Image.open(sys.argv[1]) as preview:
    assert preview.size == (1920, 1080)
    preview.save(sys.argv[2])
PY

install -m 0644 "$nro" "$iza" "$build/preview.png" "$output"
install -m 0644 "$brand" "$output/icon.jpg"
