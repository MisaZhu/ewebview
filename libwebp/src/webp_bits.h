/*
 * webp_bits.h - LSB-first bit reader for the VP8L (lossless) bitstream.
 *
 * The lossless format reads bits from bytes in least-significant-bit-first
 * order and builds multi-bit integers with the first bit read as the LSB, so
 * a plain shift-in-from-the-top accumulator is exactly what is needed.
 *
 * Reads past the end of the buffer are zero filled and flagged in ::eof,
 * which keeps a truncated stream from ever dereferencing out of bounds; the
 * caller turns the flag into WEBP_ERR_CORRUPT.
 */
#ifndef WEBP_BITS_H
#define WEBP_BITS_H

#include <stdint.h>

/* Largest single read used by the decoder: 18 LZ77 extra bits. Huffman
 * lookups peek at most 15 bits (8 root + 7 sub). */
#define WEBP_BITS_MAX_READ 24

typedef struct {
    const uint8_t* data;
    uint32_t size;
    uint32_t pos;    /* index of the next byte not yet pulled into buf */
    uint64_t buf;    /* unconsumed bits; bit 0 is the next bit to read */
    int      nbits;  /* number of valid bits currently held in buf     */
    int      eof;    /* set once a read could not be fully satisfied   */
} WebpBits;

static inline void webp_bits_init(WebpBits* br, const uint8_t* data,
                                  uint32_t size) {
    br->data = data;
    br->size = size;
    br->pos = 0;
    br->buf = 0;
    br->nbits = 0;
    br->eof = 0;
}

/* Pulls whole bytes in until buf holds more than 56 bits or input runs out. */
static inline void webp_bits_fill(WebpBits* br) {
    while (br->nbits <= 56 && br->pos < br->size) {
        br->buf |= (uint64_t)br->data[br->pos++] << br->nbits;
        br->nbits += 8;
    }
}

/* Returns the next n bits (n <= WEBP_BITS_MAX_READ) without consuming them. */
static inline uint32_t webp_bits_peek(WebpBits* br, int n) {
    if (br->nbits < n) {
        webp_bits_fill(br);
        if (br->nbits < n)
            br->eof = 1;
    }
    return (uint32_t)(br->buf & ((1ull << n) - 1));
}

/* Drops the next n bits. */
static inline void webp_bits_skip(WebpBits* br, int n) {
    if (br->nbits >= n) {
        br->buf >>= n;
        br->nbits -= n;
    } else {
        br->buf = 0;
        br->nbits = 0;
        br->eof = 1;
    }
}

/* Returns and consumes the next n bits. */
static inline uint32_t webp_bits_read(WebpBits* br, int n) {
    const uint32_t v = webp_bits_peek(br, n);
    webp_bits_skip(br, n);
    return v;
}

#endif /* WEBP_BITS_H */
