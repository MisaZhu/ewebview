/* Live HTTP/2 interop harness (h5). Links libtinyhttpsc and issues one GET,
 * printing the negotiated path's status/body so we can confirm the h2 client
 * talks to a real server. Enable the h2 path with EWEB_HTTP2=1; without it the
 * same request runs over HTTP/1.1, which makes a clean A/B regression check.
 *
 * Build (host, Apple Silicon arm64 objects already in the .a):
 *   cc -I../include -I../../../build_aarch64/virt/include \
 *      -o /tmp/h2live h2live.c ../../../build_aarch64/virt/lib/libtinyhttpsc.a
 * Run:
 *   EWEB_HTTP2=1 /tmp/h2live https://nghttp2.org/httpbin/get
 */
#include <tinyhttpsc/tinyhttpsc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* BearHttpsClientOne.c's entropy fill calls proc_usleep(); the browser provides
 * it via bin/sdlbrowser/compat.c, which drags in gumbo. Supply the same trivial
 * nanosleep-backed definition here so the harness links libtinyhttpsc alone. */
void proc_usleep(uint32_t us) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    const char *url = (argc > 1) ? argv[1] : "https://nghttp2.org/httpbin/get";
    const char *h2 = getenv("EWEB_HTTP2");
    printf("url=%s  EWEB_HTTP2=%s\n", url, h2 ? h2 : "(unset)");

    TinyHttpsRequest *req = NewHttpsRequest(url);
    if(!req) { printf("FAIL: NewHttpsRequest returned NULL\n"); return 2; }
    HttpsRequestSetTimeout(req, 20000);
    HttpsRequestSetMaxRedirections(req, 3);
    HttpsRequestAddHeader(req, "User-Agent", "ewebview-h2/1");
    HttpsRequestAddHeader(req, "Accept", "*/*");

    TinyHttpsResponse *resp = HttpsRequestFetch(req);
    if(!resp) { printf("FAIL: fetch returned NULL\n"); HttpsRequestFree(req); return 2; }

    if(HttpsResponseError(resp)) {
        printf("ERROR: code=%d msg=%s\n",
               HttpsResponseGetErrorCode(resp),
               HttpsResponseGetErrorMsg(resp) ? HttpsResponseGetErrorMsg(resp) : "(null)");
        HttpsResponseFree(resp); HttpsRequestFree(req);
        return 1;
    }

    int status = HttpsResponseGetStatusCode(resp);
    int size = 0;
    const char *body = HttpsResponseReadBodyStr(resp, &size);
    const char *ct = HttpsResponseGetHeaderValueByKey(resp, "content-type");
    printf("status=%d  body_size=%d  content-type=%s\n", status, size, ct ? ct : "(none)");
    if(body && size > 0) {
        int n = size < 200 ? size : 200;
        printf("body[0..%d]: %.*s\n", n, n, body);
    }
    int ok = (status >= 200 && status < 400 && size > 0);
    printf("%s\n", ok ? "PASS" : "FAIL");

    HttpsResponseFree(resp);
    HttpsRequestFree(req);
    return ok ? 0 : 1;
}
