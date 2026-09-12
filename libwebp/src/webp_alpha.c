/*
 * webp_alpha.c - the 'ALPH' chunk of an extended WebP file.
 *
 * A lossy frame keeps its transparency in a separate alpha plane. The plane
 * is either raw scan order bytes or a VP8L image-stream of implicit
 * dimensions, and in both cases it may be pre-filtered; the filter is undone
 * after decompression.
 */
#include "webp_internal.h"

#define ALPHA_METHOD_NONE      0
#define ALPHA_METHOD_LOSSLESS  1

#define ALPHA_FILTER_NONE       0
#define ALPHA_FILTER_HORIZONTAL 1
#define ALPHA_FILTER_VERTICAL   2
#define ALPHA_FILTER_GRADIENT   3

static int clip255(int v) {
    return (v < 0) ? 0 : ((v > 255) ? 255 : v);
}

/* Undoes the alpha pre-filter in place. The neighbours A (left), B (above)
 * and C (above left) are the already unfiltered values, and the first row and
 * column borrow the predictor of the other axis, exactly as the container
 * specification describes. */
static void alpha_unfilter(uint8_t* alpha, int width, int height, int filter) {
    int x, y;

    if (filter == ALPHA_FILTER_NONE)
        return;

    for (y = 0; y < height; ++y) {
        uint8_t* row = alpha + (size_t)y * width;
        const uint8_t* top = row - width;
        for (x = 0; x < width; ++x) {
            const int a = (x > 0) ? row[x - 1] : 0;
            const int b = (y > 0) ? top[x] : 0;
            const int c = (x > 0 && y > 0) ? top[x - 1] : 0;
            int pred;

            if (x == 0 && y == 0) {
                pred = 0;
            } else if (x == 0) {
                /* every method predicts from above on the leftmost column */
                pred = b;
            } else if (y == 0) {
                /* horizontal filtering has no A here, the other two borrow it
                 * from the left as the specification requires */
                pred = (filter == ALPHA_FILTER_HORIZONTAL) ? 0 : a;
            } else {
                switch (filter) {
                case ALPHA_FILTER_HORIZONTAL: pred = a; break;
                case ALPHA_FILTER_VERTICAL:   pred = b; break;
                default:                      pred = clip255(a + b - c); break;
                }
            }
            row[x] = (uint8_t)((pred + row[x]) & 0xff);
        }
    }
}

int webp_alpha_decode(const uint8_t* payload, uint32_t payload_size,
                      int width, int height, uint8_t** out_alpha) {
    const int filter = (int)((payload[0] >> 2) & 0x03);
    const int method = (int)(payload[0] & 0x03);
    const uint8_t* data = payload + 1;
    const uint32_t data_size = payload_size - 1;
    const uint32_t num_pixels = (uint32_t)width * (uint32_t)height;
    uint8_t* alpha = NULL;
    int ret = WEBP_ERR_CORRUPT;

    *out_alpha = NULL;
    if (width <= 0 || height <= 0 || payload_size < 1)
        return WEBP_ERR_PARAM;

    if (method == ALPHA_METHOD_LOSSLESS) {
        /* the stream carries no dimensions of its own */
        uint32_t* argb = NULL;
        int w = 0, h = 0;
        ret = webp_vp8l_decode(data, data_size, width, height, &argb, &w, &h);
        if (ret != WEBP_OK)
            return ret;
        alpha = (uint8_t*)malloc(num_pixels);
        if (alpha == NULL) {
            free(argb);
            return WEBP_ERR_OOM;
        }
        /* only the green channel carries the transparency */
        {
            uint32_t i;
            for (i = 0; i < num_pixels; ++i)
                alpha[i] = (uint8_t)WEBP_G(argb[i]);
        }
        free(argb);
        ret = WEBP_OK;
    } else if (method == ALPHA_METHOD_NONE) {
        if (data_size < num_pixels)
            return WEBP_ERR_CORRUPT;
        alpha = (uint8_t*)malloc(num_pixels);
        if (alpha == NULL)
            return WEBP_ERR_OOM;
        memcpy(alpha, data, num_pixels);
        ret = WEBP_OK;
    } else {
        return WEBP_ERR_UNSUPPORTED;
    }

    alpha_unfilter(alpha, width, height, filter);
    *out_alpha = alpha;
    return ret;
}
