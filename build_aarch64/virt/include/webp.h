/*
 * webp.h - WebP still-image decoding for EwokOS.
 *
 * Supports both WebP compression modes for *static* (non-animated) images:
 *   - lossless (VP8L bitstream), including the four optional transforms,
 *     meta prefix codes, LZ77 backward references and the color cache;
 *   - lossy (VP8 bitstream). A standalone still image is always a VP8 key
 *     frame, so inter prediction / motion vectors / reference frame
 *     management are not reachable and are reported as unsupported.
 * Extended files with an 'ALPH' chunk get their alpha plane decoded
 * (VP8L-compressed or raw) and merged into the color image.
 *
 * The decoder is plain C99 with no dependency on the EwokOS SDK so it can
 * also be built and tested on a host machine.
 */
#ifndef WEBP_H
#define WEBP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* webp_decode() result codes; WEBP_OK (0) means success. */
#define WEBP_OK               0
#define WEBP_ERR_PARAM       (-1)  /* NULL argument / empty buffer        */
#define WEBP_ERR_NOT_WEBP    (-2)  /* not a RIFF....WEBP container       */
#define WEBP_ERR_UNSUPPORTED (-3)  /* lossy-only stream, animation, ...  */
#define WEBP_ERR_CORRUPT     (-4)  /* malformed or truncated bitstream   */
#define WEBP_ERR_OOM         (-5)  /* allocation failure                 */
#define WEBP_ERR_TOO_LARGE   (-6)  /* dimensions above the safety limit  */

typedef struct {
    int       width;
    int       height;
    uint32_t* pixels;  /* ARGB: a=31..24 r=23..16 g=15..8 b=7..0 */
} webp_image_t;

/* Returns 1 when data starts with a RIFF/WEBP container header. */
int webp_is_webp(const uint8_t* data, uint32_t size);

/* Decodes a WebP image held in memory. On WEBP_OK, *out owns a pixel buffer
 * that must be released with webp_image_free(); on any error *out is cleared
 * and nothing needs to be freed. */
int webp_decode(const uint8_t* data, uint32_t size, webp_image_t* out);

/* Releases the pixel buffer of a successfully decoded image. */
void webp_image_free(webp_image_t* img);

/* Human readable name for a WEBP_* result code (never returns NULL). */
const char* webp_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif /* WEBP_H */
