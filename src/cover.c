#include "cover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_DDS
#define STBI_NO_PVR
#define STBI_NO_PKM
#define STBI_NO_QOI
#define STBI_NO_EXT
#define STBI_MAX_DIMENSIONS 2048
#include <stb_image.h>

void cover_clear(Cover *cover) {
    free(cover->pixels);
    *cover = (Cover){0};
}

bool cover_decode(Cover *cover, const void *bytes, size_t size) {
    if (!size || size > COVER_BYTES_LIMIT) return false;
    int width, height, channels;
    if (!stbi_info_from_memory(bytes, (int)size, &width, &height, &channels) ||
        width <= 0 || width > STBI_MAX_DIMENSIONS || height <= 0 || height > STBI_MAX_DIMENSIONS)
        return false;
    unsigned char *decoded = stbi_load_from_memory(bytes, (int)size, &width, &height, &channels, 4);
    if (!decoded) return false;
    if (width > 512 || height > 512) {
        const int longest = width > height ? width : height;
        const int w = (width * 512 + longest - 1) / longest;
        const int h = (height * 512 + longest - 1) / longest;
        unsigned char *small = malloc((size_t)w * h * 4);
        if (!small) {
            free(decoded);
            return false;
        }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                memcpy(small + 4 * (y * w + x), decoded + 4 * ((y * height / h) * width + x * width / w), 4);
        free(decoded);
        decoded = small;
        width = w;
        height = h;
    }
    cover_clear(cover);
    *cover = (Cover){decoded, width, height};
    return true;
}

bool cover_read_file(Cover *cover, const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    const long size = fseek(file, 0, SEEK_END) ? 0 : ftell(file);
    unsigned char *data = size > 0 && size <= COVER_BYTES_LIMIT ? malloc((size_t)size) : NULL;
    const bool loaded = data && !fseek(file, 0, SEEK_SET) &&
        fread(data, 1, (size_t)size, file) == (size_t)size && cover_decode(cover, data, (size_t)size);
    free(data);
    fclose(file);
    return loaded;
}
