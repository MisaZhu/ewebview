/*
 * webp_huff.c - canonical prefix code tables for the VP8L bitstream.
 *
 * Codes are transmitted as lengths only, so the decoder rebuilds the
 * canonical code (shortest first, symbols ascending) and stores it bit
 * reversed: the bit reader hands out the first transmitted bit as bit 0, so a
 * reversed code can be compared straight against the peeked value.
 *
 * Lookup is two level. An 8 bit root table covers every code of length <= 8
 * in a single indexed load; longer codes are grouped by their low 8 reversed
 * bits into sub tables of at most 2^7 entries, which keeps memory bounded
 * (the format allows up to 5 * num_groups codes per image).
 *
 * Node packing, 32 bits:
 *   direct hit : (code_len << 28) | symbol     code_len in 1..15
 *   sub table  : (sub_bits << 24) | sub_offset sub_bits in 1..7
 * A sub table marker is recognised by (node >> 28) == 0, which is safe
 * because sub_bits never reaches bit 28. An all zero node means the slot was
 * never filled, which can only happen for an under-subscribed (non conformant
 * but tolerated) code; reading it reports a corrupt stream instead of
 * silently returning a wrong symbol.
 */
#include "webp_huff.h"

#include <stdlib.h>
#include <string.h>

#define HUFF_LEN(node)       ((int)((node) >> 28))
#define HUFF_SYM(node)       ((uint32_t)((node) & 0x0fffffffu))
#define HUFF_SUB_BITS(node)  ((int)(((node) >> 24) & 0xfu))
#define HUFF_SUB_OFF(node)   ((uint32_t)((node) & 0x00ffffffu))
#define HUFF_MAX_NODES       0x00ffffffu

