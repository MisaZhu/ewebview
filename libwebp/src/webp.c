/*
 * webp.c - RIFF container walk and public entry points.
 *
 * Layouts handled:
 *   simple lossy     RIFF/WEBP + 'VP8 '
 *   simple lossless  RIFF/WEBP + 'VP8L'
 *   extended         RIFF/WEBP + 'VP8X' [+ 'ALPH'] + ('VP8 ' | 'VP8L')
 * Animated files ('ANIM'/'ANMF') are rejected: this is a still image decoder.
 */
#include "webp_internal.h"

int webp_is_webp(const uint8_t* data, uint32_t size) {
    return (data != NULL && size >= 12 &&
            memcmp(data, "RIFF", 4) == 0 &&
            memcmp(data + 8, "WEBP", 4) == 0);
}

int webp_decode(const uint8_t* data, uint32_t size, webp_image_t* out) {
    const uint8_t* alph = NULL;
    const uint8_t* vp8 = NULL;
    const uint8_t* vp8l = NULL;
    uint32_t alph_size = 0, vp8_size = 0, vp8l_size = 0;
    uint32_t offset, end;
    uint32_t* pixels = NULL;
    uint32_t i;
    int width = 0, height = 0;
    int animated = 0;
    int ret;

    if (out == NULL)
        return WEBP_ERR_PARAM;
    out->width = 0;
    out->height = 0;
    out->pixels = NULL;
    if (!webp_is_webp(data, size))
        return WEBP_ERR_NOT_WEBP;

    /* the RIFF size field says where the chunks end, but a truncated download
     * must never be trusted beyond the bytes that actually arrived */
    end = 8u + webp_get_le32(data + 4);
    if (end > size)
        end = size;
    if (end < 12)
        return WEBP_ERR_CORRUPT;

    for (offset = 12; offset + 8 <= end; ) {
        const uint8_t* tag = data + offset;
        const uint8_t* payload = tag + 8;
        uint32_t chunk_size = webp_get_le32(tag + 4);
        const uint32_t avail = end - (offset + 8);

        if (chunk_size > avail)
            chunk_size = avail;

        if (memcmp(tag, "VP8X", 4) == 0) {
            if (chunk_size >= 10) {
                /* bit layout of the flags byte, MSB first:
                 * Rsv Rsv I L E X A R. The canvas dimensions it also carries
                 * are redundant here: a still image frame always covers the
                 * whole canvas and the bitstream repeats the size. */
                animated = (int)((payload[0] >> 1) & 1u);
            }
        } else if (memcmp(tag, "ANMF", 4) == 0 ||
                   memcmp(tag, "ANIM", 4) == 0) {
            animated = 1;
        } else if (memcmp(tag, "ALPH", 4) == 0) {
            if (alph == NULL && chunk_size >= 1) {
                alph = payload;
                alph_size = chunk_size;
            }
        } else if (memcmp(tag, "VP8 ", 4) == 0) {
            if (vp8 == NULL) {
                vp8 = payload;
                vp8_size = chunk_size;
            }
        } else if (memcmp(tag, "VP8L", 4) == 0) {
            if (vp8l == NULL) {
                vp8l = payload;
                vp8l_size = chunk_size;
            }
        }
        /* chunks are word aligned: an odd payload carries a padding byte */
        offset += 8 + chunk_size + (chunk_size & 1u);
    }

    if (animated)
        return WEBP_ERR_UNSUPPORTED;

    if (vp8l != NULL) {
        ret = webp_vp8l_decode(vp8l, vp8l_size, 0, 0, &pixels, &width, &height);
    } else if (vp8 != NULL) {
        ret = webp_vp8_decode(vp8, vp8_size, &pixels, &width, &height);
    } else {
        return WEBP_ERR_CORRUPT;   /* no bitstream chunk at all */
    }
    if (ret != WEBP_OK)
        return ret;

    /* A 'VP8L' frame already holds its alpha and should not be paired with an
     * 'ALPH' chunk; only a lossy frame needs the plane merged in. */
    if (alph != NULL && vp8l == NULL) {
        uint8_t* alpha = NULL;
        const uint32_t num_pixels = (uint32_t)width * (uint32_t)height;
        ret = webp_alpha_decode(alph, alph_size, width, height, &alpha);
        if (ret != WEBP_OK) {
            free(pixels);
            return ret;
        }
        for (i = 0; i < num_pixels; ++i)
            pixels[i] = ((uint32_t)alpha[i] << 24) | (pixels[i] & 0x00ffffffu);
        free(alpha);
    } else {
        /* no alpha plane: the lossy output is fully opaque */
        const uint32_t num_pixels = (uint32_t)width * (uint32_t)height;
        if (vp8l == NULL) {
            for (i = 0; i < num_pixels; ++i)
                pixels[i] |= 0xff000000u;
        }
    }

    out->pixels = pixels;
    out->width = width;
    out->height = height;
    return WEBP_OK;
}

void webp_image_free(webp_image_t* img) {
    if (img == NULL)
        return;
    free(img->pixels);
    img->pixels = NULL;
    img->width = 0;
    img->height = 0;
}

const char* webp_strerror(int code) {
    switch (code) {
    case WEBP_OK:               return "ok";
    case WEBP_ERR_PARAM:        return "invalid argument";
    case WEBP_ERR_NOT_WEBP:     return "not a WebP file";
    case WEBP_ERR_UNSUPPORTED:  return "unsupported WebP feature";
    case WEBP_ERR_CORRUPT:      return "corrupt WebP bitstream";
    case WEBP_ERR_OOM:          return "out of memory";
    case WEBP_ERR_TOO_LARGE:    return "image too large";
    default:                    return "unknown error";
    }
}
