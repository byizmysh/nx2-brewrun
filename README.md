# NX2 BreRun

<img src="assets/icon.png" width="176" alt="NX2 BreRun app icon">

Homebrew launcher for running Nintendo Switch 2 game backups in NUP format.

It scans the SD card, shows a gallery of game dumps, and launches compatible titles using the nx2-relink runtime. It reads NUP files directly from the SD card without creating a separate NRO or RomFS image for each game. Use dumps of games you own.

Based on [nx2-relink](https://github.com/Olmectron/nx2-relink) by [Olmectron](https://github.com/Olmectron).

## Install on Switch 2

### kaiHEN and Homebrew Menu

1. Create `SD:/switch/nx2-brerun/` on the console's SD card.
2. Copy `nx2-brerun-v0.6.nro` into that directory as `nx2-brerun.nro`. The complete path is `SD:/switch/nx2-brerun/nx2-brerun.nro`.
3. Open **NX2 BreRun** in Homebrew Menu.

### IZ2S Launcher

Use an IZ2S build that supports `nx2-brerun.nro` in the module folder.

1. Extract `nx2-brerun-v0.6.zip` into the **root of the SD card**. The `iz2s/` directory from the archive goes at the top level of the card.
2. Check that the program is at `SD:/iz2s/modules/nx2-brerun/nx2-brerun.nro`.
3. Open **NX2 BreRun** in the IZ2S app library. Refresh or reopen the library if the entry is missing.

To install manually from the standalone NRO, create `SD:/iz2s/modules/nx2-brerun/` and copy the file there as `nx2-brerun.nro`.

## Add games

- Put `.nup` files anywhere on the SD card. `SD:/Games/` is a convenient location. Keep consecutive parts of a split dump together and use the same naming pattern for every part. Supported examples include `game.nup.001`, `game.nup.aa`, `game.nup.part1`, `game.part1.nup`, `game.001.nup`, `game.nu0`, and `00`, `01` in a game folder.
- When the archive header is readable and data is missing, the library marks the entry **Incomplete NUP** and shows a message to check the transfer and split parts.
- A game folder can contain `romfs.bin` or `romfs.bin.00`, `romfs.bin.01`, and the remaining consecutive parts. To launch it, the same folder needs the original executable modules and `main.npdm` inside `exefs/`.
- The scanner finds NUPs by their headers and skips `.nro` files. It displays the title and artwork embedded in the dump when available. Use a sibling `game.nup.jpg` or `game.nup.png` for a NUP without artwork. For an unpacked game folder, use `cover.jpg`, `cover.png`, `icon.jpg`, or `icon.png` and optionally `title.txt` for the display name.
- For both NUPs and unpacked games, NX2 BreRun creates `nup-<title-id>/` next to the NRO you launched. Logs are in that directory and saved data is in its `save/` subdirectory.

| Launch location | Saved data |
| --- | --- |
| **kaiHEN / Homebrew Menu** | `SD:/switch/nx2-brerun/nup-<title-id>/save/` |
| **[IZ2S](https://github.com/byizmysh/IZ2S-Platform)** | `SD:/iz2s/modules/nx2-brerun/nup-<title-id>/save/` |

To carry over a previous save, copy its `save/` folder into the matching `nup-<title-id>/` directory beside the NRO you use.

Games that use CacheStorage keep it in `nup-<title-id>/cache/<index>/`, next to `save/`. The index is written as four hexadecimal digits, for example `cache/0000/`.

### Split a large NUP for FAT32

Set the split size to **4000 MiB** (4,194,304,000 bytes) so each part fits below the FAT32 limit of 4 GiB. [FAT32 file size limits](https://learn.microsoft.com/en-us/windows/win32/fileio/filesystem-functionality-comparison#limits)

1. Open **7-Zip File Manager** (`7zFM.exe`), browse to the original `.nup` file, and select it.
2. Choose **File → Split file…**, pick an output folder, and enter `4194304000` in **Split to volumes, bytes**. This produces raw segments such as `Game.nup.001` and `Game.nup.002`. [7-Zip split-file example](https://www.7-zip.org/recover.html)
3. Copy every part into the same folder, for example `SD:/Games/Game/`. Keep the full filenames, including the consecutive number suffixes.
4. Open BreRun or press **X** in the library to rescan. BreRun displays the parts as one game and reads them directly.

WinRAR's volume option creates an archive. If the original NUP is inside a multi-volume RAR, extract it on your computer starting with `.part1.rar`, then split the extracted `.nup` with the steps above. [WinRAR volume extraction](https://www.rarlab.com/rar_file.htm)

## Browse and delete saved data

Select a game and press **Y** to open **Options**, then choose **Saved data**. The list shows the files and folders inside that game's `save/` directory. Use up and down to move, **A** to open a folder, and **B** to go back. To delete an entry, select it, press **Y**, and confirm with **A**. Deleting a folder also deletes its contents.

Press **−** in the game library to open **Files**. The file browser also uses a list. Use **Y** there to toggle the game filter. Press **+** in BreRun to return to the launcher.

## Transfer over the local network

1. In the game library or **Files**, press **ZR** to open **Transfer**.
2. Open the displayed HTTP address in a browser on the same network. Enter the code shown on the console, select a `.nup` file, and press **Send file**.
3. When the console confirms the upload, press **B**. The library rescans the card and picks up the game in `SD:/Games/`.

If the card refuses a single file of 4 GiB or more, BreRun splits it into parts of up to 4000 MiB. For example, `Game.nup` becomes `SD:/Games/Game/001`, `002`, and so on. BreRun scans the parts as one game and reads them directly at launch. To force a different part size, add `split=2000` to `menu.cfg` next to the active NRO, for example. Use a value from 1 to 4000 MiB, or use 0 for automatic splitting. An interrupted upload is discarded, and existing entries are protected from replacement.

Automatic splitting happens in BreRun's browser-based **Transfer**. The IZ2S FTP server writes one file per upload. To use an FTP client with FAT32, split large NUPs on the computer first and upload every part into the same game folder.

## Loader room

Select a game and open **Y → Options → Loader room**. Use left or right to choose a value from 384 to 2048 MiB, then press **A** to save it for the next launch. BreRun stores the setting in `nup-<title-id>/loader.cfg` beside the active NRO. Leave it at the default of 384 MiB for games that already launch. Increasing the reserve helps when the loader runs short while preparing a game and leaves less memory for the game while it runs.

## Game language

Select a game and open **Y → Options → Language**. Use the D-pad or left stick to choose one of the 18 Switch 2 languages and press **A** to save it for the next launch. **System** uses the console's current language. BreRun saves the choice separately for each game in `nup-<title-id>/loader.cfg`. The game must include the chosen translation.

## Add-on content

BreRun reads up to 64 add-ons bundled in the same NUP as the game. When an add-on has multiple layers, later versions override earlier files. The game and its add-ons are read directly from the archive, including split NUPs.

Add-on content is enabled by default. Select the game and press **Y → Options**, then highlight **Add-on content** and press **A** to toggle it for the next launch. BreRun saves the choice as `dlc=on` or `dlc=off` in the game's `loader.cfg`.

## Build from source on Linux

Install Bash, Python 3 with Pillow, GNU Make, and the [devkitPro toolchain with ounce-devkit](https://github.com/ouncebrew/ounce-devkit). The SDK directory must contain devkitA64, libounce, the Switch 2 EGL/GLES libraries, and the `nacptool` and `elf2nro` tools. Install Noto Sans Regular, Noto Sans Bold, and Noto Sans CJK Bold. The default font paths match Arch Linux. Use `IZ2S_REGULAR_FONT`, `IZ2S_UI_FONT`, and `IZ2S_CJK_FONT` for other paths.

Prepare the following sources on your build machine.

- Extract [nx2-relink v0.1.12](https://github.com/Olmectron/nx2-relink/releases/tag/v0.1.12). `NX2_RELINK_SOURCE` must point to the directory with its `runtime/` subdirectory.
- Extract [Zstandard v1.5.7](https://github.com/meta/zstd/releases/tag/v1.5.7). Set `ZSTD_SRC` to its source directory.
- Put [`stb_image.h` v2.30](https://github.com/nothings/stb/blob/master/stb_image.h) in a directory of your choice and set `IZ2S_STB_INCLUDE` to that directory.

From the repository root, substitute your paths and run the following commands.

```bash
export DEVKITPRO=/opt/devkitpro
export NX2_RELINK_SOURCE=/path/to/nx2-relink-0.1.12
export IZ2S_STB_INCLUDE=/path/to/stb-headers
export IZ2S_PYTHON=python3
ZSTD_SRC=/path/to/zstd-1.5.7
toolchain="$DEVKITPRO/devkitA64/bin/aarch64-none-elf"

make -C "$ZSTD_SRC/lib" libzstd.a \
  CC="$toolchain-gcc" AR="$toolchain-ar" \
  CFLAGS="-O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE" \
  ZSTD_LIB_COMPRESSION=0 ZSTD_NO_ASM=1 ZSTD_LEGACY_SUPPORT=0

export IZ2S_ZSTD_INCLUDE="$ZSTD_SRC/lib"
export IZ2S_ZSTD_LIB="$ZSTD_SRC/lib/libzstd.a"
./build.sh --release
```

Use a clean output directory. The release build writes `nx2-brerun-v0.6.nro` and `nx2-brerun-v0.6.zip` into `dist/`. Pass a directory as the next argument to change the destination. The build generates the font atlas and converts the high-resolution PNG in `assets/icon.png` to a 256 × 256 JPEG for the NRO. It uses no dumps, keys, or firmware files.

## Credits and notices

Split file naming follows the formats documented by Olmectron in [nx2-flow v0.2.3](https://github.com/Olmectron/nx2-flow/releases/tag/v0.2.3).
Embedded add-on content and packed NPDM diagnostics follow work by ptichalouf and Olmectron in [nx2-flow v0.2.4](https://github.com/Olmectron/nx2-flow/releases/tag/v0.2.4).
The saved-file compatibility fix and indexed SD cache follow work by ptichalouf, cosminneamtiu, and hellmew in [nx2-flow v0.2.5](https://github.com/Olmectron/nx2-flow/releases/tag/v0.2.5).

See [LICENSE](LICENSE) for third-party notices.
