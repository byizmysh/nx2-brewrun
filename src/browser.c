#include "boot.h"
#include "cover.h"
#include "library_cache.h"
#include "nup_parts.h"
#include "settings.h"
#include "transfer.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef IZ_PREVIEW
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>
#endif
#include "font.h"

enum { WIDTH = 1920, HEIGHT = 1080, GAME_COLUMNS = 4, GAME_ROWS = 2,
       GAME_PAGE = GAME_COLUMNS * GAME_ROWS, FILE_PAGE = 10 };
enum { GRID_X = 88, GRID_Y = 222, GRID_BOTTOM = 938, TILE_WIDTH = 268, TILE_HEIGHT = 480,
       TILE_X_GAP = 17, TILE_Y_GAP = 18, TILE_STRIDE = TILE_HEIGHT + TILE_Y_GAP };
enum { BRAND_X = 64, BRAND_Y = 42, BRAND_SIZE = 70 };
enum {
    BG = 0x0d1421, PANEL = 0x131d30, CARD = 0x1b2940, CONTROL = 0x242e42,
    OUTLINE = 0x34445c, TEXT = 0xf4f7fc, MUTED = 0xb1bed1, TEAL = 0x8ce8d7,
    GOLD = 0xffce88, ALERT_BG = 0x392435, ALERT_INK = 0xffc5d7,
    DELETE_EDGE = 0xd46375, DELETE_TEXT = 0xff96a5
};
enum { BODY_TEXT = 1, TITLE_TEXT = 2, FOOTER_TEXT = 3 };
typedef enum { VIEW_LIBRARY, VIEW_FILES, VIEW_SAVES } View;
typedef enum { DIALOG_NONE, DIALOG_OPTIONS, DIALOG_TRANSFER, DIALOG_CANCEL_TRANSFER,
               DIALOG_SETTINGS, DIALOG_ROOM, DIALOG_EXEFS, DIALOG_LANGUAGE, DIALOG_CONFIRM_SETTINGS,
               DIALOG_DELETE, DIALOG_DELETE_IMAGE, DIALOG_DELETE_ALL_SAVES, DIALOG_LAUNCH_ERROR } Dialog;
enum { GAME_OPTION_SAVES, GAME_OPTION_SETTINGS,
       GAME_OPTION_DELETE_IMAGE, GAME_OPTION_DELETE_ALL_SAVES, GAME_OPTION_COUNT };
enum { GAME_SETTING_ROOM, GAME_SETTING_EXEFS, GAME_SETTING_LANGUAGE, GAME_SETTING_NATIVE_NVN, GAME_SETTING_DLC };
enum { LANGUAGE_COLUMNS = 2 };

static const char sd_root[] = "sdmc:/";
extern const unsigned char iz_brand[], iz_brand_end[];
extern const unsigned char iz_brand_compact[], iz_brand_compact_end[];

typedef IzLibraryEntry Entry;
typedef struct {
    char name[512];
    uint64_t key;
    Cover image;
    bool used;
} CoverSlot;

typedef struct {
    Entry *items;
    size_t count, capacity, selected, first, next_cover, scan_count, cover_warm;
    size_t library_first, library_column;
    double scroll, scroll_from, scroll_to;
    uint64_t scroll_started;
    uint64_t cache_generation;
    char cache_dir[512];
    CoverSlot covers[GAME_PAGE * 2];
    Cover brand;
    char path[512], notice[200];
    char save_root[512], save_path[512], return_game[512], save_title[128];
    bool show_all, scanning, restore_game, return_save_options;
    View view;
    Dialog dialog;
    unsigned game_option;
    char address[sizeof("http://255.255.255.255:65535")], code[9];
    IzTransferStatus transfer_status;
    uint64_t received, total;
    char transfer_name[IZ_TRANSFER_NAME_CAP], cancel_transfer_name[IZ_TRANSFER_NAME_CAP];
    unsigned transfer_pending, transfer_completed, transfer_job, cancel_transfer_job;
    bool transfer_refresh;
    unsigned game_setting, requested_room;
    IzGameSettings settings, original_settings;
    Dialog settings_return;
    bool settings_discard, settings_error, settings_exit, exit_requested;
    IzLaunchError launch_error;
} Browser;

static size_t page_size(const Browser *b) {
    return b->view == VIEW_LIBRARY ? GAME_PAGE : FILE_PAGE;
}

static size_t game_rows(size_t count) {
    return count / GAME_COLUMNS + (count % GAME_COLUMNS != 0);
}

static const char *view_path(const Browser *b) {
    if (b->view == VIEW_SAVES) return b->save_path;
    return b->view == VIEW_FILES ? b->path : sd_root;
}

static bool library_game(const Browser *b) {
    return b->view == VIEW_LIBRARY && b->count;
}

static bool game_option_available(const Entry *game, unsigned option) {
    return option == GAME_OPTION_DELETE_IMAGE ? game->loose || game->nup : game->title_id != 0;
}

static bool settings_dirty(const Browser *b) {
    const IzGameSettings *now = &b->settings, *before = &b->original_settings;
    return now->room != before->room || now->exefs_budget != before->exefs_budget ||
           now->language != before->language || now->native_nvn != before->native_nvn ||
           now->dlc_enabled != before->dlc_enabled;
}

static unsigned min_room_for_settings(const Browser *b) {
    return iz_room_for_exefs(IZ_ROOM_DEFAULT_MIB, b->settings.exefs_budget);
}

static const char *display_name(const Entry *entry) {
    if (entry->title[0]) return entry->title;
    if (entry->loose && !entry->name[0]) return "SD card";
    const char *slash = strrchr(entry->name, '/');
    return slash ? slash + 1 : entry->name;
}

static uint32_t *pixels;
static int clip_top, clip_bottom = HEIGHT;
static uint32_t rgba(unsigned rgb) {
    return 0xff000000u | (rgb & 0xffu) << 16 | (rgb & 0xff00u) | (rgb >> 16);
}

static void rect(int x, int y, int w, int h, unsigned color) {
    if (x < 0 || w < 1 || h < 1 || x + w > WIDTH) return;
    const uint32_t c = rgba(color);
    for (int iy = y > clip_top ? y : clip_top; iy < y + h && iy < clip_bottom; ++iy) {
        uint32_t *line = pixels + iy * WIDTH + x;
        for (int ix = 0; ix < w; ++ix) line[ix] = c;
    }
}

static void card(int x, int y, int w, int h, unsigned color) {
    const int inset[] = {9, 6, 4, 3, 2, 1, 1, 0};
    for (int d = 0; d < 8; ++d) {
        rect(x + inset[d], y + d, w - 2 * inset[d], 1, color);
        rect(x + inset[d], y + h - 1 - d, w - 2 * inset[d], 1, color);
    }
    rect(x, y + 8, w, h - 16, color);
}

static unsigned next_utf8(const char **text) {
    const unsigned char *p = (const unsigned char *)*text;
    const unsigned lead = *p++;
    if (lead < 0x80) {
        *text = (const char *)p;
        return lead >= 32 && lead != 127 ? lead : 0xfffd;
    }
    const unsigned count = lead >= 0xc2 && lead <= 0xdf ? 1 :
        lead >= 0xe0 && lead <= 0xef ? 2 : lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
    unsigned code = lead & (count == 1 ? 31u : count == 2 ? 15u : 7u);
    for (unsigned i = 0; i < count; ++i) {
        if ((*p & 0xc0) != 0x80) {
            *text = (const char *)p;
            return 0xfffd;
        }
        code = code << 6 | (*p++ & 63);
    }
    *text = (const char *)p;
    const unsigned minimum[] = {0, 0x80, 0x800, 0x10000};
    return count && code >= minimum[count] && code <= 0x10ffff &&
        !(code >= 0xd800 && code <= 0xdfff) ? code : 0xfffd;
}

static unsigned font_u32(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static const unsigned char *font_find(unsigned code, int scale) {
    const bool large = scale == TITLE_TEXT, footer = scale == FOOTER_TEXT;
    const unsigned char *table = iz_font + (footer ? FONT_FOOTER_OFFSET : large ? FONT_BIG_OFFSET : 0);
    size_t lo = 0, hi = footer ? FONT_FOOTER_COUNT : large ? FONT_BIG_COUNT : FONT_SMALL_COUNT;
    const size_t count = hi;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const unsigned point = font_u32(table + mid * FONT_RECORD);
        if (point < code) lo = mid + 1;
        else hi = mid;
    }
    if (lo < count && font_u32(table + lo * FONT_RECORD) == code)
        return table + lo * FONT_RECORD;
    return code == '?' ? NULL : font_find('?', scale);
}

static int text_width(const char *label, int scale) {
    int width = 0;
    while (*label) {
        const unsigned char *entry = font_find(next_utf8(&label), scale);
        if (entry) width += entry[12];
    }
    return width;
}

static void glyph(int x, int y, const unsigned char *entry, unsigned ink, int scale, int right) {
    if (!entry) return;
    x += (int8_t)entry[8];
    y += (scale == TITLE_TEXT ? 44 : 23) + (int8_t)entry[9];
    const unsigned char *alpha = iz_font + font_u32(entry + 4);
    const unsigned red = (ink >> 16) & 255, green = (ink >> 8) & 255, blue = ink & 255;
    for (int j = 0; j < entry[11]; ++j) {
        if (y + j < clip_top || y + j >= clip_bottom) continue;
        uint32_t *row = pixels + (y + j) * WIDTH;
        for (int i = 0; i < entry[10]; ++i) {
            const unsigned point = j * entry[10] + i;
            const unsigned opacity = ((alpha[point / 2] >> (4 * (point & 1))) & 15u) * 17u;
            if (!opacity || x + i < 0 || x + i >= WIDTH || x + i >= right) continue;
            const uint32_t bg = row[x + i];
            const unsigned r = (red * opacity + (bg & 255) * (255 - opacity)) / 255;
            const unsigned g = (green * opacity + ((bg >> 8) & 255) * (255 - opacity)) / 255;
            const unsigned b = (blue * opacity + ((bg >> 16) & 255) * (255 - opacity)) / 255;
            row[x + i] = 0xff000000u | b << 16 | g << 8 | r;
        }
    }
}

static void text(int x, int y, const char *label, unsigned ink, unsigned background,
                 int scale, int width) {
    (void)background;
    const bool truncate = text_width(label, scale) > width;
    const int right = x + width;
    const unsigned char *ellipsis = truncate ? font_find(0x2026, scale) : NULL;
    const int last = right - (ellipsis ? ellipsis[12] : 0);
    while (*label) {
        const unsigned char *entry = font_find(next_utf8(&label), scale);
        if (!entry) continue;
        if (x + entry[12] > last) break;
        glyph(x, y, entry, ink, scale, right);
        x += entry[12];
    }
    if (ellipsis) glyph(x, y, ellipsis, ink, scale, right);
}

static void folder(int x, int y, unsigned color) {
    rect(x + 3, y + 1, 15, 5, color);
    card(x, y + 5, 38, 25, color);
}

static void package_icon(int x, int y, unsigned color, unsigned background) {
    card(x, y, 34, 36, color);
    rect(x + 7, y + 7, 20, 3, background);
    rect(x + 7, y + 15, 16, 3, background);
    rect(x + 7, y + 23, 12, 3, background);
}

#ifndef IZ_PREVIEW
static bool has_extension(const char *name, const char *extension) {
    const char *dot = strrchr(name, '.');
    return dot && !strcasecmp(dot, extension);
}

static bool has_nup_name(const char *name, bool first_part) {
    if (has_extension(name, ".nup")) return true;
    const char *suffix = strrchr(name, '.');
    return first_part && suffix && suffix - name >= 4 && !strncasecmp(suffix - 4, ".nup", 4);
}
#endif

static void bytes_label(char *buffer, size_t length, uint64_t bytes) {
    const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double value = (double)bytes;
    unsigned unit = 0;
    while (value >= 1024 && unit < 3) { value /= 1024; ++unit; }
    if (unit) snprintf(buffer, length, "%.1f %s", value, units[unit]);
    else snprintf(buffer, length, "%llu B", (unsigned long long)bytes);
}

