#include "boot.h"
#include "cover.h"
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

enum { WIDTH = 1920, HEIGHT = 1080, GAME_PAGE = 4, FILE_PAGE = 10, MAX_ENTRIES = 12000 };
enum { BRAND_X = 64, BRAND_Y = 42, BRAND_SIZE = 70 };
enum {
    BG = 0x0d1421, PANEL = 0x131d30, CARD = 0x1b2940, CONTROL = 0x242e42,
    OUTLINE = 0x34445c, TEXT = 0xf4f7fc, MUTED = 0xb1bed1, TEAL = 0x8ce8d7,
    GOLD = 0xffce88, ALERT_BG = 0x392435, ALERT_INK = 0xffc5d7
};
enum { BODY_TEXT = 1, TITLE_TEXT = 2, FOOTER_TEXT = 3 };
typedef enum { VIEW_LIBRARY, VIEW_FILES, VIEW_SAVES } View;
typedef enum { DIALOG_NONE, DIALOG_OPTIONS, DIALOG_TRANSFER, DIALOG_ROOM, DIALOG_LANGUAGE, DIALOG_DELETE } Dialog;
enum { GAME_OPTION_SAVES, GAME_OPTION_ROOM, GAME_OPTION_LANGUAGE, GAME_OPTION_DLC };
enum { LANGUAGE_COLUMNS = 2 };

static const char sd_root[] = "sdmc:/";
extern const unsigned char iz_brand[], iz_brand_end[];
extern const unsigned char iz_brand_compact[], iz_brand_compact_end[];

typedef struct {
    char name[512];
    char title[128];
    uint64_t bytes, title_id, cover_offset;
    uint32_t cover_size;
    bool directory, loose, needs_exefs, nup, split, incomplete;
} Entry;
typedef struct {
    char name[512];
    Cover image;
    bool used;
} CoverSlot;

typedef struct {
    Entry *items;
    size_t count, selected, first, next_cover;
    CoverSlot covers[GAME_PAGE * 2];
    Cover brand;
    char path[512], notice[200];
    char save_root[512], save_path[512], return_game[512], save_title[128];
    bool show_all, scanning, restore_game;
    View view;
    Dialog dialog;
    unsigned game_option;
    char address[sizeof("http://255.255.255.255:65535")], code[9];
    IzTransferStatus transfer_status;
    uint64_t received, total;
    unsigned room;
    IzGameLanguage language;
    bool dlc_enabled;
} Browser;

static size_t page_size(const Browser *b) {
    return b->view == VIEW_LIBRARY ? GAME_PAGE : FILE_PAGE;
}

static const char *view_path(const Browser *b) {
    if (b->view == VIEW_SAVES) return b->save_path;
    return b->view == VIEW_FILES ? b->path : sd_root;
}

static bool game_selected(const Browser *b) {
    return b->view == VIEW_LIBRARY && b->count && b->items[b->selected].title_id;
}

static const char *display_name(const Entry *entry) {
    if (entry->title[0]) return entry->title;
    if (entry->loose && !entry->name[0]) return "SD card";
    const char *slash = strrchr(entry->name, '/');
    return slash ? slash + 1 : entry->name;
}

static uint32_t *pixels;
static uint32_t rgba(unsigned rgb) {
    return 0xff000000u | (rgb & 0xffu) << 16 | (rgb & 0xff00u) | (rgb >> 16);
}

