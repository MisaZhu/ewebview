/* Offline unit test for the HPACK codec (RFC 7541 Appendix C vectors).
 * Build: gcc -I../include -o /tmp/h2test h2test.c ../src/bearhttp2.c
 */
#include "tinyhttpsc/bearhttp2.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg) do { if(!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else printf("ok: %s\n", msg); } while(0)

static void hexdump(const unsigned char *p, size_t n) {
    for(size_t i = 0; i < n; i++) printf("%02x", p[i]);
}

/* C.4.1: Huffman-encoded literal "www.example.com" -> f1e3 c2e5 f23a 6ba0 ab90 f4ff (12 bytes) */
static void test_huffman_known(void) {
    H2Buf b; h2_buf_init(&b);
    const char *s = "www.example.com";
    h2_huffman_encode((const unsigned char *)s, strlen(s), &b);
    printf("huffman(www.example.com) = "); hexdump(b.data, b.len); printf(" (len %zu)\n", b.len);
    const unsigned char expect[] = {0xf1,0xe3,0xc2,0xe5,0xf2,0x3a,0x6b,0xa0,0xab,0x90,0xf4,0xff};
    CHECK(b.len == sizeof(expect) && memcmp(b.data, expect, sizeof(expect)) == 0,
          "C.4.1 huffman encode www.example.com");
    /* round-trip decode */
    H2Buf d; h2_buf_init(&d);
    h2_huffman_decode(b.data, b.len, &d);
    CHECK(d.len == strlen(s) && memcmp(d.data, s, d.len) == 0, "huffman round-trip");
    h2_buf_free(&b); h2_buf_free(&d);
}

/* C.3: literal with incremental indexing, indexed name ":authority"(1),
 * Huffman value "www.example.com" -> 40 88 f1e3 c2e5 f23a 6ba0 ab90 f4ff */
static void test_encode_authority(void) {
    H2Buf b; h2_buf_init(&b);
    h2_hpack_encode_field(&b, ":authority", "www.example.com");
    printf("encode(:authority, www.example.com) = "); hexdump(b.data, b.len); printf("\n");
    /* 0x40 | index(1) = 0x41, then 0x88 (huff,len 12), then 12 bytes */
    CHECK(b.len >= 2 && (b.data[0] & 0xc0) == 0x40 && (b.data[0] & 0x3f) == 1,
          "C.3 literal-with-incremental name index 1");
    CHECK(b.data[1] == 0x8c, "C.3 huffman value len 12 (0x8c)");
    /* decode it back */
    H2HpackDec d; h2_hpack_dec_init(&d);
    int r = h2_hpack_decode_block(&d, b.data, b.len);
    CHECK(r == 0 && d.header_count == 1, "C.3 decode one field");
    if(d.header_count == 1) {
        printf("  decoded: %s = %s\n", d.headers[0].name, d.headers[0].value);
        CHECK(strcmp(d.headers[0].name, ":authority") == 0 &&
              strcmp(d.headers[0].value, "www.example.com") == 0, "C.3 round-trip value");
    }
    h2_hpack_dec_free(&d);
    h2_buf_free(&b);
}

/* Indexed header field: ":method GET" is static index 2 -> single byte 0x82 */
static void test_indexed(void) {
    H2Buf b; h2_buf_init(&b);
    h2_hpack_encode_field(&b, ":method", "GET");
    printf("encode(:method,GET) = "); hexdump(b.data, b.len); printf("\n");
    CHECK(b.len == 1 && b.data[0] == 0x82, "indexed :method GET == 0x82");
    H2HpackDec d; h2_hpack_dec_init(&d);
    unsigned char wire[] = {0x82};
    int r = h2_hpack_decode_block(&d, wire, 1);
    CHECK(r == 0 && d.header_count == 1 && strcmp(d.headers[0].name, ":method") == 0 &&
          strcmp(d.headers[0].value, "GET") == 0, "decode 0x82 -> :method GET");
    h2_hpack_dec_free(&d);
    h2_buf_free(&b);
}

/* Decode the RFC 7541 C.3 first-request wire block and check fields. */
static void test_decode_c3_block(void) {
    /* :method GET (0x82), :scheme http (0x86), :path / (0x84), :authority
     * www.example.com (literal w/ incr, name idx 1 -> 0x41, huffman value 0x88) */
    unsigned char wire[] = {
        0x82, 0x86, 0x84, 0x41, 0x8c,
        0xf1,0xe3,0xc2,0xe5,0xf2,0x3a,0x6b,0xa0,0xab,0x90,0xf4,0xff
    };
    H2HpackDec d; h2_hpack_dec_init(&d);
    int r = h2_hpack_decode_block(&d, wire, sizeof(wire));
    CHECK(r == 0, "C.3 block decode ok");
    printf("  decoded %d fields:\n", d.header_count);
    for(int i = 0; i < d.header_count; i++)
        printf("    %s = %s\n", d.headers[i].name, d.headers[i].value);
    CHECK(d.header_count == 4, "C.3 four fields");
    if(d.header_count == 4) {
        CHECK(strcmp(d.headers[0].name, ":method") == 0 && strcmp(d.headers[0].value, "GET") == 0, "f0 :method GET");
        CHECK(strcmp(d.headers[1].name, ":scheme") == 0 && strcmp(d.headers[1].value, "http") == 0, "f1 :scheme http");
        CHECK(strcmp(d.headers[2].name, ":path") == 0 && strcmp(d.headers[2].value, "/") == 0, "f2 :path /");
        CHECK(strcmp(d.headers[3].name, ":authority") == 0 && strcmp(d.headers[3].value, "www.example.com") == 0, "f3 :authority");
    }
    h2_hpack_dec_free(&d);
}