static const CoverSlot *find_cover(const Browser *b, const Entry *entry) {
    for (size_t i = 0; i < sizeof(b->covers) / sizeof(*b->covers); ++i)
        if (b->covers[i].used && b->covers[i].key == entry->art_key && !strcmp(b->covers[i].name, entry->name))
            return &b->covers[i];
    return NULL;
}

static void clear_covers(Browser *b) {
    for (size_t i = 0; i < sizeof(b->covers) / sizeof(*b->covers); ++i) {
        cover_clear(&b->covers[i].image);
        b->covers[i].used = false;
    }
    b->next_cover = 0;
}

static void cover_pixels(int x, int y, int width, int height, const Cover *image) {
    const int start = y < clip_top ? clip_top - y : 0;
    for (int j = start; j < height && y + j < clip_bottom; ++j) {
        const unsigned char *src = image->pixels + (j * image->height / height) * image->width * 4;
        uint32_t *dest = pixels + (y + j) * WIDTH + x;
        for (int i = 0; i < width; ++i) {
            const unsigned char *p = src + (i * image->width / width) * 4;
            if (p[3] == 255) dest[i] = rgba((unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2]);
            else if (p[3]) {
                const uint32_t bg = dest[i];
                const unsigned r = (p[0] * p[3] + (bg & 255) * (255 - p[3])) / 255;
                const unsigned g = (p[1] * p[3] + ((bg >> 8) & 255) * (255 - p[3])) / 255;
                const unsigned b = (p[2] * p[3] + ((bg >> 16) & 255) * (255 - p[3])) / 255;
                dest[i] = 0xff000000u | b << 16 | g << 8 | r;
            }
        }
    }
}

static unsigned blend_color(unsigned a, unsigned b, unsigned amount) {
    unsigned color = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const unsigned channel = (((a >> shift) & 255) * (255 - amount) +
                                  ((b >> shift) & 255) * amount) / 255;
        color |= channel << shift;
    }
    return color;
}

static void rounded_panel(int x, int y, int width, int height, int radius, unsigned top, unsigned bottom) {
    for (int row = 0; row < height; ++row) {
        const int edge = row < height / 2 ? row : height - row - 1;
        int inset = 0;
        if (edge < radius) {
            const int distance = radius - edge - 1;
            inset = radius - (int)sqrt((double)(radius * radius - distance * distance));
        }
        rect(x + inset, y + row, width - 2 * inset, 1, blend_color(top, bottom, row * 255u / (height - 1)));
    }
}

static void read_brand(Browser *b, bool compact) {
    const unsigned char *start = compact ? iz_brand_compact : iz_brand;
    const unsigned char *end = compact ? iz_brand_compact_end : iz_brand_end;
    cover_decode(&b->brand, start, (uintptr_t)end - (uintptr_t)start);
}

static void artwork(const Browser *b, const Entry *e, int x, int y, int width, int height) {
    const CoverSlot *slot = e ? find_cover(b, e) : NULL;
    const Cover *cover = slot ? &slot->image : NULL;
    rounded_panel(x, y, width, height, 16, 0x101928, 0x0c1322);
    const unsigned ink = e && (e->directory || e->needs_exefs) ? GOLD : TEAL;
    if (cover && cover->pixels) {
        int w = width - 16, h = height - 16;
        if ((int64_t)w * cover->height > (int64_t)h * cover->width)
            w = h * cover->width / cover->height;
        else h = w * cover->height / cover->width;
        if (w > 0 && h > 0) cover_pixels(x + (width - w) / 2, y + (height - h) / 2, w, h, cover);
    } else {
        const int inner = height > 230 ? 124 : 94;
        const int cx = x + width / 2, cy = y + height / 2;
        card(cx - inner / 2, cy - inner / 2, inner, inner, PANEL);
        card(cx - inner / 2 + 7, cy - inner / 2 + 7, inner - 14, inner - 14, CARD);
        if (e && e->directory) folder(cx - 19, cy - 15, ink);
        else package_icon(cx - 17, cy - 18, ink, CARD);
    }
}

static void wrapped_title(int x, int y, const char *label, int scale, int width) {
    char first[512];
    const char *cursor = label, *end = label, *space = NULL;
    int used = 0;
    while (*cursor && cursor - label < (ptrdiff_t)sizeof(first) - 5) {
        const char *start = cursor;
        const unsigned point = next_utf8(&cursor);
        const unsigned char *letter = font_find(point, scale);
        if (letter) used += letter[12];
        if (used > width) break;
        end = cursor;
        if (point == ' ') space = start;
    }
    if (!*end || end == label) {
        text(x, y, label, TEXT, PANEL, scale, width);
        return;
    }
    if (space && space != label) end = space;
    const size_t size = (size_t)(end - label);
    memcpy(first, label, size);
    first[size] = 0;
    text(x, y, first, TEXT, PANEL, scale, width);
    while (*end == ' ') ++end;
    text(x, y + (scale == TITLE_TEXT ? 50 : 28), end, TEXT, PANEL, scale, width);
}

static int keycap_width(const char *key) {
    const int width = text_width(key, BODY_TEXT) + 20;
    return width > 36 ? width : 36;
}

static int shortcut_width(const char *key, const char *label) {
    return keycap_width(key) + 10 + text_width(label, FOOTER_TEXT);
}

static int shortcut(int x, const char *key, const char *label) {
    const int width = keycap_width(key), text_size = text_width(key, BODY_TEXT);
    rounded_panel(x, 1004, width, 36, 18, CONTROL, CONTROL);
    text(x + (width - text_size) / 2, 1010, key, TEXT, CONTROL, BODY_TEXT, text_size + 2);
    text(x + width + 10, 1010, label, MUTED, BG, FOOTER_TEXT, text_width(label, FOOTER_TEXT) + 3);
    return x + shortcut_width(key, label) + 37;
}

static void draw_footer(const Browser *b) {
    rect(64, 982, 1792, 1, OUTLINE);
    const int launcher = 1856 - shortcut_width("+", "Launcher");
    shortcut(launcher, "+", "Launcher");
    if (b->dialog == DIALOG_TRANSFER) {
        int next = shortcut(64, "B", "Hide");
        if (b->transfer_status == IZ_TRANSFER_RECEIVING) next = shortcut(next, "X", "Pause transfer");
        if (b->transfer_job) next = shortcut(next, "Y", "Cancel transfer");
        shortcut(next, "L", "Stop receiver");
        return;
    }
    if (b->dialog == DIALOG_CANCEL_TRANSFER) {
        const int next = shortcut(64, "A", "Discard partial data");
        shortcut(next, "B", "Back");
        return;
    }
    if (b->dialog == DIALOG_LAUNCH_ERROR) {
        shortcut(64, "A / B", "Close");
        return;
    }
    if (b->dialog != DIALOG_NONE) {
        const bool confirm_delete = b->dialog == DIALOG_DELETE || b->dialog == DIALOG_DELETE_IMAGE ||
            b->dialog == DIALOG_DELETE_ALL_SAVES;
        const bool toggle = b->dialog == DIALOG_SETTINGS && b->game_setting >= GAME_SETTING_NATIVE_NVN;
        const bool select = b->dialog == DIALOG_ROOM || b->dialog == DIALOG_EXEFS ||
            b->dialog == DIALOG_LANGUAGE || b->dialog == DIALOG_CONFIRM_SETTINGS;
        const char *action = select ? "Select" :
            confirm_delete ? "Delete" : toggle ? "Change" : "Open";
        int next = shortcut(64, "A", action);
        next = shortcut(next, "B", "Back");
        if (!confirm_delete) next = shortcut(next, "D-pad", "Choose");
        if (b->dialog == DIALOG_EXEFS) shortcut(next, "L/R", "128 MiB");
        return;
    }
    const bool library = b->view == VIEW_LIBRARY, saves = b->view == VIEW_SAVES;
    int next = 64;
    if (!saves || (b->count && b->items[b->selected].directory))
        next = shortcut(next, "A", library ? "Play" : "Open");
    next = shortcut(next, "B", "Back");
    next = shortcut(next, "L/R", "Page");
    next = shortcut(next, "D-pad", "Move");
    next = shortcut(next, "X", "Refresh");
    if (library_game(b)) next = shortcut(next, "Y", "Options");
    if (b->view == VIEW_FILES) next = shortcut(next, "Y", "Filter");
    if (saves && b->count) next = shortcut(next, "Y", "Delete");
    if (!saves) {
        char label[36] = "Transfers";
        if (b->transfer_status == IZ_TRANSFER_RECEIVING && b->total)
            snprintf(label, sizeof(label), "Transfers %u%%", (unsigned)(100 * b->received / b->total));
        shortcut(next, "ZR", label);
    }

    const char *mode = library ? "Files" : "Games";
    shortcut(launcher - 43 - shortcut_width("−", mode), "−", mode);
}

static const char *transfer_label(IzTransferStatus status) {
    switch (status) {
        case IZ_TRANSFER_READY: return "Waiting for a NUP file";
        case IZ_TRANSFER_RECEIVING: return "Receiving file";
        case IZ_TRANSFER_DONE: return "Saved to SD:/Games/";
        case IZ_TRANSFER_SPLIT: return "Saved in parts to SD:/Games/";
        case IZ_TRANSFER_INTERRUPTED: return "Paused. Select the same file in the browser to resume.";
        case IZ_TRANSFER_CANCELLED: return "Transfer cancelled. Partial data removed.";
        case IZ_TRANSFER_INVALID: return "Invalid or incomplete NUP";
        case IZ_TRANSFER_EXISTS: return "A file or folder with that name already exists.";
        case IZ_TRANSFER_NO_SPACE: return "The SD card is full.";
        case IZ_TRANSFER_DISK_ERROR: return "Could not write the SD card.";
        case IZ_TRANSFER_LOCKED: return "Too many attempts. Stop the receiver to get a new code.";
        case IZ_TRANSFER_RESUME_ERROR: return "Saved transfer could not be verified.";
        default: return "The network disconnected.";
    }
}

static void game_menu_row(int y, const char *label, const char *detail, bool active, bool danger) {
    const unsigned background = danger ? (active ? 0x482735 : 0x2e202b) : (active ? 0x294357 : CARD);
    const unsigned accent = danger ? DELETE_TEXT : TEAL;
    const unsigned edge = active ? accent : DELETE_EDGE;
    const int inset = danger ? 2 : 0;
    if (danger) rounded_panel(524, y, 872, 56, 10, edge, edge);
    rounded_panel(524 + inset, y + inset, 872 - 2 * inset, 56 - 2 * inset, 10 - inset, background, background);
    if (active) rect(527, y + 8, 4, 40, accent);
    text(544, y + 4, label, active || danger ? accent : TEXT, background, BODY_TEXT, 824);
    text(544, y + 30, detail, danger ? ALERT_INK : MUTED, background, FOOTER_TEXT, 824);
}

static void draw_game_options(const Browser *b, const Entry *current) {
    const char *labels[] = {"Saved data", "Game settings", "Delete game image", "Delete saved data"};
    const char *details[] = {"Open this game's save files", "Memory, language, graphics and add-on content",
        current->loose ? "Remove the game's RomFS image from the SD card" : "Remove the NUP from the SD card",
        "Clear all saves for this game"};
    text(524, 294, "Game options", TEXT, PANEL, TITLE_TEXT, 872);
    text(524, 388, display_name(current), MUTED, PANEL, BODY_TEXT, 872);
    int y = 427;
    for (unsigned i = 0; i < GAME_OPTION_COUNT; ++i) {
        if (!game_option_available(current, i)) continue;
        game_menu_row(y, labels[i], details[i], i == b->game_option, i >= GAME_OPTION_DELETE_IMAGE);
        y += 63;
    }
}

