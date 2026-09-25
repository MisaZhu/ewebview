/* Minimal HTTP/2 (RFC 7540) client framing + HPACK (RFC 7541) codec.
 *
 * Self-contained: no third-party dependency. Sits on top of the existing
 * BearSSL TLS transport in BearHttpsClientOne.c. The connection negotiates
 * "h2" via ALPN; when the peer selects it, the request/response exchange is
 * driven through these helpers instead of the HTTP/1.1 text path.
 *
 * Scope (single request/response per connection, which is all the browser
 * port needs today):
 *   - connection preface + SETTINGS exchange (SETTINGS_MAX_FRAME_SIZE honoured)
 *   - one HEADERS stream (stream id 1) with HPACK-encoded request headers
 *   - DATA frames for a request body, respecting the peer flow-control window
 *   - response HEADERS/CONTINUATION decode (HPACK, static table + Huffman)
 *   - response DATA accumulation with WINDOW_UPDATE to keep the stream flowing
 *   - GOAWAY / RST_STREAM / PING handling
 *
 * The decoded response headers are re-serialised into an HTTP/1.1-style block
 * so the existing raw_content/parse_headers/body machinery can be reused
 * verbatim (status line + "key: value\r\n" lines + blank line).
 */
#ifndef BEARHTTP2_H
#define BEARHTTP2_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame types (RFC 7540 sec. 6) */
#define H2_FRAME_DATA          0x0
#define H2_FRAME_HEADERS       0x1
#define H2_FRAME_PRIORITY      0x2
#define H2_FRAME_RST_STREAM    0x3
#define H2_FRAME_SETTINGS      0x4
#define H2_FRAME_PUSH_PROMISE  0x5
#define H2_FRAME_PING          0x6
#define H2_FRAME_GOAWAY        0x7
#define H2_FRAME_WINDOW_UPDATE 0x8
#define H2_FRAME_CONTINUATION  0x9

/* SETTINGS ids (RFC 7540 sec. 6.5.2) */
#define H2_SETTINGS_HEADER_TABLE_SIZE   0x1
#define H2_SETTINGS_ENABLE_PUSH         0x2
#define H2_SETTINGS_MAX_CONCURRENT      0x3
#define H2_SETTINGS_INITIAL_WINDOW_SIZE 0x4
#define H2_SETTINGS_MAX_FRAME_SIZE      0x5
#define H2_SETTINGS_MAX_HEADER_LIST     0x6

#define H2_DEFAULT_FRAME_SIZE   16384
#define H2_DEFAULT_WINDOW       65535
#define H2_FLAG_END_STREAM      0x1
#define H2_FLAG_END_HEADERS     0x4
#define H2_FLAG_ACK             0x1
#define H2_FLAG_PADDED          0x8

/* One decoded response header (points into the HPACK decoder's string pool). */
typedef struct {
    char *name;
    char *value;
} H2Header;

/* Growable byte buffer used for both wire I/O staging and the synthesised
 * HTTP/1.1-style response header block. */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} H2Buf;

void h2_buf_init(H2Buf *b);
void h2_buf_free(H2Buf *b);
int  h2_buf_append(H2Buf *b, const void *p, size_t n);

/* HPACK encoder state (request side). Only the static table is used; entries
 * are emitted as literal-without-indexing so no dynamic table is maintained,
 * which keeps the encoder trivial and stateless across a single request. */
typedef struct {
    unsigned header_table_size; /* from SETTINGS, default 4096 (unused: static only) */
} H2HpackEnc;

void h2_hpack_enc_init(H2HpackEnc *e);
/* Encode one header field into out (HPACK literal, Huffman when smaller). */
int  h2_hpack_encode_field(H2Buf *out, const char *name, const char *value);

/* HPACK decoder state (response side): dynamic table + Huffman decode. */
#define H2_MAX_DYNAMIC 64
typedef struct {
    char *name;
    char *value;
    size_t size; /* RFC 7541 sec. 4.1: len(name)+len(value)+32 */
} H2HpackEntry;

typedef struct {
    H2HpackEntry dyn[H2_MAX_DYNAMIC];
    int dyn_count;          /* number of occupied slots, most-recent first */
    size_t dyn_size;        /* sum of entry sizes */
    unsigned max_table_size;/* SETTINGS_HEADER_TABLE_SIZE, default 4096 */
    /* decoded header list for the current header block */
    H2Header *headers;
    int header_count;
    int header_cap;
    char *pool;             /* backing store for header strings */
    size_t pool_len, pool_cap;
} H2HpackDec;

void h2_hpack_dec_init(H2HpackDec *d);
void h2_hpack_dec_free(H2HpackDec *d);
/* Decode one HPACK header block (the payload of HEADERS/CONTINUATION frames,
 * already de-padded). Appends fields to d->headers. Returns 0 on success. */
int  h2_hpack_decode_block(H2HpackDec *d, const unsigned char *p, size_t n);

/* Connection-level codec driving one request/response over an opaque transport.
 * The caller supplies send/recv that move raw TLS-plaintext bytes. */
typedef int (*H2SendFn)(void *ctx, const unsigned char *buf, size_t len);
typedef int (*H2RecvFn)(void *ctx, unsigned char *buf, size_t len); /* >0 read, 0 eof, <0 err */

typedef struct {
    H2SendFn send;
    H2RecvFn recv;
    void *io_ctx;
    H2HpackDec dec;
    unsigned peer_max_frame;   /* SETTINGS_MAX_FRAME_SIZE from peer */
    int peer_window;           /* connection-level flow-control window */
    int stream_window;         /* stream 1 flow-control window */
    int last_stream_id;        /* from GOAWAY */
    bool goaway;
    unsigned char *rbuf;       /* raw frame staging */
    size_t rbuf_len, rbuf_cap;
} H2Conn;

void h2_conn_init(H2Conn *c, H2SendFn s, H2RecvFn r, void *ctx);
void h2_conn_free(H2Conn *c);

/* Send the client connection preface + our SETTINGS, then read/ACK the peer's
 * SETTINGS. Returns 0 on success. */
int  h2_conn_preface(H2Conn *c);

/* Issue one request. method/route/authority are the pseudo-header values;
 * extra_headers is an array of name/value pairs (count = n_extra). body may be
 * NULL. On success, out_headers receives an HTTP/1.1-style header block
 * ("HTTP/2 <status>\r\nk: v\r\n...\r\n") and out_body the response payload;
 * status receives the numeric :status. Returns 0 on success. */
int  h2_conn_request(H2Conn *c,
                     const char *method, const char *scheme,
                     const char *authority, const char *route,
                     const char *const *extra_headers, int n_extra,
                     const unsigned char *body, size_t body_len,
                     H2Buf *out_headers, H2Buf *out_body, int *status);

/* HPACK Huffman helpers (exposed for unit testing). */
int  h2_huffman_encode(const unsigned char *src, size_t n, H2Buf *out);
int  h2_huffman_decode(const unsigned char *src, size_t n, H2Buf *out);

#ifdef __cplusplus
}
#endif

#endif /* BEARHTTP2_H */