static uint32_t reverse_bits(uint32_t v, int len) {
    uint32_t r = 0;
    int i;
    for (i = 0; i < len; ++i) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

void webp_huff_init(WebpHuff* h) {
    memset(h, 0, sizeof(*h));
}

void webp_huff_free(WebpHuff* h) {
    free(h->nodes);
    h->nodes = NULL;
    h->root_bits = 0;
}

int webp_huff_build(WebpHuff* h, const uint8_t* lengths, int alphabet_size) {
    int counts[WEBP_HUFF_MAX_LEN + 1];
    int next_code[WEBP_HUFF_MAX_LEN + 2];
    uint32_t* rcodes = NULL;
    uint8_t* sub_bits = NULL;
    uint32_t* sub_off = NULL;
    int max_len = 0;
    int non_zero = 0;
    int last_sym = 0;
    int root_bits, root_size, sub_total = 0;
    int sym, len, i, code;
    uint32_t total = 0;
    int ok = 0;

    if (alphabet_size <= 0)
        return 0;

    memset(counts, 0, sizeof(counts));
    for (sym = 0; sym < alphabet_size; ++sym) {
        len = lengths[sym];
        if (len > 0) {
            if (len > WEBP_HUFF_MAX_LEN)
                return 0;
            counts[len]++;
            if (len > max_len)
                max_len = len;
            non_zero++;
            last_sym = sym;
        }
    }
    if (non_zero == 0) {
        /* Empty code. The format says such a code is transmitted as a single
         * symbol 0, but a stream of all zero lengths is harmless as long as
         * nothing ever tries to read from it: mark it and fail on use. */
        h->empty = 1;
        return 1;
    }
    if (non_zero == 1) {
        /* Single leaf tree. The format marks it with length 1 but no bits are
         * consumed when it is used, so it degenerates to a constant symbol. */
        h->single = 1;
        h->sym = (uint32_t)last_sym;
        return 1;
    }

    /* The described tree must not be over-subscribed. An under-subscribed one
     * is rejected by the reference encoder but tolerated here: the unfilled
     * slots decode to "invalid" rather than being papered over. */
    for (len = 1; len <= max_len; ++len)
        total += (uint32_t)counts[len] << (WEBP_HUFF_MAX_LEN - len);
    if (total > (1u << WEBP_HUFF_MAX_LEN))
        return 0;

    rcodes = (uint32_t*)malloc((size_t)alphabet_size * sizeof(*rcodes));
    if (rcodes == NULL)
        goto end;

    /* Canonical assignment. */
    code = 0;
    for (len = 1; len <= max_len; ++len) {
        code = (code + counts[len - 1]) << 1;  /* counts[0] is always 0 */
        next_code[len] = code;
    }
    for (sym = 0; sym < alphabet_size; ++sym) {
        len = lengths[sym];
        if (len > 0) {
            uint32_t c = (uint32_t)next_code[len]++;
            if ((int)(c >> len) != 0)
                goto end;  /* code overflowed its length: inconsistent input */
            rcodes[sym] = reverse_bits(c, len);
        } else {
            rcodes[sym] = 0;
        }
    }

    root_bits = (max_len <= WEBP_HUFF_ROOT_BITS) ? max_len : WEBP_HUFF_ROOT_BITS;
    root_size = 1 << root_bits;

    sub_bits = (uint8_t*)calloc((size_t)root_size, sizeof(*sub_bits));
    sub_off = (uint32_t*)malloc((size_t)root_size * sizeof(*sub_off));
    if (sub_bits == NULL || sub_off == NULL)
        goto end;
    memset(sub_off, 0, (size_t)root_size * sizeof(*sub_off));

    /* Widest sub table needed per root entry. */
    for (sym = 0; sym < alphabet_size; ++sym) {
        len = lengths[sym];
        if (len > root_bits) {
            const uint32_t key = rcodes[sym] & (uint32_t)(root_size - 1);
            const int extra = len - root_bits;
            if (extra > (int)sub_bits[key])
                sub_bits[key] = (uint8_t)extra;
        }
    }
    for (i = 0; i < root_size; ++i) {
        if (sub_bits[i] != 0) {
            const uint32_t want = (uint32_t)root_size + (uint32_t)sub_total +
                                  (1u << sub_bits[i]);
            if (want > HUFF_MAX_NODES)
                goto end;
            sub_off[i] = (uint32_t)(root_size + sub_total);
            sub_total += 1 << sub_bits[i];
        }
    }

    h->nodes = (uint32_t*)calloc((size_t)(root_size + sub_total),
                                 sizeof(*h->nodes));
    if (h->nodes == NULL)
        goto end;

    for (sym = 0; sym < alphabet_size; ++sym) {
        len = lengths[sym];
        if (len > 0 && len <= root_bits) {
            const uint32_t step = 1u << len;
            uint32_t f;
            for (f = rcodes[sym]; f < (uint32_t)root_size; f += step)
                h->nodes[f] = ((uint32_t)len << 28) | (uint32_t)sym;
        }
    }
    for (sym = 0; sym < alphabet_size; ++sym) {
        len = lengths[sym];
        if (len > root_bits) {
            const uint32_t key = rcodes[sym] & (uint32_t)(root_size - 1);
            const uint32_t base = sub_off[key];
            const uint32_t size = 1u << sub_bits[key];
            const int sub_len = len - root_bits;
            const uint32_t step = 1u << sub_len;
            uint32_t f;
            for (f = rcodes[sym] >> root_bits; f < size; f += step)
                h->nodes[base + f] = ((uint32_t)sub_len << 28) | (uint32_t)sym;
        }
    }
    /* Escape markers last: in a valid tree no direct code owns those slots. */
    for (i = 0; i < root_size; ++i) {
        if (sub_bits[i] != 0)
            h->nodes[i] = ((uint32_t)sub_bits[i] << 24) | sub_off[i];
    }

    h->root_bits = root_bits;
    h->single = 0;
    ok = 1;

end:
    free(rcodes);
    free(sub_bits);
    free(sub_off);
    if (!ok) {
        free(h->nodes);
        h->nodes = NULL;
    }
    return ok;
}

int webp_huff_read(const WebpHuff* h, WebpBits* br) {
    uint32_t node;
    int len;

    if (h->empty)
        return -1;  /* corrupt stream: no symbol was ever defined */
    if (h->single)
        return (int)h->sym;

    node = h->nodes[webp_bits_peek(br, h->root_bits)];
    len = HUFF_LEN(node);
    if (len != 0) {
        webp_bits_skip(br, len);
        return (int)HUFF_SYM(node);
    }
    if (node == 0)
        return -1;  /* under-subscribed code: this pattern was never assigned */
    {
        const int sub_bits = HUFF_SUB_BITS(node);
        const uint32_t base = HUFF_SUB_OFF(node);
        const uint32_t rest =
            webp_bits_peek(br, h->root_bits + sub_bits) >> h->root_bits;
        node = h->nodes[base + rest];
        if (node == 0)
            return -1;
        webp_bits_skip(br, h->root_bits + HUFF_LEN(node));
        return (int)HUFF_SYM(node);
    }
}