static void draw_game_settings(const Browser *b, const Entry *current) {
    char room[48], budget[48];
    snprintf(room, sizeof(room), "%u MiB total reserve", b->settings.room);
    if (b->settings.exefs_budget)
        snprintf(budget, sizeof(budget), "%u MiB", b->settings.exefs_budget);
    else snprintf(budget, sizeof(budget), "Auto  ·  %u MiB per module", IZ_EXEFS_MIN_MIB);
    const char *labels[] = {"Loader room", "ExeFS budget", "Language", "Native NVN", "Add-on content"};
    const char *details[] = {room, budget, iz_game_language_label(b->settings.language),
        b->settings.native_nvn ? "On" : "Off", b->settings.dlc_enabled ? "On" : "Off"};
    text(524, 294, "Game settings", TEXT, PANEL, TITLE_TEXT, 872);
    text(524, 388, display_name(current), MUTED, PANEL, BODY_TEXT, 872);
    const unsigned last = current->nup ? GAME_SETTING_DLC : GAME_SETTING_NATIVE_NVN;
    for (unsigned i = 0; i <= last; ++i)
        game_menu_row(427 + 63 * (int)i, labels[i], details[i], i == b->game_setting, false);
    if (settings_dirty(b))
        text(524, 435 + 63 * (int)(last + 1), "Unsaved changes", GOLD, PANEL, FOOTER_TEXT, 872);
}

static void draw_settings_confirmation(const Browser *b, const Entry *current) {
    text(524, 294, "Save changes?", TEXT, PANEL, TITLE_TEXT, 872);
    text(524, 388, display_name(current), MUTED, PANEL, BODY_TEXT, 872);
    game_menu_row(462, "Save changes", "Apply the new settings on the next launch", !b->settings_discard, false);
    game_menu_row(535, "Discard changes", "Keep the saved settings", b->settings_discard, false);
    if (min_room_for_settings(b) > b->settings.room) {
        char problem[96];
        snprintf(problem, sizeof(problem), "The ExeFS budget needs at least %u MiB of Loader room.",
                 min_room_for_settings(b));
        text(524, 625, problem, ALERT_INK, PANEL, FOOTER_TEXT, 872);
    } else if (b->settings_error) {
        text(524, 625, "Could not write the game settings to the SD card.", ALERT_INK, PANEL, FOOTER_TEXT, 872);
    } else if (b->settings.room > b->requested_room) {
        char change[116];
        snprintf(change, sizeof(change), "Loader room will increase to %u MiB for the %u MiB ExeFS budget.",
                 b->settings.room, b->settings.exefs_budget);
        text(524, 625, change, GOLD, PANEL, FOOTER_TEXT, 872);
    }
}

static void draw_language_picker(const Browser *b, const Entry *current) {
    rounded_panel(396, 158, 1128, 778, 24, PANEL, 0x111a2a);
    text(444, 188, "Game language", TEXT, PANEL, TITLE_TEXT, 1032);
    rect(444, 253, 1032, 1, OUTLINE);
    text(444, 276, display_name(current), MUTED, PANEL, BODY_TEXT, 1032);
    for (unsigned i = 0; i < IZ_LANGUAGE_COUNT; ++i) {
        const int x = 444 + 524 * (i % LANGUAGE_COLUMNS), y = 333 + 54 * (i / LANGUAGE_COLUMNS);
        const bool selected = i == b->settings.language;
        const unsigned background = selected ? 0x294357 : CARD;
        rounded_panel(x, y, 508, 44, 10, background, background);
        if (selected) rect(x, y + 7, 4, 30, TEAL);
        text(x + 18, y + 8, iz_game_language_label((IzGameLanguage)i),
             selected ? TEAL : TEXT, background, BODY_TEXT, 472);
    }
    text(444, 887, "Available translations depend on the game.", MUTED, PANEL, FOOTER_TEXT, 1032);
}

static void draw_launch_error(const IzLaunchError *error) {
    const int x = 410, width = 1100;
    rounded_panel(356, 174, 1208, 732, 24, PANEL, 0x111a2a);
    text(x, 221, "Game could not start", ALERT_INK, PANEL, TITLE_TEXT, width);
    rect(x, 295, width, 1, OUTLINE);
    const char *reason = error->reason ? error->reason : "No additional details were returned.";
    wrapped_title(x, 321, reason, BODY_TEXT, width);

    char line[128];
    int y = 404;
    if (error->module) snprintf(line, sizeof(line), "%s  /  Module %s", error->stage, error->module);
    else snprintf(line, sizeof(line), "%s", error->stage);
    text(x, y, line, TEAL, PANEL, BODY_TEXT, width);
    y += 45;
    if (error->path[0]) {
        text(x, y, error->path, MUTED, PANEL, BODY_TEXT, width);
        y += 41;
    }
    if (error->title || error->result) {
        if (error->result) snprintf(line, sizeof(line), "Horizon result  0x%08lX", (unsigned long)error->result);
        else snprintf(line, sizeof(line), "Title ID  %016llX", (unsigned long long)error->title);
        text(x, y, line, MUTED, PANEL, BODY_TEXT, width);
        y += 41;
    }
    if (error->room) {
        char budget[24] = "Auto";
        if (error->saved_budget) snprintf(budget, sizeof(budget), "%u MiB", error->saved_budget);
        snprintf(line, sizeof(line), "Loader room  %u MiB  /  ExeFS budget  %s", error->room, budget);
        text(x, y, line, TEXT, PANEL, BODY_TEXT, width);
        y += 41;
    }
    if (error->total_limit) {
        snprintf(line, sizeof(line), "ExeFS limits  %u MiB per initialized module  /  %u MiB total",
                 error->initialized_limit, error->total_limit);
        text(x, y, line, MUTED, PANEL, BODY_TEXT, width);
        y += 41;
    }
    if (error->exefs.check) {
        snprintf(line, sizeof(line), "%s  /  Needs at least %u MiB  /  Limit %u MiB",
                 iz_exefs_check_label(error->exefs.check), iz_bytes_to_mib_ceil(error->exefs.required),
                 iz_bytes_to_mib_ceil(error->exefs.available));
        text(x, y, line, ALERT_INK, PANEL, BODY_TEXT, width);
    }
    rect(x, 816, width, 1, OUTLINE);
    text(x, 838, error->logged ? "Saved to " IZ_LAUNCH_LOG " beside the BreRun NRO." :
         "Could not write " IZ_LAUNCH_LOG " to the SD card.", MUTED, PANEL, FOOTER_TEXT, width);
}

static void draw_dialog(const Browser *b, const Entry *current) {
    if (b->dialog == DIALOG_LAUNCH_ERROR) {
        draw_launch_error(&b->launch_error);
        return;
    }
    if (b->dialog == DIALOG_LANGUAGE) {
        draw_language_picker(b, current);
        return;
    }
    int height = b->dialog == DIALOG_TRANSFER ? 580 : 495;
    if (b->dialog == DIALOG_OPTIONS) {
        height = 189;
        for (unsigned i = 0; i < GAME_OPTION_COUNT; ++i)
            if (game_option_available(current, i)) height += 63;
    }
    if (b->dialog == DIALOG_SETTINGS) height = current->nup ? 535 : 472;
    rounded_panel(464, 251, 992, height, 24, PANEL, 0x111a2a);
    rect(524, 367, 872, 1, OUTLINE);
    if (b->dialog == DIALOG_OPTIONS) {
        draw_game_options(b, current);
    } else if (b->dialog == DIALOG_SETTINGS) {
        draw_game_settings(b, current);
    } else if (b->dialog == DIALOG_CONFIRM_SETTINGS) {
        draw_settings_confirmation(b, current);
    } else if (b->dialog == DIALOG_DELETE) {
        text(524, 294, "Delete saved data?", TEXT, PANEL, TITLE_TEXT, 872);
        text(524, 404, b->save_title, MUTED, PANEL, BODY_TEXT, 872);
        text(524, 460, display_name(current), TEAL, PANEL, TITLE_TEXT, 872);
        const char *warning = current->directory ? "This will delete the folder and everything in it." :
            "This will permanently delete the selected save file.";
        text(524, 570, warning, ALERT_INK, PANEL, BODY_TEXT, 872);
    } else if (b->dialog == DIALOG_DELETE_IMAGE || b->dialog == DIALOG_DELETE_ALL_SAVES) {
        const bool image = b->dialog == DIALOG_DELETE_IMAGE;
        text(524, 294, image ? "Delete game image?" : "Delete all saved data?", DELETE_TEXT, PANEL, TITLE_TEXT, 872);
        text(524, 412, display_name(current), TEXT, PANEL, TITLE_TEXT, 872);
        const char *warning = !image ? "All saves for this game will be deleted." :
            current->loose ? "The RomFS image and its parts will be deleted." :
            current->split ? "The selected NUP parts will be deleted." : "The selected NUP file will be deleted.";
        text(524, 499, warning, ALERT_INK, PANEL, BODY_TEXT, 872);
    } else if (b->dialog == DIALOG_CANCEL_TRANSFER) {
        text(524, 294, "Cancel transfer?", TEXT, PANEL, TITLE_TEXT, 872);
        text(524, 410, b->cancel_transfer_name, TEAL, PANEL, BODY_TEXT, 872);
        text(524, 488, "Partial data for this transfer will be deleted from the SD card.", ALERT_INK, PANEL, BODY_TEXT, 872);
        text(524, 548, "Send the file again to start from the beginning.", MUTED, PANEL, BODY_TEXT, 872);
    } else if (b->dialog == DIALOG_ROOM || b->dialog == DIALOG_EXEFS) {
        const bool edit_budget = b->dialog == DIALOG_EXEFS;
        const unsigned room = b->settings.room, budget = b->settings.exefs_budget;
        const unsigned available = room - IZ_ROOM_OVERHEAD_MIB;
        const bool fits = budget <= available;
        char amount[64], details[96];
        if (edit_budget && !budget)
            snprintf(amount, sizeof(amount), "‹     Auto     ›");
        else snprintf(amount, sizeof(amount), "‹     %u MiB     ›", edit_budget ? budget : room);
        snprintf(details, sizeof(details), "Available for ExeFS  %u MiB  ·  Loader room  %u MiB", available, room);
        text(524, 294, edit_budget ? "ExeFS budget" : "Loader room", TEXT, PANEL, TITLE_TEXT, 872);
        text(524, 396, current ? display_name(current) : "", MUTED, PANEL, BODY_TEXT, 872);
        text(524, 463, amount, fits ? TEAL : ALERT_INK, PANEL, TITLE_TEXT, 872);
        text(524, 550, details, TEXT, PANEL, BODY_TEXT, 872);
        if (!fits) {
            snprintf(details, sizeof(details), "This budget needs a Loader room of at least %u MiB.",
                     min_room_for_settings(b));
            text(524, 606, details, ALERT_INK, PANEL, BODY_TEXT, 872);
        } else if (room > b->requested_room) {
            snprintf(details, sizeof(details), "Loader room increased to %u MiB to fit this budget.", room);
            text(524, 606, details, GOLD, PANEL, BODY_TEXT, 872);
        } else if (edit_budget) {
            snprintf(details, sizeof(details), "Auto keeps the original %u MiB limit per module.", IZ_EXEFS_MIN_MIB);
            text(524, 606, details, MUTED, PANEL, BODY_TEXT, 872);
        } else {
            text(524, 606, "A larger reserve leaves less memory for the game.", MUTED, PANEL, BODY_TEXT, 872);
        }
    } else {
        char code[28];
        snprintf(code, sizeof(code), "Code  %s", b->code);
        text(524, 294, "Transfers", TEXT, PANEL, TITLE_TEXT, 872);
        text(524, 394, "Open in a web browser on the same network.", MUTED, PANEL, BODY_TEXT, 872);
        text(524, 445, b->address, TEXT, PANEL, TITLE_TEXT, 872);
        text(524, 522, code, TEAL, PANEL, BODY_TEXT, 872);
        if (b->transfer_name[0]) text(524, 576, b->transfer_name, TEXT, PANEL, BODY_TEXT, 872);
        text(524, 618, transfer_label(b->transfer_status), TEXT, PANEL, BODY_TEXT, 872);
        if (b->total && (b->transfer_status == IZ_TRANSFER_RECEIVING || b->transfer_status == IZ_TRANSFER_INTERRUPTED)) {
            char received[40], total[40], progress[88];
            bytes_label(received, sizeof(received), b->received);
            bytes_label(total, sizeof(total), b->total);
            snprintf(progress, sizeof(progress), "%s / %s", received, total);
            rect(524, 665, 872, 8, CONTROL);
            rect(524, 665, (int)(872 * b->received / b->total), 8, TEAL);
            text(524, 686, progress, MUTED, PANEL, FOOTER_TEXT, 872);
        }
        if (b->transfer_pending) {
            char saved[48];
            snprintf(saved, sizeof(saved), "Transfers stored on SD: %u", b->transfer_pending);
            text(524, 726, saved, MUTED, PANEL, FOOTER_TEXT, 872);
        }
        text(524, 768, "Hide this screen to keep receiving in the background.", MUTED, PANEL, FOOTER_TEXT, 872);
    }
}

