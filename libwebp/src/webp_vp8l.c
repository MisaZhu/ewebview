/*
 * webp_vp8l.c - VP8L (WebP lossless) bitstream decoder.
 *
 * Implements the format described by the "Specification for WebP Lossless
 * Bitstream": the four optional transforms (predictor, cross color, subtract
 * green, color indexing), spatially variant prefix coding through meta prefix
 * codes over an entropy image, LZ77 backward references with the 120 entry
 * near-neighbour distance map, and the multiplicative color cache.
 *
 * The same entry point also decodes the headerless "image-stream" form that an
 * 'ALPH' chunk carries: the caller supplies the dimensions and the signature,
 * size and version fields are then absent.
 */
#include "webp_internal.h"

#include "webp_bits.h"
#include "webp_huff.h"

#define VP8L_MAGIC_BYTE        0x2f
#define VP8L_IMAGE_SIZE_BITS     14
#define VP8L_VERSION_BITS         3

#define VP8L_NUM_TRANSFORMS       4
#define VP8L_TR_PREDICTOR         0
#define VP8L_TR_COLOR             1
#define VP8L_TR_SUBTRACT_GREEN    2
#define VP8L_TR_COLOR_INDEXING    3
#define VP8L_MIN_TRANSFORM_BITS   2
#define VP8L_NUM_TRANSFORM_BITS   3

#define VP8L_NUM_HUFF_CODES       5
#define VP8L_HUFF_GREEN           0
#define VP8L_HUFF_RED             1
#define VP8L_HUFF_BLUE            2
#define VP8L_HUFF_ALPHA           3
#define VP8L_HUFF_DIST            4

#define VP8L_NUM_LITERAL_CODES   256
#define VP8L_NUM_LENGTH_CODES     24
#define VP8L_NUM_DISTANCE_CODES   40
#define VP8L_NUM_CL_CODES          19
#define VP8L_DEFAULT_CL_CODE       8
#define VP8L_MAX_CACHE_BITS       11
#define VP8L_MIN_HUFFMAN_BITS      2
#define VP8L_NUM_HUFFMAN_BITS      3
#define VP8L_CODE_TO_PLANE_CODES 120

/* When more than this many group indices show up in the entropy image the
 * used ones are remapped to a dense range, so that a hostile bitstream cannot
 * force a huge prefix code allocation. */
#define VP8L_INLINE_GROUP_LIMIT  200

#define DIV_ROUND_UP(n, d)  (((n) + (d) - 1) / (d))
#define SUB_SAMPLE(size, bits)  DIV_ROUND_UP((size), 1 << (bits))

/* Order in which the code length code lengths are transmitted. */
static const uint8_t kCodeLengthOrder[VP8L_NUM_CL_CODES] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

/* Plane offsets of the small distance codes. kCodeToPlane[code - 1] packs
 * them as y = value >> 4 and x = 8 - (value & 0xf). */
static const uint8_t kCodeToPlane[VP8L_CODE_TO_PLANE_CODES] = {
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a,
    0x38, 0x05, 0x37, 0x39, 0x15, 0x1b, 0x36, 0x3a, 0x25, 0x2b, 0x48, 0x04,
    0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b, 0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45,
    0x4b, 0x34, 0x3c, 0x03, 0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d,
    0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e,
    0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e,
    0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f, 0x64, 0x6c, 0x42, 0x4e,
    0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b, 0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e,
    0x00, 0x74, 0x7c, 0x41, 0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d,
    0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70
};

/* One entry per transform actually present, in bitstream order. The inverse
 * is applied back to front, which is why xsize/ysize keep the dimensions the
 * image had when the transform was read. */
typedef struct {
    int type;
    int bits;      /* predictor/color: block size bits; indexing: width bits */
    int xsize;     /* image width at read time, restored on the way back     */
    int ysize;
    int data_w;    /* dimensions of the decoded transform data image         */
    int data_h;
    uint32_t* data;
} WebpTransform;

static int decode_image_data(WebpBits* br, int width, int height, int is_argb,
                             uint32_t* pixels);

/* ------------------------------------------------------------------------ */
/* small helpers                                                            */
/* ------------------------------------------------------------------------ */

static int clip255(int v) {
    return (v < 0) ? 0 : ((v > 255) ? 255 : v);
}

static int average2(int a, int b) {
    return (a + b) / 2;
}

