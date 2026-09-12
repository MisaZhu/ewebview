/*
 * webp_huff.h - canonical prefix code tables for the VP8L bitstream.
 */
#ifndef WEBP_HUFF_H
#define WEBP_HUFF_H

#include "webp_bits.h"

#define WEBP_HUFF_MAX_LEN    15  /* longest code the format allows */
#define WEBP_HUFF_ROOT_BITS   8  /* first level lookup width       */

typedef struct {
    uint32_t* nodes;    /* flat two level lookup; NULL when single is set */
    int       root_bits;
    int       single;   /* one symbol only: decoding consumes no bits */
    int       empty;    /* every length was zero: no symbol exists      */
    uint32_t  sym;      /* the symbol of a single leaf tree */
} WebpHuff;

void webp_huff_init(WebpHuff* h);
void webp_huff_free(WebpHuff* h);

/* Builds the lookup tables from canonical code lengths (0 = unused symbol).
 * Returns 1 on success, 0 if the lengths do not describe a valid tree. */
int  webp_huff_build(WebpHuff* h, const uint8_t* lengths, int alphabet_size);

/* Reads the next symbol. */
int  webp_huff_read(const WebpHuff* h, WebpBits* br);

#endif /* WEBP_HUFF_H */