static const char *entry_type(const Entry *entry) {
    if (entry->directory) return "Folder";
    if (entry->loose) return entry->needs_exefs ? "RomFS" : "ExeFS + RomFS";
    if (entry->incomplete) return "Incomplete NUP";
    if (entry->nup) return entry->split ? "Split NUP" : "NUP";
    return "File";
}

static void draw_notice(const Browser *b) {
    card(1285, 781, 541, 143, ALERT_BG);
    text(1302, 796, "Status", ALERT_INK, ALERT_BG, 1, 490);
    const char *position = b->notice;
    for (int line = 0; line < 3 && *position; ++line) {
        size_t n = strlen(position);
        if (n > 34) {
            n = 34;
            while (n > 16 && position[n] != ' ') --n;
        }
        char segment[35];
        memcpy(segment, position, n);
        segment[n] = 0;
        text(1302, 830 + 26 * line, segment, TEXT, ALERT_BG, 1, 490);
        position += n;
        while (*position == ' ') ++position;
    }
}

static unsigned tile_color(const Entry *entry) {
    static const unsigned colors[] = {0x405570, 0x4d426e, 0x326169, 0x59455e, 0x39526c};
    uint64_t seed = entry->title_id;
    if (!seed)
        for (const unsigned char *p = (const unsigned char *)entry->name; *p; ++p)
            seed = seed * 31 + *p;
    return colors[seed % (sizeof(colors) / sizeof(*colors))];
}

static void draw_tile(const Browser *b, const Entry *entry, int x, int y, bool active) {
    const int edge = active ? 3 : 1;
    const unsigned outer = active ? TEAL : OUTLINE;
    rounded_panel(x, y, TILE_WIDTH, TILE_HEIGHT, 21, outer, outer);
    rounded_panel(x + edge, y + edge, TILE_WIDTH - 2 * edge, TILE_HEIGHT - 2 * edge,
                  21 - edge, tile_color(entry), 0x141b2e);
    const char *kind = entry_type(entry);
    const int kind_width = text_width(kind, FOOTER_TEXT);
    rounded_panel(x + 15, y + 16, kind_width + 22, 35, 10, 0x182438, 0x182438);
    text(x + 26, y + 22, kind, entry->needs_exefs || entry->incomplete ? GOLD : TEAL,
         CARD, FOOTER_TEXT, kind_width + 2);
    artwork(b, entry, x + 13, y + 65, TILE_WIDTH - 26, 242);
    rect(x + 16, y + 326, TILE_WIDTH - 32, 1, active ? TEAL : OUTLINE);
    wrapped_title(x + 15, y + 341, display_name(entry), BODY_TEXT, TILE_WIDTH - 30);
    if (!entry->directory) {
        char size[40];
        bytes_label(size, sizeof(size), entry->bytes);
        text(x + 15, y + 428, size, MUTED, CARD, FOOTER_TEXT, TILE_WIDTH - 30);
    }
}

static size_t grid_max(const Browser *b) {
    const size_t rows = game_rows(b->count);
    const size_t height = rows ? rows * TILE_STRIDE - TILE_Y_GAP : 0;
    return height > GRID_BOTTOM - GRID_Y ? height - (GRID_BOTTOM - GRID_Y) : 0;
}

static void draw_game_grid(const Browser *b) {
    const size_t pos = (size_t)(b->scroll + 0.5), first_row = pos / TILE_STRIDE;
    int y = GRID_Y - (int)(pos % TILE_STRIDE);
    clip_top = GRID_Y;
    clip_bottom = GRID_BOTTOM;
    for (size_t row = first_row; y < GRID_BOTTOM && row < game_rows(b->count); ++row, y += TILE_STRIDE) {
        for (size_t col = 0; col < GAME_COLUMNS; ++col) {
            const size_t i = row * GAME_COLUMNS + col;
            if (i >= b->count) break;
            draw_tile(b, &b->items[i], GRID_X + (int)col * (TILE_WIDTH + TILE_X_GAP), y, i == b->selected);
        }
    }
    clip_top = 0;
    clip_bottom = HEIGHT;
    const size_t max = grid_max(b);
    if (!max) return;
    const int track = GRID_BOTTOM - GRID_Y;
    const int proportional = (int)((size_t)track * (size_t)track / (max + (size_t)track));
    const int thumb = proportional < 38 ? 38 : proportional;
    const int top = (int)((double)(track - thumb) * b->scroll / (double)max);
    rounded_panel(1220, GRID_Y, 6, track, 3, OUTLINE, OUTLINE);
    rounded_panel(1220, GRID_Y + top, 6, thumb, 3, TEAL, TEAL);
}

static void draw_file_list(const Browser *b, size_t end) {
    text(99, 220, "Name", MUTED, PANEL, FOOTER_TEXT, 250);
    text(876, 220, "Type", MUTED, PANEL, FOOTER_TEXT, 155);
    text(1138, 220, "Size", MUTED, PANEL, FOOTER_TEXT, 70);
    rect(87, 257, 1124, 1, OUTLINE);
    for (size_t i = b->first; i < end; ++i) {
        const Entry *entry = &b->items[i];
        const int y = 263 + 60 * (int)(i - b->first);
        const bool active = i == b->selected;
        const unsigned background = active ? 0x294357 : (i - b->first) % 2 ? 0x172136 : 0x131d30;
        rounded_panel(87, y, 1124, 56, 9, background, background);
        if (active) rect(87, y + 8, 4, 40, TEAL);
        if (entry->directory) folder(105, y + 12, GOLD);
        else package_icon(107, y + 10, entry->nup ? TEAL : MUTED, background);
        text(157, y + 15, entry->name, TEXT, background, BODY_TEXT, 690);
        text(876, y + 15, entry_type(entry), entry->incomplete ? GOLD : MUTED, background, FOOTER_TEXT, 165);
        if (!entry->directory) {
            char size[40];
            bytes_label(size, sizeof(size), entry->bytes);
            const int width = text_width(size, FOOTER_TEXT);
            text(1190 - width, y + 15, size, active ? TEAL : MUTED, background, FOOTER_TEXT, width + 2);
        }
    }
}

static void draw(const Browser *b) {
    for (int y = 0; y < HEIGHT; ++y)
        rect(0, y, WIDTH, 1, blend_color(0x151e30, BG, y * 255u / (HEIGHT - 1)));
    if (!b->brand.pixels) package_icon(79, 59, TEAL, PANEL);
    else if (b->brand.width == BRAND_SIZE)
        cover_pixels(BRAND_X, BRAND_Y, BRAND_SIZE, BRAND_SIZE, &b->brand);
    text(149, 51, IZ_NUP_TITLE, TEXT, BG, TITLE_TEXT, 650);
    text(169 + text_width(IZ_NUP_TITLE, TITLE_TEXT), 68, IZ_NUP_VERSION, MUTED, BG, FOOTER_TEXT, 120);
    const char *view_title = b->view == VIEW_SAVES ? "SAVED DATA" : b->view == VIEW_FILES ? "FILES" : "IZ2S / SWITCH 2";
    const int view_width = text_width(view_title, BODY_TEXT);
    text(1856 - view_width, 68, view_title, TEAL, BG, BODY_TEXT, view_width + 2);

    const Entry *current = b->count ? &b->items[b->selected] : NULL;
    if (b->dialog != DIALOG_NONE) {
        draw_dialog(b, current);
        draw_footer(b);
        return;
    }

    rounded_panel(64, 139, 1172, 805, 20, PANEL, 0x111a2a);
    char count[58];
    const bool library = b->view == VIEW_LIBRARY;
    if (library && b->scanning && b->count) snprintf(count, sizeof(count), "%zu games  /  Refreshing...", b->count);
    else snprintf(count, sizeof(count), b->scanning ? "%zu found  /  Scanning..." :
        library ? "%zu games" : "%zu entries", library && b->scanning ? b->scan_count : b->count);
    if (library) text(88, 166, count, MUTED, PANEL, FOOTER_TEXT, 650);
    else {
        const int count_width = text_width(count, FOOTER_TEXT);
        text(88, 166, view_path(b), TEXT, PANEL, BODY_TEXT, 1095 - count_width);
        text(1209 - count_width, 166, count, MUTED, PANEL, FOOTER_TEXT, count_width + 3);
    }
    rect(87, 203, 1124, 1, OUTLINE);
    if (!b->count) {
        const char *empty = library ? "No complete game dumps found." :
            b->view == VIEW_SAVES ? "No saved data here." : "No matching files.";
        text(88, 249, b->scanning ? "Searching the SD card..." : empty, MUTED, PANEL, BODY_TEXT, 1010);
    } else {
        const size_t page_rows = page_size(b), page_end = b->first + page_rows;
        const size_t end = b->count < page_end ? b->count : page_end;
        if (library) draw_game_grid(b);
        else {
            draw_file_list(b, end);
            char page[48];
            snprintf(page, sizeof(page), "%zu / %zu", b->first / page_rows + 1,
                     (b->count + page_rows - 1) / page_rows);
            text(1209 - text_width(page, FOOTER_TEXT), 884, page, MUTED, PANEL, FOOTER_TEXT, 112);
        }
    }

    rounded_panel(1260, 139, 596, 805, 20, 0x1b2940, 0x131c2d);
    artwork(b, current, 1284, 164, 548, 454);
    if (b->view == VIEW_SAVES)
        text(1289, 744, b->save_title, MUTED, PANEL, FOOTER_TEXT, 540);
    if (current) {
        wrapped_title(1287, 641, display_name(current), TITLE_TEXT, 539);
        const bool warning = current->needs_exefs || current->incomplete;
        text(1289, 776, entry_type(current), warning ? GOLD : TEAL, PANEL, BODY_TEXT, 250);
        if (!current->directory) {
            char size[40];
            bytes_label(size, sizeof(size), current->bytes);
            text(1830 - text_width(size, 1), 776, size, MUTED, PANEL, BODY_TEXT, 160);
        }
        if (current->title_id) {
            char id[32];
            snprintf(id, sizeof(id), "%016llX", (unsigned long long)current->title_id);
            text(1289, 824, id, MUTED, PANEL, BODY_TEXT, 540);
        }
        const char *detail = current->name;
        if (current->needs_exefs) detail = "Original ExeFS is missing";
        if (current->incomplete) detail = "Check the transfer and split parts";
        text(1289, 874, detail, warning ? GOLD : MUTED, PANEL, BODY_TEXT, 537);
    }
    if (b->notice[0]) draw_notice(b);
    draw_footer(b);
}

#ifndef IZ_PREVIEW
static int entry_order(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->directory != y->directory) return x->directory ? -1 : 1;
    const int order = strcasecmp(display_name(x), display_name(y));
    if (order) return order;
    const int path = strcasecmp(x->name, y->name);
    return path ? path : strcmp(x->name, y->name);
}

static void notice(Browser *b, const char *message) {
    snprintf(b->notice, sizeof(b->notice), "%s", message ? message : "");
}

static bool join_path(char dst[512], const char *path, const char *name) {
    const size_t n = strlen(path);
    const int length = snprintf(dst, 512, "%s%s%s", path, n && path[n - 1] == '/' ? "" : "/", name);
    return length > 0 && length < 512;
}

static void read_loose_title(const char *folder, char title[128]) {
    char path[544], text[128];
    const int length = snprintf(path, sizeof(path), "%s/title.txt", folder);
    if (length < 0 || (size_t)length >= sizeof(path)) return;
    FILE *file = fopen(path, "rb");
    if (!file) return;
    const size_t bytes = fread(text, 1, sizeof(text), file);
    fclose(file);
    size_t count = 0;
    while (count < bytes && (unsigned char)text[count] >= 32 && text[count] != 127) ++count;
    if (count && count < sizeof(text)) memcpy(title, text, count);
}

static const char *const loose_art[] = {"/cover.png", "/cover.jpg", "/icon.jpg", "/icon.png", NULL};
static const char *const nup_art[] = {".png", ".jpg", NULL};