/* 3.5 fixed point colour transform delta; the shift is an arithmetic one, so
 * the result floors exactly like the reference implementation. */
static int color_delta(int t, int c) {
    return ((int)(int8_t)(uint8_t)t * (int)(int8_t)(uint8_t)c) >> 5;
}

/* LZ77 prefix coding: turns a prefix code plus its extra bits into a length or
 * a distance code. */
static int prefix_expand(int prefix_code, WebpBits* br) {
    if (prefix_code < 4)
        return prefix_code + 1;
    {
        const int extra_bits = (prefix_code - 2) >> 1;
        const int offset = (2 + (prefix_code & 1)) << extra_bits;
        return offset + (int)webp_bits_read(br, extra_bits) + 1;
    }
}

/* Maps a distance code onto a scan line offset. Codes above 120 are plain
 * pixel distances; the first 120 address a small neighbourhood around the
 * current pixel. */
static int plane_code_to_distance(int width, int plane_code) {
    if (plane_code > VP8L_CODE_TO_PLANE_CODES)
        return plane_code - VP8L_CODE_TO_PLANE_CODES;
    {
        const int code = kCodeToPlane[plane_code - 1];
        const int dist = (code >> 4) * width + (8 - (code & 0xf));
        /* dist < 1 happens for very narrow images */
        return (dist >= 1) ? dist : 1;
    }
}

static void cache_insert(uint32_t* cache, int bits, uint32_t argb) {
    cache[(uint32_t)((0x1e35a7bdu * argb) >> (32 - bits))] = argb;
}

/* ------------------------------------------------------------------------ */
/* predictors                                                               */
/* ------------------------------------------------------------------------ */

/* The 14 predictors of the predictor transform. The border rules are applied
 * by the caller, so L/T/TL/TR are always valid here. */
static uint32_t predict(int mode, uint32_t L, uint32_t T, uint32_t TL,
                        uint32_t TR) {
    int shift;
    uint32_t out = 0;

    switch (mode) {
    case 0:  return 0xff000000u;   /* solid black, opaque */
    case 1:  return L;
    case 2:  return T;
    case 3:  return TR;
    case 4:  return TL;
    case 11: {
        /* Select(L, T, TL): whole pixel decision, not per channel. */
        int pL = 0, pT = 0;
        for (shift = 24; shift >= 0; shift -= 8) {
            const int l = (int)((L >> shift) & 0xff);
            const int t = (int)((T >> shift) & 0xff);
            const int c = (int)((TL >> shift) & 0xff);
            pL += abs((t + l - c) - l);
            pT += abs((t + l - c) - t);
        }
        return (pL < pT) ? L : T;
    }
    default: break;
    }

    for (shift = 24; shift >= 0; shift -= 8) {
        const int l = (int)((L >> shift) & 0xff);
        const int t = (int)((T >> shift) & 0xff);
        const int c = (int)((TL >> shift) & 0xff);
        const int d = (int)((TR >> shift) & 0xff);
        int v;
        switch (mode) {
        case 5:  v = average2(average2(l, d), t); break;
        case 6:  v = average2(l, c); break;
        case 7:  v = average2(l, t); break;
        case 8:  v = average2(c, t); break;
        case 9:  v = average2(t, d); break;
        case 10: v = average2(average2(l, c), average2(t, d)); break;
        case 12: v = clip255(l + t - c); break;
        case 13: {
            const int a = average2(l, t);
            v = clip255(a + (a - c) / 2);
            break;
        }
        default: v = 0; break;
        }
        out |= (uint32_t)v << shift;
    }
    return out;
}

/* ------------------------------------------------------------------------ */
/* prefix codes                                                             */
/* ------------------------------------------------------------------------ */