static void rect(int x, int y, int w, int h, unsigned color) {
    if (x < 0 || y < 0 || w < 1 || h < 1 || x + w > WIDTH || y + h > HEIGHT) return;
    const uint32_t c = rgba(color);
    for (int iy = y; iy < y + h; ++iy) {
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
        if (y + j < 0 || y + j >= HEIGHT) continue;
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
        if (b->covers[i].used && !strcmp(b->covers[i].name, entry->name)) return &b->covers[i];
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
    for (int j = 0; j < height; ++j) {
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
        shortcut(64, "B", "Close Transfer");
        return;
    }
    if (b->dialog != DIALOG_NONE) {
        const char *action = b->dialog == DIALOG_ROOM || b->dialog == DIALOG_LANGUAGE ? "Save" :
            b->dialog == DIALOG_DELETE ? "Delete" :
            b->dialog == DIALOG_OPTIONS && b->game_option == GAME_OPTION_DLC ? "Change" : "Open";
        int next = shortcut(64, "A", action);
        next = shortcut(next, "B", "Cancel");
        if (b->dialog != DIALOG_DELETE) shortcut(next, "D-pad", "Choose");
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
    if (game_selected(b)) next = shortcut(next, "Y", "Options");
    if (b->view == VIEW_FILES) next = shortcut(next, "Y", "Filter");
    if (saves && b->count) next = shortcut(next, "Y", "Delete");
    if (!saves) shortcut(next, "ZR", "Transfer");

    const char *mode = library ? "Files" : "Games";
    shortcut(launcher - 43 - shortcut_width("−", mode), "−", mode);
}

static const char *transfer_label(IzTransferStatus status) {
    switch (status) {
        case IZ_TRANSFER_READY: return "Waiting for a NUP file";
        case IZ_TRANSFER_RECEIVING: return "Receiving file";
        case IZ_TRANSFER_DONE: return "Saved to SD:/Games/";
        case IZ_TRANSFER_SPLIT: return "Saved in parts to SD:/Games/";
        case IZ_TRANSFER_INTERRUPTED: return "Connection interrupted. You can send the file again.";
        case IZ_TRANSFER_INVALID: return "Invalid or incomplete NUP";
        case IZ_TRANSFER_EXISTS: return "A file or folder with that name already exists.";
        case IZ_TRANSFER_NO_SPACE: return "The SD card is full.";
        case IZ_TRANSFER_DISK_ERROR: return "Could not write the SD card.";
        case IZ_TRANSFER_LOCKED: return "Too many attempts. Reopen Transfer for a new code.";
        default: return "The network disconnected.";
    }
}

static void draw_game_options(const Browser *b, const Entry *current) {
    const char *labels[] = {"Saved data", "Loader room", "Language",
        b->dlc_enabled ? "Add-on content: On" : "Add-on content: Off"};
    const char *details[] = {"Open this game's save files", "Adjust the memory reserve for this game",
        iz_game_language_label(b->language),
        "Use the DLC bundled in this game's NUP"};
    text(524, 294, "Game options", TEXT, PANEL, TITLE_TEXT, 872);
    text(524, 395, display_name(current), MUTED, PANEL, BODY_TEXT, 872);
    const unsigned last = current->nup ? GAME_OPTION_DLC : GAME_OPTION_LANGUAGE;
    for (unsigned i = 0; i <= last; ++i) {
        const int y = 443 + 93 * (int)i;
        const bool active = i == b->game_option;
        const unsigned background = active ? 0x294357 : CARD;
        rounded_panel(524, y, 872, 80, 10, background, background);
        if (active) rect(524, y + 10, 4, 60, TEAL);
        text(544, y + 8, labels[i], active ? TEAL : TEXT, background, BODY_TEXT, 824);
        text(544, y + 41, details[i], MUTED, background, FOOTER_TEXT, 824);
    }
}

static void draw_language_picker(const Browser *b, const Entry *current) {
    rounded_panel(396, 158, 1128, 778, 24, PANEL, 0x111a2a);
    text(444, 188, "Game language", TEXT, PANEL, TITLE_TEXT, 1032);
    rect(444, 253, 1032, 1, OUTLINE);
    text(444, 276, display_name(current), MUTED, PANEL, BODY_TEXT, 1032);
    for (unsigned i = 0; i < IZ_LANGUAGE_COUNT; ++i) {
        const int x = 444 + 524 * (i % LANGUAGE_COLUMNS), y = 333 + 54 * (i / LANGUAGE_COLUMNS);
        const bool selected = i == b->language;
        const unsigned background = selected ? 0x294357 : CARD;
        rounded_panel(x, y, 508, 44, 10, background, background);
        if (selected) rect(x, y + 7, 4, 30, TEAL);
        text(x + 18, y + 8, iz_game_language_label((IzGameLanguage)i),
             selected ? TEAL : TEXT, background, BODY_TEXT, 472);
    }
    text(444, 887, "Available translations depend on the game.", MUTED, PANEL, FOOTER_TEXT, 1032);
}

static void draw_dialog(const Browser *b, const Entry *current) {
    if (b->dialog == DIALOG_LANGUAGE) {
        draw_language_picker(b, current);
        return;
    }
    const int height = b->dialog == DIALOG_OPTIONS && current->nup ? 582 : 495;
    rounded_panel(464, 251, 992, height, 24, PANEL, 0x111a2a);
    rect(524, 367, 872, 1, OUTLINE);
    if (b->dialog == DIALOG_OPTIONS) {
        draw_game_options(b, current);
    } else if (b->dialog == DIALOG_DELETE) {
        text(524, 294, "Delete saved data?", TEXT, PANEL, TITLE_TEXT, 872);
        text(524, 404, b->save_title, MUTED, PANEL, BODY_TEXT, 872);
        text(524, 460, display_name(current), TEAL, PANEL, TITLE_TEXT, 872);
        const char *warning = current->directory ? "This will delete the folder and everything in it." :
            "This will permanently delete the selected save file.";
        text(524, 570, warning, ALERT_INK, PANEL, BODY_TEXT, 872);
    } else if (b->dialog == DIALOG_ROOM) {
        char amount[32];
        snprintf(amount, sizeof(amount), "‹     %u MiB     ›", b->room);
        text(524, 294, "Loader room", TEXT, PANEL, TITLE_TEXT, 872);
        text(524, 396, current ? display_name(current) : "", MUTED, PANEL, BODY_TEXT, 872);
        text(524, 463, amount, TEAL, PANEL, TITLE_TEXT, 872);
        text(524, 550, "Increase only if the loader reports too little room.", TEXT, PANEL, BODY_TEXT, 872);
        text(524, 606, "A larger reserve leaves less memory for the game.", MUTED, PANEL, BODY_TEXT, 872);
    } else {
        char code[28];
        snprintf(code, sizeof(code), "Code  %s", b->code);
        text(524, 294, "Transfer", TEXT, PANEL, TITLE_TEXT, 872);
        text(524, 394, "Open in a web browser on the same network.", MUTED, PANEL, BODY_TEXT, 872);
        text(524, 445, b->address, TEXT, PANEL, TITLE_TEXT, 872);
        text(524, 522, code, TEAL, PANEL, BODY_TEXT, 872);
        text(524, 580, transfer_label(b->transfer_status), TEXT, PANEL, BODY_TEXT, 872);
        if (b->total && b->transfer_status == IZ_TRANSFER_RECEIVING) {
            char received[40], total[40], progress[88];
            bytes_label(received, sizeof(received), b->received);
            bytes_label(total, sizeof(total), b->total);
            snprintf(progress, sizeof(progress), "%s / %s", received, total);
            rect(524, 632, 872, 8, CONTROL);
            rect(524, 632, (int)(872 * b->received / b->total), 8, TEAL);
            text(524, 661, progress, MUTED, PANEL, FOOTER_TEXT, 872);
        }
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
    enum { TILE_WIDTH = 268, TILE_HEIGHT = 480 };
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
    snprintf(count, sizeof(count), b->scanning ? "%zu found  /  Scanning..." :
        library ? "%zu games" : "%zu entries", b->count);
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
        if (library) {
            const int displayed = (int)(end - b->first);
            const int x = 87 + (1124 - displayed * 268 - (displayed - 1) * 17) / 2;
            for (size_t i = b->first; i < end; ++i)
                draw_tile(b, &b->items[i], x + (int)(i - b->first) * 285, 257, i == b->selected);
        } else draw_file_list(b, end);
        char page[48];
        snprintf(page, sizeof(page), "%zu / %zu", b->first / page_rows + 1,
                 (b->count + page_rows - 1) / page_rows);
        text(1209 - text_width(page, FOOTER_TEXT), 884, page, MUTED, PANEL, FOOTER_TEXT, 112);
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
    return order ? order : strcasecmp(x->name, y->name);
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

static bool load_cover(Browser *b, const Entry *entry) {
    if ((!entry->nup && !entry->loose) || find_cover(b, entry)) return false;
    const char *const loose_names[] = {"/cover.png", "/cover.jpg", "/icon.jpg", "/icon.png"};
    const char *const nup_suffixes[] = {".png", ".jpg"};
    char source[512], path[544];
    if (!join_path(source, view_path(b), entry->name)) return false;
    CoverSlot *slot = &b->covers[b->next_cover++ % (sizeof(b->covers) / sizeof(*b->covers))];
    cover_clear(&slot->image);
    slot->used = true;
    strcpy(slot->name, entry->name);

    if (entry->cover_size && entry->cover_size <= COVER_BYTES_LIMIT) {
        void *data = malloc(entry->cover_size);
        const bool loaded = data &&
            iz_read_nup_cover(source, entry->cover_offset, data, entry->cover_size) &&
            cover_decode(&slot->image, data, entry->cover_size);
        free(data);
        if (loaded) return true;
    }
    const char *const *names = entry->loose ? loose_names : nup_suffixes;
    const unsigned count = entry->loose ? sizeof(loose_names) / sizeof(*loose_names) :
        sizeof(nup_suffixes) / sizeof(*nup_suffixes);
    for (unsigned i = 0; i < count; ++i) {
        snprintf(path, sizeof(path), "%s%s", source, names[i]);
        if (cover_read_file(&slot->image, path)) return true;
    }
    return false;
}

static bool load_next_cover(Browser *b) {
    if (!b->count || b->view == VIEW_SAVES) return false;
    const Entry *selected = &b->items[b->selected];
    if ((selected->nup || selected->loose) && !find_cover(b, selected)) return load_cover(b, selected);
    if (b->view != VIEW_LIBRARY) return false;
    for (size_t i = b->first; i < b->count && i < b->first + GAME_PAGE; ++i) {
        const Entry *entry = &b->items[i];
        if ((entry->nup || entry->loose) && !find_cover(b, entry)) return load_cover(b, entry);
    }
    return false;
}

typedef struct PendingFolder {
    struct PendingFolder *next;
    char path[512];
} PendingFolder;

typedef struct {
    PendingFolder *todo;
    FsFileSystem *sd;
    FsDir dir;
    FsDirectoryEntry rows[16];
    char path[512];
    s64 next, count;
    bool open, checked_romfs, incomplete;
} Scan;

static void scan_stop(Scan *scan) {
    if (scan->open) fsDirClose(&scan->dir);
    scan->open = false;
    while (scan->todo) {
        PendingFolder *folder = scan->todo;
        scan->todo = folder->next;
        free(folder);
    }
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
    clear_covers(b);
    b->count = b->selected = b->first = 0;
    scan->incomplete = false;
    scan->sd = fsdevGetDeviceFileSystem("sdmc");
    if (scan->sd && b->view == VIEW_SAVES) {
        struct stat status;
        if (stat(b->save_path, &status) && errno == ENOENT) {
            b->scanning = false;
            return;
        }
    }
    b->scanning = scan->sd && scan_queue(scan, view_path(b));
    if (!b->scanning) notice(b, "Could not start SD scan.");
}

static void select_entry(Browser *b, size_t index) {
    const size_t rows = page_size(b);
    b->selected = index;
    b->first = index / rows * rows;
}

static void scan_finish(Browser *b, Scan *scan) {
    scan_stop(scan);
    b->scanning = false;
    if (b->view == VIEW_LIBRARY) b->restore_game = false;
    if (!b->notice[0]) {
        if (b->count == MAX_ENTRIES) notice(b, "The display limit was reached.");
        else if (scan->incomplete) notice(b, "Some SD entries could not be scanned.");
    }
    if (!b->count) return;
    char selected[512];
    strcpy(selected, b->items[b->selected].name);
    qsort(b->items, b->count, sizeof(*b->items), entry_order);
    for (size_t i = 0; i < b->count; ++i) {
        if (strcmp(selected, b->items[i].name)) continue;
        select_entry(b, i);
        break;
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
        Entry *item = &b->items[b->count++];
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
            Entry *item = &b->items[b->count++];
            *item = (Entry){.bytes = image_size, .title_id = title_id, .loose = true, .needs_exefs = !title_id};
            strcpy(item->name, scan->path + sizeof(sd_root) - 1);
            read_loose_title(scan->path, item->title);
            return;
        }
    }

    NupPartName part;
    const bool first_part = !directory && nup_part_start(row->name, &part);
    NupPreview preview;
    const bool candidate = !directory && (size >= 128 || first_part);
    const bool nup = candidate && iz_probe_nup(path, &preview);
    const bool named_nup = !directory && has_nup_name(row->name, first_part);
    if (library && !nup) return;
    if (!library && !directory && !b->show_all && !nup && !named_nup) return;
    Entry *item = &b->items[b->count++];
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
    }
}

static bool scan_step(Browser *b, Scan *scan) {
    if (!b->scanning) return false;
    const size_t previous = b->count;
    const uint64_t started = armGetSystemTick();
    for (unsigned budget = 0; budget < 24 && armTicksToNs(armGetSystemTick() - started) < 4000000; ++budget) {
        if (b->count == MAX_ENTRIES || (!scan->open && !scan->todo)) {
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
        const size_t count = b->count;
        scan_entry(b, scan, &scan->rows[scan->next++]);
        if (b->view == VIEW_LIBRARY && b->restore_game && b->count > count &&
            !strcmp(b->items[count].name, b->return_game)) {
            select_entry(b, count);
            b->restore_game = false;
        }
    }
    return b->count != previous;
}

static void move(Browser *b, int step) {
    if (!b->count) return;
    const int rows = (int)page_size(b);
    if ((step == -rows && b->first == 0) ||
        (step == rows && b->first + (size_t)rows >= b->count)) return;
    const int position = (int)b->selected + step;
    select_entry(b, position < 0 ? 0 : (size_t)position >= b->count ? b->count - 1 : (size_t)position);
}

static u64 menu_buttons(bool *ready, u64 held, u64 down) {
    // Ignore the button that opened BreRun until it has been released.
    if (!*ready) {
        *ready = held == 0;
        return 0;
    }
    return down & HidNpadButton_Plus ? HidNpadButton_Plus : down;
}

typedef struct {
    IzTransfer receiver;
    Thread worker;
    bool active, restore_sleep;
} NetworkTransfer;

static bool start_transfer(NetworkTransfer *network, Browser *b, const char *self) {
    if (R_FAILED(socketInitializeDefault())) return false;
    const struct in_addr local = {.s_addr = (uint32_t)gethostid()};
    char ip[INET_ADDRSTRLEN];
    if (local.s_addr == INADDR_NONE || !inet_ntop(AF_INET, &local, ip, sizeof(ip)) ||
        !iz_transfer_open(&network->receiver, "sdmc:/Games", local.s_addr, IZ_TRANSFER_PORT,
                          iz_transfer_split(self), randomGet64())) {
        socketExit();
        return false;
    }
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

static void switch_view(Browser *b, Scan *scan, View view) {
    b->view = view;
    notice(b, NULL);
    scan_begin(b, scan);
}

static void open_saves(Browser *b, Scan *scan, const char *self) {
    const Entry *entry = &b->items[b->selected];
    b->dialog = DIALOG_NONE;
    if (!iz_game_save_path(self, entry->title_id, b->save_root, sizeof(b->save_root))) {
        notice(b, "Could not locate this game's saved data.");
        return;
    }
    memcpy(b->return_game, entry->name, sizeof(b->return_game));
    b->restore_game = true;
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

static void open_room(Browser *b, const char *self) {
    b->room = iz_game_room(self, b->items[b->selected].title_id);
    b->dialog = DIALOG_ROOM;
}

static bool update_dialog(Browser *b, Scan *scan, NetworkTransfer *network, const char *self, u64 keys) {
    if (b->dialog == DIALOG_TRANSFER) {
        if (keys & HidNpadButton_B) {
            stop_transfer(network);
            const int status = atomic_load(&network->receiver.status);
            b->dialog = DIALOG_NONE;
            if (status == IZ_TRANSFER_DONE || status == IZ_TRANSFER_SPLIT) {
                notice(b, transfer_label(status));
                scan_begin(b, scan);
            }
            return true;
        }
        const IzTransferStatus status = atomic_load(&network->receiver.status);
        const uint64_t received = atomic_load(&network->receiver.received);
        const uint64_t total = atomic_load(&network->receiver.total);
        if (status == b->transfer_status && (received >> 20) == (b->received >> 20) && total == b->total)
            return false;
        b->transfer_status = status;
        b->received = received;
        b->total = total;
        return true;
    }

    if (!keys) return false;
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
            else if (b->game_option == GAME_OPTION_ROOM) open_room(b, self);
            else if (b->game_option == GAME_OPTION_LANGUAGE) b->dialog = DIALOG_LANGUAGE;
            else if (iz_save_game_dlc(self, game->title_id, !b->dlc_enabled)) b->dlc_enabled = !b->dlc_enabled;
            else {
                notice(b, "Could not save the add-on content setting.");
                b->dialog = DIALOG_NONE;
            }
        } else if (keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
            if (b->game_option > GAME_OPTION_SAVES) --b->game_option;
        } else if (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
            if (b->game_option < (game->nup ? GAME_OPTION_DLC : GAME_OPTION_LANGUAGE)) ++b->game_option;
        }
        return true;
    }

    if (b->dialog == DIALOG_LANGUAGE) {
        const unsigned language = b->language;
        if (keys & HidNpadButton_A) {
            const bool saved = iz_save_game_language(self, b->items[b->selected].title_id, b->language);
            notice(b, saved ? "Game language saved." : "Could not save game language.");
            b->dialog = DIALOG_NONE;
        } else if ((keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) && language >= LANGUAGE_COLUMNS) {
            b->language -= LANGUAGE_COLUMNS;
        } else if ((keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) && language + LANGUAGE_COLUMNS < IZ_LANGUAGE_COUNT) {
            b->language += LANGUAGE_COLUMNS;
        } else if ((keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) && language % LANGUAGE_COLUMNS) {
            --b->language;
        } else if ((keys & (HidNpadButton_Right | HidNpadButton_StickLRight)) &&
                   language % LANGUAGE_COLUMNS < LANGUAGE_COLUMNS - 1 && language + 1 < IZ_LANGUAGE_COUNT) {
            ++b->language;
        }
        return true;
    }

    static const unsigned rooms[] = {IZ_ROOM_DEFAULT_MIB, 512, 768, 1024, 1536, IZ_ROOM_MAX_MIB};
    if (keys & HidNpadButton_A) {
        const bool saved = iz_save_game_room(self, b->items[b->selected].title_id, b->room);
        notice(b, saved ? "Loader room saved." : "Could not save loader room.");
        b->dialog = DIALOG_NONE;
    } else if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) {
        for (size_t i = sizeof(rooms) / sizeof(*rooms); i-- > 0;)
            if (rooms[i] < b->room) { b->room = rooms[i]; break; }
    } else if (keys & (HidNpadButton_Right | HidNpadButton_StickLRight)) {
        for (size_t i = 0; i < sizeof(rooms) / sizeof(*rooms); ++i)
            if (rooms[i] > b->room) { b->room = rooms[i]; break; }
    }
    return true;
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
    } else if (entry->needs_exefs) {
        notice(b, "This game needs its original files in the exefs folder.");
    } else if (entry->incomplete) {
        notice(b, NUP_INCOMPLETE_MESSAGE);
    } else if (entry->loose || entry->nup) {
        const char *reason = NULL;
        if (iz_queue_game(self, path, entry->title_id, &reason)) return false;
        notice(b, reason);
    } else notice(b, "Select a complete game dump to launch.");
    return true;
}

static bool browse(Browser *b, Scan *scan, const char *self, u64 keys) {
    if (keys & HidNpadButton_B) back(b, scan);
    else if (keys & HidNpadButton_Minus) {
        switch_view(b, scan, b->view == VIEW_LIBRARY ? VIEW_FILES : VIEW_LIBRARY);
    } else if (keys & HidNpadButton_Y) {
        if (game_selected(b)) {
            b->game_option = GAME_OPTION_SAVES;
            b->language = iz_game_language(self, b->items[b->selected].title_id);
            b->dlc_enabled = b->items[b->selected].nup && iz_game_dlc(self, b->items[b->selected].title_id);
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
        const u64 up = HidNpadButton_Up | HidNpadButton_StickLUp;
        const u64 down = HidNpadButton_Down | HidNpadButton_StickLDown;
        const bool library = b->view == VIEW_LIBRARY;
        const int rows = (int)page_size(b);
        if (keys & (HidNpadButton_L | (library ? up : 0))) move(b, -rows);
        else if (keys & (HidNpadButton_R | (library ? down : 0))) move(b, rows);
        else if (keys & (library ? left : up)) move(b, -1);
        else if (keys & (library ? right : down)) move(b, 1);
    }
    return true;
}

int iz_browser(const char *self, const char *message) {
    Browser b = {.path = "sdmc:/", .view = VIEW_LIBRARY};
    b.items = calloc(MAX_ENTRIES, sizeof(*b.items));
    pixels = malloc((size_t)WIDTH * HEIGHT * sizeof(*pixels));
    if (!b.items || !pixels) {
        free(b.items);
        free(pixels);
        return 1;
    }
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
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    Scan scan = {0};
    scan_begin(&b, &scan);
    if (message) notice(&b, message);
    NetworkTransfer network = {0};
    bool dirty = true, go = true, controls_ready = false;
    while (go && appletMainLoop()) {
        padUpdate(&pad);
        const u64 keys = menu_buttons(&controls_ready, padGetButtons(&pad), padGetButtonsDown(&pad));
        if (keys == HidNpadButton_Plus) break;
        if (b.dialog != DIALOG_NONE) {
            if (update_dialog(&b, &scan, &network, self, keys)) dirty = true;
        } else if ((keys & HidNpadButton_ZR) && b.view != VIEW_SAVES) {
            if (!start_transfer(&network, &b, self))
                notice(&b, "Could not start Transfer. Check Wi-Fi and the SD card.");
            dirty = true;
        } else if ((keys & HidNpadButton_ZL) && game_selected(&b)) {
            open_room(&b, self);
            dirty = true;
        } else {
            if (scan_step(&b, &scan)) dirty = true;
            if (keys) {
                go = browse(&b, &scan, self, keys);
                dirty = true;
            }
            if (go && b.dialog == DIALOG_NONE && load_next_cover(&b)) dirty = true;
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