static uint64_t hash_bytes(uint64_t hash, const void *value, size_t size) {
    const unsigned char *bytes = value;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

static uint64_t source_key(FsFileSystem *sd, const char *path, uint64_t bytes) {
    FsTimeStampRaw stamp;
    if (R_FAILED(fsFsGetFileTimeStampRaw(sd, path + 5, &stamp)) || !stamp.is_valid) return 0;
    const uint64_t identity[] = {bytes, stamp.created, stamp.modified};
    const uint64_t key = hash_bytes(UINT64_C(14695981039346656037), identity, sizeof(identity));
    return key ? key : 1;
}

static void set_art_key(Entry *entry, const char *source) {
    uint64_t key = hash_bytes(UINT64_C(14695981039346656037), entry->name, strlen(entry->name));
    if (entry->cover_size) {
        const uint64_t stamp[] = {entry->source_key, entry->bytes, entry->cover_offset, entry->cover_size};
        key = hash_bytes(key, stamp, sizeof(stamp));
        entry->art_key = key ? key : 1;
        return;
    }
    entry->art_key = 0;
    const char *const *names = entry->loose ? loose_art : nup_art;
    for (size_t i = 0; names[i]; ++i) {
        char path[544];
        snprintf(path, sizeof(path), "%s%s", source, names[i]);
        struct stat st;
        if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > COVER_BYTES_LIMIT) continue;
        const uint64_t stamp[] = {i, (uint64_t)st.st_size, (uint64_t)st.st_mtime};
        key = hash_bytes(key, stamp, sizeof(stamp));
        entry->art_key = key ? key : 1;
    }
}

static bool read_game_cover(const Entry *entry, const char *source, Cover *cover) {

    if (entry->cover_size && entry->cover_size <= COVER_BYTES_LIMIT) {
        void *data = malloc(entry->cover_size);
        const bool loaded = data &&
            iz_read_nup_cover(source, entry->cover_offset, data, entry->cover_size) &&
            cover_decode(cover, data, entry->cover_size);
        free(data);
        if (loaded) return true;
    }
    const char *const *names = entry->loose ? loose_art : nup_art;
    for (size_t i = 0; names[i]; ++i) {
        char path[544];
        snprintf(path, sizeof(path), "%s%s", source, names[i]);
        if (cover_read_file(cover, path)) return true;
    }
    return false;
}

static bool load_cover(Browser *b, const Entry *entry, bool cached_only) {
    if ((!entry->nup && !entry->loose) || find_cover(b, entry)) return false;
    const bool library = b->view == VIEW_LIBRARY;
    Cover image = {0};
    bool loaded = library && iz_library_cover_read(b->cache_dir, entry->art_key, &image);
    if (!loaded && cached_only) return false;
    if (!loaded && (!library || entry->art_key)) {
        char source[512];
        loaded = join_path(source, view_path(b), entry->name) && read_game_cover(entry, source, &image);
        if (loaded && library) iz_library_cover_write(b->cache_dir, entry->art_key, &image);
    }
    CoverSlot *slot = &b->covers[b->next_cover++ % (sizeof(b->covers) / sizeof(*b->covers))];
    cover_clear(&slot->image);
    *slot = (CoverSlot){.used = true, .image = image, .key = entry->art_key};
    strcpy(slot->name, entry->name);
    return loaded;
}

static void warm_next_cover(Browser *b) {
    if (!*b->cache_dir || b->scanning || b->view != VIEW_LIBRARY || b->cover_warm >= b->count) return;
    const Entry *entry = &b->items[b->cover_warm++];
    if (!entry->art_key || find_cover(b, entry) || iz_library_cover_exists(b->cache_dir, entry->art_key)) return;
    Cover image = {0};
    char source[512];
    if (join_path(source, sd_root, entry->name) && read_game_cover(entry, source, &image))
        iz_library_cover_write(b->cache_dir, entry->art_key, &image);
    cover_clear(&image);
}

static bool load_next_cover(Browser *b) {
    if (!b->count || b->view == VIEW_SAVES) return false;
    const Entry *selected = &b->items[b->selected];
    if ((selected->nup || selected->loose) && !find_cover(b, selected)) return load_cover(b, selected, false);
    if (b->view != VIEW_LIBRARY) return false;
    const size_t first = (size_t)b->scroll / TILE_STRIDE * GAME_COLUMNS;
    for (size_t i = first; i < b->count && i < first + (GAME_ROWS + 1) * GAME_COLUMNS; ++i) {
        const Entry *entry = &b->items[i];
        if ((entry->nup || entry->loose) && !find_cover(b, entry)) return load_cover(b, entry, false);
    }
    return false;
}

typedef struct PendingFolder {
    struct PendingFolder *next;
    char path[512];
} PendingFolder;

typedef struct {
    PendingFolder *todo;
    Entry *games;
    size_t game_capacity;
    FsFileSystem *sd;
    FsDir dir;
    FsDirectoryEntry rows[16];
    char path[512];
    s64 next, count;
    bool open, checked_romfs, incomplete, no_memory;
} Scan;

static Entry *new_entry(Browser *b, Scan *scan) {
    const bool library = b->view == VIEW_LIBRARY;
    Entry **items = library ? &scan->games : &b->items;
    size_t *capacity = library ? &scan->game_capacity : &b->capacity;
    size_t *count = library ? &b->scan_count : &b->count;
    if (*count == *capacity) {
        const size_t max = SIZE_MAX / sizeof(**items);
        const size_t grown = !*capacity ? 32 : *capacity > max / 2 ? max : *capacity * 2;
        Entry *expanded = grown > *capacity ? realloc(*items, grown * sizeof(*expanded)) : NULL;
        if (!expanded) {
            scan->no_memory = true;
            return NULL;
        }
        *items = expanded;
        *capacity = grown;
    }
    return &(*items)[(*count)++];
}

static void scan_stop(Scan *scan) {
    if (scan->open) fsDirClose(&scan->dir);
    scan->open = false;
    while (scan->todo) {
        PendingFolder *folder = scan->todo;
        scan->todo = folder->next;
        free(folder);
    }
    free(scan->games);
    scan->games = NULL;
    scan->game_capacity = 0;
}

static bool scan_queue(Scan *scan, const char *path) {
    PendingFolder *folder = malloc(sizeof(*folder));
    if (!folder) return false;
    strcpy(folder->path, path);
    folder->next = scan->todo;
    scan->todo = folder;
    return true;
}

static void scan_begin(Browser *b, Scan *scan) {
    scan_stop(scan);
    b->scan_count = 0;
    if (b->view != VIEW_LIBRARY) {
        clear_covers(b);
        b->count = b->selected = b->first = 0;
    }
    scan->incomplete = false;
    scan->no_memory = false;
    scan->sd = fsdevGetDeviceFileSystem("sdmc");
    if (scan->sd && b->view == VIEW_SAVES) {
        struct stat status;
        if (stat(b->save_path, &status) && errno == ENOENT) {
            b->scanning = false;
            return;
        }
    }
    b->scanning = scan->sd != NULL;
    if (b->scanning && b->view == VIEW_LIBRARY) {
        for (size_t i = 0; iz_library_roots[i]; ++i) {
            char path[512];
            if (!join_path(path, sd_root, iz_library_roots[i])) continue;
            struct stat status;
            if (stat(path, &status)) {
                if (errno != ENOENT) scan->incomplete = true;
            } else if (S_ISDIR(status.st_mode) && !scan_queue(scan, path)) scan->no_memory = true;
        }
    } else if (b->scanning) b->scanning = scan_queue(scan, view_path(b));
    if (!b->scanning) notice(b, "Could not start SD scan.");
}

static void select_entry(Browser *b, size_t index) {
    b->selected = index;
    const size_t step = b->view == VIEW_LIBRARY ? GAME_COLUMNS : FILE_PAGE;
    b->first = index / step * step;
}

static const Entry *find_game(const Entry *entries, size_t count, const char *path) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(entries[i].name, path)) return &entries[i];
    return NULL;
}

static bool same_game(const Entry *x, const Entry *y) {
    return !strcmp(x->name, y->name) && !strcmp(x->title, y->title) && x->bytes == y->bytes && x->title_id == y->title_id &&
        x->cover_offset == y->cover_offset && x->cover_size == y->cover_size && x->source_key == y->source_key &&
        x->art_key == y->art_key && x->nup == y->nup && x->loose == y->loose && x->split == y->split &&
        x->needs_exefs == y->needs_exefs && x->incomplete == y->incomplete;
}

static void load_library_cache(Browser *b) {
    Entry *entries;
    size_t count;
    if (!iz_library_cache_read(b->cache_dir, &entries, &count, &b->cache_generation)) return;
    free(b->items);
    b->items = entries;
    b->count = b->capacity = count;
    b->selected = b->first = b->library_column = 0;
    if (!count) return;
    if (b->restore_game) {
        const Entry *entry = find_game(b->items, b->count, b->return_game);
        if (entry) {
            select_entry(b, (size_t)(entry - b->items));
            b->restore_game = false;
        }
        b->library_column = b->selected % GAME_COLUMNS;
    }
    const size_t requested = b->first / GAME_COLUMNS * TILE_STRIDE, max = grid_max(b);
    b->scroll = b->scroll_to = (double)(requested < max ? requested : max);
    const size_t first = (size_t)b->scroll / TILE_STRIDE * GAME_COLUMNS;
    for (size_t i = first; i < b->count && i < first + GAME_PAGE; ++i) load_cover(b, &b->items[i], true);
}

static void scan_finish(Browser *b, Scan *scan) {
    const bool library = b->view == VIEW_LIBRARY;
    const bool have_path = (library && b->restore_game) || b->count;
    char path[sizeof(b->return_game)];
    if (have_path) strcpy(path, library && b->restore_game ? b->return_game : b->items[b->selected].name);
    if (library && (!b->count || (!scan->incomplete && !scan->no_memory))) {
        if (b->scan_count) qsort(scan->games, b->scan_count, sizeof(*scan->games), entry_order);
        bool changed = b->count != b->scan_count || !b->cache_generation;
        for (size_t i = 0; !changed && i < b->count; ++i) changed = !same_game(&b->items[i], &scan->games[i]);
        if (changed && !scan->incomplete && !scan->no_memory &&
            iz_library_cache_write(b->cache_dir, scan->games, b->scan_count, &b->cache_generation)) {
            for (size_t i = 0; i < b->count; ++i) {
                if (!b->items[i].art_key) continue;
                const Entry *next = find_game(scan->games, b->scan_count, b->items[i].name);
                if (!next || next->art_key != b->items[i].art_key)
                    iz_library_cover_forget(b->cache_dir, b->items[i].art_key);
            }
        }
        free(b->items);
        b->items = scan->games;
        b->capacity = scan->game_capacity;
        b->count = b->scan_count;
        scan->games = NULL;
        b->cover_warm = 0;
    }
    if (library) b->restore_game = false;
    scan_stop(scan);
    b->scanning = false;
    if (!b->notice[0]) {
        if (scan->no_memory) notice(b, "Not enough memory to list the remaining SD entries.");
        else if (scan->incomplete) notice(b, "Some SD entries could not be scanned.");
    }
    if (!b->count) {
        b->selected = b->first = 0;
        b->return_save_options = false;
        return;
    }
    if (!library) qsort(b->items, b->count, sizeof(*b->items), entry_order);
    size_t choice = b->selected < b->count ? b->selected : b->count - 1;
    for (size_t i = 0; have_path && i < b->count; ++i)
        if (!strcmp(path, b->items[i].name)) { choice = i; break; }
    select_entry(b, choice);
    if (library) {
        b->library_column = choice % GAME_COLUMNS;
        if (b->return_save_options && !strcmp(b->items[choice].name, b->return_game))
            b->dialog = DIALOG_OPTIONS;
        b->return_save_options = false;
    }
}