/* Normal code length code: the lengths themselves are Huffman coded. */
static int read_code_lengths(WebpBits* br, const uint8_t* cl_lengths,
                             uint8_t* lengths, int alphabet_size) {
    WebpHuff clc;
    int max_symbol = alphabet_size;
    int symbol = 0;
    int prev = VP8L_DEFAULT_CL_CODE;

    webp_huff_init(&clc);
    if (!webp_huff_build(&clc, cl_lengths, VP8L_NUM_CL_CODES))
        return 0;

    if (webp_bits_read(br, 1)) {   /* an explicit max_symbol follows */
        const int length_nbits = 2 + 2 * (int)webp_bits_read(br, 3);
        max_symbol = 2 + (int)webp_bits_read(br, length_nbits);
        if (max_symbol > alphabet_size) {
            webp_huff_free(&clc);
            return 0;
        }
    }

    while (symbol < alphabet_size) {
        int code, repeat, value, i;
        if (max_symbol-- == 0)
            break;
        code = webp_huff_read(&clc, br);
        if (code < 0) {
            webp_huff_free(&clc);
            return 0;
        }
        if (code < 16) {
            lengths[symbol++] = (uint8_t)code;
            if (code != 0)
                prev = code;
            continue;
        }
        if (code == 16) {
            repeat = 3 + (int)webp_bits_read(br, 2);
            value = prev;
        } else if (code == 17) {
            repeat = 3 + (int)webp_bits_read(br, 3);
            value = 0;
        } else {
            repeat = 11 + (int)webp_bits_read(br, 7);
            value = 0;
        }
        if (symbol + repeat > alphabet_size) {   /* run overflows the alphabet */
            webp_huff_free(&clc);
            return 0;
        }
        for (i = 0; i < repeat; ++i)
            lengths[symbol++] = (uint8_t)value;
    }
    webp_huff_free(&clc);
    return 1;
}

/* Reads one prefix code description and builds its lookup table. */
static int read_prefix_code(WebpBits* br, WebpHuff* h, int alphabet_size) {
    uint8_t* lengths;
    int ok;

    webp_huff_init(h);
    if (alphabet_size <= 0 || br->eof)
        return 0;
    lengths = (uint8_t*)calloc((size_t)alphabet_size, sizeof(*lengths));
    if (lengths == NULL)
        return 0;

    if (webp_bits_read(br, 1)) {   /* simple code length code */
        const int num_symbols = (int)webp_bits_read(br, 1) + 1;
        const int first_8bits = (int)webp_bits_read(br, 1);
        int symbol = (int)webp_bits_read(br, first_8bits ? 8 : 1);
        if (symbol < alphabet_size) {
            lengths[symbol] = 1;
            if (num_symbols == 2) {
                symbol = (int)webp_bits_read(br, 8);
                if (symbol < alphabet_size)
                    lengths[symbol] = 1;
            }
        }
    } else {
        uint8_t cl_lengths[VP8L_NUM_CL_CODES];
        const int num_codes = (int)webp_bits_read(br, 4) + 4;
        int i;
        if (num_codes > VP8L_NUM_CL_CODES) {
            free(lengths);
            return 0;
        }
        memset(cl_lengths, 0, sizeof(cl_lengths));
        for (i = 0; i < num_codes; ++i)
            cl_lengths[kCodeLengthOrder[i]] = (uint8_t)webp_bits_read(br, 3);
        if (!read_code_lengths(br, cl_lengths, lengths, alphabet_size)) {
            free(lengths);
            return 0;
        }
    }

    if (br->eof) {
        free(lengths);
        return 0;
    }
    ok = webp_huff_build(h, lengths, alphabet_size);
    free(lengths);
    return ok;
}

/* Reads the five prefix codes of one group: green/length/cache, red, blue,
 * alpha and distance. */
