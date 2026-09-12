/*
 * webp_internal.h - declarations shared between the libwebp translation
 * units. Not installed; internal use only.
 */
#ifndef WEBP_INTERNAL_H
#define WEBP_INTERNAL_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "webp.h"

/* Safety limits. The format itself allows 16384x16384 for VP8L and
 * 16383x16383 for VP8, but an embedded target cannot hold such a canvas,
 * so reject anything absurd instead of exhausting memory. */
#define WEBP_MAX_DIM        8192
#define WEBP_MAX_PIXELS     (32u * 1024u * 1024u)

/* Byte-wise little endian loads. The aarch64 build uses -mstrict-align,
 * so casting an unaligned uint8_t* to uint32_t* is not an option. */
static inline uint32_t webp_get_le24(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static inline uint32_t webp_get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ARGB channel access, using the layout mandated by the WebP lossless spec:
 * alpha in bits 31..24, red 23..16, green 15..8, blue 7..0. This is also the
 * layout of graph_t::buffer, so decoded pixels can be blitted directly. */
#define WEBP_ARGB(a, r, g, b) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
     ((uint32_t)(g) << 8) | (uint32_t)(b))
#define WEBP_A(p)  ((int)(((p) >> 24) & 0xff))
#define WEBP_R(p)  ((int)(((p) >> 16) & 0xff))
#define WEBP_G(p)  ((int)(((p) >> 8) & 0xff))
#define WEBP_B(p)  ((int)((p) & 0xff))

/* Per-channel modulo-256 addition of two ARGB pixels. */
static inline uint32_t webp_add_pixels(uint32_t a, uint32_t b) {
    const uint32_t alpha_green = (a & 0xff00ff00u) + (b & 0xff00ff00u);
    const uint32_t red_blue = (a & 0x00ff00ffu) + (b & 0x00ff00ffu);
    return (alpha_green & 0xff00ff00u) | (red_blue & 0x00ff00ffu);
}

/* webp_vp8l.c - lossless decoder */

/* Decodes a VP8L bitstream. When known_width/known_height are > 0 the stream
 * is treated as a headerless "image-stream" (the form used inside an 'ALPH'
 * chunk): no 0x2f signature, no dimensions, no version bits, and the
 * transform loop starts immediately. */
int webp_vp8l_decode(const uint8_t* data, uint32_t size,
                     int known_width, int known_height,
                     uint32_t** out_pixels, int* out_width, int* out_height);

/* webp_alpha.c - 'ALPH' chunk */

/* Decodes the alpha plane of an 'ALPH' chunk payload into a freshly
 * allocated width*height byte buffer (caller frees). */
int webp_alpha_decode(const uint8_t* payload, uint32_t payload_size,
                      int width, int height, uint8_t** out_alpha);

/* webp_vp8.c - lossy decoder */

/* Decodes a 'VP8 ' chunk payload into a freshly allocated ARGB buffer. */
int webp_vp8_decode(const uint8_t* data, uint32_t size,
                    uint32_t** out_pixels, int* out_width, int* out_height);

#endif /* WEBP_INTERNAL_H */