static void scan_entry(Browser *b, Scan *scan, const FsDirectoryEntry *row) {
    if (!memchr(row->name, 0, sizeof(row->name)) || !row->name[0]) return;
    const bool library = b->view == VIEW_LIBRARY, saves = b->view == VIEW_SAVES;
    if (!saves && row->name[0] == '.') return;
    const bool directory = row->type == FsDirEntryType_Dir;
    if (!directory && (row->type != FsDirEntryType_File || (!saves && has_extension(row->name, ".nro")))) return;
    char path[512];
    if (saves) {
        if (!iz_save_child_path(b->save_root, scan->path, row->name, path, sizeof(path))) return;
        Entry *item = new_entry(b, scan);
        if (!item) return;
        *item = (Entry){.bytes = row->file_size > 0 ? (uint64_t)row->file_size : 0, .directory = directory};
        strcpy(item->name, row->name);
        return;
    }
    if (!join_path(path, scan->path, row->name)) {
        scan->incomplete = true;
        return;
    }
    if (directory && library) {
        if (!scan_queue(scan, path)) scan->incomplete = true;
        return;
    }

    const char *name = library ? path + sizeof(sd_root) - 1 : row->name;
    uint64_t size = row->file_size > 0 ? (uint64_t)row->file_size : 0;
    if (library && !scan->checked_romfs &&
        (!strcasecmp(row->name, "romfs.bin") || !strcasecmp(row->name, "romfs.bin.00"))) {
        scan->checked_romfs = true;
        uint64_t image_size, title_id;
        if (iz_probe_loose(scan->path, &image_size, &title_id)) {
            Entry *item = new_entry(b, scan);
            if (!item) return;
            *item = (Entry){.bytes = image_size, .title_id = title_id, .loose = true, .needs_exefs = !title_id};
            strcpy(item->name, scan->path + sizeof(sd_root) - 1);
            read_loose_title(scan->path, item->title);
            set_art_key(item, scan->path);
            return;
        }
    }

    NupPartName part;
    const bool first_part = !directory && nup_part_start(row->name, &part);
    const Entry *cached = library && !directory ? find_game(b->items, b->count, name) : NULL;
    uint64_t key = 0;
    if (cached && cached->nup && !cached->split && !cached->incomplete) {
        key = source_key(scan->sd, path, size);
        if (key && key == cached->source_key) {
            Entry *item = new_entry(b, scan);
            if (item) {
                *item = *cached;
                set_art_key(item, path);
            }
            return;
        }
    }
    NupPreview preview;
    const bool candidate = !directory && (size >= 128 || first_part);
    const bool nup = candidate && iz_probe_nup(path, &preview);
    const bool named_nup = !directory && has_nup_name(row->name, first_part);
    if (library && !nup) return;
    if (!library && !directory && !b->show_all && !nup && !named_nup) return;
    Entry *item = new_entry(b, scan);
    if (!item) return;
    *item = (Entry){.bytes = nup ? preview.size : size, .directory = directory,
        .nup = nup || named_nup, .split = nup ? preview.split : named_nup && first_part,
        .incomplete = nup && preview.incomplete,
        .title_id = nup ? preview.title_id : 0};
    strcpy(item->name, name);
    if (nup) {
        memcpy(item->title, preview.title, sizeof(item->title));
        if (!item->title[0] && first_part && part.bare) {
            const char *folder = strrchr(scan->path, '/');
            if (folder) snprintf(item->title, sizeof(item->title), "%.*s", (int)sizeof(item->title) - 1, folder + 1);
        }
        item->cover_offset = preview.cover_offset;
        item->cover_size = preview.cover_size;
        if (library) {
            item->source_key = key ? key : source_key(scan->sd, path, size);
            set_art_key(item, path);
        }
    }
}

static bool scan_step(Browser *b, Scan *scan) {
    if (!b->scanning) return false;
    const size_t previous = b->count;
    const size_t previous_scan = b->scan_count;
    const uint64_t started = armGetSystemTick();
    for (unsigned budget = 0; budget < 24 && armTicksToNs(armGetSystemTick() - started) < 4000000; ++budget) {
        if (scan->no_memory || (!scan->open && !scan->todo)) {
            scan_finish(b, scan);
            return true;
        }
        if (!scan->open) {
            PendingFolder *folder = scan->todo;
            scan->todo = folder->next;
            strcpy(scan->path, folder->path);
            free(folder);
            scan->next = scan->count = 0;
            scan->checked_romfs = false;
            scan->open = R_SUCCEEDED(fsFsOpenDirectory(scan->sd, scan->path + 5,
                FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &scan->dir));
            if (!scan->open) scan->incomplete = true;
            continue;
        }
        if (scan->next == scan->count) {
            scan->next = 0;
            const Result result = fsDirRead(&scan->dir, &scan->count,
                sizeof(scan->rows) / sizeof(*scan->rows), scan->rows);
            if (R_FAILED(result) || !scan->count) {
                if (R_FAILED(result)) scan->incomplete = true;
                fsDirClose(&scan->dir);
                scan->open = false;
                continue;
            }
        }
        scan_entry(b, scan, &scan->rows[scan->next++]);
    }
    return b->count != previous || (!b->count && b->scan_count != previous_scan);
}

static void move(Browser *b, int step) {
    if (!b->count) return;
    const int rows = (int)page_size(b);
    if ((step == -rows && b->first == 0) ||
        (step == rows && b->first + (size_t)rows >= b->count)) return;
    if (step < 0) {
        const size_t back = (size_t)-step;
        select_entry(b, back > b->selected ? 0 : b->selected - back);
    } else {
        const size_t forward = (size_t)step;
        select_entry(b, forward >= b->count - b->selected ? b->count - 1 : b->selected + forward);
    }
}

static void move_game(Browser *b, int horizontal, int vertical) {
    if (!b->count) return;
    const size_t current_row = b->selected / GAME_COLUMNS;
    if (horizontal) {
        const size_t column = b->selected % GAME_COLUMNS;
        if (horizontal < 0 ? !column : column + 1 == GAME_COLUMNS || b->selected + 1 == b->count) return;
        const size_t next = horizontal < 0 ? b->selected - 1 : b->selected + 1;
        b->library_column = next % GAME_COLUMNS;
        select_entry(b, next);
        return;
    }
    const size_t last = game_rows(b->count) - 1;
    const size_t step = (size_t)(vertical < 0 ? -vertical : vertical);
    const size_t row = vertical < 0 ? (step > current_row ? 0 : current_row - step) :
        (step > last - current_row ? last : current_row + step);
    if (row != current_row) {
        const size_t next = row * GAME_COLUMNS + b->library_column;
        select_entry(b, next < b->count ? next : b->count - 1);
    }
}

static u64 menu_buttons(bool *ready, u64 held, u64 down) {
    // Ignore the button that opened BreRun until it has been released.
    if (!*ready) {
        *ready = held == 0;
        return 0;
    }
    return down & HidNpadButton_Plus ? HidNpadButton_Plus : down;
}

static const u64 grid_up = HidNpadButton_Up | HidNpadButton_StickLUp;
static const u64 grid_down = HidNpadButton_Down | HidNpadButton_StickLDown;

typedef struct { u64 direction, next; } GridRepeat;

static bool update_grid_scroll(Browser *b, uint64_t now) {
    const double max = (double)grid_max(b);
    const double wanted = (double)(b->first / GAME_COLUMNS) * TILE_STRIDE;
    const double target = wanted < max ? wanted : max;
    const double previous = b->scroll;
    if (b->view != VIEW_LIBRARY || !b->count || b->dialog != DIALOG_NONE) {
        b->scroll = b->scroll_to = b->view == VIEW_LIBRARY ? target : 0;
    } else {
        if (b->scroll > max) b->scroll = max;
        if (b->scroll_to != target) {
            b->scroll_from = b->scroll;
            b->scroll_to = target;
            b->scroll_started = now;
        }
        if (b->scroll != b->scroll_to) {
            const double t = (double)(now - b->scroll_started) / 195000000.0;
            const double rest = 1.0 - t;
            b->scroll = t >= 1.0 ? target : target + (b->scroll_from - target) * rest * rest * rest;
            if (b->scroll > max) b->scroll = max;
        }
    }
    return previous != b->scroll;
}

static u64 repeat_grid(const Browser *b, GridRepeat *repeat, bool ready, u64 held, u64 pressed) {
    const bool up = held & grid_up, down = held & grid_down;
    const u64 direction = up != down ? (up ? HidNpadButton_Up : HidNpadButton_Down) : 0;
    if (!ready || !library_game(b) || b->dialog != DIALOG_NONE || !direction ||
        (held & ~(grid_up | grid_down))) {
        *repeat = (GridRepeat){0};
        return pressed;
    }
    const u64 now = armTicksToNs(armGetSystemTick());
    if (pressed || repeat->direction != direction) {
        *repeat = (GridRepeat){.direction = direction, .next = now + 280000000};
    } else if (now >= repeat->next) {
        repeat->next = now + 135000000;
        const size_t row = b->selected / GAME_COLUMNS;
        if (up ? row > 0 : row + 1 < game_rows(b->count)) return direction;
    }
    return pressed;
}

typedef struct {
    IzTransfer receiver;
    Thread worker;
    bool active, restore_sleep;
} NetworkTransfer;

static bool start_transfer(NetworkTransfer *network, Browser *b, const char *self) {
    if (network->active) {
        b->dialog = DIALOG_TRANSFER;
        return true;
    }
    SocketInitConfig sockets = *socketGetDefaultInitConfig();
    sockets.tcp_rx_buf_size = 256 * 1024;
    sockets.tcp_rx_buf_max_size = 1024 * 1024;
    if (R_FAILED(socketInitialize(&sockets)) && R_FAILED(socketInitializeDefault())) return false;
    const struct in_addr local = {.s_addr = (uint32_t)gethostid()};
    char ip[INET_ADDRSTRLEN];
    if (local.s_addr == INADDR_NONE || !inet_ntop(AF_INET, &local, ip, sizeof(ip)) ||
        !iz_transfer_open(&network->receiver, "sdmc:/Games", local.s_addr, IZ_TRANSFER_PORT,
                          iz_transfer_split(self), randomGet64())) {
        socketExit();
        return false;
    }
    network->receiver.logo = iz_brand;
    network->receiver.logo_size = (uintptr_t)iz_brand_end - (uintptr_t)iz_brand;
    Result result = threadCreate(&network->worker, iz_transfer_run, &network->receiver,
                                 NULL, 128 * 1024, 0x2c, -2);
    if (R_SUCCEEDED(result)) {
        result = threadStart(&network->worker);
        if (R_FAILED(result)) threadClose(&network->worker);
    }
    if (R_FAILED(result)) {
        iz_transfer_close(&network->receiver);
        socketExit();
        return false;
    }
    bool sleep_disabled;
    network->restore_sleep = R_SUCCEEDED(appletIsAutoSleepDisabled(&sleep_disabled)) && !sleep_disabled &&
        R_SUCCEEDED(appletSetAutoSleepDisabled(true));
    network->active = true;
    snprintf(b->address, sizeof(b->address), "http://%s:%u", ip, IZ_TRANSFER_PORT);
    strcpy(b->code, network->receiver.code);
    b->transfer_status = IZ_TRANSFER_READY;
    b->received = b->total = 0;
    b->transfer_name[0] = 0;
    b->transfer_pending = b->transfer_completed = b->transfer_job = 0;
    b->dialog = DIALOG_TRANSFER;
    return true;
}

static void stop_transfer(NetworkTransfer *network) {
    if (!network->active) return;
    atomic_store(&network->receiver.stop, true);
    threadWaitForExit(&network->worker);
    threadClose(&network->worker);
    iz_transfer_close(&network->receiver);
    socketExit();
    if (network->restore_sleep) appletSetAutoSleepDisabled(false);
    network->active = false;
}

static bool poll_transfer(const NetworkTransfer *network, Browser *b) {
    if (!network->active) return false;
    const IzTransfer *receiver = &network->receiver;
    const IzTransferStatus status = atomic_load(&receiver->status);
    const uint64_t total = atomic_load(&receiver->total), current = atomic_load(&receiver->received);
    const uint64_t received = current > total ? total : current;
    const unsigned pending = atomic_load(&receiver->pending), completed = atomic_load(&receiver->completed);
    const unsigned job = atomic_load(&receiver->job);
    char name[IZ_TRANSFER_NAME_CAP];
    iz_transfer_filename(receiver, name);
    const bool changed = status != b->transfer_status || total != b->total || pending != b->transfer_pending ||
        job != b->transfer_job || strcmp(name, b->transfer_name);
    const bool progress = b->dialog == DIALOG_TRANSFER ? (received >> 20) != (b->received >> 20) :
        total && (100 * received / total) != (100 * b->received / total);
    b->transfer_status = status;
    b->received = received;
    b->total = total;
    b->transfer_pending = pending;
    b->transfer_job = job;
    strcpy(b->transfer_name, name);
    if (completed != b->transfer_completed) {
        b->transfer_completed = completed;
        b->transfer_refresh = true;
    }
    return changed || (status == IZ_TRANSFER_RECEIVING && progress);
}