static int read_huff_group(WebpBits* br, WebpHuff* codes, int cache_size) {
    static const int base_size[VP8L_NUM_HUFF_CODES] = {
        VP8L_NUM_LITERAL_CODES + VP8L_NUM_LENGTH_CODES,
        VP8L_NUM_LITERAL_CODES, VP8L_NUM_LITERAL_CODES,
        VP8L_NUM_LITERAL_CODES, VP8L_NUM_DISTANCE_CODES
    };
    int i;

    for (i = 0; i < VP8L_NUM_HUFF_CODES; ++i) {
        const int size = base_size[i] + ((i == VP8L_HUFF_GREEN) ? cache_size : 0);
        if (!read_prefix_code(br, &codes[i], size)) {
            for (; i >= 0; --i)
                webp_huff_free(&codes[i]);
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* entropy coded image                                                      */
/* ------------------------------------------------------------------------ */

static int huff_group_at(const uint32_t* entropy, int bits, int entropy_w,
                         int x, int y) {
    if (entropy == NULL)
        return 0;
    return (int)entropy[(y >> bits) * entropy_w + (x >> bits)];
}

/* Decodes width*height pixels. is_argb enables meta prefix codes, which only
 * the top level ARGB image may use. */
static int decode_image_data(WebpBits* br, int width, int height, int is_argb,
                             uint32_t* pixels) {
    int cache_bits = 0, cache_size = 0;
    uint32_t* cache = NULL;
    uint32_t* entropy = NULL;
    int entropy_bits = 0, entropy_w = 0, entropy_h = 0;
    int num_groups = 1, num_groups_max = 1;
    int* mapping = NULL;
    WebpHuff* groups = NULL;
    WebpHuff scratch[VP8L_NUM_HUFF_CODES];
    int x = 0, y = 0, i, j;
    int ok = 0;

    if (width <= 0 || height <= 0 || (uint64_t)width * height > WEBP_MAX_PIXELS)
        return 0;

    /* color-cache-info */
    if (webp_bits_read(br, 1)) {
        cache_bits = (int)webp_bits_read(br, 4);
        if (cache_bits < 1 || cache_bits > VP8L_MAX_CACHE_BITS)
            return 0;
        cache_size = 1 << cache_bits;
    }

    /* meta-prefix: an entropy image selects the group used per block */
    if (is_argb && webp_bits_read(br, 1)) {
        entropy_bits = VP8L_MIN_HUFFMAN_BITS + (int)webp_bits_read(br, VP8L_NUM_HUFFMAN_BITS);
        entropy_w = SUB_SAMPLE(width, entropy_bits);
        entropy_h = SUB_SAMPLE(height, entropy_bits);
        entropy = (uint32_t*)malloc((size_t)entropy_w * entropy_h * sizeof(*entropy));
        if (entropy == NULL)
            return 0;
        if (!decode_image_data(br, entropy_w, entropy_h, 0, entropy)) {
            free(entropy);
            return 0;
        }
        num_groups_max = 0;
        for (i = 0; i < entropy_w * entropy_h; ++i) {
            const int group = (int)((entropy[i] >> 8) & 0xffff);
            entropy[i] = (uint32_t)group;
            if (group + 1 > num_groups_max)
                num_groups_max = group + 1;
        }
        if (num_groups_max > VP8L_INLINE_GROUP_LIMIT ||
            num_groups_max > entropy_w * entropy_h) {
            /* remap the used indices onto a dense range */
            mapping = (int*)malloc((size_t)num_groups_max * sizeof(*mapping));
            if (mapping == NULL) {
                free(entropy);
                return 0;
            }
            for (i = 0; i < num_groups_max; ++i)
                mapping[i] = -1;
            num_groups = 0;
            for (i = 0; i < entropy_w * entropy_h; ++i) {
                int* slot = &mapping[entropy[i]];
                if (*slot == -1)
                    *slot = num_groups++;
                entropy[i] = (uint32_t)*slot;
            }
        } else {
            num_groups = num_groups_max;
        }
    }
    if (br->eof)
        goto end;

    groups = (WebpHuff*)calloc((size_t)num_groups * VP8L_NUM_HUFF_CODES,
                               sizeof(*groups));
    if (groups == NULL)
        goto end;
    for (i = 0; i < VP8L_NUM_HUFF_CODES; ++i)
        webp_huff_init(&scratch[i]);

    /* Every group index below the maximum is described in the stream, even the
     * unused ones; those are read and thrown away. */
    for (i = 0; i < num_groups_max; ++i) {
        const int slot = (mapping != NULL) ? mapping[i] : i;
        if (slot < 0) {
            if (!read_huff_group(br, scratch, cache_size))
                goto end;
            for (j = 0; j < VP8L_NUM_HUFF_CODES; ++j)
                webp_huff_free(&scratch[j]);
            continue;
        }
        if (!read_huff_group(br, &groups[slot * VP8L_NUM_HUFF_CODES], cache_size))
            goto end;
    }
    if (br->eof)
        goto end;

    if (cache_size > 0) {
        cache = (uint32_t*)calloc((size_t)cache_size, sizeof(*cache));
        if (cache == NULL)
            goto end;
    }

    /* lz77-coded-image */
    while (y < height) {
        const WebpHuff* codes =
            &groups[huff_group_at(entropy, entropy_bits, entropy_w, x, y) *
                    VP8L_NUM_HUFF_CODES];
        const int code = webp_huff_read(&codes[VP8L_HUFF_GREEN], br);
        if (code < 0)
            goto end;

        if (code < VP8L_NUM_LITERAL_CODES) {
            const int r = webp_huff_read(&codes[VP8L_HUFF_RED], br);
            const int b = webp_huff_read(&codes[VP8L_HUFF_BLUE], br);
            const int a = webp_huff_read(&codes[VP8L_HUFF_ALPHA], br);
            uint32_t argb;
            if (r < 0 || b < 0 || a < 0)
                goto end;
            argb = WEBP_ARGB(a, r, code, b);
            pixels[y * width + x] = argb;
            if (cache != NULL)
                cache_insert(cache, cache_bits, argb);
            ++x;
        } else if (code < VP8L_NUM_LITERAL_CODES + VP8L_NUM_LENGTH_CODES) {
            const int pos = y * width + x;
            const int length = prefix_expand(code - VP8L_NUM_LITERAL_CODES, br);
            const int dist_sym = webp_huff_read(&codes[VP8L_HUFF_DIST], br);
            int dist;
            if (dist_sym < 0)
                goto end;
            dist = plane_code_to_distance(width, prefix_expand(dist_sym, br));
            if (pos < dist || width * height - pos < length)
                goto end;
            for (i = 0; i < length; ++i) {
                const uint32_t argb = pixels[pos - dist + i];
                pixels[pos + i] = argb;
                if (cache != NULL)
                    cache_insert(cache, cache_bits, argb);
            }
            x += length;
        } else if (cache != NULL &&
                   code < VP8L_NUM_LITERAL_CODES + VP8L_NUM_LENGTH_CODES +
                          cache_size) {
            const uint32_t argb =
                cache[code - VP8L_NUM_LITERAL_CODES - VP8L_NUM_LENGTH_CODES];
            pixels[y * width + x] = argb;
            cache_insert(cache, cache_bits, argb);
            ++x;
        } else {
            goto end;   /* no cache present, or index out of range */
        }
        while (x >= width) {
            x -= width;
            ++y;
        }
    }
    ok = !br->eof;

end:
    free(cache);
    free(entropy);
    free(mapping);
    if (groups != NULL) {
        for (i = 0; i < num_groups * VP8L_NUM_HUFF_CODES; ++i)
            webp_huff_free(&groups[i]);
        free(groups);
    }
    return ok;
}

/* ------------------------------------------------------------------------ */
/* transforms: reading                                                      */
/* ------------------------------------------------------------------------ */

static int read_transform(WebpBits* br, WebpTransform* tr, int* width,
                          int height, unsigned* seen) {
    const int type = (int)webp_bits_read(br, 2);

    tr->type = type;
    tr->bits = 0;
    tr->xsize = *width;
    tr->ysize = height;
    tr->data_w = 0;
    tr->data_h = 0;
    tr->data = NULL;

    /* each transform type may only appear once */
    if (*seen & (1u << type))
        return 0;
    *seen |= 1u << type;

    if (type == VP8L_TR_PREDICTOR || type == VP8L_TR_COLOR) {
        tr->bits = VP8L_MIN_TRANSFORM_BITS +
                   (int)webp_bits_read(br, VP8L_NUM_TRANSFORM_BITS);
        tr->data_w = SUB_SAMPLE(tr->xsize, tr->bits);
        tr->data_h = SUB_SAMPLE(tr->ysize, tr->bits);
        tr->data = (uint32_t*)malloc((size_t)tr->data_w * tr->data_h *
                                     sizeof(*tr->data));
        if (tr->data == NULL)
            return -1;
        if (!decode_image_data(br, tr->data_w, tr->data_h, 0, tr->data))
            return 0;
    } else if (type == VP8L_TR_COLOR_INDEXING) {
        const int num_colors = (int)webp_bits_read(br, 8) + 1;
        const int bits = (num_colors > 16) ? 0 :
                         (num_colors > 4)  ? 1 :
                         (num_colors > 2)  ? 2 : 3;
        uint8_t* bytes;
        int i;

        tr->bits = bits;
        tr->data_w = num_colors;
        tr->data_h = 1;
        /* The palette is expanded to every index the packing can produce, so
         * an out of range index reads the black tail instead of running off
         * the end of the table. */
        tr->data = (uint32_t*)calloc((size_t)(1 << (8 >> bits)),
                                     sizeof(*tr->data));
        if (tr->data == NULL)
            return -1;
        if (!decode_image_data(br, num_colors, 1, 0, tr->data))
            return 0;
        /* the palette is stored subtraction coded: accumulate it */
        bytes = (uint8_t*)tr->data;
        for (i = 4; i < 4 * num_colors; ++i)
            bytes[i] = (uint8_t)(bytes[i] + bytes[i - 4]);
        /* the remaining tail stays zero (transparent black) */
    } else if (type != VP8L_TR_SUBTRACT_GREEN) {
        return 0;
    }

    /* only color indexing changes the geometry of what follows */
    if (type == VP8L_TR_COLOR_INDEXING)
        *width = SUB_SAMPLE(tr->xsize, tr->bits);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* transforms: inverse                                                      */
/* ------------------------------------------------------------------------ */

static void inverse_predictor(const WebpTransform* tr, uint32_t* px) {
    const int width = tr->xsize;
    const int height = tr->ysize;
    const int tiles_per_row = tr->data_w;
    int x, y;

    /* first pixel is predicted from solid black, the rest of row 0 from L */
    px[0] = webp_add_pixels(px[0], 0xff000000u);
    for (x = 1; x < width; ++x)
        px[x] = webp_add_pixels(px[x], px[x - 1]);

    for (y = 1; y < height; ++y) {
        uint32_t* row = px + (size_t)y * width;
        const uint32_t* top = row - width;
        const uint32_t* modes = tr->data + (size_t)(y >> tr->bits) * tiles_per_row;

        /* leftmost column follows T */
        row[0] = webp_add_pixels(row[0], top[0]);
        for (x = 1; x < width; ++x) {
            int mode = (int)((modes[x >> tr->bits] >> 8) & 0xf);
            uint32_t pred;
            if (mode > 13)
                mode = 0;
            /* On the rightmost column TR wraps to the first pixel of the same
             * row, which has already been reconstructed. */
            pred = predict(mode, row[x - 1], top[x], top[x - 1],
                           (x == width - 1) ? row[0] : top[x + 1]);
            row[x] = webp_add_pixels(row[x], pred);
        }
    }
}

static void inverse_color(const WebpTransform* tr, uint32_t* px) {
    const int width = tr->xsize;
    const int height = tr->ysize;
    const int tiles_per_row = tr->data_w;
    int x, y;

    for (y = 0; y < height; ++y) {
        uint32_t* row = px + (size_t)y * width;
        const uint32_t* elems = tr->data + (size_t)(y >> tr->bits) * tiles_per_row;
        for (x = 0; x < width; ++x) {
            const uint32_t argb = row[x];
            const uint32_t elem = elems[x >> tr->bits];
            /* the element stores red_to_blue in R, green_to_blue in G and
             * green_to_red in B */
            const int green_to_red = (int)(elem & 0xff);
            const int green_to_blue = (int)((elem >> 8) & 0xff);
            const int red_to_blue = (int)((elem >> 16) & 0xff);
            const int green = (int)((argb >> 8) & 0xff);
            int red = (int)((argb >> 16) & 0xff);
            int blue = (int)(argb & 0xff);

            red = (red + color_delta(green_to_red, green)) & 0xff;
            blue = (blue + color_delta(green_to_blue, green) +
                    color_delta(red_to_blue, (int)(int8_t)red)) & 0xff;
            row[x] = (argb & 0xff00ff00u) | ((uint32_t)red << 16) | (uint32_t)blue;
        }
    }
}

static void inverse_subtract_green(uint32_t* px, int num_pixels) {
    int i;
    for (i = 0; i < num_pixels; ++i) {
        const uint32_t argb = px[i];
        const uint32_t green = (argb >> 8) & 0xff;
        uint32_t red_blue = argb & 0x00ff00ffu;
        red_blue += (green << 16) | green;
        px[i] = (argb & 0xff00ff00u) | (red_blue & 0x00ff00ffu);
    }
}

/* Expands a pixel bundled, paletted image back to full width. *pxp is
 * replaced by a freshly allocated buffer on success. */
static int inverse_color_indexing(const WebpTransform* tr, uint32_t** pxp) {
    const int width = tr->xsize;
    const int height = tr->ysize;
    const int bits_per_pixel = 8 >> tr->bits;
    const int packed_w = SUB_SAMPLE(width, tr->bits);
    const uint32_t bit_mask = (uint32_t)((1 << bits_per_pixel) - 1);
    const uint32_t* src = *pxp;
    uint32_t* dst;
    int x, y;

    dst = (uint32_t*)malloc((size_t)width * height * sizeof(*dst));
    if (dst == NULL)
        return WEBP_ERR_OOM;

    for (y = 0; y < height; ++y) {
        const uint32_t* srow = src + (size_t)y * packed_w;
        uint32_t* drow = dst + (size_t)y * width;
        for (x = 0; x < width; ++x) {
            const uint32_t packed = WEBP_G(srow[x >> tr->bits]);
            const int shift = (x & ((1 << tr->bits) - 1)) * bits_per_pixel;
            drow[x] = tr->data[(packed >> shift) & bit_mask];
        }
    }
    free(*pxp);
    *pxp = dst;
    return WEBP_OK;
}

/* ------------------------------------------------------------------------ */
/* public entry point                                                       */
/* ------------------------------------------------------------------------ */

int webp_vp8l_decode(const uint8_t* data, uint32_t size,
                     int known_width, int known_height,
                     uint32_t** out_pixels, int* out_width, int* out_height) {
    WebpTransform transforms[VP8L_NUM_TRANSFORMS];
    WebpBits br;
    unsigned seen = 0;
    int num_transforms = 0;
    int width = 0, height = 0;
    uint32_t* pixels = NULL;
    int i, ret = WEBP_OK;

    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;

    if (known_width > 0 && known_height > 0) {
        /* headerless image-stream, as found inside an 'ALPH' chunk */
        width = known_width;
        height = known_height;
        webp_bits_init(&br, data, size);
    } else {
        webp_bits_init(&br, data, size);
        if (size < 5 || data[0] != VP8L_MAGIC_BYTE)
            return WEBP_ERR_CORRUPT;
        webp_bits_init(&br, data + 1, size - 1);
        width = (int)webp_bits_read(&br, VP8L_IMAGE_SIZE_BITS) + 1;
        height = (int)webp_bits_read(&br, VP8L_IMAGE_SIZE_BITS) + 1;
        (void)webp_bits_read(&br, 1);   /* alpha_is_used: a hint only */
        if (webp_bits_read(&br, VP8L_VERSION_BITS) != 0 || br.eof)
            return WEBP_ERR_CORRUPT;
    }

    if (width > WEBP_MAX_DIM || height > WEBP_MAX_DIM ||
        (uint64_t)width * height > WEBP_MAX_PIXELS)
        return WEBP_ERR_TOO_LARGE;

    memset(transforms, 0, sizeof(transforms));

    /* optional-transform */
    while (webp_bits_read(&br, 1)) {
        int res;
        if (num_transforms >= VP8L_NUM_TRANSFORMS) {
            ret = WEBP_ERR_CORRUPT;
            goto end;
        }
        res = read_transform(&br, &transforms[num_transforms], &width, height,
                             &seen);
        if (res < 0) {
            ret = WEBP_ERR_OOM;
            goto end;
        }
        if (res == 0 || br.eof) {
            ret = WEBP_ERR_CORRUPT;
            goto end;
        }
        ++num_transforms;
    }

    pixels = (uint32_t*)malloc((size_t)width * height * sizeof(*pixels));
    if (pixels == NULL) {
        ret = WEBP_ERR_OOM;
        goto end;
    }
    if (!decode_image_data(&br, width, height, 1, pixels))
        ret = WEBP_ERR_CORRUPT;

    /* inverse transforms run back to front, each one restoring the geometry
     * that was in effect when it was read */
    for (i = num_transforms - 1; i >= 0 && ret == WEBP_OK; --i) {
        const WebpTransform* tr = &transforms[i];
        switch (tr->type) {
        case VP8L_TR_PREDICTOR:
            inverse_predictor(tr, pixels);
            break;
        case VP8L_TR_COLOR:
            inverse_color(tr, pixels);
            break;
        case VP8L_TR_SUBTRACT_GREEN:
            inverse_subtract_green(pixels, width * height);
            break;
        case VP8L_TR_COLOR_INDEXING:
            ret = inverse_color_indexing(tr, &pixels);
            width = tr->xsize;
            break;
        default:
            ret = WEBP_ERR_CORRUPT;
            break;
        }
    }

    if (ret != WEBP_OK)
        goto end;
    *out_pixels = pixels;
    *out_width = width;
    *out_height = height;
    pixels = NULL;

end:
    free(pixels);
    for (i = 0; i < VP8L_NUM_TRANSFORMS; ++i)
        free(transforms[i].data);
    return ret;
}
