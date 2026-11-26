/* ZBIC table conversion follows nx2-relink's tools/zbic.py (MIT, Olmectron). */
#include "zbic.h"
#include "nup.h"

#include <stdlib.h>
#include <string.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

enum { BLOCK_LIMIT = 128 << 10 };

typedef struct {
    const unsigned char *at, *end;
} Input;

typedef struct {
    unsigned char *at, *end;
} Output;

static const unsigned char *take(Input *in, size_t size) {
    if (size > (size_t)(in->end - in->at)) return NULL;
    const unsigned char *first = in->at;
    in->at += size;
    return first;
}

static bool append(Output *out, const void *from, size_t size) {
    if (size > (size_t)(out->end - out->at)) return false;
    memcpy(out->at, from, size);
    out->at += size;
    return true;
}

static uint64_t little(const unsigned char *bytes, unsigned size) {
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

typedef struct {
    Input bytes;
    uint64_t value;
} Digits;

static void fill_digits(Digits *digits) {
    while (digits->bytes.at != digits->bytes.end && digits->value < (UINT64_C(1) << 40))
        digits->value = (digits->value << 8) | *--digits->bytes.end;
}

static unsigned digit(Digits *digits, unsigned radix) {
    const unsigned result = digits->value % radix;
    digits->value /= radix;
    fill_digits(digits);
    return result;
}

static void table_sums(Digits *digits, unsigned *sums, unsigned low, unsigned high) {
    if (high - low < 2) return;
    const unsigned middle = (high + low) / 2;
    sums[middle] = sums[low] + digit(digits, sums[high] - sums[low] + 1);
    table_sums(digits, sums, middle, high);
    table_sums(digits, sums, low, middle);
}

typedef struct {
    Output *out;
    unsigned bits, used;
} BitWriter;

static bool write_bits(BitWriter *writer, unsigned bits, unsigned count) {
    writer->bits |= bits << writer->used;
    writer->used += count;
    while (writer->used >= 8) {
        const unsigned char byte = writer->bits;
        if (!append(writer->out, &byte, 1)) return false;
        writer->bits >>= 8;
        writer->used -= 8;
    }
    return true;
}

static bool standard_table(Output *out, const int *counts, unsigned length, unsigned log) {
    BitWriter writer = {.out = out, .bits = log - 5, .used = 4};
    unsigned threshold = 1u << log, remaining = threshold + 1, width = log + 1;
    for (unsigned i = 0; remaining > 1;) {
        if (i == length) return false;
        const int count = counts[i++];
        const unsigned value = count + 1, limit = 2 * threshold - 1 - remaining;
        if (!write_bits(&writer, value < threshold ? value : value + limit,
                        value < limit ? width - 1 : width)) return false;
        remaining -= (unsigned)abs(count);
        while (remaining < threshold) {
            --width;
            threshold >>= 1;
        }
        if (count != 0) continue;
        unsigned run = 0;
        while (i < length && !counts[i]) {
            ++i;
            ++run;
        }
        while (run >= 3) {
            if (!write_bits(&writer, 3, 2)) return false;
            run -= 3;
        }
        if (!write_bits(&writer, run, 2)) return false;
    }
    const unsigned char rest = writer.bits;
    return !writer.used || append(out, &rest, 1);
}

static bool convert_table(Input *in, Output *out, unsigned max_symbol) {
    const unsigned char *head = take(in, 1);
    if (!head) return false;
    const unsigned size = *head & 127, shifted = *head >> 7;
    const unsigned char *body = take(in, size);
    if (!body) return false;
    Digits digits = {.bytes = {body, body + size}};
    fill_digits(&digits);
    const unsigned last = digit(&digits, 52) + 1;
    if (last > max_symbol) return false;
    const unsigned log = digit(&digits, 8) + 5, total = 1u << log;
    unsigned span = 2, sums[65] = {0};
    while (span <= last) span *= 2;
    sums[span] = digit(&digits, total) + 1 + ((last + 1) * shifted);
    table_sums(&digits, sums, 0, span);
    if (sums[1]) digit(&digits, sums[1] + 1);
    int counts[53];
    unsigned check = 0;
    for (unsigned i = 0; i <= last; ++i) {
        counts[i] = (int)(sums[i + 1] - sums[i]) - (int)shifted;
        check += (unsigned)abs(counts[i]);
    }
    return digits.bytes.at == digits.bytes.end && check == total &&
        standard_table(out, counts, last + 1, log);
}

static bool convert_literals(Input *in, Output *out) {
    if (in->at == in->end) return false;
    const unsigned kind = *in->at & 3;
    unsigned form = *in->at >> 2 & 3;
    const unsigned packed_head[] = {3, 3, 4, 5}, field_bits[] = {10, 10, 14, 18};
    const unsigned plain_head[] = {1, 2, 1, 3};
    unsigned width = kind < 2 ? plain_head[form] : packed_head[form];
    const unsigned char *first = take(in, width);
    if (!first) return false;
    const uint64_t header = little(first, width);
    if (kind < 2) {
        const size_t size = kind ? 1 : header >> (width == 1 ? 3 : 4);
        return take(in, size) && append(out, first, width + size);
    }

    unsigned bits = field_bits[form];
    const unsigned mask = (1u << bits) - 1;
    const unsigned decoded = (header >> 4) & mask, packed = (header >> (4 + bits)) & mask;
    const unsigned char *body = take(in, packed);
    if (!body) return false;
    if (kind != 2 || !packed || body[0] >= 128) return append(out, first, width + packed);
    const unsigned old_size = body[0];
    if (old_size >= packed) return false;
    Input tree = {body + 1, body + 1 + old_size};
    unsigned char weights[128];
    Output converted = {weights + 1, weights + sizeof(weights)};
    if (!convert_table(&tree, &converted, 255) ||
        !append(&converted, tree.at, (size_t)(tree.end - tree.at))) return false;
    const size_t weight_size = (size_t)(converted.at - weights);
    weights[0] = (unsigned char)(weight_size - 1);
    const size_t literals = weight_size + packed - old_size - 1;
    while (form && form < 3 && ((decoded | literals) >> field_bits[form])) ++form;
    bits = field_bits[form];
    width = packed_head[form];
    if ((decoded | literals) >> bits) return false;
    const uint64_t new_header = kind | (form << 2) | ((uint64_t)decoded << 4) |
        ((uint64_t)literals << (4 + bits));
    unsigned char bytes[5];
    for (unsigned i = 0; i < width; ++i) bytes[i] = new_header >> (8 * i);
    return append(out, bytes, width) && append(out, weights, weight_size) &&
        append(out, body + old_size + 1, packed - old_size - 1);
}

static bool convert_block(Input in, Output *out) {
    if (!convert_literals(&in, out) || in.at == in.end) return false;
    const unsigned first = *in.at;
    const unsigned count_size = first < 128 ? 1 : first < 255 ? 2 : 3;
    const unsigned char *count = take(&in, count_size);
    if (!count || !append(out, count, count_size)) return false;
    if (first) {
        const unsigned char *modes = take(&in, 1);
        if (!modes || !append(out, modes, 1)) return false;
        const unsigned max_symbols[] = {35, 31, 52};
        for (unsigned i = 0; i < 3; ++i) {
            const unsigned mode = (*modes >> (6 - 2 * i)) & 3;
            if (mode == 1) {
                const unsigned char *symbol = take(&in, 1);
                if (!symbol || !append(out, symbol, 1)) return false;
            } else if (mode == 2 && !convert_table(&in, out, max_symbols[i])) return false;
        }
    }
    return append(out, in.at, (size_t)(in.end - in.at));
}

/* The bufferless decoder keeps its history in the contiguous NSO output. */
static bool decode_chunk(ZSTD_DCtx *ctx, Input in, Output *out) {
    while (in.at != in.end) {
        const size_t size = ZSTD_nextSrcSizeToDecompress(ctx);
        const unsigned char *bytes = take(&in, size);
        if (!size || !bytes) return false;
        const size_t written = ZSTD_decompressContinue(ctx, out->at, (size_t)(out->end - out->at), bytes, size);
        if (ZSTD_isError(written)) return false;
        out->at += written;
    }
    return true;
}

static bool decode_frame(ZSTD_DCtx *ctx, Input *in, Output *out, unsigned char *block) {
    const unsigned char *prefix = take(in, 5);
    if (!prefix || memcmp(prefix, "ZBIC", 4)) return false;
    unsigned char header[ZSTD_FRAMEHEADERSIZE_MAX] = {0x28, 0xb5, 0x2f, 0xfd, prefix[4]};
    const size_t header_size = ZSTD_frameHeaderSize(header, 5);
    if (ZSTD_isError(header_size) || header_size < 5 || header_size > sizeof(header)) return false;
    const unsigned char *rest = take(in, header_size - 5);
    if (!rest) return false;
    memcpy(header + 5, rest, header_size - 5);
    ZSTD_frameHeader descriptor;
    if (ZSTD_getFrameHeader(&descriptor, header, header_size) ||
        ZSTD_isError(ZSTD_decompressBegin(ctx)) ||
        !decode_chunk(ctx, (Input){header, header + header_size}, out)) return false;
    unsigned last;
    do {
        const unsigned char *word = take(in, 3);
        if (!word) return false;
        const unsigned value = (unsigned)little(word, 3);
        last = value & 1;
        const unsigned kind = (value >> 1) & 3, size = value >> 3;
        if (kind == 3 || size > BLOCK_LIMIT) return false;
        const size_t stored = kind == 1 ? 1 : size;
        const unsigned char *body = take(in, stored);
        if (!body) return false;
        if (kind == 2) {
            Output converted = {block + 3, block + 3 + BLOCK_LIMIT};
            if (!convert_block((Input){body, body + stored}, &converted)) return false;
            const unsigned code = ((unsigned)(converted.at - block - 3) << 3) | 4 | last;
            for (unsigned i = 0; i < 3; ++i) block[i] = code >> (8 * i);
            if (!decode_chunk(ctx, (Input){block, converted.at}, out)) return false;
        } else if (!decode_chunk(ctx, (Input){word, body + stored}, out)) return false;
    } while (!last);
    if (descriptor.checksumFlag) {
        const unsigned char *checksum = take(in, 4);
        if (!checksum || !decode_chunk(ctx, (Input){checksum, checksum + 4}, out)) return false;
    }
    return !ZSTD_nextSrcSizeToDecompress(ctx);
}

bool iz_zbic_decode(const void *packed, size_t packed_size, void *image, size_t image_size) {
    Input in = {packed, (const unsigned char *)packed + packed_size};
    Output out = {image, (unsigned char *)image + image_size};
    unsigned char *block = malloc(BLOCK_LIMIT + 3);
    ZSTD_DCtx *ctx = ZSTD_createDCtx();
    bool ok = ctx && block && packed_size;
    while (ok && in.at != in.end) ok = decode_frame(ctx, &in, &out, block);
    ZSTD_freeDCtx(ctx);
    free(block);
    return ok && out.at == out.end;
}