static void switch_view(Browser *b, Scan *scan, View view) {
    b->return_save_options = false;
    if (b->view != view) {
        if (library_game(b)) {
            strcpy(b->return_game, b->items[b->selected].name);
            b->restore_game = true;
            b->library_first = b->first;
        }
        b->count = b->selected = 0;
        b->first = view == VIEW_LIBRARY ? b->library_first : 0;
        clear_covers(b);
    }
    b->view = view;
    if (view == VIEW_LIBRARY && !b->count) load_library_cache(b);
    notice(b, NULL);
    scan_begin(b, scan);
}

static void open_saves(Browser *b, Scan *scan, const char *self) {
    const Entry *entry = &b->items[b->selected];
    b->dialog = DIALOG_NONE;
    if (!iz_game_save_path(self, entry->title_id, b->save_root, sizeof(b->save_root))) {
        b->dialog = DIALOG_OPTIONS;
        notice(b, "Could not locate this game's saved data.");
        return;
    }
    snprintf(b->save_title, sizeof(b->save_title), "%.127s", display_name(entry));
    strcpy(b->save_path, b->save_root);
    switch_view(b, scan, VIEW_SAVES);
}

static void delete_save(Browser *b, Scan *scan) {
    b->dialog = DIALOG_NONE;
    if (b->view != VIEW_SAVES || !b->count) return;
    const Entry *entry = &b->items[b->selected];
    char target[512];
    FsDirEntryType type;
    scan_stop(scan);
    if (!scan->sd ||
        !iz_save_child_path(b->save_root, b->save_path, entry->name, target, sizeof(target)) ||
        R_FAILED(fsFsGetEntryType(scan->sd, target + 5, &type)) ||
        type != (entry->directory ? FsDirEntryType_Dir : FsDirEntryType_File)) {
        notice(b, "The selected save entry is no longer available.");
    } else {
        const Result result = entry->directory ? fsFsDeleteDirectoryRecursively(scan->sd, target + 5) :
            fsFsDeleteFile(scan->sd, target + 5);
        notice(b, R_SUCCEEDED(result) ? "Saved data deleted." : "Could not finish deleting the selected entry.");
    }
    scan_begin(b, scan);
}

static void delete_game_image(Browser *b, Scan *scan) {
    const Entry *game = &b->items[b->selected];
    char path[512];
    unsigned removed = 0;
    scan_stop(scan);
    const bool deleted = join_path(path, sd_root, game->name) &&
        iz_delete_game_image(path, game->loose, game->bytes, game->title_id, &removed);
    b->dialog = DIALOG_NONE;
    if (deleted) {
        const uint64_t old_art = game->art_key;
        --b->count;
        if (b->selected < b->count)
            memmove(b->items + b->selected, b->items + b->selected + 1,
                    (b->count - b->selected) * sizeof(*b->items));
        if (b->count) {
            const size_t next = b->selected < b->count ? b->selected : b->count - 1;
            b->library_column = next % GAME_COLUMNS;
            select_entry(b, next);
        } else b->selected = b->first = 0;
        if (iz_library_cache_write(b->cache_dir, b->items, b->count, &b->cache_generation))
            iz_library_cover_forget(b->cache_dir, old_art);
    }
    notice(b, deleted ? "Game image deleted." : removed ? "Some image parts could not be deleted." :
           "Could not delete this game image. Check the SD card and try again.");
    scan_begin(b, scan);
}

static void delete_all_saves(Browser *b, const char *self) {
    b->dialog = DIALOG_NONE;
    char path[512];
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd || !self || strncmp(self, sd_root, sizeof(sd_root) - 1) ||
        !iz_game_save_path(self, b->items[b->selected].title_id, path, sizeof(path))) {
        notice(b, "Could not locate this game's saved data.");
        return;
    }
    struct stat status;
    if (stat(path, &status)) {
        notice(b, errno == ENOENT ? "This game has no saved data." : "Could not read this game's saved data.");
        return;
    }
    const bool deleted = S_ISDIR(status.st_mode) && R_SUCCEEDED(fsFsCleanDirectoryRecursively(sd, path + 5));
    notice(b, deleted ? "Saved data deleted." : "Could not finish deleting this game's saved data.");
}

static void open_game_settings(Browser *b, const char *self) {
    b->settings = iz_load_game_settings(self, b->items[b->selected].title_id);
    b->original_settings = b->settings;
    b->requested_room = b->settings.room;
    b->game_setting = GAME_SETTING_ROOM;
    b->dialog = DIALOG_SETTINGS;
}

static void confirm_game_settings(Browser *b, bool exit) {
    b->settings_return = b->dialog;
    b->settings_discard = false;
    b->settings_error = false;
    b->settings_exit = exit;
    b->dialog = DIALOG_CONFIRM_SETTINGS;
}

static void edit_language(Browser *b, u64 keys) {
    const unsigned language = b->settings.language;
    if ((keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) && language >= LANGUAGE_COLUMNS)
        b->settings.language -= LANGUAGE_COLUMNS;
    else if ((keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) && language + LANGUAGE_COLUMNS < IZ_LANGUAGE_COUNT)
        b->settings.language += LANGUAGE_COLUMNS;
    else if ((keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) && language % LANGUAGE_COLUMNS)
        --b->settings.language;
    else if ((keys & (HidNpadButton_Right | HidNpadButton_StickLRight)) &&
             language % LANGUAGE_COLUMNS < LANGUAGE_COLUMNS - 1 && language + 1 < IZ_LANGUAGE_COUNT)
        ++b->settings.language;
}

static void edit_memory(Browser *b, u64 keys) {
    static const unsigned rooms[] = {IZ_ROOM_DEFAULT_MIB, 512, 768, 1024, 1536, IZ_ROOM_MAX_MIB};
    const u64 left = HidNpadButton_Left | HidNpadButton_StickLLeft;
    const u64 right = HidNpadButton_Right | HidNpadButton_StickLRight;
    if (b->dialog == DIALOG_EXEFS) {
        const unsigned step = keys & (HidNpadButton_L | HidNpadButton_R) ? 128 : 8;
        unsigned *budget = &b->settings.exefs_budget;
        if (keys & (left | HidNpadButton_L))
            *budget = *budget <= IZ_EXEFS_MIN_MIB ? 0 :
                *budget < IZ_EXEFS_MIN_MIB + step ? IZ_EXEFS_MIN_MIB : *budget - step;
        else if (keys & (right | HidNpadButton_R))
            *budget = !*budget ? IZ_EXEFS_MIN_MIB :
                *budget > IZ_EXEFS_MAX_MIB - step ? IZ_EXEFS_MAX_MIB : *budget + step;
        else return;
        b->settings.room = iz_room_for_exefs(b->requested_room, *budget);
    } else if (keys & left) {
        const unsigned minimum = min_room_for_settings(b);
        if (b->settings.room <= minimum) return;
        b->requested_room = minimum;
        for (size_t i = sizeof(rooms) / sizeof(*rooms); i-- > 0;)
            if (rooms[i] < b->settings.room && rooms[i] >= minimum) {
                b->requested_room = rooms[i];
                break;
            }
        b->settings.room = b->requested_room;
    } else if (keys & right) {
        for (size_t i = 0; i < sizeof(rooms) / sizeof(*rooms); ++i)
            if (rooms[i] > b->settings.room && rooms[i] >= min_room_for_settings(b)) {
                b->requested_room = b->settings.room = rooms[i];
                break;
            }
    }
}

static void update_game_settings(Browser *b, const char *self, u64 keys) {
    const u64 up = HidNpadButton_Up | HidNpadButton_StickLUp;
    const u64 down = HidNpadButton_Down | HidNpadButton_StickLDown;
    const Entry *game = &b->items[b->selected];
    if (b->dialog == DIALOG_SETTINGS) {
        if (keys & HidNpadButton_B) {
            if (settings_dirty(b)) confirm_game_settings(b, false);
            else b->dialog = DIALOG_OPTIONS;
        } else if (keys & HidNpadButton_A) {
            switch (b->game_setting) {
                case GAME_SETTING_ROOM: b->dialog = DIALOG_ROOM; break;
                case GAME_SETTING_EXEFS: b->dialog = DIALOG_EXEFS; break;
                case GAME_SETTING_LANGUAGE: b->dialog = DIALOG_LANGUAGE; break;
                case GAME_SETTING_NATIVE_NVN: b->settings.native_nvn = !b->settings.native_nvn; break;
                case GAME_SETTING_DLC: b->settings.dlc_enabled = !b->settings.dlc_enabled; break;
            }
        } else if ((keys & up) && b->game_setting > GAME_SETTING_ROOM) --b->game_setting;
        else if ((keys & down) && b->game_setting < (game->nup ? GAME_SETTING_DLC : GAME_SETTING_NATIVE_NVN))
            ++b->game_setting;
        return;
    }
    if (b->dialog == DIALOG_CONFIRM_SETTINGS) {
        if (keys & HidNpadButton_B) {
            b->dialog = b->settings_return;
        } else if (keys & HidNpadButton_A) {
            if (!b->settings_discard) {
                if (min_room_for_settings(b) > b->settings.room) return;
                b->settings_error = !iz_save_game_settings(self, game->title_id, &b->settings);
                if (b->settings_error) return;
            }
            b->exit_requested = b->settings_exit;
            b->dialog = DIALOG_OPTIONS;
        } else if (keys & up) {
            b->settings_discard = false;
        } else if (keys & down) {
            b->settings_discard = true;
        }
        return;
    }
    if (keys & (HidNpadButton_A | HidNpadButton_B)) b->dialog = DIALOG_SETTINGS;
    else if (b->dialog == DIALOG_LANGUAGE) edit_language(b, keys);
    else edit_memory(b, keys);
}

static bool update_dialog(Browser *b, Scan *scan, NetworkTransfer *network, const char *self, u64 keys) {
    if (b->dialog == DIALOG_LAUNCH_ERROR) {
        if (!(keys & (HidNpadButton_A | HidNpadButton_B))) return false;
        b->dialog = DIALOG_NONE;
        return true;
    }
    if (b->dialog == DIALOG_CANCEL_TRANSFER) {
        if (keys & HidNpadButton_B) b->dialog = DIALOG_TRANSFER;
        else if (keys & HidNpadButton_A) {
            iz_transfer_request(&network->receiver, b->cancel_transfer_job, IZ_TRANSFER_CANCEL);
            b->dialog = DIALOG_TRANSFER;
        }
        return b->dialog == DIALOG_TRANSFER;
    }
    if (b->dialog == DIALOG_TRANSFER) {
        if (keys & HidNpadButton_L) {
            stop_transfer(network);
            b->transfer_status = IZ_TRANSFER_READY;
            b->dialog = DIALOG_NONE;
            return true;
        }
        if (keys & HidNpadButton_B) {
            b->dialog = DIALOG_NONE;
            return true;
        }
        if ((keys & HidNpadButton_Y) && b->transfer_job) {
            b->cancel_transfer_job = b->transfer_job;
            strcpy(b->cancel_transfer_name, b->transfer_name);
            b->dialog = DIALOG_CANCEL_TRANSFER;
            return true;
        }
        if ((keys & HidNpadButton_X) && b->transfer_status == IZ_TRANSFER_RECEIVING)
            iz_transfer_request(&network->receiver, b->transfer_job, IZ_TRANSFER_PAUSE);
        return false;
    }

    if (!keys) return false;
    if (b->dialog >= DIALOG_SETTINGS && b->dialog <= DIALOG_CONFIRM_SETTINGS) {
        update_game_settings(b, self, keys);
        return true;
    }
    if (b->dialog == DIALOG_DELETE_IMAGE || b->dialog == DIALOG_DELETE_ALL_SAVES) {
        if (keys & HidNpadButton_B) b->dialog = DIALOG_OPTIONS;
        else if (keys & HidNpadButton_A) {
            if (b->dialog == DIALOG_DELETE_IMAGE) delete_game_image(b, scan);
            else delete_all_saves(b, self);
        }
        return true;
    }
    if (keys & HidNpadButton_B) {
        b->dialog = DIALOG_NONE;
        return true;
    }
    if (b->dialog == DIALOG_DELETE) {
        if (keys & HidNpadButton_A) delete_save(b, scan);
        return true;
    }
    if (b->dialog == DIALOG_OPTIONS) {
        const Entry *game = &b->items[b->selected];
        if (keys & HidNpadButton_A) {
            if (b->game_option == GAME_OPTION_SAVES) open_saves(b, scan, self);
            else if (b->game_option == GAME_OPTION_SETTINGS) open_game_settings(b, self);
            else if (b->game_option == GAME_OPTION_DELETE_IMAGE) b->dialog = DIALOG_DELETE_IMAGE;
            else if (b->game_option == GAME_OPTION_DELETE_ALL_SAVES) b->dialog = DIALOG_DELETE_ALL_SAVES;
        } else {
            const int step = (keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) ? -1 :
                (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) ? 1 : 0;
            if (step) {
                for (int i = (int)b->game_option + step; i >= 0 && i < GAME_OPTION_COUNT; i += step) {
                    if (!game_option_available(game, (unsigned)i)) continue;
                    b->game_option = (unsigned)i;
                    break;
                }
            }
        }
        return true;
    }

    return false;
}

