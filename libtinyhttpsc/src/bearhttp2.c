/* Minimal HTTP/2 client framing + HPACK codec. See bearhttp2.h for scope.
 * No third-party dependency: the Huffman table below is generated from
 * RFC 7541 Appendix B and the framing follows RFC 7540 directly. */
#include "tinyhttpsc/bearhttp2.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

/* ------------------------------------------------------------------ buffer */
void h2_buf_init(H2Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
void h2_buf_free(H2Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

int h2_buf_append(H2Buf *b, const void *p, size_t n) {
    if(n == 0) return 0;
    if(b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while(nc < b->len + n) nc *= 2;
        unsigned char *nd = (unsigned char *)realloc(b->data, nc);
        if(!nd) return -1;
        b->data = nd; b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

/* ------------------------------------------------- HPACK Huffman (RFC 7541) */
/* index = symbol 0..256 (256 = EOS). Generated from Appendix B. */
/* HPACK Huffman code table (RFC 7541 Appendix B), index = symbol 0..256 (256=EOS) */
static const struct { unsigned code; unsigned char len; } hpack_huff[257] = {
	{0x00001ff8,13}, /* 0 */
	{0x007fffd8,23}, /* 1 */
	{0x0fffffe2,28}, /* 2 */
	{0x0fffffe3,28}, /* 3 */
	{0x0fffffe4,28}, /* 4 */
	{0x0fffffe5,28}, /* 5 */
	{0x0fffffe6,28}, /* 6 */
	{0x0fffffe7,28}, /* 7 */
	{0x0fffffe8,28}, /* 8 */
	{0x00ffffea,24}, /* 9 */
	{0x3ffffffc,30}, /* 10 */
	{0x0fffffe9,28}, /* 11 */
	{0x0fffffea,28}, /* 12 */
	{0x3ffffffd,30}, /* 13 */
	{0x0fffffeb,28}, /* 14 */
	{0x0fffffec,28}, /* 15 */
	{0x0fffffed,28}, /* 16 */
	{0x0fffffee,28}, /* 17 */
	{0x0fffffef,28}, /* 18 */
	{0x0ffffff0,28}, /* 19 */
	{0x0ffffff1,28}, /* 20 */
	{0x0ffffff2,28}, /* 21 */
	{0x3ffffffe,30}, /* 22 */
	{0x0ffffff3,28}, /* 23 */
	{0x0ffffff4,28}, /* 24 */
	{0x0ffffff5,28}, /* 25 */
	{0x0ffffff6,28}, /* 26 */
	{0x0ffffff7,28}, /* 27 */
	{0x0ffffff8,28}, /* 28 */
	{0x0ffffff9,28}, /* 29 */
	{0x0ffffffa,28}, /* 30 */
	{0x0ffffffb,28}, /* 31 */
	{0x00000014, 6}, /* 32 */
	{0x000003f8,10}, /* 33 */
	{0x000003f9,10}, /* 34 */
	{0x00000ffa,12}, /* 35 */
	{0x00001ff9,13}, /* 36 */
	{0x00000015, 6}, /* 37 */
	{0x000000f8, 8}, /* 38 */
	{0x000007fa,11}, /* 39 */
	{0x000003fa,10}, /* 40 */
	{0x000003fb,10}, /* 41 */
	{0x000000f9, 8}, /* 42 */
	{0x000007fb,11}, /* 43 */
	{0x000000fa, 8}, /* 44 */
	{0x00000016, 6}, /* 45 */
	{0x00000017, 6}, /* 46 */
	{0x00000018, 6}, /* 47 */
	{0x00000000, 5}, /* 48 */
	{0x00000001, 5}, /* 49 */
	{0x00000002, 5}, /* 50 */
	{0x00000019, 6}, /* 51 */
	{0x0000001a, 6}, /* 52 */
	{0x0000001b, 6}, /* 53 */
	{0x0000001c, 6}, /* 54 */
	{0x0000001d, 6}, /* 55 */
	{0x0000001e, 6}, /* 56 */
	{0x0000001f, 6}, /* 57 */
	{0x0000005c, 7}, /* 58 */
	{0x000000fb, 8}, /* 59 */
	{0x00007ffc,15}, /* 60 */
	{0x00000020, 6}, /* 61 */
	{0x00000ffb,12}, /* 62 */
	{0x000003fc,10}, /* 63 */
	{0x00001ffa,13}, /* 64 */
	{0x00000021, 6}, /* 65 */
	{0x0000005d, 7}, /* 66 */
	{0x0000005e, 7}, /* 67 */
	{0x0000005f, 7}, /* 68 */
	{0x00000060, 7}, /* 69 */
	{0x00000061, 7}, /* 70 */
	{0x00000062, 7}, /* 71 */
	{0x00000063, 7}, /* 72 */
	{0x00000064, 7}, /* 73 */
	{0x00000065, 7}, /* 74 */
	{0x00000066, 7}, /* 75 */
	{0x00000067, 7}, /* 76 */
	{0x00000068, 7}, /* 77 */
	{0x00000069, 7}, /* 78 */
	{0x0000006a, 7}, /* 79 */
	{0x0000006b, 7}, /* 80 */
	{0x0000006c, 7}, /* 81 */
	{0x0000006d, 7}, /* 82 */
	{0x0000006e, 7}, /* 83 */
	{0x0000006f, 7}, /* 84 */
	{0x00000070, 7}, /* 85 */
	{0x00000071, 7}, /* 86 */
	{0x00000072, 7}, /* 87 */
	{0x000000fc, 8}, /* 88 */
	{0x00000073, 7}, /* 89 */
	{0x000000fd, 8}, /* 90 */
	{0x00001ffb,13}, /* 91 */
	{0x0007fff0,19}, /* 92 */
	{0x00001ffc,13}, /* 93 */
	{0x00003ffc,14}, /* 94 */
	{0x00000022, 6}, /* 95 */
	{0x00007ffd,15}, /* 96 */
	{0x00000003, 5}, /* 97 */
	{0x00000023, 6}, /* 98 */
	{0x00000004, 5}, /* 99 */
	{0x00000024, 6}, /* 100 */
	{0x00000005, 5}, /* 101 */
	{0x00000025, 6}, /* 102 */
	{0x00000026, 6}, /* 103 */
	{0x00000027, 6}, /* 104 */
	{0x00000006, 5}, /* 105 */
	{0x00000074, 7}, /* 106 */
	{0x00000075, 7}, /* 107 */
	{0x00000028, 6}, /* 108 */
	{0x00000029, 6}, /* 109 */
	{0x0000002a, 6}, /* 110 */
	{0x00000007, 5}, /* 111 */
	{0x0000002b, 6}, /* 112 */
	{0x00000076, 7}, /* 113 */
	{0x0000002c, 6}, /* 114 */
	{0x00000008, 5}, /* 115 */
	{0x00000009, 5}, /* 116 */
	{0x0000002d, 6}, /* 117 */
	{0x00000077, 7}, /* 118 */
	{0x00000078, 7}, /* 119 */
	{0x00000079, 7}, /* 120 */
	{0x0000007a, 7}, /* 121 */
	{0x0000007b, 7}, /* 122 */
	{0x00007ffe,15}, /* 123 */
	{0x000007fc,11}, /* 124 */
	{0x00003ffd,14}, /* 125 */
	{0x00001ffd,13}, /* 126 */
	{0x0ffffffc,28}, /* 127 */
	{0x000fffe6,20}, /* 128 */
	{0x003fffd2,22}, /* 129 */
	{0x000fffe7,20}, /* 130 */
	{0x000fffe8,20}, /* 131 */
	{0x003fffd3,22}, /* 132 */
	{0x003fffd4,22}, /* 133 */
	{0x003fffd5,22}, /* 134 */
	{0x007fffd9,23}, /* 135 */
	{0x003fffd6,22}, /* 136 */
	{0x007fffda,23}, /* 137 */
	{0x007fffdb,23}, /* 138 */
	{0x007fffdc,23}, /* 139 */
	{0x007fffdd,23}, /* 140 */
	{0x007fffde,23}, /* 141 */
	{0x00ffffeb,24}, /* 142 */
	{0x007fffdf,23}, /* 143 */
	{0x00ffffec,24}, /* 144 */
	{0x00ffffed,24}, /* 145 */
	{0x003fffd7,22}, /* 146 */
	{0x007fffe0,23}, /* 147 */
	{0x00ffffee,24}, /* 148 */
	{0x007fffe1,23}, /* 149 */
	{0x007fffe2,23}, /* 150 */
	{0x007fffe3,23}, /* 151 */
	{0x007fffe4,23}, /* 152 */
	{0x001fffdc,21}, /* 153 */
	{0x003fffd8,22}, /* 154 */
	{0x007fffe5,23}, /* 155 */
	{0x003fffd9,22}, /* 156 */
	{0x007fffe6,23}, /* 157 */
	{0x007fffe7,23}, /* 158 */
	{0x00ffffef,24}, /* 159 */
	{0x003fffda,22}, /* 160 */
	{0x001fffdd,21}, /* 161 */
	{0x000fffe9,20}, /* 162 */
	{0x003fffdb,22}, /* 163 */
	{0x003fffdc,22}, /* 164 */
	{0x007fffe8,23}, /* 165 */
	{0x007fffe9,23}, /* 166 */
	{0x001fffde,21}, /* 167 */
	{0x007fffea,23}, /* 168 */
	{0x003fffdd,22}, /* 169 */
	{0x003fffde,22}, /* 170 */
	{0x00fffff0,24}, /* 171 */
	{0x001fffdf,21}, /* 172 */
	{0x003fffdf,22}, /* 173 */
	{0x007fffeb,23}, /* 174 */
	{0x007fffec,23}, /* 175 */
	{0x001fffe0,21}, /* 176 */
	{0x001fffe1,21}, /* 177 */
	{0x003fffe0,22}, /* 178 */
	{0x001fffe2,21}, /* 179 */
	{0x007fffed,23}, /* 180 */
	{0x003fffe1,22}, /* 181 */
	{0x007fffee,23}, /* 182 */
	{0x007fffef,23}, /* 183 */
	{0x000fffea,20}, /* 184 */
	{0x003fffe2,22}, /* 185 */
	{0x003fffe3,22}, /* 186 */
	{0x003fffe4,22}, /* 187 */
	{0x007ffff0,23}, /* 188 */
	{0x003fffe5,22}, /* 189 */
	{0x003fffe6,22}, /* 190 */
	{0x007ffff1,23}, /* 191 */
	{0x03ffffe0,26}, /* 192 */
	{0x03ffffe1,26}, /* 193 */
	{0x000fffeb,20}, /* 194 */
	{0x0007fff1,19}, /* 195 */
	{0x003fffe7,22}, /* 196 */
	{0x007ffff2,23}, /* 197 */
	{0x003fffe8,22}, /* 198 */
	{0x01ffffec,25}, /* 199 */
	{0x03ffffe2,26}, /* 200 */
	{0x03ffffe3,26}, /* 201 */
	{0x03ffffe4,26}, /* 202 */
	{0x07ffffde,27}, /* 203 */
	{0x07ffffdf,27}, /* 204 */
	{0x03ffffe5,26}, /* 205 */
	{0x00fffff1,24}, /* 206 */
	{0x01ffffed,25}, /* 207 */
	{0x0007fff2,19}, /* 208 */
	{0x001fffe3,21}, /* 209 */
	{0x03ffffe6,26}, /* 210 */
	{0x07ffffe0,27}, /* 211 */
	{0x07ffffe1,27}, /* 212 */
	{0x03ffffe7,26}, /* 213 */
	{0x07ffffe2,27}, /* 214 */
	{0x00fffff2,24}, /* 215 */
	{0x001fffe4,21}, /* 216 */
	{0x001fffe5,21}, /* 217 */
	{0x03ffffe8,26}, /* 218 */
	{0x03ffffe9,26}, /* 219 */
	{0x0ffffffd,28}, /* 220 */
	{0x07ffffe3,27}, /* 221 */
	{0x07ffffe4,27}, /* 222 */
	{0x07ffffe5,27}, /* 223 */
	{0x000fffec,20}, /* 224 */
	{0x00fffff3,24}, /* 225 */
	{0x000fffed,20}, /* 226 */
	{0x001fffe6,21}, /* 227 */
	{0x003fffe9,22}, /* 228 */
	{0x001fffe7,21}, /* 229 */
	{0x001fffe8,21}, /* 230 */
	{0x007ffff3,23}, /* 231 */
	{0x003fffea,22}, /* 232 */
	{0x003fffeb,22}, /* 233 */
	{0x01ffffee,25}, /* 234 */
	{0x01ffffef,25}, /* 235 */
	{0x00fffff4,24}, /* 236 */
	{0x00fffff5,24}, /* 237 */
	{0x03ffffea,26}, /* 238 */
	{0x007ffff4,23}, /* 239 */
	{0x03ffffeb,26}, /* 240 */
	{0x07ffffe6,27}, /* 241 */
	{0x03ffffec,26}, /* 242 */
	{0x03ffffed,26}, /* 243 */
	{0x07ffffe7,27}, /* 244 */
	{0x07ffffe8,27}, /* 245 */
	{0x07ffffe9,27}, /* 246 */
	{0x07ffffea,27}, /* 247 */
	{0x07ffffeb,27}, /* 248 */
	{0x0ffffffe,28}, /* 249 */
	{0x07ffffec,27}, /* 250 */
	{0x07ffffed,27}, /* 251 */
	{0x07ffffee,27}, /* 252 */
	{0x07ffffef,27}, /* 253 */
	{0x07fffff0,27}, /* 254 */
	{0x03ffffee,26}, /* 255 */
	{0x3fffffff,30}, /* 256 */
};

int h2_huffman_encode(const unsigned char *src, size_t n, H2Buf *out) {
    unsigned long long acc = 0;
    int nbits = 0;
    for(size_t i = 0; i < n; i++) {
        unsigned code = hpack_huff[src[i]].code;
        int len = hpack_huff[src[i]].len;
        acc = (acc << len) | code;
        nbits += len;
        while(nbits >= 8) {
            nbits -= 8;
            unsigned char byte = (unsigned char)((acc >> nbits) & 0xff);
            if(h2_buf_append(out, &byte, 1) != 0) return -1;
        }
    }
    if(nbits > 0) { /* pad with EOS prefix (all ones) */
        acc = (acc << (8 - nbits)) | ((1u << (8 - nbits)) - 1);
        unsigned char byte = (unsigned char)(acc & 0xff);
        if(h2_buf_append(out, &byte, 1) != 0) return -1;
    }
    return 0;
}

/* Decode by walking a bitwise prefix match against the code table. The table
 * is small (257 entries) and header strings are short, so a linear scan per
 * code is fast enough and avoids building a separate trie. */
int h2_huffman_decode(const unsigned char *src, size_t n, H2Buf *out) {
    unsigned long long acc = 0;
    int nbits = 0;
    size_t i = 0;
    while(i < n || nbits > 0) {
        /* refill */
        while(nbits < 30 && i < n) {
            acc = (acc << 8) | src[i++];
            nbits += 8;
        }
        int matched = 0;
        for(int sym = 0; sym < 256; sym++) {
            int len = hpack_huff[sym].len;
            if(len > nbits) continue;
            unsigned code = hpack_huff[sym].code;
            unsigned long long top = (acc >> (nbits - len)) & ((1ull << len) - 1);
            if(top == code) {
                unsigned char c = (unsigned char)sym;
                if(h2_buf_append(out, &c, 1) != 0) return -1;
                nbits -= len;
                acc &= (1ull << nbits) - 1;
                matched = 1;
                break;
            }
        }
        if(!matched) {
            /* remaining bits must be an EOS prefix (all ones, <30 bits) */
            unsigned long long rem = acc & ((1ull << nbits) - 1);
            if(nbits < 30 && rem == ((1ull << nbits) - 1)) break;
            return -1; /* invalid code */
        }
    }
    return 0;
}

/* ------------------------------------------- HPACK static table (RFC 7541 A) */
/* index 1..61 */
static const char *const hpack_static[62][2] = {
    {NULL, NULL},                       /* 0 unused */
    {":authority", ""},                 /* 1 */
    {":method", "GET"},                 /* 2 */
    {":method", "POST"},                /* 3 */
    {":path", "/"},                     /* 4 */
    {":path", "/index.html"},           /* 5 */
    {":scheme", "http"},                /* 6 */
    {":scheme", "https"},               /* 7 */
    {":status", "200"},                 /* 8 */
    {":status", "204"},                 /* 9 */
    {":status", "206"},                 /* 10 */
    {":status", "304"},                 /* 11 */
    {":status", "400"},                 /* 12 */
    {":status", "404"},                 /* 13 */
    {":status", "500"},                 /* 14 */
    {"accept-charset", ""},             /* 15 */
    {"accept-encoding", "gzip, deflate"},/* 16 */
    {"accept-language", ""},            /* 17 */
    {"accept-ranges", ""},              /* 18 */
    {"accept", ""},                     /* 19 */
    {"access-control-allow-origin", ""},/* 20 */
    {"age", ""},                        /* 21 */
    {"allow", ""},                      /* 22 */
    {"authorization", ""},              /* 23 */
    {"cache-control", ""},              /* 24 */
    {"content-disposition", ""},        /* 25 */
    {"content-encoding", ""},           /* 26 */
    {"content-language", ""},           /* 27 */
    {"content-length", ""},             /* 28 */
    {"content-location", ""},           /* 29 */
    {"content-range", ""},              /* 30 */
    {"content-type", ""},               /* 31 */
    {"cookie", ""},                     /* 32 */
    {"date", ""},                       /* 33 */
    {"etag", ""},                       /* 34 */
    {"expect", ""},                     /* 35 */
    {"expires", ""},                    /* 36 */
    {"from", ""},                       /* 37 */
    {"host", ""},                       /* 38 */
    {"if-match", ""},                   /* 39 */
    {"if-modified-since", ""},          /* 40 */
    {"if-none-match", ""},              /* 41 */
    {"if-range", ""},                   /* 42 */
    {"if-unmodified-since", ""},        /* 43 */
    {"last-modified", ""},              /* 44 */
    {"link", ""},                       /* 45 */
    {"location", ""},                   /* 46 */
    {"max-forwards", ""},               /* 47 */
    {"proxy-authenticate", ""},         /* 48 */
    {"proxy-authorization", ""},        /* 49 */
    {"range", ""},                      /* 50 */
    {"referer", ""},                    /* 51 */
    {"refresh", ""},                    /* 52 */
    {"retry-after", ""},                /* 53 */
    {"server", ""},                     /* 54 */
    {"set-cookie", ""},                 /* 55 */
    {"strict-transport-security", ""},  /* 56 */
    {"transfer-encoding", ""},          /* 57 */
    {"user-agent", ""},                 /* 58 */
    {"vary", ""},                       /* 59 */
    {"via", ""},                        /* 60 */
    {"www-authenticate", ""},           /* 61 */
};

/* ------------------------------------------------------- integer coding */
/* RFC 7541 sec. 5.1. Decode an integer with the given prefix bit width.
 * *pos advances past the integer. Returns the value, or -1 on truncation. */
static long hpack_get_int(const unsigned char *p, size_t n, size_t *pos, int prefix) {
    if(*pos >= n) return -1;
    unsigned mask = (unsigned)((1 << prefix) - 1);
    unsigned val = p[*pos] & mask;
    (*pos)++;
    if(val < mask) return (long)val;
    /* extended: 7-bit chunks, little-endian, high bit = continuation */
    long m = 0;
    for(;;) {
        if(*pos >= n) return -1;
        unsigned b = p[*pos];
        (*pos)++;
        val += (unsigned)((b & 0x7f) << m);
        m += 7;
        if(!(b & 0x80)) break;
        if(m > 28) return -1; /* overflow guard */
    }
    return (long)val;
}

static int hpack_put_int(H2Buf *out, unsigned first_byte_mask, int prefix, unsigned long val) {
    unsigned mask = (unsigned)((1 << prefix) - 1);
    unsigned char b;
    if(val < mask) {
        b = (unsigned char)(first_byte_mask | val);
        return h2_buf_append(out, &b, 1);
    }
    b = (unsigned char)(first_byte_mask | mask);
    if(h2_buf_append(out, &b, 1) != 0) return -1;
    val -= mask;
    while(val >= 128) {
        b = (unsigned char)((val & 0x7f) | 0x80);
        if(h2_buf_append(out, &b, 1) != 0) return -1;
        val >>= 7;
    }
    b = (unsigned char)val;
    return h2_buf_append(out, &b, 1);
}

/* Encode a string literal (RFC 7541 sec. 5.2). Tries Huffman and picks the
 * shorter representation, as real encoders do. */
static int hpack_put_str(H2Buf *out, const char *s, size_t len) {
    H2Buf huff; h2_buf_init(&huff);
    int hr = h2_huffman_encode((const unsigned char *)s, len, &huff);
    if(hr == 0 && huff.len < len) {
        if(hpack_put_int(out, 0x80, 7, (unsigned long)huff.len) != 0) { h2_buf_free(&huff); return -1; }
        int r = h2_buf_append(out, huff.data, huff.len);
        h2_buf_free(&huff);
        return r;
    }
    h2_buf_free(&huff);
    if(hpack_put_int(out, 0x00, 7, (unsigned long)len) != 0) return -1;
    return h2_buf_append(out, s, len);
}

/* ------------------------------------------------------------- encoder */
void h2_hpack_enc_init(H2HpackEnc *e) { e->header_table_size = 4096; }

/* Find a static-table entry matching (name,value) exactly, or name only.
 * Returns the index, or 0. *exact set when the value also matches. */
static int hpack_static_find(const char *name, const char *value, int *exact) {
    int name_idx = 0;
    *exact = 0;
    for(int i = 1; i <= 61; i++) {
        if(strcmp(hpack_static[i][0], name) != 0) continue;
        if(hpack_static[i][1][0] && value && strcmp(hpack_static[i][1], value) == 0) {
            *exact = 1;
            return i;
        }
        if(!name_idx) name_idx = i;
    }
    return name_idx;
}

int h2_hpack_encode_field(H2Buf *out, const char *name, const char *value) {
    int exact = 0;
    int idx = hpack_static_find(name, value, &exact);
    if(exact) {
        /* indexed header field: 1xxxxxxx */
        return hpack_put_int(out, 0x80, 7, (unsigned long)idx);
    }
    /* literal WITH incremental indexing: 01xxxxxx (6-bit prefix). Using the
     * incremental form lets the peer add the field to its dynamic table, which
     * is what RFC 7541 C.3 shows for :authority and keeps multi-request
     * connections compact. Name is referenced by static index when possible. */
    if(idx > 0) {
        if(hpack_put_int(out, 0x40, 6, (unsigned long)idx) != 0) return -1;
    } else {
        if(hpack_put_int(out, 0x40, 6, 0) != 0) return -1;
        if(hpack_put_str(out, name, strlen(name)) != 0) return -1;
    }
    return hpack_put_str(out, value ? value : "", value ? strlen(value) : 0);
}

/* ------------------------------------------------------------- decoder */
void h2_hpack_dec_init(H2HpackDec *d) {
    memset(d, 0, sizeof(*d));
    d->max_table_size = 4096;
}

void h2_hpack_dec_free(H2HpackDec *d) {
    for(int i = 0; i < d->dyn_count; i++) {
        free(d->dyn[i].name);
        free(d->dyn[i].value);
    }
    free(d->headers);
    free(d->pool);
    memset(d, 0, sizeof(*d));
}

/* Append a copy of (name,value) to the decoded header list, strings stored in
 * the decoder pool so they outlive the frame payload. */
static int hpack_dec_push(H2HpackDec *d, const char *name, size_t nlen,
                          const char *value, size_t vlen) {
    if(d->header_count >= d->header_cap) {
        int nc = d->header_cap ? d->header_cap * 2 : 16;
        H2Header *nh = (H2Header *)realloc(d->headers, (size_t)nc * sizeof(H2Header));
        if(!nh) return -1;
        d->headers = nh; d->header_cap = nc;
    }
    /* reserve pool space for both strings + NUL terminators */
    size_t need = nlen + 1 + vlen + 1;
    if(d->pool_len + need > d->pool_cap) {
        size_t nc = d->pool_cap ? d->pool_cap : 512;
        while(nc < d->pool_len + need) nc *= 2;
        char *np = (char *)realloc(d->pool, nc);
        if(!np) return -1;
        d->pool = np; d->pool_cap = nc;
    }
    char *ns = d->pool + d->pool_len;
    memcpy(ns, name, nlen); ns[nlen] = '\0';
    char *vs = ns + nlen + 1;
    memcpy(vs, value, vlen); vs[vlen] = '\0';
    d->pool_len += need;
    d->headers[d->header_count].name = ns;
    d->headers[d->header_count].value = vs;
    d->header_count++;
    return 0;
}

/* Insert into the dynamic table (most-recent-first), evicting as needed to
 * stay within max_table_size. */
static int hpack_dec_insert(H2HpackDec *d, const char *name, size_t nlen,
                            const char *value, size_t vlen) {
    size_t esize = nlen + vlen + 32;
    /* make room: evict from the tail (oldest) */
    while(d->dyn_size + esize > d->max_table_size && d->dyn_count > 0) {
        d->dyn_count--;
        d->dyn_size -= d->dyn[d->dyn_count].size;
        free(d->dyn[d->dyn_count].name);
        free(d->dyn[d->dyn_count].value);
        d->dyn[d->dyn_count].name = d->dyn[d->dyn_count].value = NULL;
    }
    if(esize > d->max_table_size) return 0; /* entry too large: drop, not fatal */
    if(d->dyn_count >= H2_MAX_DYNAMIC) {
        /* shift out the oldest to bound memory */
        d->dyn_size -= d->dyn[H2_MAX_DYNAMIC - 1].size;
        free(d->dyn[H2_MAX_DYNAMIC - 1].name);
        free(d->dyn[H2_MAX_DYNAMIC - 1].value);
        memmove(&d->dyn[1], &d->dyn[0], sizeof(H2HpackEntry) * (H2_MAX_DYNAMIC - 1));
        d->dyn_count = H2_MAX_DYNAMIC - 1;
    }
    /* shift everything back by one, insert at front */
    memmove(&d->dyn[1], &d->dyn[0], sizeof(H2HpackEntry) * (size_t)d->dyn_count);
    d->dyn_count++;
    char *n = (char *)malloc(nlen + 1);
    char *v = (char *)malloc(vlen + 1);
    if(!n || !v) { free(n); free(v); return -1; }
    memcpy(n, name, nlen); n[nlen] = '\0';
    memcpy(v, value, vlen); v[vlen] = '\0';
    d->dyn[0].name = n; d->dyn[0].value = v; d->dyn[0].size = esize;
    d->dyn_size += esize;
    return 0;
}

/* Resolve an HPACK index to (name,value). Static table is 1..61; dynamic table
 * indices continue from 62 (dyn[0] == 62). Returns 0 on success. */
static int hpack_dec_lookup(H2HpackDec *d, long idx, const char **name, size_t *nlen,
                            const char **value, size_t *vlen) {
    if(idx <= 0) return -1;
    if(idx <= 61) {
        *name = hpack_static[idx][0]; *nlen = strlen(*name);
        *value = hpack_static[idx][1]; *vlen = strlen(*value);
        return 0;
    }
    long di = idx - 62;
    if(di >= d->dyn_count) return -1;
    *name = d->dyn[di].name; *nlen = strlen(*name);
    *value = d->dyn[di].value; *vlen = strlen(*value);
    return 0;
}

/* Decode a string literal at *pos (handles the Huffman flag). Returns a malloc'd
 * NUL-terminated string and sets *out_len; caller frees. NULL on error. */
static char *hpack_dec_str(const unsigned char *p, size_t n, size_t *pos, size_t *out_len) {
    if(*pos >= n) return NULL;
    int huff = (p[*pos] & 0x80) ? 1 : 0;
    long len = hpack_get_int(p, n, pos, 7);
    if(len < 0 || (size_t)len > n - *pos) return NULL;
    const unsigned char *src = p + *pos;
    *pos += (size_t)len;
    H2Buf dec; h2_buf_init(&dec);
    if(huff) {
        if(h2_huffman_decode(src, (size_t)len, &dec) != 0) { h2_buf_free(&dec); return NULL; }
    } else {
        if(h2_buf_append(&dec, src, (size_t)len) != 0) { h2_buf_free(&dec); return NULL; }
    }
    /* NUL-terminate for convenience */
    unsigned char z = 0;
    h2_buf_append(&dec, &z, 1);
    *out_len = dec.len - 1;
    return (char *)dec.data; /* caller frees */
}

int h2_hpack_decode_block(H2HpackDec *d, const unsigned char *p, size_t n) {
    size_t pos = 0;
    while(pos < n) {
        unsigned char b = p[pos];
        if(b & 0x80) {
            /* indexed header field: 1xxxxxxx */
            long idx = hpack_get_int(p, n, &pos, 7);
            const char *nm, *vl; size_t nl, vll;
            if(idx < 0 || hpack_dec_lookup(d, idx, &nm, &nl, &vl, &vll) != 0) return -1;
            if(hpack_dec_push(d, nm, nl, vl, vll) != 0) return -1;
        } else if((b & 0xc0) == 0x40) {
            /* literal with incremental indexing: 01xxxxxx */
            long idx = hpack_get_int(p, n, &pos, 6);
            char *nm = NULL, *vl = NULL; size_t nl = 0, vll = 0;
            const char *snm = NULL, *svl = NULL; size_t snl = 0, svllen = 0;
            if(idx > 0) {
                if(hpack_dec_lookup(d, idx, &snm, &snl, &svl, &svllen) != 0) return -1;
            } else {
                nm = hpack_dec_str(p, n, &pos, &nl);
                if(!nm) return -1;
                snm = nm; snl = nl;
            }
            vl = hpack_dec_str(p, n, &pos, &vll);
            if(!vl) { free(nm); return -1; }
            svl = vl;
            int r = hpack_dec_push(d, snm, snl, svl, vll);
            if(r == 0) r = hpack_dec_insert(d, snm, snl, svl, vll);
            free(nm); free(vl);
            if(r != 0) return -1;
        } else if((b & 0xe0) == 0x20) {
            /* dynamic table size update: 001xxxxx */
            long sz = hpack_get_int(p, n, &pos, 5);
            if(sz < 0) return -1;
            d->max_table_size = (unsigned)sz;
            /* evict to fit the new size */
            while(d->dyn_size > d->max_table_size && d->dyn_count > 0) {
                d->dyn_count--;
                d->dyn_size -= d->dyn[d->dyn_count].size;
                free(d->dyn[d->dyn_count].name);
                free(d->dyn[d->dyn_count].value);
                d->dyn[d->dyn_count].name = d->dyn[d->dyn_count].value = NULL;
            }
        } else {
            /* literal without indexing (0000xxxx) / never indexed (0001xxxx) */
            int prefix = 4;
            long idx = hpack_get_int(p, n, &pos, prefix);
            char *nm = NULL, *vl = NULL; size_t nl = 0, vll = 0;
            const char *snm = NULL, *svl = NULL; size_t snl = 0;
            if(idx > 0) {
                const char *sv2; size_t svl2;
                if(hpack_dec_lookup(d, idx, &snm, &snl, &sv2, &svl2) != 0) return -1;
            } else {
                nm = hpack_dec_str(p, n, &pos, &nl);
                if(!nm) return -1;
                snm = nm; snl = nl;
            }
            vl = hpack_dec_str(p, n, &pos, &vll);
            if(!vl) { free(nm); return -1; }
            svl = vl;
            int r = hpack_dec_push(d, snm, snl, svl, vll);
            free(nm); free(vl);
            if(r != 0) return -1;
        }
    }
    return 0;
}

/* =====================================================================
 * Connection-level framing (RFC 7540). Everything below drives a single
 * request/response over the opaque send/recv transport supplied by the
 * caller (the BearSSL TLS record layer in BearHttpsClientOne.c).
 * ===================================================================== */

/* ------------------------------------------------- big-endian wire helpers */
static void h2_put_be16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}
static void h2_put_be24(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)((v >> 16) & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)(v & 0xff);
}
static void h2_put_be32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}
static unsigned h2_get_be16(const unsigned char *p) {
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}
static unsigned h2_get_be24(const unsigned char *p) {
    return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | (unsigned)p[2];
}
static unsigned h2_get_be32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

/* Read exactly n bytes. Returns 0 on success, -1 on EOF/error. */
static int h2_read_exact(H2Conn *c, unsigned char *buf, size_t n) {
    size_t off = 0;
    while(off < n) {
        int r = c->recv(c->io_ctx, buf + off, n - off);
        if(r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

/* Send a whole frame (9-byte header + payload). Returns 0 on success. */
static int h2_send_frame(H2Conn *c, unsigned char type, unsigned char flags,
                         unsigned stream_id, const unsigned char *payload, size_t len) {
    unsigned char hdr[9];
    h2_put_be24(hdr, (unsigned)len);
    hdr[3] = type;
    hdr[4] = flags;
    h2_put_be32(hdr + 5, stream_id & 0x7fffffffu);
    if(c->send(c->io_ctx, hdr, 9) != 0) return -1;
    if(len && c->send(c->io_ctx, payload, len) != 0) return -1;
    return 0;
}

/* Read one frame into c->rbuf; sets type/flags/stream_id. 0 on success. */
static int h2_read_frame(H2Conn *c, unsigned char *type, unsigned char *flags,
                         unsigned *stream_id) {
    unsigned char hdr[9];
    if(h2_read_exact(c, hdr, 9) != 0) return -1;
    unsigned len = h2_get_be24(hdr);
    *type = hdr[3];
    *flags = hdr[4];
    *stream_id = h2_get_be32(hdr + 5) & 0x7fffffffu;
    /* RFC 7540 sec. 4.2: a frame larger than the negotiated SETTINGS_MAX_FRAME_SIZE
     * we advertised (we keep the 16384 default) is a connection error. Allow up to
     * the max legal size to stay tolerant, but never more than the 24-bit field. */
    if(len > 16777215u) return -1;
    if(len + 1 > c->rbuf_cap) {
        unsigned char *nb = (unsigned char *)realloc(c->rbuf, len + 1);
        if(!nb) return -1;
        c->rbuf = nb; c->rbuf_cap = len + 1;
    }
    if(len && h2_read_exact(c, c->rbuf, len) != 0) return -1;
    c->rbuf[len] = '\0';
    c->rbuf_len = len;
    return 0;
}

/* Fold the peer's SETTINGS into our connection state. */
static void h2_apply_settings(H2Conn *c, const unsigned char *p, size_t len) {
    for(size_t i = 0; i + 6 <= len; i += 6) {
        unsigned id = h2_get_be16(p + i);
        unsigned val = h2_get_be32(p + i + 2);
        switch(id) {
        case H2_SETTINGS_MAX_FRAME_SIZE:
            if(val >= 16384u && val <= 16777215u) c->peer_max_frame = val;
            break;
        case H2_SETTINGS_INITIAL_WINDOW_SIZE:
            if(val <= 2147483647u) {
                /* Delta-apply to the stream window per RFC 7540 sec. 6.9.2. */
                int oldw = c->stream_window;
                c->stream_window = (int)val;
                (void)oldw;
            }
            break;
        default:
            break; /* HEADER_TABLE_SIZE / ENABLE_PUSH / MAX_CONCURRENT ignored */
        }
    }
}

/* Handle one connection-level control frame. Returns 1 if fully handled,
 * 0 if it is a stream frame the caller must process. *fatal is set for
 * GOAWAY / RST_STREAM. */
static int h2_handle_control(H2Conn *c, unsigned char type, unsigned char flags,
                             unsigned sid, int *fatal) {
    *fatal = 0;
    switch(type) {
    case H2_FRAME_SETTINGS:
        if(!(flags & H2_FLAG_ACK)) {
            h2_apply_settings(c, c->rbuf, c->rbuf_len);
            h2_send_frame(c, H2_FRAME_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
        }
        return 1;
    case H2_FRAME_PING:
        if(!(flags & H2_FLAG_ACK))
            h2_send_frame(c, H2_FRAME_PING, H2_FLAG_ACK, 0, c->rbuf, c->rbuf_len);
        return 1;
    case H2_FRAME_WINDOW_UPDATE:
        if(c->rbuf_len >= 4) {
            unsigned inc = h2_get_be32(c->rbuf) & 0x7fffffffu;
            if(sid == 0) c->peer_window += (int)inc;
            else if(sid == 1) c->stream_window += (int)inc;
        }
        return 1;
    case H2_FRAME_GOAWAY:
        c->goaway = true;
        if(c->rbuf_len >= 4)
            c->last_stream_id = (int)(h2_get_be32(c->rbuf) & 0x7fffffffu);
        *fatal = 1;
        return 1;
    case H2_FRAME_RST_STREAM:
        *fatal = 1;
        return 1;
    default:
        return 0; /* HEADERS / CONTINUATION / DATA / PRIORITY / PUSH_PROMISE */
    }
}

/* --------------------------------------------------------------- lifecycle */
void h2_conn_init(H2Conn *c, H2SendFn s, H2RecvFn r, void *ctx) {
    memset(c, 0, sizeof(*c));
    c->send = s;
    c->recv = r;
    c->io_ctx = ctx;
    c->peer_max_frame = H2_DEFAULT_FRAME_SIZE;
    c->peer_window = H2_DEFAULT_WINDOW;
    c->stream_window = H2_DEFAULT_WINDOW;
    h2_hpack_dec_init(&c->dec);
}

void h2_conn_free(H2Conn *c) {
    free(c->rbuf);
    c->rbuf = NULL; c->rbuf_len = c->rbuf_cap = 0;
    h2_hpack_dec_free(&c->dec);
}

/* --------------------------------------------------------------- preface */
int h2_conn_preface(H2Conn *c) {
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if(c->send(c->io_ctx, (const unsigned char *)preface, sizeof(preface) - 1) != 0)
        return -1;
    /* Our SETTINGS: disable server push (we never handle PUSH_PROMISE). */
    unsigned char s[6];
    h2_put_be16(s, H2_SETTINGS_ENABLE_PUSH);
    h2_put_be32(s + 2, 0);
    if(h2_send_frame(c, H2_FRAME_SETTINGS, 0, 0, s, sizeof(s)) != 0)
        return -1;
    /* The server preface begins with its (possibly empty) SETTINGS frame. Read
     * frames until we have applied it; tolerate an interleaved WINDOW_UPDATE. */
    int got_settings = 0;
    for(int guard = 0; guard < 32 && !got_settings; guard++) {
        unsigned char type, flags; unsigned sid;
        if(h2_read_frame(c, &type, &flags, &sid) != 0) return -1;
        int fatal = 0;
        if(type == H2_FRAME_SETTINGS && !(flags & H2_FLAG_ACK)) got_settings = 1;
        if(h2_handle_control(c, type, flags, sid, &fatal) == 0) {
            /* A stream frame before the server SETTINGS: protocol violation for
             * our purposes; bail out so the caller can fall back to HTTP/1.1. */
            return -1;
        }
        if(fatal) return -1;
    }
    return got_settings ? 0 : -1;
}

/* -------------------------------------------------- request header framing */
/* Send an HPACK header block as HEADERS (+ CONTINUATION fragments when the
 * block exceeds the peer's max frame size). */
static int h2_send_header_block(H2Conn *c, unsigned stream_id,
                                const unsigned char *blk, size_t len, int end_stream) {
    unsigned maxf = c->peer_max_frame ? c->peer_max_frame : H2_DEFAULT_FRAME_SIZE;
    size_t off = 0;
    int first = 1;
    while(off < len) {
        size_t remaining = len - off;
        size_t chunk = remaining > maxf ? maxf : remaining;
        unsigned char type = first ? H2_FRAME_HEADERS : H2_FRAME_CONTINUATION;
        unsigned char flags = 0;
        if(chunk == remaining) flags |= H2_FLAG_END_HEADERS; /* last fragment */
        if(first && end_stream) flags |= H2_FLAG_END_STREAM;
        if(h2_send_frame(c, type, flags, stream_id, blk + off, chunk) != 0) return -1;
        off += chunk;
        first = 0;
    }
    return first ? -1 : 0; /* empty block is a bug */
}

/* Send the request body as DATA frames, honouring the connection and stream
 * flow-control windows. Pumps control frames while the window is closed. */
static int h2_send_body(H2Conn *c, unsigned stream_id,
                        const unsigned char *body, size_t body_len) {
    unsigned maxf = c->peer_max_frame ? c->peer_max_frame : H2_DEFAULT_FRAME_SIZE;
    size_t off = 0;
    while(off < body_len) {
        /* Wait until at least one byte can flow. */
        while(c->peer_window <= 0 || c->stream_window <= 0) {
            unsigned char type, flags; unsigned sid;
            if(h2_read_frame(c, &type, &flags, &sid) != 0) return -1;
            int fatal = 0;
            if(h2_handle_control(c, type, flags, sid, &fatal) == 1) {
                if(fatal) return -1;
                continue;
            }
            /* A response frame arrived before we finished sending: unusual for
             * our single-shot client. Stop and let the caller read it. */
            return -1;
        }
        size_t remaining = body_len - off;
        size_t chunk = remaining;
        if(chunk > maxf) chunk = maxf;
        if((int)chunk > c->peer_window) chunk = (size_t)c->peer_window;
        if((int)chunk > c->stream_window) chunk = (size_t)c->stream_window;
        if(chunk == 0) continue;
        unsigned char flags = (off + chunk == body_len) ? H2_FLAG_END_STREAM : 0;
        if(h2_send_frame(c, H2_FRAME_DATA, flags, stream_id, body + off, chunk) != 0)
            return -1;
        c->peer_window -= (int)chunk;
        c->stream_window -= (int)chunk;
        off += chunk;
    }
    /* If there was no body at all the END_STREAM went on the HEADERS frame. */
    return 0;
}

/* Connection-specific headers are forbidden in HTTP/2 (RFC 7540 sec. 8.1.2.2). */
static int h2_is_forbidden_header(const char *name) {
    static const char *const bad[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding",
        "upgrade", "host", NULL
    };
    for(int i = 0; bad[i]; i++) {
        const char *a = name, *b = bad[i];
        while(*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            if(ca != *b) break;
            a++; b++;
        }
        if(*a == '\0' && *b == '\0') return 1;
    }
    return 0;
}

/* ------------------------------------------------------------- one request */
static const char *h2_reason(int status) {
    switch(status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return "";
    }
}

int h2_conn_request(H2Conn *c,
                    const char *method, const char *scheme,
                    const char *authority, const char *route,
                    const char *const *extra_headers, int n_extra,
                    const unsigned char *body, size_t body_len,
                    H2Buf *out_headers, H2Buf *out_body, int *status) {
    if(status) *status = 0;
    /* --- build the HPACK request header block (pseudo-headers first) --- */
    H2Buf hb; h2_buf_init(&hb);
    if(h2_hpack_encode_field(&hb, ":method", method) != 0 ||
       h2_hpack_encode_field(&hb, ":scheme", scheme) != 0 ||
       h2_hpack_encode_field(&hb, ":authority", authority) != 0 ||
       h2_hpack_encode_field(&hb, ":path", route) != 0) {
        h2_buf_free(&hb); return -1;
    }
    int have_content_length = 0;
    for(int i = 0; i < n_extra; i++) {
        const char *nm = extra_headers[2 * i];
        const char *vl = extra_headers[2 * i + 1];
        if(!nm || !nm[0]) continue;
        if(h2_is_forbidden_header(nm)) continue;
        /* RFC 7540 sec. 8.1.2: field names MUST be lowercase on the wire.
         * Callers (and HTTP/1.1 habit) often pass "User-Agent" etc.; strict
         * servers (nghttp2) reject uppercase with a PROTOCOL_ERROR RST_STREAM,
         * so fold the name to ASCII-lowercase before HPACK-encoding it. */
        size_t nl = strlen(nm);
        char *lnm = (char *)malloc(nl + 1);
        if(!lnm) { h2_buf_free(&hb); return -1; }
        for(size_t k = 0; k < nl; k++) {
            char ch = nm[k];
            lnm[k] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
        }
        lnm[nl] = '\0';
        if(strcmp(lnm, "content-length") == 0) have_content_length = 1;
        int er = h2_hpack_encode_field(&hb, lnm, vl ? vl : "");
        free(lnm);
        if(er != 0) { h2_buf_free(&hb); return -1; }
    }
    if(body_len > 0 && !have_content_length) {
        char cl[24];
        snprintf(cl, sizeof(cl), "%zu", body_len);
        if(h2_hpack_encode_field(&hb, "content-length", cl) != 0) {
            h2_buf_free(&hb); return -1;
        }
    }

    /* --- send HEADERS (+ body). END_STREAM rides on HEADERS when no body. --- */
    int end_stream_on_headers = (body == NULL || body_len == 0);
    if(h2_send_header_block(c, 1, hb.data, hb.len, end_stream_on_headers) != 0) {
        h2_buf_free(&hb); return -1;
    }
    h2_buf_free(&hb);
    if(!end_stream_on_headers) {
        if(h2_send_body(c, 1, body, body_len) != 0) return -1;
    }

    /* --- read the response --- */
    H2Buf hblk; h2_buf_init(&hblk);   /* accumulated HEADERS/CONTINUATION payload */
    int headers_done = 0, stream_done = 0, saw_rst = 0;
    const char *h2dbg = getenv("EWEB_H2DBG");
    int dbg = (h2dbg && h2dbg[0] && h2dbg[0] != '0');
    while(!stream_done) {
        unsigned char type, flags; unsigned sid;
        if(h2_read_frame(c, &type, &flags, &sid) != 0) break; /* EOF: treat as end */
        if(dbg) fprintf(stderr, "[h2.frame] type=%u flags=0x%02x sid=%u len=%zu\n",
                        type, flags, sid, c->rbuf_len);
        if(type == H2_FRAME_RST_STREAM && c->rbuf_len >= 4)
            fprintf(stderr, "[h2.rst] sid=%u error=%u\n", sid, h2_get_be32(c->rbuf));
        int fatal = 0;
        if(h2_handle_control(c, type, flags, sid, &fatal) == 1) {
            if(fatal) { if(type == H2_FRAME_RST_STREAM) saw_rst = 1; stream_done = 1; }
            continue;
        }
        if(sid != 1) continue; /* ignore any pushed/other streams */

        if(type == H2_FRAME_HEADERS || type == H2_FRAME_CONTINUATION) {
            const unsigned char *pl = c->rbuf;
            size_t pl_len = c->rbuf_len;
            if(type == H2_FRAME_HEADERS && (flags & H2_FLAG_PADDED)) {
                if(pl_len < 1) { stream_done = 1; break; }
                unsigned pad = pl[0]; pl++; pl_len--;
                if(pad > pl_len) { stream_done = 1; break; }
                pl_len -= pad;
            }
            if(type == H2_FRAME_HEADERS && (flags & 0x20)) { /* PRIORITY */
                if(pl_len < 5) { stream_done = 1; break; }
                pl += 5; pl_len -= 5;
            }
            if(h2_buf_append(&hblk, pl, pl_len) != 0) { stream_done = 1; break; }
            if(flags & H2_FLAG_END_HEADERS) {
                if(h2_hpack_decode_block(&c->dec, hblk.data, hblk.len) != 0) {
                    stream_done = 1; break;
                }
                headers_done = 1;
            }
            if(flags & H2_FLAG_END_STREAM) stream_done = 1;
        } else if(type == H2_FRAME_DATA) {
            const unsigned char *pl = c->rbuf;
            size_t pl_len = c->rbuf_len;
            if(flags & H2_FLAG_PADDED) {
                if(pl_len < 1) { stream_done = 1; break; }
                unsigned pad = pl[0]; pl++; pl_len--;
                if(pad > pl_len) { stream_done = 1; break; }
                pl_len -= pad;
            }
            if(pl_len && h2_buf_append(out_body, pl, pl_len) != 0) { stream_done = 1; break; }
            /* Replenish both windows immediately so a large body keeps flowing. */
            if(pl_len) {
                unsigned char wu[4];
                h2_put_be32(wu, (unsigned)pl_len);
                h2_send_frame(c, H2_FRAME_WINDOW_UPDATE, 0, 0, wu, 4);
                h2_send_frame(c, H2_FRAME_WINDOW_UPDATE, 0, 1, wu, 4);
                c->peer_window += (int)pl_len;
                c->stream_window += (int)pl_len;
            }
            if(flags & H2_FLAG_END_STREAM) stream_done = 1;
        }
        /* PRIORITY / PUSH_PROMISE / unknown: ignored */
    }
    h2_buf_free(&hblk);

    if(!headers_done) return saw_rst ? -2 : -1; /* no response headers received */

    /* --- synthesise an HTTP/1.1-style header block from the decoded fields --- */
    const char *st = "200";
    for(int i = 0; i < c->dec.header_count; i++) {
        if(strcmp(c->dec.headers[i].name, ":status") == 0) {
            st = c->dec.headers[i].value;
            break;
        }
    }
    int stn = atoi(st);
    if(status) *status = stn;
    char line[64];
    int ln = snprintf(line, sizeof(line), "HTTP/2 %d %s\r\n", stn, h2_reason(stn));
    if(ln > 0) h2_buf_append(out_headers, line, (size_t)ln);
    for(int i = 0; i < c->dec.header_count; i++) {
        const char *nm = c->dec.headers[i].name;
        if(nm[0] == ':') continue; /* skip pseudo-headers */
        h2_buf_append(out_headers, nm, strlen(nm));
        h2_buf_append(out_headers, ": ", 2);
        h2_buf_append(out_headers, c->dec.headers[i].value, strlen(c->dec.headers[i].value));
        h2_buf_append(out_headers, "\r\n", 2);
    }
    h2_buf_append(out_headers, "\r\n", 2);
    return 0;
}