/* Dynamic table: encode a custom header twice; second time it should be found
 * in the dynamic table (decoder inserts it). Verify decode produces it. */
static void test_dynamic(void) {
    H2Buf b; h2_buf_init(&b);
    h2_hpack_encode_field(&b, "custom-key", "custom-value");
    H2HpackDec d; h2_hpack_dec_init(&d);
    int r = h2_hpack_decode_block(&d, b.data, b.len);
    CHECK(r == 0 && d.header_count == 1 && strcmp(d.headers[0].name, "custom-key") == 0 &&
          strcmp(d.headers[0].value, "custom-value") == 0, "dynamic literal round-trip");
    CHECK(d.dyn_count == 1, "decoder inserted into dynamic table");
    h2_hpack_dec_free(&d);
    h2_buf_free(&b);
}

/* ---------------- framing round-trip over an in-memory transport ---------- */
typedef struct { H2Buf in; size_t in_off; H2Buf out; } MemIo;

static int mem_send(void *ctx, const unsigned char *buf, size_t len) {
    MemIo *io = (MemIo *)ctx;
    return h2_buf_append(&io->out, buf, len);
}
static int mem_recv(void *ctx, unsigned char *buf, size_t len) {
    MemIo *io = (MemIo *)ctx;
    size_t avail = io->in.len - io->in_off;
    if(avail == 0) return 0; /* eof */
    if(len > avail) len = avail;
    memcpy(buf, io->in.data + io->in_off, len);
    io->in_off += len;
    return (int)len;
}
static void put_frame(H2Buf *b, unsigned char type, unsigned char flags,
                      unsigned sid, const unsigned char *pl, size_t len) {
    unsigned char h[9];
    h[0] = (unsigned char)((len >> 16) & 0xff);
    h[1] = (unsigned char)((len >> 8) & 0xff);
    h[2] = (unsigned char)(len & 0xff);
    h[3] = type; h[4] = flags;
    h[5] = (unsigned char)((sid >> 24) & 0xff);
    h[6] = (unsigned char)((sid >> 16) & 0xff);
    h[7] = (unsigned char)((sid >> 8) & 0xff);
    h[8] = (unsigned char)(sid & 0xff);
    h2_buf_append(b, h, 9);
    if(len) h2_buf_append(b, pl, len);
}

static void test_framing(void) {
    MemIo io; h2_buf_init(&io.in); h2_buf_init(&io.out); io.in_off = 0;
    /* Seed the server side of the pipe. */
    put_frame(&io.in, 4, 0, 0, NULL, 0);                 /* server SETTINGS (empty) */
    put_frame(&io.in, 4, 1, 0, NULL, 0);                 /* SETTINGS ack for ours */
    H2Buf hb; h2_buf_init(&hb);                          /* response header block */
    h2_hpack_encode_field(&hb, ":status", "200");
    h2_hpack_encode_field(&hb, "content-type", "text/html");
    put_frame(&io.in, 1, 0x4, 1, hb.data, hb.len);       /* HEADERS END_HEADERS */
    const char *bodytxt = "Hello h2";
    put_frame(&io.in, 0, 0x1, 1, (const unsigned char *)bodytxt, strlen(bodytxt)); /* DATA END_STREAM */
    h2_buf_free(&hb);

    H2Conn c; h2_conn_init(&c, mem_send, mem_recv, &io);
    CHECK(h2_conn_preface(&c) == 0, "preface handshake completes");
    /* client must have emitted the preface + a SETTINGS frame */
    CHECK(io.out.len >= 24 && memcmp(io.out.data, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) == 0,
          "client preface bytes on the wire");

    H2Buf oh, ob; h2_buf_init(&oh); h2_buf_init(&ob);
    const char *extra[] = { "accept", "*/*", "connection", "close" }; /* connection must be dropped */
    int st = 0;
    int r = h2_conn_request(&c, "GET", "https", "www.example.com", "/index.html",
                            extra, 2, NULL, 0, &oh, &ob, &st);
    CHECK(r == 0, "h2_conn_request succeeds");
    CHECK(st == 200, "status parsed as 200");
    oh.data[oh.len] = 0; /* safe: cap > len */
    printf("  synthesised headers:\n%.*s", (int)oh.len, oh.data);
    printf("  body: %.*s\n", (int)ob.len, ob.data);
    CHECK(strstr((char *)oh.data, "HTTP/2 200") != NULL, "status line present");
    CHECK(strstr((char *)oh.data, "content-type: text/html") != NULL, "content-type header present");
    CHECK(ob.len == strlen(bodytxt) && memcmp(ob.data, bodytxt, ob.len) == 0, "response body matches");
    /* "connection" is forbidden in h2 and must not appear in the request bytes */
    CHECK(memmem(io.out.data, io.out.len, "connection", 10) == NULL, "forbidden connection header dropped");

    h2_buf_free(&oh); h2_buf_free(&ob);
    h2_conn_free(&c);
    h2_buf_free(&io.in); h2_buf_free(&io.out);
}

int main(void) {
    test_huffman_known();
    test_indexed();
    test_encode_authority();
    test_decode_c3_block();
    test_dynamic();
    test_framing();
    printf("\n%s (%d failures)\n", fails == 0 ? "ALL PASS" : "FAILURES", fails);
    return fails == 0 ? 0 : 1;
}