static void back(Browser *b, Scan *scan) {
    if (b->view == VIEW_LIBRARY) {
        switch_view(b, scan, VIEW_FILES);
        return;
    }
    const bool saves = b->view == VIEW_SAVES;
    char *path = saves ? b->save_path : b->path;
    if (!strcmp(path, saves ? b->save_root : sd_root)) {
        switch_view(b, scan, VIEW_LIBRARY);
        if (saves) {
            if (library_game(b) && !strcmp(b->items[b->selected].name, b->return_game))
                b->dialog = DIALOG_OPTIONS;
            else b->return_save_options = b->scanning;
        }
        return;
    }
    char *end = strrchr(path, '/');
    if (end == path + 5) end[1] = 0;
    else *end = 0;
    notice(b, NULL);
    scan_begin(b, scan);
}

static bool open_entry(Browser *b, Scan *scan, const char *self) {
    if (!b->count) return true;
    const Entry *entry = &b->items[b->selected];
    const bool saves = b->view == VIEW_SAVES;
    if (saves && !entry->directory) return true;
    char path[512];
    const bool joined = saves ? iz_save_child_path(b->save_root, b->save_path, entry->name, path, sizeof(path)) :
        join_path(path, view_path(b), entry->name);
    if (!joined) notice(b, "Could not open this path.");
    else if (entry->directory) {
        strcpy(saves ? b->save_path : b->path, path);
        notice(b, NULL);
        scan_begin(b, scan);
    } else if (entry->needs_exefs || entry->incomplete || entry->loose || entry->nup) {
        IzLaunchError *error = &b->launch_error;
        *error = (IzLaunchError){.title = entry->title_id};
        strcpy(error->path, path);
        if (entry->needs_exefs) {
            error->stage = "ExeFS";
            error->reason = "This game needs its original files in the exefs folder.";
        } else if (entry->incomplete) {
            error->stage = "Open NUP";
            error->reason = NUP_INCOMPLETE_MESSAGE;
        } else if (iz_queue_game(self, path, entry->title_id, error)) return false;
        iz_log_launch_error(self, error);
        b->dialog = DIALOG_LAUNCH_ERROR;
    } else notice(b, "Select a complete game dump to launch.");
    return true;
}

static bool browse(Browser *b, Scan *scan, const char *self, u64 keys) {
    if (keys & HidNpadButton_B) back(b, scan);
    else if (keys & HidNpadButton_Minus) {
        switch_view(b, scan, b->view == VIEW_LIBRARY ? VIEW_FILES : VIEW_LIBRARY);
    } else if (keys & HidNpadButton_Y) {
        if (library_game(b)) {
            const Entry *game = &b->items[b->selected];
            b->game_option = game->title_id ? GAME_OPTION_SAVES : GAME_OPTION_DELETE_IMAGE;
            b->dialog = DIALOG_OPTIONS;
        } else if (b->view == VIEW_SAVES && b->count) {
            b->dialog = DIALOG_DELETE;
        } else if (b->view == VIEW_FILES) {
            b->show_all = !b->show_all;
            notice(b, NULL);
            scan_begin(b, scan);
        }
    } else if (keys & HidNpadButton_X) {
        notice(b, NULL);
        scan_begin(b, scan);
    } else if (keys & HidNpadButton_A) {
        return open_entry(b, scan, self);
    } else {
        const u64 left = HidNpadButton_Left | HidNpadButton_StickLLeft;
        const u64 right = HidNpadButton_Right | HidNpadButton_StickLRight;
        if (b->view == VIEW_LIBRARY) {
            if (keys & HidNpadButton_L) move_game(b, 0, -GAME_ROWS);
            else if (keys & HidNpadButton_R) move_game(b, 0, GAME_ROWS);
            else if (keys & grid_up) move_game(b, 0, -1);
            else if (keys & grid_down) move_game(b, 0, 1);
            else if (keys & left) move_game(b, -1, 0);
            else if (keys & right) move_game(b, 1, 0);
        } else {
            if (keys & HidNpadButton_L) move(b, -FILE_PAGE);
            else if (keys & HidNpadButton_R) move(b, FILE_PAGE);
            else if (keys & grid_up) move(b, -1);
            else if (keys & grid_down) move(b, 1);
        }
    }
    return true;
}

int iz_browser(const char *self, const IzLaunchError *error) {
    Browser b = {.path = "sdmc:/", .view = VIEW_LIBRARY};
    if (error) {
        b.launch_error = *error;
        b.dialog = DIALOG_LAUNCH_ERROR;
    }
    pixels = malloc((size_t)WIDTH * HEIGHT * sizeof(*pixels));
    if (!pixels) return 1;
    Framebuffer fb;
    int width = WIDTH, height = HEIGHT;
    Result result = framebufferCreate(&fb, nwindowGetDefault(), width, height,
                                      PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(result)) {
        width = 1280;
        height = 720;
        result = framebufferCreate(&fb, nwindowGetDefault(), width, height, PIXEL_FORMAT_RGBA_8888, 2);
    }
    if (R_SUCCEEDED(result)) {
        result = framebufferMakeLinear(&fb);
        if (R_FAILED(result)) framebufferClose(&fb);
    }
    if (R_FAILED(result)) {
        free(b.items);
        free(pixels);
        return (int)result;
    }
    read_brand(&b, width != WIDTH);
    if (iz_library_cache_open(self, b.cache_dir)) load_library_cache(&b);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    Scan scan = {0};
    NetworkTransfer network = {0};
    GridRepeat repeat = {0};
    bool dirty = true, go = true, controls_ready = false, first_frame = true;
    while (go && appletMainLoop()) {
        padUpdate(&pad);
        const u64 held = padGetButtons(&pad);
        const u64 pressed = menu_buttons(&controls_ready, held, padGetButtonsDown(&pad));
        u64 keys = repeat_grid(&b, &repeat, controls_ready, held, pressed);
        if (keys == HidNpadButton_Plus) {
            if (b.dialog != DIALOG_CONFIRM_SETTINGS) {
                if (b.dialog < DIALOG_SETTINGS || b.dialog > DIALOG_LANGUAGE || !settings_dirty(&b)) break;
                confirm_game_settings(&b, true);
                dirty = true;
            }
            keys = 0;
        }
        if (poll_transfer(&network, &b)) dirty = true;
        if (!keys && b.transfer_refresh && b.dialog == DIALOG_NONE && b.view != VIEW_SAVES && !b.scanning) {
            b.transfer_refresh = false;
            scan_begin(&b, &scan);
            dirty = true;
        }
        if (b.dialog != DIALOG_NONE) {
            if (update_dialog(&b, &scan, &network, self, keys)) dirty = true;
            if (b.exit_requested) break;
        } else if ((keys & HidNpadButton_ZR) && b.view != VIEW_SAVES) {
            if (!start_transfer(&network, &b, self))
                notice(&b, "Could not start Transfer. Check Wi-Fi and the SD card.");
            dirty = true;
        } else if ((keys & HidNpadButton_ZL) && library_game(&b) && b.items[b.selected].title_id) {
            b.game_option = GAME_OPTION_SETTINGS;
            open_game_settings(&b, self);
            b.dialog = DIALOG_ROOM;
            dirty = true;
        } else {
            if (!first_frame && b.scroll == b.scroll_to && scan_step(&b, &scan)) dirty = true;
            if (keys) {
                go = browse(&b, &scan, self, keys);
                dirty = true;
            }
        }
        if (update_grid_scroll(&b, armTicksToNs(armGetSystemTick()))) dirty = true;
        if (!first_frame && go && b.dialog == DIALOG_NONE && b.scroll == b.scroll_to) {
            if (load_next_cover(&b)) dirty = true;
            else warm_next_cover(&b);
        }
        if (go && dirty) {
            draw(&b);
            u32 stride = 0;
            unsigned char *output = framebufferBegin(&fb, &stride);
            if (output) {
                for (int y = 0; y < height; ++y) {
                    uint32_t *line = (uint32_t *)(output + (size_t)y * stride);
                    const uint32_t *input = pixels + (y * HEIGHT / height) * WIDTH;
                    if (width == WIDTH) memcpy(line, input, WIDTH * sizeof(*line));
                    else for (int x = 0; x < width; ++x) line[x] = input[x * WIDTH / width];
                }
                if (width != WIDTH && b.brand.pixels) {
                    const int x = (BRAND_X * width + WIDTH - 1) / WIDTH;
                    const int y = (BRAND_Y * height + HEIGHT - 1) / HEIGHT;
                    const size_t row_bytes = (size_t)b.brand.width * 4;
                    for (int row = 0; row < b.brand.height; ++row)
                        memcpy(output + (size_t)(y + row) * stride + x * 4,
                               b.brand.pixels + (size_t)row * row_bytes, row_bytes);
                }
                framebufferEnd(&fb);
            }
            dirty = false;
        } else if (go) svcSleepThread(16000000);
        if (first_frame) {
            first_frame = false;
            scan_begin(&b, &scan);
            dirty = true;
        }
    }
    stop_transfer(&network);
    scan_stop(&scan);
    clear_covers(&b);
    cover_clear(&b.brand);
    framebufferClose(&fb);
    free(b.items);
    free(pixels);
    return 0;
}
#else
int main(int argc, char **argv) {
    Entry rows[] = {
        {.name = "Games/Pokemon Pokopia.nup", .title = "Pokémon Pokopia", .title_id = 0x0400a3a01ee4c000,
         .nup = true, .bytes = UINT64_C(7021211961)},
        {.name = "Games/Fast Fusion.nup", .title = "Fast Fusion", .title_id = 0x040041e0212ae000,
         .nup = true, .bytes = UINT64_C(3636134247)},
        {.name = "Games/the-legend-of-zelda-ocar", .title = "The Legend of Zelda Ocarina of Time",
         .title_id = 0x04004860224dec000, .bytes = UINT64_C(26251385180), .loose = true}
    };
    Browser browser = {.items = rows, .count = sizeof(rows) / sizeof(*rows), .view = VIEW_LIBRARY};
    read_brand(&browser, false);
    for (size_t i = 0; i < browser.count && i + 1 < (size_t)argc; ++i) {
        CoverSlot *slot = &browser.covers[i];
        strcpy(slot->name, rows[i].name);
        slot->used = cover_read_file(&slot->image, argv[i + 1]);
    }
    pixels = malloc((size_t)WIDTH * HEIGHT * sizeof(*pixels));
    if (!pixels) return 1;
    draw(&browser);
    printf("P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; ++i) {
        const unsigned char rgb[] = {pixels[i], pixels[i] >> 8, pixels[i] >> 16};
        fwrite(rgb, 1, sizeof(rgb), stdout);
    }
    free(pixels);
    clear_covers(&browser);
    cover_clear(&browser.brand);
    return 0;
}
#endif
