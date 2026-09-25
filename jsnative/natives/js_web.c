/*
 * js_web.c - Browser "BOM" natives for the mario JavaScript VM.
 *
 * See js_web.h for the contract. Pure C; the embedder context and the DOM
 * callback table come from the DOM bridge (js_dom_ctx()/js_dom_callbacks()),
 * and the extra hooks live in a js_web_state hung off vm->root under
 * WEB_BRIDGE_KEY.
 *
 * Layout note: a browser's `window` IS the global object, so everything this
 * bridge installs has to be reachable both as `window.foo` and as a bare
 * `foo`. js_dom aliases `window` onto vm->root itself, so the two names are
 * one object; singletons are created once and then linked from both, and
 * the value accessors are registered on both targets from one table.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "js_web.h"
#include "js_event.h"
#include "js_natives_priv.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#define CLS_STORAGE   "Storage"
#define CLS_LOCATION  "Location"
#define CLS_HISTORY   "History"
#define CLS_NAVIGATOR "Navigator"
#define CLS_SCREEN    "Screen"
#define CLS_PERF      "Performance"
#define CLS_CONSOLE   "Console"
#define CLS_XHR       "XMLHttpRequest"
#define CLS_ELEMENT   "Element"
/* Class names owned by the sibling bridges, repeated here because the lookup
 * goes through vm->root by name. */
#define CLS_DOCUMENT  "Document"

#define WEB_BRIDGE_KEY "@@web_bridge"
/* Storage entries live in a hidden array of {k,v} objects on the Storage
 * instance itself, so the instance - a member of vm->root - roots them. */
#define STORAGE_ITEMS  "@@items"
#define STORAGE_KEY    "@@k"
#define STORAGE_VAL    "@@v"
/* XHR per-instance state. */
#define XHR_HEADERS    "@@headers"
#define XHR_RESP_HDRS  "@@respHeaders"

/* Fallbacks for the identity strings, so feature detection that reads
 * navigator.userAgent unconditionally always sees something sane. */
#define DEFAULT_UA      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36 MyEwokoBrowser/1.0"
#define DEFAULT_LANG    "en-US"
#define DEFAULT_PLATFORM "Linux x86_64"

typedef struct {
    vm_t*              vm;
    js_web_callbacks_t cb;
    int64_t            t0_ms;       /* performance.timeOrigin */
    var_t*             window;      /* borrowed: the DOM bridge's global */
    var_t*             document;    /* borrowed */
    var_t*             location;    /* borrowed: window.location */
    var_t*             storage_local;
    var_t*             storage_session;
    var_t*             history_state;   /* pushState/replaceState payload */
    var_t*             console_counters; /* console.count() labels -> n */
    var_t*             console_timers;   /* console.time() labels -> ms */
} js_web_state;

static void web_state_free(void* p) {
    js_web_state* st = (js_web_state*)p;
    if(st == NULL) return;
    mario_free(st);
}

static js_web_state* web_state(vm_t* vm) {
    if(vm == NULL || vm->root == NULL) return NULL;
    var_t* bridge = var_find_own_member_var(vm->root, WEB_BRIDGE_KEY);
    return (bridge != NULL) ? (js_web_state*)bridge->value : NULL;
}

static void* web_ctx(vm_t* vm) { return js_dom_ctx(vm); }

/* ------------------------------------------------------------------ */
/* Number / value coercion                                            */
/* ------------------------------------------------------------------ */

/* JS ToNumber(), NaN included. Unlike js_num() (which is for CSS lengths and
 * deliberately maps junk to 0) this follows the spec, because isNaN() and
 * arithmetic helpers must be able to tell "not a number" apart from 0. */
static double js_tonum(var_t* v) {
    if(v == NULL) return NAN;
    switch(v->type) {
        case V_UNDEF: return NAN;
        case V_NULL:  return 0.0;
        case V_BOOL:  return var_get_bool(v) ? 1.0 : 0.0;
        case V_STRING: {
            const char* s = var_get_str(v);
            if(s == NULL) return NAN;
            while(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') s++;
            if(*s == 0) return 0.0;                 /* ToNumber("") == 0 */
            char* end = NULL;
            double d = strtod(s, &end);
            if(end == s) return NAN;
            while(*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r' || *end == '\f' || *end == '\v') end++;
            return (*end == 0) ? d : NAN;           /* trailing junk -> NaN */
        }
        default: return var_get_float64(v);
    }
}

/* Build the narrowest numeric var that holds `d` exactly: small integers stay
 * V_INT (so `parseInt("3") === 3` and string coercion both look right),
 * everything else becomes the canonical double. */
static var_t* js_num_var(vm_t* vm, double d) {
    if(d != d) return var_new_float64(vm, NAN);
    if(d == (double)(int32_t)d && d >= -2147483648.0 && d <= 2147483647.0)
        return var_new_int(vm, (int32_t)d);
    return var_new_float64(vm, d);
}

/* ------------------------------------------------------------------ */
/* URL parsing                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char* protocol;   /* "http:" / "file:" / "" */
    char* hostname;   /* host without the port */
    char* port;       /* "" when absent */
    char* host;       /* hostname[:port] */
    char* pathname;   /* always starts with '/' for a hierarchical URL */
    char* search;     /* "?..." or "" */
    char* hash;       /* "#..." or "" */
    char* origin;     /* "http://host[:port]" or "null" */
} js_url_t;

static void url_free(js_url_t* u) {
    if(u == NULL) return;
    if(u->protocol) mario_free(u->protocol);
    if(u->hostname) mario_free(u->hostname);
    if(u->port)     mario_free(u->port);
    if(u->host)     mario_free(u->host);
    if(u->pathname) mario_free(u->pathname);
    if(u->search)   mario_free(u->search);
    if(u->hash)     mario_free(u->hash);
    if(u->origin)   mario_free(u->origin);
    memset(u, 0, sizeof(*u));
}

/* Split a URL into its parts. Deliberately tolerant: file:// and relative
 * URLs both show up here, and pages read location.pathname off them without
 * checking. Every field is set (possibly to ""), so url_free() is always safe. */
static void url_parse(const char* url, js_url_t* u) {
    memset(u, 0, sizeof(*u));
    u->protocol = js_strdup("");
    u->hostname = js_strdup("");
    u->port     = js_strdup("");
    u->host     = js_strdup("");
    u->pathname = js_strdup("");
    u->search   = js_strdup("");
    u->hash     = js_strdup("");
    u->origin   = js_strdup("null");
    if(url == NULL) return;

    const char* p = url;

    /* scheme: everything up to "://" (or ":" for the opaque ones) */
    const char* colon = strchr(p, ':');
    const char* slash = strchr(p, '/');
    const char* qmark = strchr(p, '?');
    const char* hash  = strchr(p, '#');
    bool has_scheme = (colon != NULL) && (slash == NULL || colon < slash) &&
                      (qmark == NULL || colon < qmark) && (hash == NULL || colon < hash);
    if(has_scheme) {
        char* sch = js_strndup(p, (uint32_t)(colon - p));
        if(sch != NULL) {
            mstr_t* m = mstr_new("");
            mstr_append(m, sch);
            mstr_add(m, ':');
            mario_free(sch);
            mario_free(u->protocol);
            u->protocol = js_strdup(m->cstr);
            mstr_free(m);
        }
        p = colon + 1;
    }

    if(p[0] == '/' && p[1] == '/') {
        /* authority: //host[:port]/path */
        p += 2;
        const char* aend = p;
        while(*aend != 0 && *aend != '/' && *aend != '?' && *aend != '#') aend++;
        const char* pc = (const char*)memchr(p, ':', (size_t)(aend - p));
        if(pc != NULL) {
            mario_free(u->hostname);
            u->hostname = js_strndup(p, (uint32_t)(pc - p));
            mario_free(u->port);
            u->port = js_strndup(pc + 1, (uint32_t)(aend - pc - 1));
        }
        else {
            mario_free(u->hostname);
            u->hostname = js_strndup(p, (uint32_t)(aend - p));
        }
        mario_free(u->host);
        u->host = js_strdup(u->hostname);
        if(u->port != NULL && u->port[0] != 0) {
            mstr_t* m = mstr_new(u->host);
            mstr_add(m, ':');
            mstr_append(m, u->port);
            mario_free(u->host);
            u->host = js_strdup(m->cstr);
            mstr_free(m);
        }
        if(u->protocol != NULL && u->protocol[0] != 0 && u->hostname[0] != 0) {
            mstr_t* m = mstr_new(u->protocol);
            mstr_append(m, "//");
            mstr_append(m, u->host);
            mario_free(u->origin);
            u->origin = js_strdup(m->cstr);
            mstr_free(m);
        }
        p = aend;
    }

    /* fragment first, so it never leaks into search/pathname */
    const char* h2 = strchr(p, '#');
    if(h2 != NULL) {
        mario_free(u->hash);
        u->hash = js_strdup(h2);
        /* pathname/search are the part before '#' */
        char* head = js_strndup(p, (uint32_t)(h2 - p));
        const char* q = (head != NULL) ? strchr(head, '?') : NULL;
        if(q != NULL) {
            mario_free(u->search);
            u->search = js_strdup(q);
            mario_free(u->pathname);
            u->pathname = js_strndup(head, (uint32_t)(q - head));
        }
        else {
            mario_free(u->pathname);
            u->pathname = head;
            head = NULL;
        }
        if(head != NULL) mario_free(head);
    }
    else {
        const char* q = strchr(p, '?');
        if(q != NULL) {
            mario_free(u->search);
            u->search = js_strdup(q);
            mario_free(u->pathname);
            u->pathname = js_strndup(p, (uint32_t)(q - p));
        }
        else {
            mario_free(u->pathname);
            u->pathname = js_strdup(p);
        }
    }
    if(u->pathname == NULL) u->pathname = js_strdup("");
    if(u->pathname[0] == 0 && u->host[0] != 0) {
        mario_free(u->pathname);
        u->pathname = js_strdup("/");
    }
}

/* The current document URL, from the DOM bridge's get_url hook. */
static char* web_current_url(vm_t* vm) {
    const js_dom_callbacks_t* dom = js_dom_callbacks(vm);
    void* ctx = web_ctx(vm);
    if(dom == NULL || dom->get_url == NULL) return js_strdup("");
    char* u = dom->get_url(ctx);
    if(u == NULL) return js_strdup("");
    return u;
}

/* ------------------------------------------------------------------ */
/* Global functions: parseInt / parseFloat / isNaN / isFinite         */
/* ------------------------------------------------------------------ */

static var_t* native_parseInt(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* s = mstr_new("");
    const char* str = js_arg_cstr(env, 0, s);
    int radix = (js_arg_count(env) > 1) ? (int)js_tonum(js_arg(env, 1)) : 0;

    const char* p = str;
    while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
    bool neg = false;
    if(*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if(radix == 0 || radix == 16) {
        if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; radix = 16; }
    }
    if(radix == 0) radix = 10;

    double acc = 0.0;
    bool any = false;
    if(radix >= 2 && radix <= 36) {
        while(*p != 0) {
            char c = *p;
            int d;
            if(c >= '0' && c <= '9') d = c - '0';
            else if(c >= 'a' && c <= 'z') d = c - 'a' + 10;
            else if(c >= 'A' && c <= 'Z') d = c - 'A' + 10;
            else break;
            if(d >= radix) break;
            acc = acc * (double)radix + (double)d;
            any = true;
            p++;
        }
    }
    mstr_free(s);
    if(!any) return var_new_float64(vm, NAN);
    return js_num_var(vm, neg ? -acc : acc);
}

static var_t* native_parseFloat(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* s = mstr_new("");
    const char* str = js_arg_cstr(env, 0, s);
    const char* p = str;
    while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    /* strtod stops at the first byte that cannot be part of a number, which
     * is exactly parseFloat's "trailing junk is ignored" rule. */
    char* end = NULL;
    double d = strtod(p, &end);
    mstr_free(s);
    if(end == p) return var_new_float64(vm, NAN);
    return js_num_var(vm, d);
}

static var_t* native_isNaN(vm_t* vm, var_t* env, void* data) {
    (void)data;
    double d = js_tonum(js_arg(env, 0));
    return var_new_bool(vm, d != d);
}

static var_t* native_isFinite(vm_t* vm, var_t* env, void* data) {
    (void)data;
    double d = js_tonum(js_arg(env, 0));
    return var_new_bool(vm, d == d && d != INFINITY && d != -INFINITY);
}

/* ------------------------------------------------------------------ */
/* URI / base64 encoding                                              */
/* ------------------------------------------------------------------ */

static bool uri_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' ||
           c == '\'' || c == '(' || c == ')';
}
/* Set encodeURI leaves alone but encodeURIComponent escapes. */
static bool uri_reserved(unsigned char c) {
    return c == ';' || c == '/' || c == '?' || c == ':' || c == '@' || c == '&' ||
           c == '=' || c == '+' || c == '$' || c == ',' || c == '[' || c == ']' ||
           c == '#';
}

static void uri_append_escaped(mstr_t* out, unsigned char c) {
    static const char hex[] = "0123456789ABCDEF";
    mstr_add(out, '%');
    mstr_add(out, hex[(c >> 4) & 0xF]);
    mstr_add(out, hex[c & 0xF]);
}

static var_t* uri_encode(vm_t* vm, var_t* env, bool component) {
    mstr_t* s = mstr_new("");
    const char* str = js_arg_cstr(env, 0, s);
    mstr_t* out = mstr_new("");
    for(const unsigned char* p = (const unsigned char*)str; *p != 0; ++p) {
        if(uri_unreserved(*p) || (!component && uri_reserved(*p)))
            mstr_add(out, (char)*p);
        else
            uri_append_escaped(out, *p);
    }
    mstr_free(s);
    var_t* r = var_new_str(vm, out->cstr);
    mstr_free(out);
    return r;
}

static var_t* native_encodeURIComponent(vm_t* vm, var_t* env, void* data) { (void)data; return uri_encode(vm, env, true); }
static var_t* native_encodeURI(vm_t* vm, var_t* env, void* data)           { (void)data; return uri_encode(vm, env, false); }

static int hex_val(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static var_t* uri_decode(vm_t* vm, var_t* env, bool component) {
    mstr_t* s = mstr_new("");
    const char* str = js_arg_cstr(env, 0, s);
    mstr_t* out = mstr_new("");
    for(const char* p = str; *p != 0; ) {
        if(p[0] == '%' && hex_val(p[1]) >= 0 && hex_val(p[2]) >= 0) {
            unsigned char c = (unsigned char)((hex_val(p[1]) << 4) | hex_val(p[2]));
            /* decodeURI must not unescape the reserved set: %2F in a path has
             * to stay %2F or it would change the URL's meaning. */
            if(!component && uri_reserved(c)) {
                mstr_add(out, p[0]); mstr_add(out, p[1]); mstr_add(out, p[2]);
            }
            else {
                mstr_add(out, (char)c);
            }
            p += 3;
        }
        else if(p[0] == '+' && component) {
            /* Only the component decoder treats '+' as a space, matching what
             * application/x-www-form-urlencoded producers emit. */
            mstr_add(out, ' ');
            p += 1;
        }
        else {
            mstr_add(out, *p);
            p += 1;
        }
    }
    mstr_free(s);
    var_t* r = var_new_str(vm, out->cstr);
    mstr_free(out);
    return r;
}

static var_t* native_decodeURIComponent(vm_t* vm, var_t* env, void* data) { (void)data; return uri_decode(vm, env, true); }
static var_t* native_decodeURI(vm_t* vm, var_t* env, void* data)           { (void)data; return uri_decode(vm, env, false); }

/* Legacy escape(): alphanumerics and @*_+-./ pass through, every other byte
 * becomes %XX, and code points above U+00FF become %uXXXX (which is why it
 * has to decode UTF-8 rather than work byte-wise). */
static bool escape_keep(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '@' || c == '*' || c == '_' || c == '+' || c == '-' || c == '.' || c == '/';
}

static var_t* native_escape(vm_t* vm, var_t* env, void* data) {
    (void)data;
    static const char hex[] = "0123456789ABCDEF";
    mstr_t* s = mstr_new("");
    const char* str = js_arg_cstr(env, 0, s);
    mstr_t* out = mstr_new("");
    const unsigned char* p = (const unsigned char*)str;
    while(*p != 0) {
        unsigned char c = *p;
        if(c < 0x80) {
            if(escape_keep(c)) mstr_add(out, (char)c);
            else uri_append_escaped(out, c);
            p += 1;
            continue;
        }
        /* Decode one UTF-8 sequence and emit it as %uXXXX. */
        uint32_t cp = c;
        uint32_t extra = 0;
        if((c >> 5) == 0x6)       { cp = c & 0x1F; extra = 1; }
        else if((c >> 4) == 0xE)  { cp = c & 0x0F; extra = 2; }
        else if((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
        p += 1;
        for(uint32_t i = 0; i < extra && *p != 0; ++i, ++p)
            cp = (cp << 6) | (uint32_t)(*p & 0x3F);
        mstr_append(out, "%u");
        mstr_add(out, hex[(cp >> 12) & 0xF]);
        mstr_add(out, hex[(cp >> 8) & 0xF]);
        mstr_add(out, hex[(cp >> 4) & 0xF]);
        mstr_add(out, hex[cp & 0xF]);
    }
    mstr_free(s);
    var_t* r = var_new_str(vm, out->cstr);
    mstr_free(out);
    return r;
}

static var_t* native_unescape(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* s = mstr_new("");
    const char* str = js_arg_cstr(env, 0, s);
    mstr_t* out = mstr_new("");
    for(const char* p = str; *p != 0; ) {
        if(p[0] == '%' && (p[1] == 'u' || p[1] == 'U') &&
           hex_val(p[2]) >= 0 && hex_val(p[3]) >= 0 && hex_val(p[4]) >= 0 && hex_val(p[5]) >= 0) {
            uint32_t cp = (uint32_t)((hex_val(p[2]) << 12) | (hex_val(p[3]) << 8) |
                                     (hex_val(p[4]) << 4) | hex_val(p[5]));
            /* Re-encode as UTF-8: the rest of the engine speaks UTF-8. */
            if(cp < 0x80) mstr_add(out, (char)cp);
            else if(cp < 0x800) {
                mstr_add(out, (char)(0xC0 | (cp >> 6)));
                mstr_add(out, (char)(0x80 | (cp & 0x3F)));
            }
            else {
                mstr_add(out, (char)(0xE0 | (cp >> 12)));
                mstr_add(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
                mstr_add(out, (char)(0x80 | (cp & 0x3F)));
            }
            p += 6;
        }
        else if(p[0] == '%' && hex_val(p[1]) >= 0 && hex_val(p[2]) >= 0) {
            mstr_add(out, (char)((hex_val(p[1]) << 4) | hex_val(p[2])));
            p += 3;
        }
        else {
            mstr_add(out, *p);
            p += 1;
        }
    }
    mstr_free(s);
    var_t* r = var_new_str(vm, out->cstr);
    mstr_free(out);
    return r;
}

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_index(char c) {
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}

/* btoa(): encode the string's bytes. The spec restricts input to Latin-1 and
 * throws otherwise; emitting the UTF-8 bytes is more useful here and never
 * fails, which matters because a throw would abort the whole page script. */
static var_t* native_btoa(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* s = mstr_new("");
    const char* str = js_arg_cstr(env, 0, s);
    uint32_t len = (uint32_t)strlen(str);
    mstr_t* out = mstr_new("");
    for(uint32_t i = 0; i < len; i += 3) {
        unsigned b0 = (unsigned char)str[i];
        unsigned b1 = (i + 1 < len) ? (unsigned char)str[i+1] : 0;
        unsigned b2 = (i + 2 < len) ? (unsigned char)str[i+2] : 0;
        mstr_add(out, kB64[(b0 >> 2) & 0x3F]);
        mstr_add(out, kB64[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        mstr_add(out, (i + 1 < len) ? kB64[((b1 << 2) | (b2 >> 6)) & 0x3F] : '=');
        mstr_add(out, (i + 2 < len) ? kB64[b2 & 0x3F] : '=');
    }
    mstr_free(s);
    var_t* r = var_new_str(vm, out->cstr);
    mstr_free(out);
    return r;
}

static var_t* native_atob(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* s = mstr_new("");
    const char* str = js_arg_cstr(env, 0, s);
    mstr_t* out = mstr_new("");
    int acc = 0, bits = 0;
    for(const char* p = str; *p != 0; ++p) {
        if(*p == '=' || *p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') continue;
        int v = b64_index(*p);
        if(v < 0) continue;                  /* tolerate malformed input */
        acc = (acc << 6) | v;
        bits += 6;
        if(bits >= 8) {
            bits -= 8;
            mstr_add(out, (char)((acc >> bits) & 0xFF));
        }
    }
    mstr_free(s);
    var_t* r = var_new_str(vm, out->cstr);
    mstr_free(out);
    return r;
}

/* queueMicrotask(fn): rides the DOM bridge's timer table flagged as a microtask,
 * so js_dom_poll_timers drains it ahead of every 0-ms macrotask (setTimeout /
 * MessageChannel) - the spec microtask-checkpoint ordering pages depend on. */
static var_t* native_queueMicrotask(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* fn = js_arg_func(env, 0);
    if(fn != NULL) {
        int id = js_dom_add_microtask(vm, fn);
        if(getenv("MARIO_TIMERDBG") != NULL)
            fprintf(stderr, "[timerdbg] qmt fn=%p -> id=%d\n", (void*)fn, id);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* MessageChannel / MessagePort                                        */
/*                                                                     */
/* React's scheduler and taobao's lib-promise polyfill both flush      */
/* microtasks through a MessageChannel loop (port1.postMessage() ->    */
/* port2.onmessage); with the constructor missing the polyfill's asap  */
/* path degrades and every promise chain stalls, so the mtop bundle    */
/* never finishes init. Posts ride the DOM timer table at 0 ms like    */
/* queueMicrotask(). GC anchoring: the in-flight message lives in a    */
/* hidden @@mc_pending array on the target port, the port itself in    */
/* the bridge's @@mcports array, and the trampoline closure in the     */
/* timer table's @@timers anchor - all three rooted until dispatch.    */
/* ------------------------------------------------------------------ */
#define CLS_MSGPORT   "MessagePort"
#define CLS_MSGCHAN   "MessageChannel"
#define MC_PENDING    "@@mc_pending"
#define MC_PEER       "@@mc_peer"
#define MC_PORTS_KEY  "@@mcports"

typedef struct mc_disp { var_t* port; var_t* bridge; } mc_disp_t;

static void mc_hide(node_t* n) {
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
}

static var_t* mc_pending_of(vm_t* vm, var_t* port, bool create) {
    var_t* pend = var_find_own_member_var(port, MC_PENDING);
    if(pend == NULL && create)
        mc_hide(var_add(port, MC_PENDING, var_new_array(vm)));
    return var_find_own_member_var(port, MC_PENDING);
}

static void mc_anchor(vm_t* vm, var_t* bridge, var_t* port) {
    if(bridge == NULL) return;
    var_t* arr = var_find_own_member_var(bridge, MC_PORTS_KEY);
    if(arr == NULL) {
        mc_hide(var_add(bridge, MC_PORTS_KEY, var_new_array(vm)));
        arr = var_find_own_member_var(bridge, MC_PORTS_KEY);
    }
    if(arr == NULL) return;
    uint32_t sz = var_array_size(arr);
    uint32_t i;
    for(i = 0; i < sz; ++i) {
        node_t* nd = var_array_get(arr, (int32_t)i);
        if(nd != NULL && nd->var == port) return;
    }
    vm->gc.gc_defer++;
    var_array_add(arr, port);
    vm->gc.gc_defer--;
}

/* Rebuild the anchor array without `port` (mirrors js_reanchor_timers). */
static void mc_unanchor(vm_t* vm, var_t* bridge, var_t* port) {
    if(bridge == NULL) return;
    var_t* arr = var_find_own_member_var(bridge, MC_PORTS_KEY);
    if(arr == NULL) return;
    vm->gc.gc_defer++;
    var_t* fresh = var_new_array(vm);
    uint32_t sz = var_array_size(arr);
    uint32_t i;
    for(i = 0; i < sz; ++i) {
        node_t* nd = var_array_get(arr, (int32_t)i);
        if(nd != NULL && nd->var != NULL && nd->var != port)
            var_array_add(fresh, nd->var);
    }
    mc_hide(var_add(bridge, MC_PORTS_KEY, fresh));
    vm->gc.gc_defer--;
}

/* Timer trampoline: deliver one queued message to the port's onmessage. */
static var_t* native_mc_dispatch(vm_t* vm, var_t* env, void* data) {
    (void)env;
    mc_disp_t* d = (mc_disp_t*)data;
    if(d == NULL) return NULL;
    bool mcdbg = getenv("MARIO_MCDBG") != NULL;
    if(mcdbg)
        fprintf(stderr, "[mcdbg] dispatch port=%p\n", (void*)d->port);
    var_t* port = d->port;
    var_t* bridge = d->bridge;
    mario_free(d);
    if(port == NULL || port->status <= V_ST_GC_FREE) {
        if(mcdbg) fprintf(stderr, "[mcdbg] dispatch DROP dead-port\n");
        return NULL;
    }
    var_t* pend = mc_pending_of(vm, port, false);
    var_t* msg = NULL;
    if(pend != NULL && var_array_size(pend) > 0) {
        node_t* nd = var_array_get(pend, 0);
        msg = (nd != NULL) ? nd->var : NULL;
    }
    var_t* handler = var_find_own_member_var(port, "onmessage");
    if(mcdbg)
        fprintf(stderr, "[mcdbg] dispatch handler=%p isfunc=%d pendLeft=%u\n",
            (void*)handler, (handler != NULL && handler->is_func) ? 1 : 0,
            (unsigned)(pend != NULL ? var_array_size(pend) : 0));
    if(handler != NULL && handler->is_func) {
        var_t* ev = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(ev, "type", var_new_str(vm, "message"));
        var_add(ev, "data", (msg != NULL) ? msg : var_new(vm));
        var_add(ev, "origin", var_new_str(vm, ""));
        var_add(ev, "source", var_new_null(vm));
        var_add(ev, "ports", var_new_array(vm));
        var_t* args = var_new_array(vm);
        var_array_add(args, ev);
        var_t* r = call_m_func(vm, port, handler, args);
        if(r != NULL) var_unref(r);
        var_unref(args);
    }
    if(pend != NULL) {
        if(var_array_size(pend) > 0) var_array_del(pend, 0);
        if(var_array_size(pend) == 0) mc_unanchor(vm, bridge, port);
    }
    return NULL;
}

static var_t* native_mc_postMessage(vm_t* vm, var_t* env, void* data) {
    bool mcdbg = getenv("MARIO_MCDBG") != NULL;
    var_t* self = js_this(env);
    if(self == NULL) {
        if(mcdbg) fprintf(stderr, "[mcdbg] post DROP no-self\n");
        return NULL;
    }
    var_t* closed = var_find_own_member_var(self, "@@mc_closed");
    if(closed != NULL && var_get_int(closed) != 0) {
        if(mcdbg) fprintf(stderr, "[mcdbg] post DROP closed self=%p\n", (void*)self);
        return NULL;
    }
    var_t* peer = var_find_own_member_var(self, MC_PEER);
    if(peer == NULL || peer->status <= V_ST_GC_FREE) {
        if(mcdbg) fprintf(stderr, "[mcdbg] post DROP dead-peer self=%p peer=%p\n",
            (void*)self, (void*)peer);
        return NULL;
    }
    var_t* msg = get_func_arg(env, 0);
    var_t* pend = mc_pending_of(vm, peer, true);
    if(pend == NULL) {
        if(mcdbg) fprintf(stderr, "[mcdbg] post DROP no-pend peer=%p\n", (void*)peer);
        return NULL;
    }
    var_array_add(pend, (msg != NULL) ? msg : var_new(vm));
    mc_anchor(vm, (var_t*)data, peer);
    mc_disp_t* d = (mc_disp_t*)mario_malloc(sizeof(mc_disp_t));
    if(d == NULL) return NULL;
    d->port = peer;
    d->bridge = (var_t*)data;
    var_t* tr = var_new_native_func(vm, native_mc_dispatch, d);
    if(mcdbg) {
        var_t* h = var_find_own_member_var(peer, "onmessage");
        fprintf(stderr, "[mcdbg] post self=%p peer=%p tr=%p pend=%u handler=%d\n",
            (void*)self, (void*)peer, (void*)tr,
            (unsigned)var_array_size(pend), (h != NULL && h->is_func) ? 1 : 0);
    }
    int mc_id = js_dom_add_timer(vm, tr, 0, false);
    if(mcdbg)
        fprintf(stderr, "[mcdbg] add_timer -> %d\n", mc_id);
    if(mc_id == 0) {
        mario_free(d);   /* table full: no dispatch, drop the capture */
        var_unref(tr);   /* not anchored: release the trampoline ourselves */
    }
    /* On success js_add_timer's @@timers reanchor holds the ONLY reference to
     * tr (var_new_* starts at refs==0). Unrefing here would drop it to zero and
     * free the trampoline before it fires, so the due slot's cb reads as
     * recycled non-func heap and poll_timers silently skips it - which is
     * exactly how React 18's MessageChannel scheduler never ran. */
    return NULL;
}

static var_t* native_mc_close(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    mc_hide(var_add(self, "@@mc_closed", var_new_int(vm, 1)));
    return NULL;
}

static var_t* native_mc_noop(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return NULL;
}

/* addEventListener("message", f): the single-slot onmessage covers the
 * scheduler/polyfill usage; a second listener on the same port is rare. */
static var_t* native_mc_addEventListener(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    var_t* type = get_func_arg(env, 0);
    var_t* fn = js_arg_func(env, 1);
    if(type == NULL || fn == NULL) return NULL;
    const char* t = var_get_str(type);
    if(t != NULL && strcmp(t, "message") == 0) {
        var_t* cur = var_find_own_member_var(self, "onmessage");
        if(cur == NULL || cur->type == V_NULL)
            var_add(self, "onmessage", fn);
    }
    return NULL;
}

static var_t* mc_new_port(vm_t* vm) {
    var_t* p = new_obj(vm, CLS_MSGPORT, 0);
    if(p == NULL) return NULL;
    var_add(p, "onmessage", var_new_null(vm));
    mc_hide(var_add(p, MC_PENDING, var_new_array(vm)));
    return p;
}

static var_t* native_mc_ctor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    var_t* p1 = mc_new_port(vm);
    var_t* p2 = mc_new_port(vm);
    if(p1 == NULL || p2 == NULL) return NULL;
    mc_hide(var_add(p1, MC_PEER, p2));
    mc_hide(var_add(p2, MC_PEER, p1));
    var_add(self, "port1", p1);
    var_add(self, "port2", p2);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* console extras                                                     */
/*                                                                    */
/* The engine's Console only has write()/log(); every other level pages */
/* use is added here on the Console prototype so both the global        */
/* `console` and any `new Console()` see them.                          */
/* ------------------------------------------------------------------ */

static mstr_t* console_args_to_str(var_t* env) {
    mstr_t* ret = mstr_new("");
    var_t* args = get_func_args(env);
    if(args == NULL || !args->is_array) return ret;
    uint32_t n = var_array_size(args);
    mstr_t* one = mstr_new("");
    for(uint32_t i = 0; i < n; ++i) {
        node_t* nd = var_array_get(args, (int32_t)i);
        if(nd == NULL) continue;
        var_to_str(nd->var, one);
        if(i > 0) mstr_add(ret, ' ');
        mstr_append(ret, one->cstr);
        mstr_reset(one);
    }
    mstr_free(one);
    return ret;
}

static void console_emit(const char* prefix, var_t* env) {
    mstr_t* msg = console_args_to_str(env);
    mstr_t* line = mstr_new("");
    mstr_append(line, prefix);
    mstr_append(line, msg->cstr);
    mstr_add(line, '\n');
    if(_platform_out != NULL) _platform_out(line->cstr);
    mstr_free(line);
    mstr_free(msg);
}

static var_t* native_console_warn(vm_t* vm, var_t* env, void* data)  { (void)vm; (void)data; console_emit("[warn] ", env);  return NULL; }
static var_t* native_console_error(vm_t* vm, var_t* env, void* data) {
    (void)data;
    /* DIAG (temp): identify what page code actually passed to console.error
     * (React logs the raw thrown value during hydration recovery). */
    var_t* a0 = js_arg(env, 0);
    mstr_t* msg = console_args_to_str(env);
    mstr_t* line = mstr_new("");
    mstr_append(line, "[error] ");
    mstr_append(line, msg->cstr);
    if(a0 != NULL && a0->type == V_OBJECT) {
        var_t* nm = var_find_member_var(a0, "name");
        var_t* ms = var_find_member_var(a0, "message");
        char extra[256] = {0};
        snprintf(extra, sizeof(extra)-1, "  {obj name=%s message=%s}",
            (nm != NULL && nm->type == V_STRING) ? var_get_str(nm) : "-",
            (ms != NULL && ms->type == V_STRING) ? var_get_str(ms) : "-");
        mstr_append(line, extra);
    } else if(a0 != NULL) {
        char extra[64] = {0};
        snprintf(extra, sizeof(extra)-1, "  {type=%u}", (unsigned)a0->type);
        mstr_append(line, extra);
        /* DIAG (temp): a bare number passed to console.error - dump the VM
         * call chain so the calling site can be identified. */
        if(a0->type == V_INT) {
            char hb[96] = {0};
            snprintf(hb, sizeof(hb)-1, "[cerrdbg] int=%d scope_top=%d run_scope_base=%d pc=%u\n",
                (int)var_get_int(a0), (int)vm->scope_stack_top, (int)vm->run_scope_base, (unsigned)vm->pc);
            if(_platform_out != NULL) _platform_out(hb);
            if(getenv("MARIO_CERRBC") != NULL) {
                void bc_dump_window(bytecode_t* bc, PC center, PC radius);
                bc_dump_window(&vm->bc, vm->pc, 30);
            }
            scope_t* dsc = (vm->scope_stack_top > 0) ? vm->scope_stack[vm->scope_stack_top - 1] : NULL;
            int dd = 0;
            while(dsc != NULL && dd < 24) {
                char fb[128] = {0};
                snprintf(fb, sizeof(fb)-1,
                    "[cerrdbg]   sc[%d] func=%d block=%d try=%d loop=%d pc=%u func_pc=%u\n",
                    dd, (int)dsc->is_func, (int)dsc->is_block, (int)dsc->is_try, (int)dsc->is_loop,
                    (unsigned)dsc->pc, (dsc->func != NULL ? (unsigned)dsc->func->pc : 0u));
                if(_platform_out != NULL) _platform_out(fb);
                dsc = dsc->prev;
                dd++;
            }
        }
    }
    mstr_add(line, '\n');
    if(_platform_out != NULL) _platform_out(line->cstr);
    mstr_free(line);
    mstr_free(msg);
    return NULL;
}
static var_t* native_console_info(vm_t* vm, var_t* env, void* data)  { (void)vm; (void)data; console_emit("[info] ", env);  return NULL; }
static var_t* native_console_debug(vm_t* vm, var_t* env, void* data) { (void)vm; (void)data; console_emit("[debug] ", env); return NULL; }
static var_t* native_console_trace(vm_t* vm, var_t* env, void* data) { (void)vm; (void)data; console_emit("[trace] ", env); return NULL; }

/* dir(): the engine's var_to_str renders an object as "[object Object]", so
 * structured inspection goes through the JSON serializer instead. */
static var_t* native_console_dir(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    var_t* v = js_arg(env, 0);
    mstr_t* out = mstr_new("");
    if(v != NULL && v->type == V_OBJECT)
        var_to_json_str(v, out, 2, false);
    else
        var_to_str(v, out);
    if(_platform_out != NULL) _platform_out(out->cstr);
    if(_platform_out != NULL) _platform_out("\n");
    mstr_free(out);
    return NULL;
}

static var_t* native_console_assert(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    if(js_truthy(js_arg(env, 0))) return NULL;
    /* The first argument is the condition, so the message starts at 1. */
    mstr_t* msg = mstr_new("Assertion failed");
    if(js_arg_count(env) > 1) {
        mstr_t* s = mstr_new("");
        mstr_append(msg, ": ");
        mstr_append(msg, js_arg_cstr(env, 1, s));
        mstr_free(s);
    }
    mstr_add(msg, '\n');
    if(_platform_out != NULL) _platform_out(msg->cstr);
    mstr_free(msg);
    return NULL;
}

static var_t* native_console_clear(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return NULL;   /* no scrollback to clear on a kernel-log console */
}

static var_t* native_console_noop(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return NULL;   /* group()/groupEnd(): indentation only, nothing to do */
}

/* console.count([label]) - the label counters live in a hidden object on the
 * bridge var so they survive across calls without a C-side table. */
static var_t* native_console_count(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    if(st == NULL || st->console_counters == NULL) return NULL;
    mstr_t* s = mstr_new("");
    const char* label = (js_arg_count(env) > 0) ? js_arg_cstr(env, 0, s) : "default";
    var_t* cur = get_obj(st->console_counters, label);
    int n = (cur != NULL) ? (int)js_tonum(cur) + 1 : 1;
    var_add(st->console_counters, label, var_new_int(vm, n));
    mstr_t* line = mstr_new("");
    mstr_append(line, label);
    mstr_append(line, ": ");
    mstr_append(line, mstr_from_int(n, 10));
    mstr_add(line, '\n');
    if(_platform_out != NULL) _platform_out(line->cstr);
    mstr_free(line);
    mstr_free(s);
    return NULL;
}

static var_t* native_console_countReset(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    if(st == NULL || st->console_counters == NULL) return NULL;
    mstr_t* s = mstr_new("");
    const char* label = (js_arg_count(env) > 0) ? js_arg_cstr(env, 0, s) : "default";
    var_add(st->console_counters, label, var_new_int(vm, 0));
    mstr_free(s);
    return NULL;
}

/* console.time()/timeLog()/timeEnd() */
static var_t* native_console_time(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    if(st == NULL || st->console_timers == NULL) return NULL;
    mstr_t* s = mstr_new("");
    const char* label = (js_arg_count(env) > 0) ? js_arg_cstr(env, 0, s) : "default";
    var_add(st->console_timers, label,
            var_new_float64(vm, (double)(js_dom_monotonic_ms() - st->t0_ms)));
    mstr_free(s);
    return NULL;
}

static bool console_time_report(vm_t* vm, var_t* env, const char* suffix, bool clear) {
    js_web_state* st = web_state(vm);
    if(st == NULL || st->console_timers == NULL) return false;
    mstr_t* s = mstr_new("");
    const char* label = (js_arg_count(env) > 0) ? js_arg_cstr(env, 0, s) : "default";
    var_t* t0 = get_obj(st->console_timers, label);
    if(t0 == NULL) { mstr_free(s); return false; }
    double now = (double)(js_dom_monotonic_ms() - st->t0_ms);
    double elapsed = now - js_tonum(t0);
    mstr_t* line = mstr_new("");
    mstr_append(line, label);
    mstr_append(line, suffix);
    mstr_append(line, mstr_from_float64(elapsed));
    mstr_append(line, "ms\n");
    if(_platform_out != NULL) _platform_out(line->cstr);
    mstr_free(line);
    if(clear) var_add(st->console_timers, label, var_new_float64(vm, now));
    mstr_free(s);
    return true;
}

static var_t* native_console_timeLog(vm_t* vm, var_t* env, void* data) {
    (void)data;
    console_time_report(vm, env, ": ", false);
    return NULL;
}
static var_t* native_console_timeEnd(vm_t* vm, var_t* env, void* data) {
    (void)data;
    console_time_report(vm, env, ": ", true);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Web Storage (localStorage / sessionStorage)                        */
/*                                                                    */
/* Entries are kept as {k,v} objects inside a hidden array on the      */
/* Storage instance. An array (rather than a plain object) keeps        */
/* insertion order - which key(i)/length depend on - and needs no hash    */
/* iteration API. Persistence goes through the embedder as a            */
/* length-prefixed blob, so values may contain any byte.                */
/* ------------------------------------------------------------------ */

static var_t* storage_items(var_t* self) {
    if(self == NULL) return NULL;
    var_t* items = get_obj(self, STORAGE_ITEMS);
    return (items != NULL && items->is_array) ? items : NULL;
}

static int storage_find(var_t* items, const char* key) {
    if(items == NULL || key == NULL) return -1;
    int n = (int)var_array_size(items);
    for(int i = 0; i < n; ++i) {
        node_t* nd = var_array_get(items, i);
        if(nd == NULL || nd->var == NULL) continue;
        var_t* k = get_obj(nd->var, STORAGE_KEY);
        if(k != NULL && k->type == V_STRING) {
            const char* ks = var_get_str(k);
            if(ks != NULL && strcmp(ks, key) == 0) return i;
        }
    }
    return -1;
}

static var_t* storage_entry_at(var_t* items, int i) {
    if(items == NULL || i < 0) return NULL;
    node_t* nd = var_array_get(items, i);
    return (nd != NULL) ? nd->var : NULL;
}

/* Which of the two stores `self` is, so a mutation can be persisted to the
 * right backing store. */
static bool storage_is_session(vm_t* vm, var_t* self) {
    js_web_state* st = web_state(vm);
    return (st != NULL && st->storage_session != NULL && self == st->storage_session);
}

static void storage_persist(vm_t* vm, var_t* self) {
    js_web_state* st = web_state(vm);
    if(st == NULL || st->cb.storage_save == NULL) return;
    bool session = storage_is_session(vm, self);
    char* blob = js_web_storage_dump(vm, session);
    if(blob != NULL) {
        st->cb.storage_save(web_ctx(vm), session, blob);
        mario_free(blob);
    }
}

static var_t* native_storage_getItem(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* items = storage_items(js_this(env));
    mstr_t* s = mstr_new("");
    const char* key = js_arg_cstr(env, 0, s);
    int i = storage_find(items, key);
    mstr_free(s);
    if(i < 0) return var_new_null(vm);
    var_t* v = get_obj(storage_entry_at(items, i), STORAGE_VAL);
    return (v != NULL) ? v : var_new_null(vm);
}

static var_t* native_storage_setItem(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* items = storage_items(self);
    if(items == NULL) return NULL;
    mstr_t* ks = mstr_new("");
    mstr_t* vs = mstr_new("");
    const char* key = js_arg_cstr(env, 0, ks);
    const char* val = js_arg_cstr(env, 1, vs);
    int i = storage_find(items, key);
    if(i < 0) {
        var_t* entry = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(entry, STORAGE_KEY, var_new_str(vm, key));
        var_add(entry, STORAGE_VAL, var_new_str(vm, val));
        var_array_add(items, entry);
    }
    else {
        var_add(storage_entry_at(items, i), STORAGE_VAL, var_new_str(vm, val));
    }
    mstr_free(ks);
    mstr_free(vs);
    storage_persist(vm, self);
    return NULL;
}

static var_t* native_storage_removeItem(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* items = storage_items(self);
    mstr_t* s = mstr_new("");
    const char* key = js_arg_cstr(env, 0, s);
    int i = storage_find(items, key);
    mstr_free(s);
    if(i < 0) return NULL;
    var_array_del(items, i);
    storage_persist(vm, self);
    return NULL;
}

static var_t* native_storage_clear(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* items = storage_items(self);
    if(items != NULL) {
        /* Remove from the back so the shifting indices stay valid. */
        for(int i = (int)var_array_size(items) - 1; i >= 0; --i)
            var_array_del(items, i);
    }
    storage_persist(vm, self);
    return NULL;
}

static var_t* native_storage_key(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* items = storage_items(js_this(env));
    int i = js_arg_int(env, 0);
    var_t* entry = storage_entry_at(items, i);
    if(entry == NULL) return var_new_null(vm);
    var_t* k = get_obj(entry, STORAGE_KEY);
    return (k != NULL) ? k : var_new_null(vm);
}

static var_t* native_storage_get_length(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* items = storage_items(js_this(env));
    return var_new_int(vm, (items != NULL) ? (int)var_array_size(items) : 0);
}

char* js_web_storage_dump(vm_t* vm, bool session) {
    js_web_state* st = web_state(vm);
    mstr_t* out = mstr_new("");
    var_t* items = NULL;
    if(st != NULL)
        items = storage_items(session ? st->storage_session : st->storage_local);
    if(items != NULL) {
        int n = (int)var_array_size(items);
        for(int i = 0; i < n; ++i) {
            var_t* e = storage_entry_at(items, i);
            if(e == NULL) continue;
            var_t* k = get_obj(e, STORAGE_KEY);
            var_t* v = get_obj(e, STORAGE_VAL);
            const char* ks = (k != NULL) ? var_get_str(k) : NULL;
            const char* vs = (v != NULL) ? var_get_str(v) : NULL;
            if(ks == NULL) ks = "";
            if(vs == NULL) vs = "";
            uint32_t kl = (uint32_t)strlen(ks), vl = (uint32_t)strlen(vs);
            mstr_add_int(out, (int)kl, 10); mstr_add(out, ':');
            js_mstr_append(out, ks, kl);
            mstr_add_int(out, (int)vl, 10); mstr_add(out, ':');
            js_mstr_append(out, vs, vl);
        }
    }
    char* r = js_strdup(out->cstr);
    mstr_free(out);
    return (r != NULL) ? r : js_strdup("");
}

void js_web_storage_restore(vm_t* vm, bool session, const char* blob) {
    js_web_state* st = web_state(vm);
    if(st == NULL) return;
    var_t* store = session ? st->storage_session : st->storage_local;
    var_t* items = storage_items(store);
    if(items == NULL) return;
    for(int i = (int)var_array_size(items) - 1; i >= 0; --i)
        var_array_del(items, i);
    if(blob == NULL) return;

    const char* p = blob;
    while(*p != 0) {
        /* "<klen>:<key><vlen>:<value>" - bail out on anything malformed so a
         * truncated blob can never feed a bogus length into memcpy. */
        char* end = NULL;
        long kl = strtol(p, &end, 10);
        if(end == p || *end != ':' || kl < 0) break;
        const char* key = end + 1;
        p = key + kl;
        long vl = strtol(p, &end, 10);
        if(end == p || *end != ':' || vl < 0) break;
        const char* val = end + 1;
        if(*val == 0 && vl > 0) break;
        var_t* entry = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(entry, STORAGE_KEY, var_new_str2(vm, key, (uint32_t)kl));
        var_add(entry, STORAGE_VAL, var_new_str2(vm, val, (uint32_t)vl));
        var_array_add(items, entry);
        p = val + vl;
    }
}

var_t* js_web_storage(vm_t* vm, bool session) {
    js_web_state* st = web_state(vm);
    if(st == NULL) return var_new_null(vm);
    var_t* s = session ? st->storage_session : st->storage_local;
    return (s != NULL) ? s : var_new_null(vm);
}

/* ------------------------------------------------------------------ */
/* location                                                           */
/* ------------------------------------------------------------------ */

/* 0 href, 1 protocol, 2 host, 3 hostname, 4 port, 5 pathname, 6 search,
 * 7 hash, 8 origin. Every one re-reads the document URL, so a navigation
 * that happened outside the VM is reflected without any cache to invalidate. */
static var_t* loc_get(vm_t* vm, var_t* env, void* data) {
    (void)env;
    int which = (int)(intptr_t)data;
    char* url = web_current_url(vm);
    js_url_t u;
    url_parse(url, &u);
    const char* r = "";
    switch(which) {
        case 0:  r = url;        break;
        case 1:  r = u.protocol; break;
        case 2:  r = u.host;     break;
        case 3:  r = u.hostname; break;
        case 4:  r = u.port;     break;
        case 5:  r = u.pathname; break;
        case 6:  r = u.search;   break;
        case 7:  r = u.hash;     break;
        default: r = u.origin;   break;
    }
    var_t* out = var_new_str(vm, (r != NULL) ? r : "");
    url_free(&u);
    mario_free(url);
    return out;
}

static void web_navigate(vm_t* vm, const char* url) {
    js_web_state* st = web_state(vm);
    if(st == NULL || url == NULL || url[0] == 0) return;
    if(st->cb.navigate != NULL) st->cb.navigate(web_ctx(vm), url);
}

/* history.pushState/replaceState URL adoption: updates the URL location.*
 * reports WITHOUT navigating (no refetch, no document teardown). Falls back
 * to a no-op when the embedder provides no soft-update hook. */
static void web_update_url(vm_t* vm, const char* url) {
    js_web_state* st = web_state(vm);
    if(st == NULL || url == NULL || url[0] == 0) return;
    if(st->cb.update_url != NULL) st->cb.update_url(web_ctx(vm), url);
}

static var_t* loc_set_href(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* s = mstr_new("");
    web_navigate(vm, js_arg_cstr(env, 0, s));
    mstr_free(s);
    return NULL;
}

/* location.hash / search / pathname setters splice the new part into the
 * current URL, which is what a browser does before it navigates. */
static var_t* loc_set_part(vm_t* vm, var_t* env, void* data) {
    int which = (int)(intptr_t)data;
    mstr_t* s = mstr_new("");
    const char* v = js_arg_cstr(env, 0, s);
    char* url = web_current_url(vm);
    js_url_t u;
    url_parse(url, &u);

    mstr_t* nu = mstr_new("");
    if(u.protocol[0] != 0) { mstr_append(nu, u.protocol); mstr_append(nu, "//"); }
    mstr_append(nu, u.host);

    const char* pathname = u.pathname;
    const char* search   = u.search;
    const char* hash     = u.hash;
    mstr_t* pbuf = mstr_new("");
    mstr_t* sbuf = mstr_new("");
    mstr_t* hbuf = mstr_new("");
    if(which == 5) { mstr_append(pbuf, v); pathname = pbuf->cstr; }
    if(which == 6) {
        mstr_append(sbuf, (v[0] == '?' || v[0] == 0) ? v : "?");
        if(v[0] != '?' && v[0] != 0) mstr_append(sbuf, v);
        search = sbuf->cstr;
    }
    if(which == 7) {
        mstr_append(hbuf, (v[0] == '#' || v[0] == 0) ? v : "#");
        if(v[0] != '#' && v[0] != 0) mstr_append(hbuf, v);
        hash = hbuf->cstr;
    }
    mstr_append(nu, pathname);
    mstr_append(nu, search);
    mstr_append(nu, hash);

    web_navigate(vm, nu->cstr);
    mstr_free(hbuf);
    mstr_free(sbuf);
    mstr_free(pbuf);
    mstr_free(nu);
    url_free(&u);
    mario_free(url);
    mstr_free(s);
    return NULL;
}

static var_t* native_loc_assign(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* s = mstr_new("");
    web_navigate(vm, js_arg_cstr(env, 0, s));
    mstr_free(s);
    return NULL;
}

static var_t* native_loc_reload(vm_t* vm, var_t* env, void* data) {
    (void)data; (void)env;
    js_web_state* st = web_state(vm);
    if(st == NULL) return NULL;
    if(st->cb.reload != NULL) st->cb.reload(web_ctx(vm));
    else {
        char* url = web_current_url(vm);
        web_navigate(vm, url);
        mario_free(url);
    }
    return NULL;
}

static var_t* native_loc_toString(vm_t* vm, var_t* env, void* data) {
    return loc_get(vm, env, (void*)(intptr_t)0);
}

/* ------------------------------------------------------------------ */
/* history                                                            */
/* ------------------------------------------------------------------ */

static var_t* native_history_get_length(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    js_web_state* st = web_state(vm);
    if(st == NULL || st->cb.history_length == NULL) return var_new_int(vm, 1);
    int n = st->cb.history_length(web_ctx(vm));
    return var_new_int(vm, (n > 0) ? n : 1);
}

static var_t* native_history_get_state(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    js_web_state* st = web_state(vm);
    if(st == NULL || st->history_state == NULL) return var_new_null(vm);
    return st->history_state;
}

static var_t* native_history_back(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    js_web_state* st = web_state(vm);
    if(st != NULL && st->cb.history_back != NULL) st->cb.history_back(web_ctx(vm));
    return NULL;
}

static var_t* native_history_forward(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    js_web_state* st = web_state(vm);
    if(st != NULL && st->cb.history_forward != NULL) st->cb.history_forward(web_ctx(vm));
    return NULL;
}

static var_t* native_history_go(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    if(st == NULL) return NULL;
    int n = js_arg_int(env, 0);
    /* Without a session history stack the only meaningful delta is -1/+1. */
    if(n < 0 && st->cb.history_back != NULL) st->cb.history_back(web_ctx(vm));
    else if(n > 0 && st->cb.history_forward != NULL) st->cb.history_forward(web_ctx(vm));
    return NULL;
}

/* pushState/replaceState cannot create a real session-history entry here, but
 * they must at least remember the state object and drive the navigation, so
 * single-page apps that route through history keep working. */
static var_t* native_history_pushState(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    if(st == NULL) return NULL;
    var_t* state = js_arg(env, 0);
    /* Root the payload on the rooted bridge var, exactly like @@counters and
     * @@local: var_add on the existing key node_replace()s it, taking a ref on
     * the new state and releasing the previous one. The payload is otherwise
     * held only by the call's args, so the next gc would sweep it and leave
     * st->history_state dangling - the recycled (zeroed) var then faults in
     * hash_map_add/hash_map_get on the next state access. */
    var_t* bridge = var_find_own_member_var(vm->root, WEB_BRIDGE_KEY);
    if(bridge != NULL)
        var_add(bridge, "@@state", (state != NULL) ? state : var_new_null(vm));
    st->history_state = state;
    mstr_t* s = mstr_new("");
    const char* url = (js_arg_count(env) > 2) ? js_arg_cstr(env, 2, s) : "";
    if(url[0] != 0) web_update_url(vm, url);
    mstr_free(s);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* navigator / screen                                                 */
/* ------------------------------------------------------------------ */

/* 0 userAgent, 1 appVersion, 2 platform, 3 language, 4 vendor, 5 product,
 * 6 appCodeName, 7 appName, 8 productSub, 9 doNotTrack */
static var_t* nav_get(vm_t* vm, var_t* env, void* data) {
    (void)env;
    int which = (int)(intptr_t)data;
    js_web_state* st = web_state(vm);
    void* ctx = web_ctx(vm);
    char* owned = NULL;
    const char* r = "";

    switch(which) {
        case 0:
            if(st != NULL && st->cb.get_user_agent != NULL) owned = st->cb.get_user_agent(ctx);
            r = (owned != NULL && owned[0] != 0) ? owned : DEFAULT_UA;
            break;
        case 1:
            if(st != NULL && st->cb.get_user_agent != NULL) owned = st->cb.get_user_agent(ctx);
            r = (owned != NULL && owned[0] != 0) ? owned : DEFAULT_UA;
            break;
        case 2:
            if(st != NULL && st->cb.get_platform != NULL) owned = st->cb.get_platform(ctx);
            r = (owned != NULL && owned[0] != 0) ? owned : DEFAULT_PLATFORM;
            break;
        case 3:
            if(st != NULL && st->cb.get_language != NULL) owned = st->cb.get_language(ctx);
            r = (owned != NULL && owned[0] != 0) ? owned : DEFAULT_LANG;
            break;
        case 4:  r = "";                  break;   /* vendor: deprecated, always "" */
        case 5:  r = "Gecko";              break;
        case 6:  r = "Mozilla";            break;
        case 7:  r = "Netscape";           break;
        case 8:  r = "20030107";           break;
        default: r = "";                   break;   /* doNotTrack: unset */
    }
    var_t* out = var_new_str(vm, r);
    if(owned != NULL) mario_free(owned);
    return out;
}

/* Integer navigator/screen facts. 0 hardwareConcurrency, 1 maxTouchPoints,
 * 2 deviceMemory, 3 screen.width, 4 screen.height, 5 availWidth,
 * 6 availHeight, 7 colorDepth, 8 pixelDepth */
static var_t* metric_get(vm_t* vm, var_t* env, void* data) {
    (void)env;
    int which = (int)(intptr_t)data;
    js_web_state* st = web_state(vm);
    void* ctx = web_ctx(vm);
    int w = 0, h = 0, depth = 0;

    switch(which) {
        case 0:  return var_new_int(vm, 1);      /* single-core assumption */
        case 1:  return var_new_int(vm, 0);      /* no touch device hook */
        case 2:  return var_new_int(vm, 0);
        default: break;
    }
    if(st != NULL && st->cb.get_screen != NULL) st->cb.get_screen(ctx, &w, &h, &depth);
    if(w <= 0 || h <= 0) {
        /* No screen hook: fall back to the viewport so media queries and
         * layout maths still have something sane to work with. */
        int vw = 0, vh = 0;
        if(st != NULL && st->cb.get_viewport != NULL) st->cb.get_viewport(ctx, &vw, &vh);
        if(w <= 0) w = vw;
        if(h <= 0) h = vh;
    }
    if(depth <= 0) depth = 32;
    switch(which) {
        case 3:  return var_new_int(vm, w);
        case 4:  return var_new_int(vm, h);
        case 5:  return var_new_int(vm, w);
        case 6:  return var_new_int(vm, h);
        case 7:  return var_new_int(vm, depth);
        default: return var_new_int(vm, depth);
    }
}

static var_t* nav_javaEnabled(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return var_new_bool(vm, false);
}
static var_t* nav_sendBeacon(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return var_new_bool(vm, false);   /* no beacon transport */
}
static var_t* nav_vibrate(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return var_new_bool(vm, false);
}

/* navigator.languages is an array; build it fresh so a page mutating it
 * cannot corrupt anything persistent. */
static var_t* nav_get_languages(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    js_web_state* st = web_state(vm);
    char* lang = (st != NULL && st->cb.get_language != NULL)
               ? st->cb.get_language(web_ctx(vm)) : NULL;
    var_t* arr = var_new_array(vm);
    /* Protect the array while its only reference is this C frame. */
    vm->gc.gc_defer++;
    var_array_add(arr, var_new_str(vm, (lang != NULL && lang[0] != 0) ? lang : DEFAULT_LANG));
    if(lang != NULL) {
        /* Report the bare language too, as browsers do ("en-US", "en"). */
        const char* dash = strchr(lang, '-');
        if(dash != NULL && dash != lang)
            var_array_add(arr, var_new_str2(vm, lang, (uint32_t)(dash - lang)));
        mario_free(lang);
    }
    vm->gc.gc_defer--;
    return arr;
}

static var_t* nav_get_bool(vm_t* vm, var_t* env, void* data) {
    (void)env;
    int which = (int)(intptr_t)data;
    return var_new_bool(vm, which == 0);   /* 0 onLine -> true, 1 cookieEnabled -> true */
}

/* navigator.userAgentData: the Chromium client-hints object. Analytics and
 * bot-detection code reads `navigator.userAgentData.brands` and calls
 * getHighEntropyValues(); leaving it undefined makes the engine look like a
 * pre-Chromium browser and can flip a page onto a degraded path. The brand
 * version is taken from the reported UA so the two never disagree.
 * web_promise() is defined further down (Promise.resolve/reject helper). */
static var_t* web_promise(vm_t* vm, const char* which, var_t* value);

static void ua_brand_list(vm_t* vm, var_t* arr, const char* ver) {
    vm->gc.gc_defer++;
    for(int i = 0; i < 3; ++i) {
        var_t* b = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(b, "brand", var_new_str(vm, (i == 1) ? "Chromium" : "Not A(Brand"));
        /* Chromium's fixed greasing pattern: the minor is "0.0.0" for the
         * grease entry and "<major>.0.0" for the real ones. */
        char v[32];
        if(i == 1) snprintf(v, sizeof(v), "%s.0.0", ver);
        else       snprintf(v, sizeof(v), "%d.0.0", (i == 0) ? 8 : 24);
        var_add(b, "version", var_new_str(vm, v));
        var_array_add(arr, b);
    }
    vm->gc.gc_defer--;
}

static void ua_major(char* out, size_t cap, const char* ua) {
    const char* p = (ua != NULL) ? strstr(ua, "Chrome/") : NULL;
    size_t n = 0;
    if(p != NULL) {
        p += 7;
        while(*p >= '0' && *p <= '9' && n + 1 < cap) out[n++] = *p++;
    }
    if(n == 0 && cap >= 4) { out[n++] = '1'; out[n++] = '5'; out[n++] = '2'; }
    out[n] = 0;
}

/* The high-entropy value object (an owned var). `full` decides between the
 * low-entropy set every browser exposes and the getHighEntropyValues() one. */
static var_t* nav_ua_values(vm_t* vm, int full) {
    js_web_state* st = web_state(vm);
    void* ctx = web_ctx(vm);
    char* owned = (st != NULL && st->cb.get_user_agent != NULL) ? st->cb.get_user_agent(ctx) : NULL;
    char* plat  = (full && st != NULL && st->cb.get_platform != NULL) ? st->cb.get_platform(ctx) : NULL;
    char* lang  = (full && st != NULL && st->cb.get_language != NULL) ? st->cb.get_language(ctx) : NULL;
    char major[16];
    ua_major(major, sizeof(major), (owned != NULL && owned[0] != 0) ? owned : DEFAULT_UA);

    var_t* o = var_new_obj_no_proto(vm, NULL, NULL);
    vm->gc.gc_defer++;
    var_t* brands = var_new_array(vm);
    ua_brand_list(vm, brands, major);
    var_add(o, "brands", brands);
    var_add(o, "mobile", var_new_bool(vm, false));
    var_add(o, "platform", var_new_str(vm,
            (plat != NULL && plat[0] != 0) ? plat : DEFAULT_PLATFORM));
    if(full) {
        var_t* full_list = var_new_array(vm);
        ua_brand_list(vm, full_list, major);
        var_add(o, "architecture",    var_new_str(vm, "x86"));
        var_add(o, "bitness",         var_new_str(vm, "64"));
        var_add(o, "model",           var_new_str(vm, ""));
        var_add(o, "platformVersion", var_new_str(vm, "15.5.0"));
        var_add(o, "uaFullVersion",   var_new_str(vm, major));
        var_add(o, "fullVersionList", full_list);
        var_add(o, "wow64",           var_new_bool(vm, false));
        var_t* lv = var_new_array(vm);
        var_array_add(lv, var_new_str(vm, (lang != NULL && lang[0] != 0) ? lang : DEFAULT_LANG));
        var_add(o, "languages", lv);
    }
    vm->gc.gc_defer--;
    if(owned != NULL) mario_free(owned);
    if(plat  != NULL) mario_free(plat);
    if(lang  != NULL) mario_free(lang);
    return o;
}

static var_t* nav_ua_getHighEntropyValues(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    var_t* o = nav_ua_values(vm, 1);
    var_t* p = web_promise(vm, "resolve", o);
    return (p != NULL) ? p : o;   /* no Promise in the engine: hand back the value */
}

static var_t* nav_ua_toJSON(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return nav_ua_values(vm, 1);
}

static var_t* nav_get_userAgentData(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    var_t* o = nav_ua_values(vm, 0);
    vm->gc.gc_defer++;
    vm_reg_native_on(vm, o, "getHighEntropyValues(h)", nav_ua_getHighEntropyValues, NULL);
    vm_reg_native_on(vm, o, "toJSON()",                nav_ua_toJSON,               NULL);
    vm->gc.gc_defer--;
    return o;
}

static var_t* screen_get_orientation(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    var_t* o = var_new_obj_no_proto(vm, NULL, NULL);
    vm->gc.gc_defer++;
    var_add(o, "type", var_new_str(vm, "landscape-primary"));
    var_add(o, "angle", var_new_int(vm, 0));
    vm->gc.gc_defer--;
    return o;
}

/* ------------------------------------------------------------------ */
/* performance                                                        */
/* ------------------------------------------------------------------ */

static var_t* perf_now(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    js_web_state* st = web_state(vm);
    int64_t t0 = (st != NULL) ? st->t0_ms : 0;
    return var_new_float64(vm, (double)(js_dom_monotonic_ms() - t0));
}

static var_t* perf_timeOrigin_get(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    js_web_state* st = web_state(vm);
    /* Wall-clock ms of VM creation: timeOrigin + now() has to line up with
     * Date.now(), so derive it from the same monotonic reading. */
    int64_t mono = js_dom_monotonic_ms();
    int64_t t0 = (st != NULL) ? st->t0_ms : mono;
    return var_new_float64(vm, (double)(js_dom_wall_ms() - (mono - t0)));
}

static var_t* perf_noop(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return NULL;   /* mark/measure/clearMarks: no timeline is kept */
}

/* performance.getEntries*(): no timeline is kept; hand back an empty list so
 * instrumentation that filters entry types does not throw. */
static var_t* perf_entries_empty(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_array(vm);
}

/* ---- PerformanceObserver ----
 * Bundles read the static `supportedEntryTypes` array during chunk eval (e.g.
 * `PerformanceObserver.supportedEntryTypes.includes("soft-navigation")`). With
 * the global absent the member access lands on an empty object, `.includes`
 * throws, and webpack's chunk-eval wrapper rethrows it - rejecting the chunk
 * promise and silently stalling the whole bootstrap. Provide the class with a
 * real entry-type array; no observation is performed (callbacks never fire). */
static var_t* perfobs_supported_types(vm_t* vm) {
    static const char* const types[] = {"mark", "measure", "navigation", "resource",
                                        "paint", "longtask", "event", "first-input",
                                        "layout-shift", "largest-contentful-paint",
                                        "soft-navigation"};
    var_t* arr = var_new_array(vm);
    vm->gc.gc_defer++;
    for(unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        var_array_add(arr, var_new_str(vm, types[i]));
    vm->gc.gc_defer--;
    return arr;
}

static var_t* native_perfobs_ctor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* obj = var_new_obj(vm, get_obj(env, THIS), NULL, NULL);
    /* Park the callback in a hidden member so takeRecords/observe stay inert. */
    node_t* n = var_add(obj, "@@cb", get_func_arg(env, 0));
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
    return obj;
}

static var_t* native_perfobs_noop(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return NULL;   /* observe/disconnect: no timeline is kept */
}

static var_t* native_perfobs_takeRecords(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_array(vm);
}

static var_t* native_perfobs_supported(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return perfobs_supported_types(vm);
}

/* ------------------------------------------------------------------ */
/* Element argument helper                                            */
/*                                                                    */
/* The DOM bridge keeps element_arg() private, so this repeats its two */
/* checks against the public class lookup: reject anything that is not */
/* an object, then reject anything that is not an Element wrapper.     */
/* ------------------------------------------------------------------ */

static js_element_t web_element_arg(vm_t* vm, var_t* v) {
    if(v == NULL || v->type != V_OBJECT || v->is_func) return NULL;
    var_t* cls = js_dom_element_class(vm);
    if(cls != NULL && !var_instanceof(v, cls)) return NULL;
    return (js_element_t)v->value;
}

/* ToNumber then to int, with the non-finite and out-of-range cases clamped
 * rather than invoking C's undefined float->int conversion. */
static int js_toint(var_t* v) {
    double d = js_tonum(v);
    if(d != d) return 0;
    if(d >= 2147483647.0)  return 2147483647;
    if(d <= -2147483648.0) return -2147483648;
    return (int)d;
}

/* ------------------------------------------------------------------ */
/* window metrics / scrolling / dialogs                               */
/* ------------------------------------------------------------------ */

static void web_viewport(vm_t* vm, int* w, int* h) {
    js_web_state* st = web_state(vm);
    *w = 0; *h = 0;
    if(st != NULL && st->cb.get_viewport != NULL) st->cb.get_viewport(web_ctx(vm), w, h);
}

/* 0 = dark, 1 = light (the litehtml media_features encoding). Must agree with
 * the CSS side: a page that picks its theme through matchMedia while its
 * stylesheets gate on the media query paints a mismatched pair otherwise. */
static int web_color_scheme(vm_t* vm) {
    js_web_state* st = web_state(vm);
    if(st != NULL && st->cb.get_color_scheme != NULL)
        return st->cb.get_color_scheme(web_ctx(vm));
    return 1;   /* no hook: light, the CSS-side default */
}

static void web_scroll_pos(vm_t* vm, int* x, int* y) {
    js_web_state* st = web_state(vm);
    *x = 0; *y = 0;
    if(st != NULL && st->cb.get_scroll != NULL) st->cb.get_scroll(web_ctx(vm), x, y);
}

static void web_scroll_to(vm_t* vm, int x, int y) {
    js_web_state* st = web_state(vm);
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(st != NULL && st->cb.scroll_to != NULL) st->cb.scroll_to(web_ctx(vm), x, y);
}

/* 0 innerWidth, 1 innerHeight, 2 outerWidth, 3 outerHeight, 4 screenX,
 * 5 screenY, 6 scrollX, 7 scrollY, 8 devicePixelRatio, 9 length */
static var_t* win_metric_get(vm_t* vm, var_t* env, void* data) {
    (void)env;
    int which = (int)(intptr_t)data;
    switch(which) {
        case 4: case 5: return var_new_int(vm, 0);      /* no window-manager hook */
        case 8: return var_new_float(vm, 1.0f);         /* 1 CSS px == 1 device px */
        case 9: return var_new_int(vm, 0);              /* no frames, no workers */
        default: break;
    }
    int w = 0, h = 0;
    if(which <= 3) {
        /* No viewport hook: fall back to the screen size, which is what a
         * maximised kiosk browser reports anyway. */
        web_viewport(vm, &w, &h);
        if(w <= 0 || h <= 0) {
            js_web_state* st = web_state(vm);
            int d = 0;
            if(st != NULL && st->cb.get_screen != NULL)
                st->cb.get_screen(web_ctx(vm), &w, &h, &d);
        }
        return var_new_int(vm, (which & 1) ? h : w);
    }
    int sx = 0, sy = 0;
    web_scroll_pos(vm, &sx, &sy);
    return var_new_int(vm, (which == 6) ? sx : sy);
}

/* scrollTo()/scrollBy()/scroll() take either (x, y) or one options object -
 * the object form is what current pages emit, so both have to work. */
static void scroll_args(vm_t* vm, var_t* env, int* x, int* y) {
    (void)vm;
    var_t* a0 = js_arg(env, 0);
    if(a0 != NULL && a0->type == V_OBJECT && !a0->is_array && !a0->is_func) {
        var_t* l = get_obj(a0, "left");
        var_t* t = get_obj(a0, "top");
        var_t* ox = get_obj(a0, "x");
        var_t* oy = get_obj(a0, "y");
        *x = js_toint((l != NULL) ? l : ox);
        *y = js_toint((t != NULL) ? t : oy);
        return;
    }
    *x = js_toint(a0);
    *y = js_toint(js_arg(env, 1));
}

static var_t* native_win_scrollTo(vm_t* vm, var_t* env, void* data) {
    (void)data;
    int x = 0, y = 0;
    scroll_args(vm, env, &x, &y);
    web_scroll_to(vm, x, y);
    return NULL;
}

static var_t* native_win_scrollBy(vm_t* vm, var_t* env, void* data) {
    (void)data;
    int dx = 0, dy = 0, cx = 0, cy = 0;
    scroll_args(vm, env, &dx, &dy);
    web_scroll_pos(vm, &cx, &cy);
    web_scroll_to(vm, cx + dx, cy + dy);
    return NULL;
}

/* moveTo/moveBy/resizeTo/resizeBy/close/stop/blur/focus/print: a browser
 * refuses all of these for a script that did not open the window, so the
 * correct behaviour here is to accept the call and do nothing. print() has no
 * output device to talk to. */
static var_t* native_win_noop(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return NULL;
}

/* window.open(url): there is one window, so this navigates it and hands back
 * `window` - callers that only test the return for null keep working. */
static var_t* native_win_open(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    mstr_t* s = mstr_new("");
    const char* url = js_arg_cstr(env, 0, s);
    if(url[0] != 0) web_navigate(vm, url);
    mstr_free(s);
    return (st != NULL && st->window != NULL) ? st->window : var_new_null(vm);
}

static var_t* native_win_confirm(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    mstr_t* s = mstr_new("");
    const char* msg = js_arg_cstr(env, 0, s);
    /* No dialog hook: answer yes. Refusing by default would make every
     * "are you sure?" gate in a page dead-end. */
    bool ok = (st != NULL && st->cb.confirm != NULL) ? st->cb.confirm(web_ctx(vm), msg) : true;
    mstr_free(s);
    return var_new_bool(vm, ok);
}

static var_t* native_win_prompt(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    mstr_t* s = mstr_new("");
    mstr_t* d = mstr_new("");
    const char* msg = js_arg_cstr(env, 0, s);
    const char* def = (js_arg_count(env) > 1) ? js_arg_cstr(env, 1, d) : "";
    char* answer = (st != NULL && st->cb.prompt != NULL)
                 ? st->cb.prompt(web_ctx(vm), msg, def) : NULL;
    mstr_free(d);
    mstr_free(s);
    var_t* r = (answer != NULL) ? var_new_str(vm, answer) : var_new_null(vm);
    if(answer != NULL) mario_free(answer);
    return r;
}

/* getComputedStyle(el) returns the same CSSStyleDeclaration type el.style
 * does; the DOM bridge's el_get_style callback already resolves computed
 * values, so there is nothing extra to do here. The pseudo-element argument
 * is accepted and ignored - ::before/::after have no box to measure. */
static var_t* native_getComputedStyle(vm_t* vm, var_t* env, void* data) {
    (void)data;
    return js_dom_wrap_style(vm, web_element_arg(vm, js_arg(env, 0)));
}

/* ------------------------------------------------------------------ */
/* matchMedia                                                         */
/*                                                                    */
/* Only the subset pages actually feature-test: a media type plus      */
/* "and"-joined (min-width: Npx) / (max-width: Npx) / (width: Npx) /   */
/* (orientation: ...) / (min-height: ...) conditions, comma-separated  */
/* alternatives OR'd together. A condition we cannot parse reports     */
/* false, which sends the page down its fallback path rather than      */
/* throwing.                                                           */
/* ------------------------------------------------------------------ */

static void mq_trim(char* s) {
    char* p = s;
    while(*p == ' ' || *p == '\t') p++;
    if(p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while(n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
}

/* `c` points just past '('; the condition text runs to the next ')'. */
static bool mq_cond_match(vm_t* vm, const char* c, int vw, int vh) {
    char buf[128];
    size_t i = 0;
    while(c[i] != 0 && c[i] != ')' && i + 1 < sizeof(buf)) { buf[i] = c[i]; ++i; }
    buf[i] = 0;

    char* colon = strchr(buf, ':');
    if(colon == NULL) return false;   /* boolean context like (color): not modelled */
    *colon = 0;
    char* name = buf;
    char* val  = colon + 1;
    mq_trim(name);
    mq_trim(val);

    int n = (int)strtod(val, NULL);   /* "640px" -> 640, strtod stops at 'p' */
    if(js_ascii_casecmp(name, "min-width") == 0)  return vw >= n;
    if(js_ascii_casecmp(name, "max-width") == 0)  return vw <= n;
    if(js_ascii_casecmp(name, "width") == 0)      return vw == n;
    if(js_ascii_casecmp(name, "min-height") == 0) return vh >= n;
    if(js_ascii_casecmp(name, "max-height") == 0) return vh <= n;
    if(js_ascii_casecmp(name, "height") == 0)     return vh == n;
    if(js_ascii_casecmp(name, "orientation") == 0)
        return (js_ascii_casecmp(val, "landscape") == 0) ? (vw >= vh) : (vh > vw);
    /* device-pixel-ratio is pinned to 1, so only "1"/"<=1"/">=1" can match. */
    if(js_ascii_casecmp(name, "device-pixel-ratio") == 0)      return (n == 1);
    if(js_ascii_casecmp(name, "min-device-pixel-ratio") == 0)  return (1 >= n);
    if(js_ascii_casecmp(name, "max-device-pixel-ratio") == 0)  return (1 <= n);
    /* Interaction media features, pinned like litehtml's media_query: the
     * shell is mouse-driven desktop (fine/hover/browser). w3.org gates its
     * advanced stylesheet bootstrap on matchMedia("... (pointer: fine) ...");
     * reporting false left <html> at class="no-js" and hid the nav carets. */
    if(js_ascii_casecmp(name, "pointer") == 0)     return (js_ascii_casecmp(val, "fine") == 0);
    if(js_ascii_casecmp(name, "any-pointer") == 0) return (js_ascii_casecmp(val, "fine") == 0);
    if(js_ascii_casecmp(name, "hover") == 0)       return (js_ascii_casecmp(val, "hover") == 0);
    if(js_ascii_casecmp(name, "any-hover") == 0)   return (js_ascii_casecmp(val, "hover") == 0);
    if(js_ascii_casecmp(name, "display-mode") == 0) return (js_ascii_casecmp(val, "browser") == 0);
    /* prefers-color-scheme reads the same OS-seeded scheme the stylesheet
     * media queries see; github.com's theme bootstrap sets data-color-mode
     * from it and paints the opposite theme from its CSS when it lies. */
    if(js_ascii_casecmp(name, "prefers-color-scheme") == 0)
        return (js_ascii_casecmp(val, "dark") == 0)
                   ? (web_color_scheme(vm) == 0)
                   : (web_color_scheme(vm) != 0);
    return false;
}

static bool mq_eval_one(vm_t* vm, const char* s, size_t len, int vw, int vh) {
    char buf[256];
    if(len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = 0;

    char* p = buf;
    while(*p == ' ' || *p == '\t') p++;
    /* Leading media type. "print" never matches; "not" inverts the rest. */
    bool negate = false;
    char word[32];
    for(;;) {
        size_t wi = 0;
        while(*p != 0 && *p != ' ' && *p != '(' && wi + 1 < sizeof(word)) word[wi++] = *p++;
        word[wi] = 0;
        if(wi == 0) break;
        if(js_ascii_casecmp(word, "not") == 0) { negate = !negate; continue; }
        if(js_ascii_casecmp(word, "only") == 0) continue;
        if(js_ascii_casecmp(word, "and") == 0)  break;
        if(js_ascii_casecmp(word, "print") == 0) return false;
        break;   /* "all" / "screen" / anything else: keep evaluating */
    }

    bool ok = true;
    while(*p != 0) {
        char* open = strchr(p, '(');
        if(open == NULL) break;
        if(!mq_cond_match(vm, open + 1, vw, vh)) { ok = false; break; }
        char* close = strchr(open, ')');
        p = (close != NULL) ? close + 1 : open + 1;
    }
    return negate ? !ok : ok;
}

static bool mq_eval(vm_t* vm, const char* q) {
    if(q == NULL || q[0] == 0) return true;   /* matchMedia("") matches */
    int vw = 0, vh = 0;
    web_viewport(vm, &vw, &vh);
    const char* p = q;
    while(*p != 0) {
        const char* comma = strchr(p, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
        if(len > 0 && mq_eval_one(vm, p, len, vw, vh)) return true;
        p = (comma != NULL) ? comma + 1 : p + len;
    }
    return false;
}

/* CSS.supports() is primarily an optional-feature probe. Without the global
 * object, a probe throws before the caller can choose its fallback path. The
 * renderer has no public declaration parser here, so report unsupported
 * conservatively instead of claiming features that may render incorrectly. */
static var_t* native_css_supports(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_bool(vm, false);
}

/* CSS.escape(ident): the CSSOM serializer for identifiers, used to build a
 * selector out of an untrusted name (`[name="..."]` from a form field, an id
 * with a dot in it). Without it the caller either throws or feeds the raw
 * string to querySelector, which is a syntax error for the common cases. */
static var_t* native_css_escape(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* v = js_arg(env, 0);
    const char* s = (v != NULL) ? var_get_str(v) : NULL;
    if(s == NULL) return var_new_str(vm, "undefined");   /* String(undefined) per spec */
    size_t n = strlen(s);
    char* out = (char*)mario_malloc(n * 6 + 3);
    if(out == NULL) return var_new_str(vm, "");
    size_t o = 0;
    for(size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if(c == 0) {                                  /* NUL -> REPLACEMENT CHAR */
            out[o++] = (char)0xEF; out[o++] = (char)0xBF; out[o++] = (char)0xBD;
        } else if((c >= 0x01 && c <= 0x1F) || c == 0x7F ||
                  (i == 0 && c >= '0' && c <= '9') ||
                  (i == 1 && c >= '0' && c <= '9' && s[0] == '-')) {
            o += (size_t)sprintf(out + o, "\\%x ", (unsigned)c);
        } else if(i == 0 && c == '-' && n == 1) {
            out[o++] = '\\'; out[o++] = '-';
        } else if(c >= 0x80 || c == '-' || c == '_' ||
                  (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            out[o++] = (char)c;
        } else {
            out[o++] = '\\'; out[o++] = (char)c;
        }
    }
    out[o] = 0;
    var_t* r = var_new_str(vm, out);
    mario_free(out);
    return r;
}

static var_t* native_matchMedia(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* s = mstr_new("");
    const char* q = js_arg_cstr(env, 0, s);
    bool matches = mq_eval(vm, q);

    var_t* mql = var_new_obj_no_proto(vm, NULL, NULL);
    vm->gc.gc_defer++;
    var_add(mql, "media", var_new_str(vm, q));
    var_add(mql, "matches", var_new_bool(vm, matches));
    /* The viewport can change after this returns, but there is no re-layout
     * hook to observe, so the listener slots exist only to keep
     * addListener()/addEventListener() from throwing. w3.org's navigation
     * module registers its breakpoint watcher through the legacy
     * addListener(), so the methods must exist or main.js dies mid-init. */
    var_add(mql, "addListener",           var_new_native_func(vm, perf_noop, NULL));
    var_add(mql, "removeListener",        var_new_native_func(vm, perf_noop, NULL));
    var_add(mql, "addEventListener",       var_new_native_func(vm, perf_noop, NULL));
    var_add(mql, "removeEventListener",    var_new_native_func(vm, perf_noop, NULL));
    var_add(mql, "onchange",               var_new_null(vm));
    mstr_free(s);
    vm->gc.gc_defer--;
    return mql;
}

/* ------------------------------------------------------------------ */
/* document extras                                                    */
/* ------------------------------------------------------------------ */

static var_t* native_doc_get_cookie(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    js_web_state* st = web_state(vm);
    char* c = (st != NULL && st->cb.get_cookie != NULL) ? st->cb.get_cookie(web_ctx(vm)) : NULL;
    var_t* r = var_new_str(vm, (c != NULL) ? c : "");
    if(c != NULL) mario_free(c);
    return r;
}

static var_t* native_doc_set_cookie(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    if(st == NULL || st->cb.set_cookie == NULL) return NULL;
    mstr_t* s = mstr_new("");
    const char* c = js_arg_cstr(env, 0, s);
    /* Pass the whole "k=v; path=/; max-age=60" string on, as js_web.h promises:
     * the attributes are exactly what a real cookie store needs (Path/Domain/
     * Max-Age/Expires/SameSite), and an embedder that only wants the pair can
     * cut at the first ';' itself. Stripping them here used to make every
     * attribute silently unreachable from the store.
     *
     * Leading whitespace has to go as well as trailing. The getter hands back a
     * jar joined with "; ", so the ubiquitous delete-everything loop
     *     var a = document.cookie.split(";");
     *     for (i...) document.cookie = a[i].slice(0, eq) + "=";
     * feeds " name=" straight back in. Trimmed only at the end, that reaches the
     * embedder as a *different* cookie name, so the old entry is kept and a
     * stray " name" appears beside it. Trimming here rather than in every
     * embedder keeps the two halves of the accessor symmetric. */
    while(*c == ' ' || *c == '\t') ++c;
    /* Copy before trimming the tail: js_arg_cstr may hand back the argument's
     * own buffer, which must not be written to. */
    mstr_t* pair = mstr_new(c);
    char* trimmed = pair->cstr;
    size_t n = strlen(trimmed);
    while(n > 0 && (trimmed[n-1] == ' ' || trimmed[n-1] == '\t')) trimmed[--n] = 0;
    if(n > 0) st->cb.set_cookie(web_ctx(vm), trimmed);
    mstr_free(pair);
    mstr_free(s);
    return NULL;
}

/* 0 hidden, 1 visibilityState, 2 compatMode, 3 contentType, 4 lastModified */
static var_t* doc_info_get(vm_t* vm, var_t* env, void* data) {
    (void)env;
    int which = (int)(intptr_t)data;
    switch(which) {
        case 0: return var_new_bool(vm, false);          /* never backgrounded */
        case 1: return var_new_str(vm, "visible");
        case 2: return var_new_str(vm, "CSS1Compat");    /* standards mode */
        case 3: return var_new_str(vm, "text/html");
        default: break;
    }
    /* lastModified: the browser format is "MM/DD/YYYY HH:MM:SS", local time. */
    time_t now = (time_t)(js_dom_wall_ms() / 1000);
    struct tm tmv;
    char buf[32];
    if(localtime_r(&now, &tmv) == NULL ||
       strftime(buf, sizeof(buf), "%m/%d/%Y %H:%M:%S", &tmv) == 0)
        return var_new_str(vm, "");
    return var_new_str(vm, buf);
}

static var_t* native_doc_hasFocus(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return var_new_bool(vm, true);   /* the webview owns the input focus */
}

static var_t* native_doc_get_activeElement(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    /* The engine tracks focus, so ask the DOM bridge for the focused element
     * first. Fall back to <body> - what a browser reports when nothing else
     * has focus - then the document root. */
    const js_dom_callbacks_t* dom = js_dom_callbacks(vm);
    void* ctx = web_ctx(vm);
    js_element_t el = NULL;
    if(dom != NULL) {
        if(dom->get_active_element != NULL) el = dom->get_active_element(ctx);
        if(el == NULL && dom->get_body != NULL) el = dom->get_body(ctx);
        if(el == NULL && dom->get_element_by_id != NULL) el = dom->get_element_by_id(ctx, "body");
        if(el == NULL && dom->get_root != NULL) el = dom->get_root(ctx);
    }
    return js_dom_wrap_element(vm, el);
}

/* document.forms/images/scripts/links/anchors/embeds: 0 form, 1 img,
 * 2 script, 3 a[href], 4 a[name], 5 object. All go through the DOM bridge's
 * query_all callback, so they stay in document order like a real collection. */
static var_t* doc_collection_get(vm_t* vm, var_t* env, void* data) {
    (void)env;
    int which = (int)(intptr_t)data;
    static const char* kSelectors[] = {
        "form", "img", "script", "a[href]", "a[name]", "object"
    };
    const js_dom_callbacks_t* dom = js_dom_callbacks(vm);
    void* ctx = web_ctx(vm);
    var_t* arr = var_new_array(vm);
    if(dom == NULL || dom->query_all == NULL) return arr;
    if(which < 0 || which >= (int)(sizeof(kSelectors) / sizeof(kSelectors[0]))) return arr;

    vm->gc.gc_defer++;
    js_element_t buf[JS_DOM_QUERY_CHUNK];
    int skip = 0;
    /* Same page bound the DOM bridge uses: a hard stop against an embedder
     * that keeps reporting a full batch. */
    for(int page = 0; page < 256; ++page) {
        memset(buf, 0, sizeof(buf));
        int n = dom->query_all(ctx, NULL, kSelectors[which], skip, buf, JS_DOM_QUERY_CHUNK);
        if(n <= 0) break;
        if(n > JS_DOM_QUERY_CHUNK) n = JS_DOM_QUERY_CHUNK;
        for(int i = 0; i < n; ++i)
            var_array_add(arr, js_dom_wrap_element(vm, buf[i]));
        if(n < JS_DOM_QUERY_CHUNK) break;
        skip += n;
    }
    vm->gc.gc_defer--;
    return arr;
}

/* ------------------------------------------------------------------ */
/* Promise / class-static helpers                                     */
/* ------------------------------------------------------------------ */

/* vm_reg_static's is_static flag is inert: the engine registers the function
 * on the class PROTOTYPE and relies on var_find_member falling through to it.
 * A C-side call therefore has to do the same lookup by hand. */
static var_t* web_static_func(vm_t* vm, const char* cls_name, const char* func_name) {
    var_t* cls = var_find_own_member_var(vm->root, cls_name);
    if(cls == NULL) return NULL;
    var_t* fn = NULL;
    var_t* proto = var_get_prototype(cls);
    if(proto != NULL) fn = get_obj(proto, func_name);
    if(fn == NULL || !fn->is_func) fn = get_obj(cls, func_name);
    return (fn != NULL && fn->is_func) ? fn : NULL;
}

/* Promise.resolve(value) / Promise.reject(reason). Returns an OWNED var (the
 * caller hands it straight back as the native's result) or NULL when the
 * engine has no Promise, so callers can fall back to the bare value. */
static var_t* web_promise(vm_t* vm, const char* which, var_t* value) {
    var_t* cls = var_find_own_member_var(vm->root, "Promise");
    var_t* fn = web_static_func(vm, "Promise", which);
    if(cls == NULL || fn == NULL) return NULL;
    var_t* args = var_new_array(vm);
    var_array_add(args, value);
    var_t* p = call_m_func(vm, cls, fn, args);
    var_unref(args);
    return p;
}

/* Invoke a zero-argument `on*` handler stored on `self` with `this` bound to
 * it. Same release contract js_event.c uses for listener calls. */
static void web_fire_handler(vm_t* vm, var_t* self, const char* prop) {
    if(self == NULL) return;
    var_t* fn = get_obj(self, prop);
    if(fn == NULL || !fn->is_func) return;
    var_t* args = var_new_array(vm);
    var_t* ret = call_m_func(vm, self, fn, args);
    var_unref(args);
    if(ret != NULL) var_unref(ret);
}

static void web_set_int(vm_t* vm, var_t* self, const char* name, int v) {
    var_add(self, name, var_new_int(vm, v));
}

static void web_set_str(vm_t* vm, var_t* self, const char* name, const char* v) {
    var_add(self, name, var_new_str(vm, (v != NULL) ? v : ""));
}

/* ------------------------------------------------------------------ */
/* XMLHttpRequest                                                     */
/*                                                                    */
/* Synchronous by construction: the embedder's http_request hook blocks */
/* until the response is complete, so send() walks readyState 1 -> 4 in */
/* one call and fires every event before returning. That is exactly the */
/* async == false path a browser supports, and it keeps the bridge free */
/* of a worker thread (there is no JS thread to yield to anyway).       */
/* ------------------------------------------------------------------ */

#define XHR_METHOD "@@method"
#define XHR_URL    "@@url"

/* Case-insensitive "Name: value" lookup inside a "\r\n"-joined header block.
 * Returns a mario_malloc'd copy of the value, or NULL when absent. */
static char* hdr_find(const char* block, const char* name) {
    if(block == NULL || name == NULL) return NULL;
    size_t nlen = strlen(name);
    const char* p = block;
    while(*p != 0) {
        const char* eol = strstr(p, "\r\n");
        size_t line_len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        const char* colon = (const char*)memchr(p, ':', line_len);
        if(colon != NULL) {
            size_t klen = (size_t)(colon - p);
            char key[128];
            if(klen < sizeof(key)) {
                memcpy(key, p, klen);
                key[klen] = 0;
                if(js_ascii_casecmp(key, name) == 0 ||
                   (klen == nlen && js_ascii_casecmp(key, name) == 0)) {
                    const char* v = colon + 1;
                    const char* lend = p + line_len;
                    while(v < lend && (*v == ' ' || *v == '\t')) v++;
                    size_t vlen = (size_t)(lend - v);
                    while(vlen > 0 && (v[vlen-1] == ' ' || v[vlen-1] == '\t')) vlen--;
                    return js_strndup(v, (uint32_t)vlen);
                }
            }
        }
        if(eol == NULL) break;
        p = eol + 2;
    }
    (void)nlen;
    return NULL;
}

static var_t* xhr_req_headers(vm_t* vm, var_t* self) {
    var_t* h = get_obj(self, XHR_HEADERS);
    if(h != NULL && h->is_array) return h;
    vm->gc.gc_defer++;
    h = var_new_array(vm);
    node_t* n = var_add(self, XHR_HEADERS, h);
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
    vm->gc.gc_defer--;
    return h;
}

static var_t* native_xhr_ctor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    web_set_int(vm, self, "readyState", 0);
    web_set_int(vm, self, "status", 0);
    web_set_str(vm, self, "statusText", "");
    web_set_str(vm, self, "responseText", "");
    web_set_str(vm, self, "response", "");
    web_set_str(vm, self, "responseURL", "");
    web_set_str(vm, self, "responseType", "");
    var_add(self, "responseXML", var_new_null(vm));
    var_add(self, "onreadystatechange", var_new_null(vm));
    var_add(self, "onload", var_new_null(vm));
    var_add(self, "onerror", var_new_null(vm));
    var_add(self, "onabort", var_new_null(vm));
    var_add(self, "ontimeout", var_new_null(vm));
    var_add(self, "onloadstart", var_new_null(vm));
    var_add(self, "onloadend", var_new_null(vm));
    xhr_req_headers(vm, self);
    var_add(self, XHR_RESP_HDRS, var_new_str(vm, ""));
    var_add(self, XHR_METHOD, var_new_str(vm, "GET"));
    var_add(self, XHR_URL, var_new_str(vm, ""));
    return NULL;
}

/* readyState transitions fire onreadystatechange, so the assignment and the
 * notification live in one place. */
static void xhr_set_ready(vm_t* vm, var_t* self, int state) {
    web_set_int(vm, self, "readyState", state);
    web_fire_handler(vm, self, "onreadystatechange");
}

static var_t* native_xhr_open(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    mstr_t* m = mstr_new("");
    mstr_t* u = mstr_new("");
    const char* method = js_arg_cstr(env, 0, m);
    const char* url    = js_arg_cstr(env, 1, u);
    web_set_str(vm, self, XHR_METHOD, (method[0] != 0) ? method : "GET");
    web_set_str(vm, self, XHR_URL, url);
    /* open() discards the previous response and any pending request headers. */
    web_set_int(vm, self, "status", 0);
    web_set_str(vm, self, "statusText", "");
    web_set_str(vm, self, "responseText", "");
    web_set_str(vm, self, "response", "");
    web_set_str(vm, self, XHR_RESP_HDRS, "");
    var_t* hdrs = xhr_req_headers(vm, self);
    if(hdrs != NULL)
        for(int i = (int)var_array_size(hdrs) - 1; i >= 0; --i) var_array_del(hdrs, i);
    xhr_set_ready(vm, self, 1);
    mstr_free(u);
    mstr_free(m);
    return NULL;
}

static var_t* native_xhr_setRequestHeader(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    var_t* hdrs = xhr_req_headers(vm, self);
    if(hdrs == NULL) return NULL;
    mstr_t* k = mstr_new("");
    mstr_t* v = mstr_new("");
    const char* key = js_arg_cstr(env, 0, k);
    const char* val = js_arg_cstr(env, 1, v);
    if(key[0] != 0) {
        /* Repeating a header appends ", value" rather than replacing it, which
         * is what the XHR spec asks for (Cookie/Accept stacking). */
        int found = -1;
        int n = (int)var_array_size(hdrs);
        for(int i = 0; i < n; ++i) {
            node_t* nd = var_array_get(hdrs, i);
            if(nd == NULL || nd->var == NULL) continue;
            var_t* ek = get_obj(nd->var, STORAGE_KEY);
            const char* eks = (ek != NULL) ? var_get_str(ek) : NULL;
            if(eks != NULL && js_ascii_casecmp(eks, key) == 0) { found = i; break; }
        }
        if(found >= 0) {
            var_t* e = var_array_get(hdrs, found)->var;
            var_t* ev = get_obj(e, STORAGE_VAL);
            mstr_t* merged = mstr_new((ev != NULL) ? var_get_str(ev) : "");
            mstr_append(merged, ", ");
            mstr_append(merged, val);
            var_add(e, STORAGE_VAL, var_new_str(vm, merged->cstr));
            mstr_free(merged);
        }
        else {
            vm->gc.gc_defer++;
            var_t* entry = var_new_obj_no_proto(vm, NULL, NULL);
            var_add(entry, STORAGE_KEY, var_new_str(vm, key));
            var_add(entry, STORAGE_VAL, var_new_str(vm, val));
            var_array_add(hdrs, entry);
            vm->gc.gc_defer--;
        }
    }
    mstr_free(v);
    mstr_free(k);
    return NULL;
}

static var_t* native_xhr_getResponseHeader(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return var_new_null(vm);
    mstr_t* s = mstr_new("");
    const char* name = js_arg_cstr(env, 0, s);
    var_t* block = get_obj(self, XHR_RESP_HDRS);
    char* v = hdr_find((block != NULL) ? var_get_str(block) : NULL, name);
    mstr_free(s);
    var_t* r = (v != NULL) ? var_new_str(vm, v) : var_new_null(vm);
    if(v != NULL) mario_free(v);
    return r;
}

static var_t* native_xhr_getAllResponseHeaders(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* block = (self != NULL) ? get_obj(self, XHR_RESP_HDRS) : NULL;
    return var_new_str(vm, (block != NULL) ? var_get_str(block) : "");
}

static var_t* native_xhr_overrideMimeType(vm_t* vm, var_t* env, void* data) {
    (void)data; (void)env; (void)vm;
    return NULL;   /* the response is always handed back as text */
}

static var_t* native_xhr_abort(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    /* send() is blocking, so abort() can only ever run before or after it.
     * Reset to UNSENT and notify, which is what a browser leaves behind. */
    web_set_int(vm, self, "readyState", 0);
    web_fire_handler(vm, self, "onabort");
    web_fire_handler(vm, self, "onreadystatechange");
    return NULL;
}

static var_t* native_xhr_send(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    var_t* self = js_this(env);
    if(st == NULL || self == NULL) return NULL;

    var_t* mv = get_obj(self, XHR_METHOD);
    var_t* uv = get_obj(self, XHR_URL);
    const char* method = (mv != NULL) ? var_get_str(mv) : "GET";
    const char* url    = (uv != NULL) ? var_get_str(uv) : "";
    if(method == NULL) method = "GET";
    if(url == NULL || url[0] == 0) return NULL;

    /* Flatten the request header array into the "\r\n" block the hook wants. */
    mstr_t* hdrs = mstr_new("");
    var_t* hl = xhr_req_headers(vm, self);
    if(hl != NULL) {
        int n = (int)var_array_size(hl);
        for(int i = 0; i < n; ++i) {
            node_t* nd = var_array_get(hl, i);
            if(nd == NULL || nd->var == NULL) continue;
            var_t* k = get_obj(nd->var, STORAGE_KEY);
            var_t* v = get_obj(nd->var, STORAGE_VAL);
            const char* ks = (k != NULL) ? var_get_str(k) : NULL;
            const char* vs = (v != NULL) ? var_get_str(v) : NULL;
            if(ks == NULL || ks[0] == 0) continue;
            mstr_append(hdrs, ks);
            mstr_append(hdrs, ": ");
            mstr_append(hdrs, (vs != NULL) ? vs : "");
            mstr_append(hdrs, "\r\n");
        }
    }

    mstr_t* body = mstr_new("");
    const char* bs = NULL;
    var_t* bv = (js_arg_count(env) > 0) ? js_arg(env, 0) : NULL;
    if(bv != NULL && bv->type != V_UNDEF && bv->type != V_NULL) {
        bs = js_arg_cstr(env, 0, body);
    }

    web_fire_handler(vm, self, "onloadstart");
    xhr_set_ready(vm, self, 1);

    int status = 0;
    char* out_body = NULL;
    char* out_hdrs = NULL;
    /* No hook at all: report a network failure (status 0), which is what a
     * browser does for an unreachable URL, rather than throwing. */
    if(getenv("MARIO_HTTPDBG") != NULL) {
        fprintf(stderr, "[httpdbg] XHR %s %s\n", method, url ? url : "(null)");
        /* The JS-set header block: shows whether the page supplied its own
         * Content-Type / X-CSRFToken / Accept before the hook adds Cookie. */
        if(hdrs->len > 0)
            fprintf(stderr, "[httpdbg] reqhdrs:\n%s", hdrs->cstr);
    }
    bool ok = (st->cb.http_request != NULL) &&
              st->cb.http_request(web_ctx(vm), method, url,
                                  (hdrs->len > 0) ? hdrs->cstr : NULL, bs,
                                  &status, &out_body, &out_hdrs);
    mstr_free(body);
    mstr_free(hdrs);

    if(ok) {
        web_set_str(vm, self, XHR_RESP_HDRS, (out_hdrs != NULL) ? out_hdrs : "");
        web_set_int(vm, self, "status", status);
        web_set_str(vm, self, "statusText", "");
        web_set_str(vm, self, "responseText", (out_body != NULL) ? out_body : "");
        web_set_str(vm, self, "response",     (out_body != NULL) ? out_body : "");
        web_set_str(vm, self, "responseURL", url);
        xhr_set_ready(vm, self, 2);
        xhr_set_ready(vm, self, 3);
        xhr_set_ready(vm, self, 4);
        web_fire_handler(vm, self, "onload");
    }
    else {
        /* Failed request: readyState 4 with status 0, per the XHR spec. */
        web_set_str(vm, self, XHR_RESP_HDRS, "");
        web_set_int(vm, self, "status", 0);
        web_set_str(vm, self, "statusText", "");
        web_set_str(vm, self, "responseText", "");
        web_set_str(vm, self, "response", "");
        xhr_set_ready(vm, self, 4);
        web_fire_handler(vm, self, "onerror");
    }
    web_fire_handler(vm, self, "onloadend");

    if(out_body != NULL) mario_free(out_body);
    if(out_hdrs != NULL) mario_free(out_hdrs);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* fetch() / Response / Headers                                       */
/*                                                                    */
/* Built in C on top of the same synchronous hook rather than as a JS   */
/* shim: the Response methods have to close over the response body, and */
/* a per-instance hidden member does that without relying on the        */
/* engine's closure handling during a registration-time prelude.        */
/* ------------------------------------------------------------------ */

#define CLS_RESPONSE "Response"
#define CLS_HEADERS  "Headers"
#define RESP_TEXT    "@@text"
#define RESP_HDR_OBJ "@@headers"

static var_t* headers_new(vm_t* vm, const char* block) {
    var_t* h = new_obj(vm, CLS_HEADERS, 0);
    if(h == NULL) return var_new_obj_no_proto(vm, NULL, NULL);
    var_add(h, XHR_RESP_HDRS, var_new_str(vm, (block != NULL) ? block : ""));
    return h;
}

static var_t* native_headers_get(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    mstr_t* s = mstr_new("");
    const char* name = js_arg_cstr(env, 0, s);
    var_t* block = (self != NULL) ? get_obj(self, XHR_RESP_HDRS) : NULL;
    char* v = hdr_find((block != NULL) ? var_get_str(block) : NULL, name);
    mstr_free(s);
    var_t* r = (v != NULL) ? var_new_str(vm, v) : var_new_null(vm);
    if(v != NULL) mario_free(v);
    return r;
}

static var_t* native_headers_has(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    mstr_t* s = mstr_new("");
    const char* name = js_arg_cstr(env, 0, s);
    var_t* block = (self != NULL) ? get_obj(self, XHR_RESP_HDRS) : NULL;
    char* v = hdr_find((block != NULL) ? var_get_str(block) : NULL, name);
    mstr_free(s);
    bool has = (v != NULL);
    if(v != NULL) mario_free(v);
    return var_new_bool(vm, has);
}

static var_t* response_new(vm_t* vm, int status, const char* url,
                           const char* body, const char* hdrs) {
    var_t* r = new_obj(vm, CLS_RESPONSE, 0);
    if(r == NULL) return var_new_obj_no_proto(vm, NULL, NULL);
    web_set_int(vm, r, "status", status);
    web_set_int(vm, r, "redirected", 0);
    var_add(r, "ok", var_new_bool(vm, status >= 200 && status < 300));
    web_set_str(vm, r, "statusText", "");
    web_set_str(vm, r, "url", (url != NULL) ? url : "");
    web_set_str(vm, r, "type", "basic");
    var_add(r, RESP_TEXT, var_new_str(vm, (body != NULL) ? body : ""));
    var_add(r, RESP_HDR_OBJ, headers_new(vm, hdrs));
    return r;
}

/* text()/json() hand back a Promise, which is the only part of the fetch
 * contract pages actually depend on. */
static var_t* native_resp_text(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* t = (self != NULL) ? get_obj(self, RESP_TEXT) : NULL;
    var_t* s = (t != NULL) ? t : var_new_str(vm, "");
    var_t* p = web_promise(vm, "resolve", s);
    return (p != NULL) ? p : s;
}

static var_t* native_resp_json(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* t = (self != NULL) ? get_obj(self, RESP_TEXT) : NULL;
    var_t* json_cls = var_find_own_member_var(vm->root, "JSON");
    var_t* parse = web_static_func(vm, "JSON", "parse");
    var_t* parsed = NULL;
    if(json_cls != NULL && parse != NULL && t != NULL) {
        var_t* args = var_new_array(vm);
        var_array_add(args, t);
        parsed = call_m_func(vm, json_cls, parse, args);
        var_unref(args);
        if(getenv("MARIO_RESPJSONDBG") != NULL) {
            const char* ts = var_get_str(t);
            int has_n = 0, has_list = 0;
            if(parsed != NULL && parsed->type == V_OBJECT) {
                var_t* nn = var_find_own_member_var(parsed, "n");
                var_t* ll = var_find_own_member_var(parsed, "list");
                has_n = (nn != NULL); has_list = (ll != NULL);
            }
            fprintf(stderr, "[respjson] tlen=%d ts=%.40s parsed=%p type=%d has_n=%d has_list=%d\n",
                ts ? (int)strlen(ts) : -1, ts ? ts : "(null)",
                (void*)parsed, parsed ? (int)parsed->type : -1, has_n, has_list);
        }
    }
    if(parsed == NULL || parsed->type == V_UNDEF) {
        /* Malformed body: reject, exactly as JSON.parse throwing would. */
        var_t* reason = var_new_str(vm, "SyntaxError: unexpected token in JSON");
        var_t* p = web_promise(vm, "reject", reason);
        return (p != NULL) ? p : reason;
    }
    var_t* p = web_promise(vm, "resolve", parsed);
    if(p != NULL) return p;
    return parsed;
}

static var_t* native_resp_get_headers(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    var_t* self = js_this(env);
    var_t* h = (self != NULL) ? get_obj(self, RESP_HDR_OBJ) : NULL;
    return (h != NULL) ? h : headers_new(vm, "");
}

/* hash_map_iterate callback that flattens one `{Name: value}` entry into the
 * "\r\n"-joined header block. Hidden/internal members (the "@@"-prefixed ones
 * the bridges hang state off) are skipped so they can never leak into a
 * request. */
typedef struct { mstr_t* out; } hdr_obj_ctx;

static void hdr_obj_each(const char* key, void* value, void* user_data) {
    hdr_obj_ctx* hc = (hdr_obj_ctx*)user_data;
    if(hc == NULL || hc->out == NULL || key == NULL || key[0] == 0) return;
    if(key[0] == '@') return;
    node_t* nd = (node_t*)value;
    if(nd == NULL || nd->var == NULL) return;
    if(nd->invisable || nd->be_unenumerable) return;
    mstr_t* tmp = mstr_new("");
    mstr_append(hc->out, key);
    mstr_append(hc->out, ": ");
    mstr_append(hc->out, js_cstr(nd->var, tmp));
    mstr_append(hc->out, "\r\n");
    mstr_free(tmp);
}

static var_t* native_fetch(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_web_state* st = web_state(vm);
    if(st == NULL) return var_new_null(vm);

    /* fetch(url) or fetch(Request-ish object with a .url). */
    mstr_t* us = mstr_new("");
    var_t* a0 = js_arg(env, 0);
    const char* url = js_arg_cstr(env, 0, us);
    if((a0 != NULL && a0->type == V_OBJECT) && (url == NULL || url[0] == 0)) {
        var_t* u = get_obj(a0, "url");
        if(u != NULL) url = var_get_str(u);
    }
    char method[16] = "GET";
    mstr_t* body = mstr_new("");
    mstr_t* hdrs = mstr_new("");
    const char* bs = NULL;

    var_t* init = js_arg(env, 1);
    if(init != NULL && init->type == V_OBJECT) {
        var_t* m = get_obj(init, "method");
        if(m != NULL && m->type == V_STRING) {
            const char* ms = var_get_str(m);
            if(ms != NULL && ms[0] != 0) snprintf(method, sizeof(method), "%s", ms);
        }
        var_t* b = get_obj(init, "body");
        if(b != NULL && b->type != V_UNDEF && b->type != V_NULL) bs = js_cstr(b, body);
        /* headers: a plain {Name: value} object, or a "N: v\r\n" string. */
        var_t* h = get_obj(init, "headers");
        if(h != NULL) {
            if(h->type == V_STRING) {
                const char* hs = var_get_str(h);
                if(hs != NULL) mstr_append(hdrs, hs);
            }
            else if(h->is_array) {
                /* [[name, value], ...] */
                int n = (int)var_array_size(h);
                for(int i = 0; i < n; ++i) {
                    node_t* nd = var_array_get(h, i);
                    if(nd == NULL || nd->var == NULL || !nd->var->is_array) continue;
                    node_t* kn = var_array_get(nd->var, 0);
                    node_t* vn = var_array_get(nd->var, 1);
                    if(kn == NULL || kn->var == NULL) continue;
                    mstr_t* tmp = mstr_new("");
                    mstr_append(hdrs, js_cstr(kn->var, tmp));
                    mstr_append(hdrs, ": ");
                    mstr_reset(tmp);
                    if(vn != NULL && vn->var != NULL) mstr_append(hdrs, js_cstr(vn->var, tmp));
                    mstr_append(hdrs, "\r\n");
                    mstr_free(tmp);
                }
            }
            else {
                /* {Name: value, ...}: node_t has no sibling link, so the walk
                 * goes through the children hash map. */
                hdr_obj_ctx hc;
                hc.out = hdrs;
                hash_map_iterate(&h->children, hdr_obj_each, &hc);
            }
        }
    }

    int status = 0;
    char* out_body = NULL;
    char* out_hdrs = NULL;
    if(getenv("MARIO_HTTPDBG") != NULL) {
        fprintf(stderr, "[httpdbg] FETCH %s %s\n", method, url ? url : "(null)");
        fprintf(stderr, "[httpdbg] fetch hdrs(len=%d):\n%s", (int)hdrs->len,
                (hdrs->len > 0) ? hdrs->cstr : "(empty)\n");
    }
    bool ok = (st->cb.http_request != NULL) &&
              st->cb.http_request(web_ctx(vm), method, url,
                                  (hdrs->len > 0) ? hdrs->cstr : NULL, bs,
                                  &status, &out_body, &out_hdrs);
    mstr_free(hdrs);
    mstr_free(body);

    var_t* result = NULL;
    if(ok) {
        result = response_new(vm, status, url,
                              (out_body != NULL) ? out_body : "",
                              (out_hdrs != NULL) ? out_hdrs : "");
    }
    mstr_free(us);
    if(out_body != NULL) mario_free(out_body);
    if(out_hdrs != NULL) mario_free(out_hdrs);

    if(!ok) {
        /* A network failure rejects; an HTTP 4xx/5xx resolves with ok == false,
         * matching fetch's contract. */
        var_t* reason = var_new_str(vm, "TypeError: network request failed");
        var_t* p = web_promise(vm, "reject", reason);
        if(p != NULL) return p;
        return reason;
    }
    var_t* p = web_promise(vm, "resolve", result);
    if(p != NULL) return p;
    return result;
}

/* ------------------------------------------------------------------ */
/* URLSearchParams / URL                                              */
/*                                                                    */
/* Both are real browser globals that pages (and core-js's URL        */
/* modules) reference by bare name. mario resolves a bare global      */
/* against vm->root, NOT against `window`/globalThis, so a polyfill   */
/* that only does `globalThis.URLSearchParams = ...` stays invisible  */
/* to `var kO = URLSearchParams` - which is exactly the top-level     */
/* reference in core-js's web.url-search-params.iterable module that  */
/* surfaced as `Uncaught Error: 'URLSearchParams' undefined!`. These  */
/* natives live on vm->root (vm_new_class) so the bare name resolves. */
/* ------------------------------------------------------------------ */

#define CLS_URLSP   "URLSearchParams"
#define CLS_URL     "URL"
#define USP_ENTRIES "@@entries"   /* array of [name, value] string pairs */
#define URL_SP      "@@sp"        /* URL's live URLSearchParams */

/* application/x-www-form-urlencoded serializer: keep the alphanumerics and
 * "*-._", turn space into '+', percent-escape everything else (upper hex). */
static bool usp_plain(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_';
}
static void usp_encode_into(mstr_t* out, const char* s) {
    if(s == NULL) return;
    static const char hex[] = "0123456789ABCDEF";
    for(const unsigned char* p = (const unsigned char*)s; *p != 0; ++p) {
        if(usp_plain(*p))       mstr_add(out, (char)*p);
        else if(*p == ' ')      mstr_add(out, '+');
        else {
            mstr_add(out, '%');
            mstr_add(out, hex[(*p >> 4) & 0xF]);
            mstr_add(out, hex[*p & 0xF]);
        }
    }
}
/* Inverse: '+' -> space, %XX -> byte; a stray '%' that is not a valid escape
 * is kept literally (browsers are tolerant here). */
static void usp_decode_into(mstr_t* out, const char* s) {
    if(s == NULL) return;
    for(const char* p = s; *p != 0; ) {
        if(*p == '+') { mstr_add(out, ' '); ++p; }
        else if(p[0] == '%' && hex_val(p[1]) >= 0 && hex_val(p[2]) >= 0) {
            mstr_add(out, (char)((hex_val(p[1]) << 4) | hex_val(p[2])));
            p += 3;
        }
        else { mstr_add(out, *p); ++p; }
    }
}

static var_t* usp_entries(var_t* self) {
    return (self != NULL) ? var_find_own_member_var(self, USP_ENTRIES) : NULL;
}
static const char* usp_pair_at(var_t* pair, int idx) {
    if(pair == NULL) return "";
    node_t* n = var_array_get(pair, idx);
    if(n == NULL || n->var == NULL) return "";
    const char* s = var_get_str(n->var);
    return (s != NULL) ? s : "";
}
#define usp_pair_name(pair)  usp_pair_at((pair), 0)
#define usp_pair_value(pair) usp_pair_at((pair), 1)

/* Freshly-created vars start at refs==0 and var_array_add()/var_add() ref them
 * to 1, transferring ownership to the container - so no matching var_unref()
 * (the storage_new()/response_new() convention). */
static void usp_add_entry(vm_t* vm, var_t* entries, const char* name, const char* value) {
    if(entries == NULL) return;
    var_t* pair = var_new_array(vm);
    var_array_add(pair, var_new_str(vm, (name != NULL) ? name : ""));
    var_array_add(pair, var_new_str(vm, (value != NULL) ? value : ""));
    var_array_add(entries, pair);
}

static void usp_replace_entries(var_t* self, var_t* keep) {
    node_t* n = var_add(self, USP_ENTRIES, keep);
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
}

static void usp_serialize_into(var_t* e, mstr_t* out) {
    if(e == NULL) return;
    int cnt = (int)var_array_size(e);
    bool first = true;
    for(int i = 0; i < cnt; ++i) {
        node_t* pn = var_array_get(e, i);
        if(pn == NULL || pn->var == NULL) continue;
        if(!first) mstr_add(out, '&');
        first = false;
        usp_encode_into(out, usp_pair_name(pn->var));
        mstr_add(out, '=');
        usp_encode_into(out, usp_pair_value(pn->var));
    }
}

/* Parse a query string ("?a=1&b=2" or "a=1&b=2") into the entries array. */
static void usp_parse(vm_t* vm, var_t* entries, const char* q) {
    if(q == NULL || entries == NULL) return;
    if(*q == '?') ++q;
    const char* p = q;
    while(*p != 0) {
        const char* amp = strchr(p, '&');
        const char* seg_end = (amp != NULL) ? amp : p + strlen(p);
        if(seg_end > p) {
            const char* eq = (const char*)memchr(p, '=', (size_t)(seg_end - p));
            char* rawn;
            char* rawv;
            if(eq != NULL) {
                rawn = js_strndup(p, (uint32_t)(eq - p));
                rawv = js_strndup(eq + 1, (uint32_t)(seg_end - eq - 1));
            }
            else {
                rawn = js_strndup(p, (uint32_t)(seg_end - p));
                rawv = js_strdup("");
            }
            mstr_t* nm = mstr_new("");
            mstr_t* vv = mstr_new("");
            usp_decode_into(nm, rawn);
            usp_decode_into(vv, rawv);
            usp_add_entry(vm, entries, nm->cstr, vv->cstr);
            mstr_free(nm);
            mstr_free(vv);
            if(rawn != NULL) mario_free(rawn);
            if(rawv != NULL) mario_free(rawv);
        }
        if(amp == NULL) break;
        p = amp + 1;
    }
}

typedef struct { vm_t* vm; var_t* entries; } usp_obj_ctx;
static void usp_obj_each(const char* key, void* value, void* ud) {
    usp_obj_ctx* c = (usp_obj_ctx*)ud;
    if(c == NULL || key == NULL || key[0] == 0 || key[0] == '@') return;
    node_t* nd = (node_t*)value;
    if(nd == NULL || nd->var == NULL || nd->invisable || nd->be_unenumerable) return;
    mstr_t* tmp = mstr_new("");
    usp_add_entry(c->vm, c->entries, key, js_cstr(nd->var, tmp));
    mstr_free(tmp);
}

static bool usp_is_usp(var_t* v) {
    return (v != NULL && v->type == V_OBJECT && usp_entries(v) != NULL);
}

static var_t* native_usp_ctor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    var_t* entries = var_new_array(vm);
    node_t* n = var_add(self, USP_ENTRIES, entries);
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
    var_t* init = js_arg(env, 0);
    if(init == NULL) return NULL;
    if(init->type == V_STRING) {
        usp_parse(vm, entries, var_get_str(init));
    }
    else if(init->type == V_OBJECT && init->is_array) {
        int cnt = (int)var_array_size(init);
        for(int i = 0; i < cnt; ++i) {
            node_t* pn = var_array_get(init, i);
            if(pn == NULL || pn->var == NULL || !pn->var->is_array) continue;
            mstr_t* a = mstr_new("");
            mstr_t* b = mstr_new("");
            node_t* kn = var_array_get(pn->var, 0);
            node_t* vn = var_array_get(pn->var, 1);
            const char* ks = (kn != NULL && kn->var != NULL) ? js_cstr(kn->var, a) : "";
            const char* vs = (vn != NULL && vn->var != NULL) ? js_cstr(vn->var, b) : "";
            usp_add_entry(vm, entries, ks, vs);
            mstr_free(a);
            mstr_free(b);
        }
    }
    else if(usp_is_usp(init)) {
        var_t* src = usp_entries(init);
        int cnt = (int)var_array_size(src);
        for(int i = 0; i < cnt; ++i) {
            node_t* pn = var_array_get(src, i);
            if(pn == NULL || pn->var == NULL) continue;
            usp_add_entry(vm, entries, usp_pair_name(pn->var), usp_pair_value(pn->var));
        }
    }
    else if(init->type == V_OBJECT) {
        usp_obj_ctx c; c.vm = vm; c.entries = entries;
        hash_map_iterate(&init->children, usp_obj_each, &c);
    }
    return NULL;
}

static var_t* native_usp_append(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* e = usp_entries(js_this(env));
    if(e == NULL) return NULL;
    mstr_t* a = mstr_new("");
    mstr_t* b = mstr_new("");
    usp_add_entry(vm, e, js_arg_cstr(env, 0, a), js_arg_cstr(env, 1, b));
    mstr_free(a);
    mstr_free(b);
    return NULL;
}

static var_t* native_usp_delete(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* e = usp_entries(self);
    if(e == NULL) return NULL;
    mstr_t* a = mstr_new("");
    mstr_t* b = mstr_new("");
    const char* name = js_arg_cstr(env, 0, a);
    bool has_val = (js_arg_count(env) > 1);
    const char* val = has_val ? js_arg_cstr(env, 1, b) : "";
    var_t* keep = var_new_array(vm);
    int cnt = (int)var_array_size(e);
    for(int i = 0; i < cnt; ++i) {
        node_t* pn = var_array_get(e, i);
        if(pn == NULL || pn->var == NULL) continue;
        bool match = (strcmp(usp_pair_name(pn->var), name) == 0) &&
                     (!has_val || strcmp(usp_pair_value(pn->var), val) == 0);
        if(!match) var_array_add(keep, pn->var);
    }
    usp_replace_entries(self, keep);
    mstr_free(a);
    mstr_free(b);
    return NULL;
}

static var_t* native_usp_get(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* e = usp_entries(js_this(env));
    mstr_t* a = mstr_new("");
    const char* name = js_arg_cstr(env, 0, a);
    var_t* r = var_new_null(vm);
    if(e != NULL) {
        int cnt = (int)var_array_size(e);
        for(int i = 0; i < cnt; ++i) {
            node_t* pn = var_array_get(e, i);
            if(pn == NULL || pn->var == NULL) continue;
            if(strcmp(usp_pair_name(pn->var), name) == 0) {
                r = var_new_str(vm, usp_pair_value(pn->var));
                break;
            }
        }
    }
    mstr_free(a);
    return r;
}

static var_t* native_usp_getAll(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* e = usp_entries(js_this(env));
    mstr_t* a = mstr_new("");
    const char* name = js_arg_cstr(env, 0, a);
    var_t* arr = var_new_array(vm);
    if(e != NULL) {
        int cnt = (int)var_array_size(e);
        for(int i = 0; i < cnt; ++i) {
            node_t* pn = var_array_get(e, i);
            if(pn == NULL || pn->var == NULL) continue;
            if(strcmp(usp_pair_name(pn->var), name) == 0)
                var_array_add(arr, var_new_str(vm, usp_pair_value(pn->var)));
        }
    }
    mstr_free(a);
    return arr;
}

static var_t* native_usp_has(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* e = usp_entries(js_this(env));
    mstr_t* a = mstr_new("");
    mstr_t* b = mstr_new("");
    const char* name = js_arg_cstr(env, 0, a);
    bool has_val = (js_arg_count(env) > 1);
    const char* val = has_val ? js_arg_cstr(env, 1, b) : "";
    bool found = false;
    if(e != NULL) {
        int cnt = (int)var_array_size(e);
        for(int i = 0; i < cnt; ++i) {
            node_t* pn = var_array_get(e, i);
            if(pn == NULL || pn->var == NULL) continue;
            if(strcmp(usp_pair_name(pn->var), name) == 0 &&
               (!has_val || strcmp(usp_pair_value(pn->var), val) == 0)) { found = true; break; }
        }
    }
    mstr_free(a);
    mstr_free(b);
    return var_new_bool(vm, found);
}

static var_t* native_usp_set(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* e = usp_entries(self);
    if(e == NULL) return NULL;
    mstr_t* a = mstr_new("");
    mstr_t* b = mstr_new("");
    const char* name = js_arg_cstr(env, 0, a);
    const char* val  = js_arg_cstr(env, 1, b);
    var_t* keep = var_new_array(vm);
    bool done = false;
    int cnt = (int)var_array_size(e);
    for(int i = 0; i < cnt; ++i) {
        node_t* pn = var_array_get(e, i);
        if(pn == NULL || pn->var == NULL) continue;
        if(strcmp(usp_pair_name(pn->var), name) == 0) {
            if(!done) { usp_add_entry(vm, keep, name, val); done = true; }
        }
        else var_array_add(keep, pn->var);
    }
    if(!done) usp_add_entry(vm, keep, name, val);
    usp_replace_entries(self, keep);
    mstr_free(a);
    mstr_free(b);
    return NULL;
}

static var_t* native_usp_toString(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* out = mstr_new("");
    usp_serialize_into(usp_entries(js_this(env)), out);
    var_t* r = var_new_str(vm, out->cstr);
    mstr_free(out);
    return r;
}

static var_t* native_usp_get_size(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* e = usp_entries(js_this(env));
    return var_new_int(vm, (e != NULL) ? (int)var_array_size(e) : 0);
}

static var_t* native_usp_forEach(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* cb = js_arg_func(env, 0);
    if(self == NULL || cb == NULL) return NULL;
    var_t* thisArg = js_arg(env, 1);
    /* The callback may mutate `self` (delete()/append()), which swaps the
     * entries array out from under us, so re-read it every step and copy the
     * name/value into C locals before the call. */
    for(int i = 0; ; ++i) {
        var_t* e = usp_entries(self);
        if(e == NULL || i >= (int)var_array_size(e)) break;
        node_t* pn = var_array_get(e, i);
        if(pn == NULL || pn->var == NULL) continue;
        char* nm = js_strdup(usp_pair_name(pn->var));
        char* vl = js_strdup(usp_pair_value(pn->var));
        var_t* args = var_new_array(vm);
        var_array_add(args, var_new_str(vm, (vl != NULL) ? vl : ""));
        var_array_add(args, var_new_str(vm, (nm != NULL) ? nm : ""));
        var_array_add(args, self);
        var_array_reverse(args);
        var_t* res = call_m_func(vm, thisArg, cb, args);
        var_unref(args);
        if(res != NULL) var_unref(res);
        if(nm != NULL) mario_free(nm);
        if(vl != NULL) mario_free(vl);
    }
    return NULL;
}

/* keys()/values()/entries() hand back a plain array (mario iterates arrays
 * with for..of); core-js's iterable module layers the real iterator protocol
 * and Symbol.iterator on top of the prototype. */
static var_t* usp_collect(vm_t* vm, var_t* e, int mode) {
    var_t* arr = var_new_array(vm);
    if(e == NULL) return arr;
    int cnt = (int)var_array_size(e);
    for(int i = 0; i < cnt; ++i) {
        node_t* pn = var_array_get(e, i);
        if(pn == NULL || pn->var == NULL) continue;
        if(mode == 0)       var_array_add(arr, var_new_str(vm, usp_pair_name(pn->var)));
        else if(mode == 1)  var_array_add(arr, var_new_str(vm, usp_pair_value(pn->var)));
        else {
            var_t* pair = var_new_array(vm);
            var_array_add(pair, var_new_str(vm, usp_pair_name(pn->var)));
            var_array_add(pair, var_new_str(vm, usp_pair_value(pn->var)));
            var_array_add(arr, pair);
        }
    }
    return arr;
}
static var_t* native_usp_keys(vm_t* vm, var_t* env, void* data)    { (void)data; return usp_collect(vm, usp_entries(js_this(env)), 0); }
static var_t* native_usp_values(vm_t* vm, var_t* env, void* data)  { (void)data; return usp_collect(vm, usp_entries(js_this(env)), 1); }
static var_t* native_usp_entries(vm_t* vm, var_t* env, void* data) { (void)data; return usp_collect(vm, usp_entries(js_this(env)), 2); }

/* for..of over a URLSearchParams walks [name, value] pairs: github's query
 * builders do `for (const [k, v] of params)`. mario's iteration protocol
 * looks up "@@S:iterator" and then drives next(), so the symbol method hands
 * back a small cursor object over the collected pair array. */
static var_t* native_usp_iter_next(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* arr = (self != NULL) ? var_find_own_member_var(self, "@@it_arr") : NULL;
    var_t* iv = (self != NULL) ? var_find_own_member_var(self, "@@it_i") : NULL;
    int i = (iv != NULL && iv->type == V_INT && iv->value != NULL) ? *(int*)iv->value : 0;
    int n = (arr != NULL) ? (int)var_array_size(arr) : 0;
    var_t* step = var_new_obj_no_proto(vm, NULL, NULL);
    if(step == NULL) return NULL;
    if(i < n) {
        node_t* pn = var_array_get(arr, i);
        var_add(step, "value", (pn != NULL && pn->var != NULL) ? pn->var : var_new(vm));
        var_add(step, "done", var_new_bool(vm, 0));
    }
    else {
        var_add(step, "value", var_new(vm));
        var_add(step, "done", var_new_bool(vm, 1));
    }
    if(iv != NULL && iv->type == V_INT && iv->value != NULL)
        *(int*)iv->value = i + 1;
    return step;
}

static var_t* native_usp_symbol_iterator(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* arr = usp_collect(vm, usp_entries(js_this(env)), 2);
    var_t* it = var_new_obj_no_proto(vm, NULL, NULL);
    if(it == NULL) return arr;
    var_add(it, "@@it_arr", arr);
    var_add(it, "@@it_i", var_new_int(vm, 0));
    /* vm_reg_native_on: the cursor is a no-proto object, so vm_reg_static would
     * resolve var_get_prototype(it)==NULL and fall through to vm->root, leaving
     * the iterator without a callable next() ("can not find function 'next' on
     * object{}"). */
    vm_reg_native_on(vm, it, "next()", native_usp_iter_next, NULL);
    return it;
}

/* Stable insertion sort of the entries by name (spec: sort by code units). */
static var_t* native_usp_sort(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* e = usp_entries(self);
    if(e == NULL) return NULL;
    int cnt = (int)var_array_size(e);
    if(cnt <= 1) return NULL;
    var_t** items = (var_t**)mario_malloc(sizeof(var_t*) * (size_t)cnt);
    if(items == NULL) return NULL;
    for(int i = 0; i < cnt; ++i) {
        node_t* pn = var_array_get(e, i);
        items[i] = (pn != NULL) ? pn->var : NULL;
    }
    for(int i = 1; i < cnt; ++i) {
        var_t* cur = items[i];
        const char* cn = usp_pair_name(cur);
        int j = i - 1;
        while(j >= 0 && items[j] != NULL && strcmp(usp_pair_name(items[j]), cn) > 0) {
            items[j + 1] = items[j];
            --j;
        }
        items[j + 1] = cur;
    }
    var_t* keep = var_new_array(vm);
    for(int i = 0; i < cnt; ++i)
        if(items[i] != NULL) var_array_add(keep, items[i]);
    mario_free(items);
    usp_replace_entries(self, keep);
    return NULL;
}

/* ---- URL ---------------------------------------------------------- */

typedef struct {
    char* protocol; char* username; char* password;
    char* hostname; char* port; char* pathname;
    char* search; char* hash;
} url_parts_t;

static void url_parts_free(url_parts_t* p) {
    if(p == NULL) return;
    if(p->protocol) mario_free(p->protocol);
    if(p->username) mario_free(p->username);
    if(p->password) mario_free(p->password);
    if(p->hostname) mario_free(p->hostname);
    if(p->port)     mario_free(p->port);
    if(p->pathname) mario_free(p->pathname);
    if(p->search)   mario_free(p->search);
    if(p->hash)     mario_free(p->hash);
    memset(p, 0, sizeof(*p));
}

static void url_lower(char* s) {
    if(s == NULL) return;
    for(char* p = s; *p != 0; ++p) *p = js_ascii_lower(*p);
}

/* Parse an ABSOLUTE url into parts. Handles the userinfo ("user:pass@host")
 * that url_parse() (used for location) deliberately ignores. Every field is
 * set (possibly ""), so url_parts_free() is always safe. */
static void url_parse2(const char* url, url_parts_t* p) {
    memset(p, 0, sizeof(*p));
    p->protocol = js_strdup("");
    p->username = js_strdup("");
    p->password = js_strdup("");
    p->hostname = js_strdup("");
    p->port     = js_strdup("");
    p->pathname = js_strdup("");
    p->search   = js_strdup("");
    p->hash     = js_strdup("");
    if(url == NULL) return;

    const char* colon = strchr(url, ':');
    const char* slash = strchr(url, '/');
    const char* qm    = strchr(url, '?');
    const char* hs    = strchr(url, '#');
    bool has_scheme = (colon != NULL) && (slash == NULL || colon < slash) &&
                      (qm == NULL || colon < qm) && (hs == NULL || colon < hs);
    const char* rest = url;
    if(has_scheme) {
        char* sch = js_strndup(url, (uint32_t)(colon - url));
        mstr_t* m = mstr_new((sch != NULL) ? sch : "");
        mstr_add(m, ':');
        if(sch != NULL) mario_free(sch);
        mario_free(p->protocol);
        url_lower(m->cstr);
        p->protocol = js_strdup(m->cstr);
        mstr_free(m);
        rest = colon + 1;
    }

    bool hier = (rest[0] == '/' && rest[1] == '/');
    if(hier) {
        rest += 2;
        const char* aend = rest;
        while(*aend != 0 && *aend != '/' && *aend != '?' && *aend != '#') ++aend;
        const char* hoststart = rest;
        const char* at = (const char*)memchr(rest, '@', (size_t)(aend - rest));
        if(at != NULL) {
            const char* uc = (const char*)memchr(rest, ':', (size_t)(at - rest));
            if(uc != NULL) {
                mario_free(p->username);
                p->username = js_strndup(rest, (uint32_t)(uc - rest));
                mario_free(p->password);
                p->password = js_strndup(uc + 1, (uint32_t)(at - uc - 1));
            }
            else {
                mario_free(p->username);
                p->username = js_strndup(rest, (uint32_t)(at - rest));
            }
            hoststart = at + 1;
        }
        const char* pc = (const char*)memchr(hoststart, ':', (size_t)(aend - hoststart));
        if(pc != NULL) {
            mario_free(p->hostname);
            p->hostname = js_strndup(hoststart, (uint32_t)(pc - hoststart));
            mario_free(p->port);
            p->port = js_strndup(pc + 1, (uint32_t)(aend - pc - 1));
        }
        else {
            mario_free(p->hostname);
            p->hostname = js_strndup(hoststart, (uint32_t)(aend - hoststart));
        }
        url_lower(p->hostname);
        rest = aend;
    }

    const char* base_end = rest + strlen(rest);
    const char* hh = strchr(rest, '#');
    if(hh != NULL) {
        mario_free(p->hash);
        p->hash = js_strdup(hh);
        base_end = hh;
    }
    const char* qq = (const char*)memchr(rest, '?', (size_t)(base_end - rest));
    if(qq != NULL) {
        mario_free(p->pathname);
        p->pathname = js_strndup(rest, (uint32_t)(qq - rest));
        mario_free(p->search);
        p->search = js_strndup(qq, (uint32_t)(base_end - qq));
    }
    else {
        mario_free(p->pathname);
        p->pathname = js_strndup(rest, (uint32_t)(base_end - rest));
    }
    if(p->pathname == NULL) p->pathname = js_strdup("");
    if(p->pathname[0] == 0 && (hier || p->hostname[0] != 0)) {
        mario_free(p->pathname);
        p->pathname = js_strdup("/");
    }
}

static bool url_special(const char* proto) {
    return strcmp(proto, "http:") == 0 || strcmp(proto, "https:") == 0 ||
           strcmp(proto, "ftp:") == 0 || strcmp(proto, "file:") == 0 ||
           strcmp(proto, "ws:") == 0 || strcmp(proto, "wss:") == 0;
}
static bool url_default_port(const char* proto, const char* port) {
    if(port == NULL || port[0] == 0) return false;
    if(strcmp(proto, "http:") == 0 || strcmp(proto, "ws:") == 0)   return strcmp(port, "80") == 0;
    if(strcmp(proto, "https:") == 0 || strcmp(proto, "wss:") == 0) return strcmp(port, "443") == 0;
    if(strcmp(proto, "ftp:") == 0)                                  return strcmp(port, "21") == 0;
    return false;
}

/* Resolve `ref` against `base` into an absolute URL string (mario_malloc'd).
 * Approximate but covers the forms pages and core-js actually use. */
static char* url_resolve2(const char* base, const char* ref) {
    if(ref == NULL) ref = "";
    if(base == NULL) base = "";
    const char* colon = strchr(ref, ':');
    const char* slash = strchr(ref, '/');
    const char* qm    = strchr(ref, '?');
    const char* hs    = strchr(ref, '#');
    bool ref_abs = (colon != NULL) && (slash == NULL || colon < slash) &&
                   (qm == NULL || colon < qm) && (hs == NULL || colon < hs);
    if(ref_abs || base[0] == 0) return js_strdup(ref);

    url_parts_t b;
    url_parse2(base, &b);
    mstr_t* auth = mstr_new("");
    if(b.username[0] != 0) {
        mstr_append(auth, b.username);
        if(b.password[0] != 0) { mstr_add(auth, ':'); mstr_append(auth, b.password); }
        mstr_add(auth, '@');
    }
    mstr_t* out = mstr_new("");
    if(ref[0] == '/' && ref[1] == '/') {
        mstr_append(out, b.protocol);
        mstr_append(out, ref);
    }
    else {
        mstr_append(out, b.protocol);
        if(b.hostname[0] != 0 || url_special(b.protocol)) {
            mstr_append(out, "//");
            mstr_append(out, auth->cstr);
            mstr_append(out, b.hostname);
            if(b.port[0] != 0) { mstr_add(out, ':'); mstr_append(out, b.port); }
        }
        if(ref[0] == '/') {
            mstr_append(out, ref);
        }
        else if(ref[0] == '?') {
            mstr_append(out, b.pathname);
            mstr_append(out, ref);
        }
        else if(ref[0] == '#') {
            mstr_append(out, b.pathname);
            mstr_append(out, b.search);
            mstr_append(out, ref);
        }
        else {
            const char* last = NULL;
            for(const char* z = b.pathname; *z != 0; ++z)
                if(*z == '/') last = z;
            if(last != NULL) {
                for(const char* z = b.pathname; z <= last; ++z) mstr_add(out, *z);
            }
            else mstr_add(out, '/');
            mstr_append(out, ref);
        }
    }
    char* r = js_strdup(out->cstr);
    mstr_free(out);
    mstr_free(auth);
    url_parts_free(&b);
    return r;
}

enum { UP_HREF, UP_PROTOCOL, UP_HOST, UP_HOSTNAME, UP_PORT, UP_PATHNAME,
       UP_SEARCH, UP_HASH, UP_ORIGIN, UP_USERNAME, UP_PASSWORD };

static const char* url_part(var_t* self, const char* key) {
    var_t* v = (self != NULL) ? var_find_own_member_var(self, key) : NULL;
    if(v == NULL || v->type != V_STRING) return "";
    const char* s = var_get_str(v);
    return (s != NULL) ? s : "";
}
static void url_store(vm_t* vm, var_t* self, const char* key, const char* val) {
    node_t* n = var_add(self, key, var_new_str(vm, (val != NULL) ? val : ""));
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
}
static void url_store_parts(vm_t* vm, var_t* self, url_parts_t* p) {
    url_store(vm, self, "@@protocol", p->protocol);
    url_store(vm, self, "@@username", p->username);
    url_store(vm, self, "@@password", p->password);
    url_store(vm, self, "@@hostname", p->hostname);
    url_store(vm, self, "@@port",     p->port);
    url_store(vm, self, "@@pathname", p->pathname);
    url_store(vm, self, "@@hash",     p->hash);
}

static void url_host_into(var_t* self, mstr_t* out) {
    mstr_append(out, url_part(self, "@@hostname"));
    const char* port = url_part(self, "@@port");
    const char* proto = url_part(self, "@@protocol");
    if(port[0] != 0 && !url_default_port(proto, port)) {
        mstr_add(out, ':');
        mstr_append(out, port);
    }
}
static void url_href_into(var_t* self, mstr_t* out) {
    const char* proto = url_part(self, "@@protocol");
    mstr_append(out, proto);
    if(url_special(proto)) {
        mstr_append(out, "//");
        const char* un = url_part(self, "@@username");
        if(un[0] != 0) {
            mstr_append(out, un);
            const char* pw = url_part(self, "@@password");
            if(pw[0] != 0) { mstr_add(out, ':'); mstr_append(out, pw); }
            mstr_add(out, '@');
        }
        url_host_into(self, out);
    }
    mstr_append(out, url_part(self, "@@pathname"));
    mstr_t* ss = mstr_new("");
    usp_serialize_into(usp_entries(get_obj(self, URL_SP)), ss);
    if(ss->len > 0) { mstr_add(out, '?'); mstr_append(out, ss->cstr); }
    mstr_free(ss);
    mstr_append(out, url_part(self, "@@hash"));
}

static var_t* native_url_ctor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    mstr_t* a = mstr_new("");
    mstr_t* b = mstr_new("");
    const char* ref  = js_arg_cstr(env, 0, a);
    const char* base = (js_arg_count(env) > 1) ? js_arg_cstr(env, 1, b) : "";
    char* abs = url_resolve2(base, ref);
    url_parts_t p;
    url_parse2((abs != NULL) ? abs : "", &p);
    vm->gc.gc_defer++;
    url_store_parts(vm, self, &p);
    var_t* sp = new_obj(vm, CLS_URLSP, 0);
    if(sp != NULL) {
        var_t* e = usp_entries(sp);
        if(e == NULL) {
            e = var_new_array(vm);
            node_t* n = var_add(sp, USP_ENTRIES, e);
            if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
        }
        usp_parse(vm, e, p.search);
        node_t* sn = var_add(self, URL_SP, sp);
        if(sn != NULL) { sn->invisable = 1; sn->be_unenumerable = 1; }
    }
    vm->gc.gc_defer--;
    url_parts_free(&p);
    if(abs != NULL) mario_free(abs);
    mstr_free(a);
    mstr_free(b);
    return NULL;
}

static var_t* native_url_get(vm_t* vm, var_t* env, void* data) {
    var_t* self = js_this(env);
    if(self == NULL) return var_new_str(vm, "");
    int idx = (int)(intptr_t)data;
    mstr_t* out = mstr_new("");
    switch(idx) {
        case UP_PROTOCOL: mstr_append(out, url_part(self, "@@protocol")); break;
        case UP_USERNAME: mstr_append(out, url_part(self, "@@username")); break;
        case UP_PASSWORD: mstr_append(out, url_part(self, "@@password")); break;
        case UP_HOSTNAME: mstr_append(out, url_part(self, "@@hostname")); break;
        case UP_PORT:     mstr_append(out, url_part(self, "@@port")); break;
        case UP_PATHNAME: mstr_append(out, url_part(self, "@@pathname")); break;
        case UP_HASH:     mstr_append(out, url_part(self, "@@hash")); break;
        case UP_HOST:     url_host_into(self, out); break;
        case UP_ORIGIN: {
            const char* proto = url_part(self, "@@protocol");
            if(url_special(proto) && url_part(self, "@@hostname")[0] != 0) {
                mstr_append(out, proto);
                mstr_append(out, "//");
                url_host_into(self, out);
            }
            else mstr_append(out, "null");
            break;
        }
        case UP_SEARCH: {
            usp_serialize_into(usp_entries(get_obj(self, URL_SP)), out);
            if(out->len > 0) {
                mstr_t* q = mstr_new("?");
                mstr_append(q, out->cstr);
                mstr_reset(out);
                mstr_append(out, q->cstr);
                mstr_free(q);
            }
            break;
        }
        case UP_HREF: url_href_into(self, out); break;
    }
    var_t* r = var_new_str(vm, out->cstr);
    mstr_free(out);
    return r;
}

static var_t* native_url_toString(vm_t* vm, var_t* env, void* data) {
    (void)data;
    return native_url_get(vm, env, (void*)(intptr_t)UP_HREF);
}

static var_t* native_url_set(vm_t* vm, var_t* env, void* data) {
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    int idx = (int)(intptr_t)data;
    mstr_t* s = mstr_new("");
    const char* v = js_arg_cstr(env, 0, s);
    if(idx == UP_HREF) {
        char* abs = url_resolve2("", v);
        url_parts_t p;
        url_parse2((abs != NULL) ? abs : v, &p);
        url_store_parts(vm, self, &p);
        var_t* sp = get_obj(self, URL_SP);
        var_t* e = (sp != NULL) ? usp_entries(sp) : NULL;
        if(e == NULL && sp != NULL) {
            e = var_new_array(vm);
            node_t* n = var_add(sp, USP_ENTRIES, e);
            if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
        }
        if(e != NULL) {
            var_t* fresh = var_new_array(vm);
            usp_parse(vm, fresh, p.search);
            usp_replace_entries(sp, fresh);
        }
        url_parts_free(&p);
        if(abs != NULL) mario_free(abs);
    }
    else if(idx == UP_SEARCH) {
        var_t* sp = get_obj(self, URL_SP);
        if(sp != NULL) {
            var_t* fresh = var_new_array(vm);
            usp_parse(vm, fresh, v);
            usp_replace_entries(sp, fresh);
        }
    }
    else if(idx == UP_PATHNAME) {
        mstr_t* pn = mstr_new("");
        if(v[0] != '/') mstr_add(pn, '/');
        mstr_append(pn, v);
        url_store(vm, self, "@@pathname", pn->cstr);
        mstr_free(pn);
    }
    else if(idx == UP_HASH) {
        mstr_t* hh = mstr_new("");
        if(v[0] != 0 && v[0] != '#') mstr_add(hh, '#');
        mstr_append(hh, v);
        url_store(vm, self, "@@hash", hh->cstr);
        mstr_free(hh);
    }
    else if(idx == UP_HOSTNAME) { char* c = js_strdup(v); url_lower(c); url_store(vm, self, "@@hostname", c); if(c) mario_free(c); }
    else if(idx == UP_PROTOCOL) { char* c = js_strdup(v); url_lower(c); url_store(vm, self, "@@protocol", c); if(c) mario_free(c); }
    else if(idx == UP_HOST) {
        const char* cc = strchr(v, ':');
        if(cc != NULL) {
            char* hn = js_strndup(v, (uint32_t)(cc - v));
            url_lower(hn);
            url_store(vm, self, "@@hostname", hn);
            url_store(vm, self, "@@port", cc + 1);
            if(hn != NULL) mario_free(hn);
        }
        else { char* hn = js_strdup(v); url_lower(hn); url_store(vm, self, "@@hostname", hn); url_store(vm, self, "@@port", ""); if(hn) mario_free(hn); }
    }
    else if(idx == UP_PORT)     url_store(vm, self, "@@port", v);
    else if(idx == UP_USERNAME) url_store(vm, self, "@@username", v);
    else if(idx == UP_PASSWORD) url_store(vm, self, "@@password", v);
    mstr_free(s);
    return NULL;
}

static var_t* native_url_get_searchParams(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    var_t* sp = (self != NULL) ? get_obj(self, URL_SP) : NULL;
    if(sp != NULL) return sp;
    return new_obj(vm, CLS_URLSP, 0);
}

static var_t* native_url_createObjectURL(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_str(vm, "blob:");
}
static var_t* native_url_revokeObjectURL(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return NULL;
}

static void web_register_url(vm_t* vm, var_t* bridge) {
    var_t* cls = vm_new_class(vm, CLS_URLSP);
    if(cls != NULL) {
        vm_reg_native(vm, cls, "constructor(init)",   native_usp_ctor,     bridge);
        vm_reg_native(vm, cls, "append(n, v)",        native_usp_append,   bridge);
        vm_reg_native(vm, cls, "delete(n, v)",        native_usp_delete,   bridge);
        vm_reg_native(vm, cls, "get(n)",              native_usp_get,      bridge);
        vm_reg_native(vm, cls, "getAll(n)",           native_usp_getAll,   bridge);
        vm_reg_native(vm, cls, "has(n, v)",           native_usp_has,      bridge);
        vm_reg_native(vm, cls, "set(n, v)",           native_usp_set,      bridge);
        vm_reg_native(vm, cls, "sort()",              native_usp_sort,     bridge);
        vm_reg_native(vm, cls, "forEach(f, t)",       native_usp_forEach,  bridge);
        vm_reg_native(vm, cls, "keys()",              native_usp_keys,     bridge);
        vm_reg_native(vm, cls, "values()",            native_usp_values,   bridge);
        vm_reg_native(vm, cls, "entries()",           native_usp_entries,  bridge);
        vm_reg_native(vm, cls, "toString()",          native_usp_toString, bridge);
        js_acc_cls(vm, cls, "size", native_usp_get_size, NULL, bridge);
        /* for..of over the params object itself (not just entries()). */
        var_t* proto = var_find_member_var(cls, "prototype");
        if(proto != NULL)
            vm_reg_static(vm, proto, "@@S:iterator()", native_usp_symbol_iterator, bridge);
    }

    cls = vm_new_class(vm, CLS_URL);
    if(cls != NULL) {
        vm_reg_native(vm, cls, "constructor(u, b)", native_url_ctor, bridge);
        js_acc_cls(vm, cls, "href",        native_url_get, native_url_set, (void*)(intptr_t)UP_HREF);
        js_acc_cls(vm, cls, "protocol",    native_url_get, native_url_set, (void*)(intptr_t)UP_PROTOCOL);
        js_acc_cls(vm, cls, "username",    native_url_get, native_url_set, (void*)(intptr_t)UP_USERNAME);
        js_acc_cls(vm, cls, "password",    native_url_get, native_url_set, (void*)(intptr_t)UP_PASSWORD);
        js_acc_cls(vm, cls, "host",        native_url_get, native_url_set, (void*)(intptr_t)UP_HOST);
        js_acc_cls(vm, cls, "hostname",    native_url_get, native_url_set, (void*)(intptr_t)UP_HOSTNAME);
        js_acc_cls(vm, cls, "port",        native_url_get, native_url_set, (void*)(intptr_t)UP_PORT);
        js_acc_cls(vm, cls, "pathname",    native_url_get, native_url_set, (void*)(intptr_t)UP_PATHNAME);
        js_acc_cls(vm, cls, "search",      native_url_get, native_url_set, (void*)(intptr_t)UP_SEARCH);
        js_acc_cls(vm, cls, "hash",        native_url_get, native_url_set, (void*)(intptr_t)UP_HASH);
        js_acc_cls(vm, cls, "origin",      native_url_get, NULL,           (void*)(intptr_t)UP_ORIGIN);
        js_acc_cls(vm, cls, "searchParams", native_url_get_searchParams, NULL, bridge);
        vm_reg_native(vm, cls, "toString()", native_url_toString, bridge);
        vm_reg_native(vm, cls, "toJSON()",   native_url_toString, bridge);
        vm_reg_static(vm, cls, "createObjectURL(b)",  native_url_createObjectURL, bridge);
        vm_reg_static(vm, cls, "revokeObjectURL(u)",  native_url_revokeObjectURL, bridge);
    }
}

/* ------------------------------------------------------------------ */
/* crypto                                                             */
/* ------------------------------------------------------------------ */

/* No entropy source is wired through the bridge, so randomness comes from the
 * monotonic clock mixed with a counter. That is fine for the two things pages
 * use these for - keys for client-side state and cache-busting ids - and it is
 * explicitly NOT offered as a cryptographic source. */
static uint32_t web_rand_seed = 0;

static uint32_t web_rand_next(void) {
    if(web_rand_seed == 0)
        web_rand_seed = (uint32_t)(js_dom_monotonic_ms() & 0xFFFFFFFFu) ^ 0x9E3779B9u;
    /* xorshift32: cheap, no libm, good enough for non-security ids. */
    uint32_t x = web_rand_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    web_rand_seed = x;
    return x;
}

/* crypto.getRandomValues(array): fills a JS array (or a TypedArray's backing
 * array) with bytes 0..255. The WebIDL integer types are not modelled, so the
 * values stay plain numbers. */
static var_t* native_crypto_getRandomValues(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* arr = js_arg(env, 0);
    if(arr == NULL || !arr->is_array) return arr;
    int n = (int)var_array_size(arr);
    for(int i = 0; i < n; ++i) {
        node_t* nd = var_array_get(arr, i);
        uint32_t r = web_rand_next();
        if(nd != NULL && nd->var != NULL)
            node_replace(nd, var_new_int(vm, (int)(r & 0xFF)));
        else
            var_array_add(arr, var_new_int(vm, (int)(r & 0xFF)));
    }
    return arr;
}

/* crypto.randomUUID(): RFC 4122 shape with the version/variant nibbles set. */
static var_t* native_crypto_randomUUID(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    unsigned char b[16];
    for(int i = 0; i < 16; ++i) b[i] = (unsigned char)(web_rand_next() & 0xFF);
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);   /* version 4 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);   /* variant 10 */
    char out[40];
    static const char hex[] = "0123456789abcdef";
    int o = 0;
    for(int i = 0; i < 16; ++i) {
        if(i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hex[(b[i] >> 4) & 0xF];
        out[o++] = hex[b[i] & 0xF];
    }
    out[o] = 0;
    return var_new_str(vm, out);
}

/* ------------------------------------------------------------------ */
/* crypto.subtle.digest (SHA-1 / SHA-256 / SHA-512)                    */
/*                                                                     */
/* Self-contained FIPS 180-4 implementations: the rest of the engine    */
/* hashes through BearSSL inside libtinyhttpsc, which is not visible    */
/* from this bridge and is TLS-only by contract. digests are the only   */
/* SubtleCrypto operation pages use without a key (integrity checks,    */
/* cache keys, subresource hashes); the key-based ones stay absent so    */
/* a feature test reports them honestly instead of failing mid-call.    */
/* ------------------------------------------------------------------ */

static uint32_t sha_ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint64_t sha_ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static uint32_t sha_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t sha_be64(const uint8_t* p) {
    return ((uint64_t)sha_be32(p) << 32) | (uint64_t)sha_be32(p + 4);
}

static void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for(int i = 0; i < 16; ++i) w[i] = sha_be32(p + 4 * i);
    for(int i = 16; i < 80; ++i) {
        uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (t << 1) | (t >> 31);
    }
    for(int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if(i < 20)      { f = (b & c) | ((~b) & d);               k = 0x5A827999u; }
        else if(i < 40) { f = b ^ c ^ d;                          k = 0x6ED9EBA1u; }
        else if(i < 60) { f = (b & c) | (b & d) | (c & d);         k = 0x8F1BBCDCu; }
        else            { f = b ^ c ^ d;                          k = 0xCA62C1D6u; }
        uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1_hash(const uint8_t* data, uint32_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    uint8_t tail[128];
    uint32_t i = 0;
    for(; len - i >= 64; i += 64) sha1_block(h, data + i);
    uint32_t rem = len - i;
    memset(tail, 0, sizeof(tail));
    if(rem != 0) memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    uint32_t tl = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8u;
    for(int j = 0; j < 8; ++j) tail[tl - 1 - j] = (uint8_t)(bits >> (8 * j));
    for(uint32_t k = 0; k < tl; k += 64) sha1_block(h, tail + k);
    for(int j = 0; j < 5; ++j) {
        out[4*j]   = (uint8_t)(h[j] >> 24);
        out[4*j+1] = (uint8_t)(h[j] >> 16);
        out[4*j+2] = (uint8_t)(h[j] >> 8);
        out[4*j+3] = (uint8_t)(h[j]);
    }
}

static const uint32_t sha256_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void sha256_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for(int i = 0; i < 16; ++i) w[i] = sha_be32(p + 4 * i);
    for(int i = 16; i < 64; ++i) {
        uint32_t s0 = sha_ror32(w[i-15], 7) ^ sha_ror32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = sha_ror32(w[i-2], 17) ^ sha_ror32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for(int i = 0; i < 64; ++i) {
        uint32_t S1 = sha_ror32(e, 6) ^ sha_ror32(e, 11) ^ sha_ror32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha_ror32(a, 2) ^ sha_ror32(a, 13) ^ sha_ror32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256_hash(const uint8_t* data, uint32_t len, uint8_t out[32]) {
    uint32_t h[8] = { 0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                      0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u };
    uint8_t tail[128];
    uint32_t i = 0;
    for(; len - i >= 64; i += 64) sha256_block(h, data + i);
    uint32_t rem = len - i;
    memset(tail, 0, sizeof(tail));
    if(rem != 0) memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    uint32_t tl = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8u;
    for(int j = 0; j < 8; ++j) tail[tl - 1 - j] = (uint8_t)(bits >> (8 * j));
    for(uint32_t k = 0; k < tl; k += 64) sha256_block(h, tail + k);
    for(int j = 0; j < 8; ++j) {
        out[4*j]   = (uint8_t)(h[j] >> 24);
        out[4*j+1] = (uint8_t)(h[j] >> 16);
        out[4*j+2] = (uint8_t)(h[j] >> 8);
        out[4*j+3] = (uint8_t)(h[j]);
    }
}

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

static void sha512_block(uint64_t h[8], const uint8_t* p) {
    uint64_t w[80];
    for(int i = 0; i < 16; ++i) w[i] = sha_be64(p + 8 * i);
    for(int i = 16; i < 80; ++i) {
        uint64_t s0 = sha_ror64(w[i-15], 1) ^ sha_ror64(w[i-15], 8) ^ (w[i-15] >> 7);
        uint64_t s1 = sha_ror64(w[i-2], 19) ^ sha_ror64(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for(int i = 0; i < 80; ++i) {
        uint64_t S1 = sha_ror64(e, 14) ^ sha_ror64(e, 18) ^ sha_ror64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = hh + S1 + ch + sha512_k[i] + w[i];
        uint64_t S0 = sha_ror64(a, 28) ^ sha_ror64(a, 34) ^ sha_ror64(a, 39);
        uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha512_hash(const uint8_t* data, uint32_t len, uint8_t out[64]) {
    uint64_t h[8] = { 0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,
                      0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                      0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,
                      0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL };
    uint8_t tail[256];
    uint32_t i = 0;
    for(; len - i >= 128; i += 128) sha512_block(h, data + i);
    uint32_t rem = len - i;
    memset(tail, 0, sizeof(tail));
    if(rem != 0) memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    uint32_t tl = (rem < 112) ? 128 : 256;
    uint64_t bits = (uint64_t)len * 8u;
    for(int j = 0; j < 8; ++j) tail[tl - 1 - j] = (uint8_t)(bits >> (8 * j));
    for(uint32_t k = 0; k < tl; k += 128) sha512_block(h, tail + k);
    for(int j = 0; j < 8; ++j)
        for(int b = 0; b < 8; ++b)
            out[8*j+b] = (uint8_t)(h[j] >> (56 - 8*b));
}

/* ASCII case-insensitive compare without pulling in <strings.h> (the bridge is
 * built for freestanding targets where it is not guaranteed to exist). */
static int sha_name_eq(const char* a, const char* b) {
    while(*a != 0 && *b != 0) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if(ca != cb) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

/* Borrow the bytes of a BufferSource (ArrayBuffer / TypedArray view) or of a
 * plain string. Returns a pointer that stays valid for the call; `owned` is
 * set when the caller has to mario_free() it (the TypedArray copy path). */
static const uint8_t* web_bytes(var_t* v, uint32_t* len, uint8_t** owned) {
    *len = 0; *owned = NULL;
    if(v == NULL) return NULL;
    if(var_is_arraybuffer(v)) {
        *len = (uint32_t)v->size;
        return (const uint8_t*)v->value;
    }
    if(var_is_typedarray(v)) {
        var_t* buf = var_find_own_member_var(v, "buffer");
        if(buf == NULL || buf->value == NULL) return NULL;
        int64_t off = var_get_int64(var_find_own_member_var(v, "byteOffset"));
        int64_t bl  = var_get_int64(var_find_own_member_var(v, "byteLength"));
        if(off < 0 || bl < 0 || (uint64_t)off + (uint64_t)bl > (uint64_t)buf->size) return NULL;
        uint8_t* p = (uint8_t*)mario_malloc((size_t)(bl ? bl : 1));
        if(p == NULL) return NULL;
        if(bl > 0) memcpy(p, (const uint8_t*)buf->value + off, (size_t)bl);
        *owned = p; *len = (uint32_t)bl;
        return p;
    }
    const char* s = var_get_str(v);
    if(s == NULL) return NULL;
    *len = (uint32_t)strlen(s);
    return (const uint8_t*)s;
}

/* Build an ArrayBuffer holding `len` bytes (the layout native_ArrayBuffer.c
 * uses: bytes in ->value, hidden @@exotic marker, unenumerable byteLength). */
static var_t* web_arraybuffer(vm_t* vm, const uint8_t* bytes, uint32_t len) {
    var_t* cls = var_find_own_member_var(vm->root, "ArrayBuffer");
    if(cls == NULL) return NULL;
    uint8_t* buf = (len != 0) ? (uint8_t*)mario_malloc(len) : NULL;
    if(len != 0 && buf == NULL) return NULL;
    if(len != 0) memcpy(buf, bytes, len);
    var_t* o = var_new_obj(vm, var_get_prototype(cls), buf, mario_free);
    if(o == NULL) { if(buf != NULL) mario_free(buf); return NULL; }
    o->size = len;
    node_t* mn = var_add(o, EXOTIC_MARKER, var_new_str(vm, EXOTIC_ARRAYBUFFER));
    if(mn != NULL) { mn->invisable = 1; mn->be_unenumerable = 1; }
    node_t* bn = var_add(o, "byteLength", var_new_int(vm, (int)len));
    if(bn != NULL) bn->be_unenumerable = 1;
    return o;
}

/* crypto.subtle.digest(algorithm, data) -> Promise<ArrayBuffer>. `algorithm`
 * is the usual {name:"SHA-256"} object or the bare name string. */
static var_t* native_crypto_subtle_digest(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* algo = js_arg(env, 0);
    var_t* name = (algo != NULL) ? var_find_member_var(algo, "name") : NULL;
    const char* s = (name != NULL) ? var_get_str(name) : var_get_str(algo);
    uint8_t out[64];
    uint32_t outlen = 0;
    if(s != NULL) {
        if(sha_name_eq(s, "SHA-1"))        outlen = 20;
        else if(sha_name_eq(s, "SHA-256")) outlen = 32;
        else if(sha_name_eq(s, "SHA-384")) outlen = 48;
        else if(sha_name_eq(s, "SHA-512")) outlen = 64;
    }
    if(outlen == 0) {
        var_t* reason = var_new_obj_no_proto(vm, NULL, NULL);
        vm->gc.gc_defer++;
        var_add(reason, "name",    var_new_str(vm, "NotSupportedError"));
        var_add(reason, "message", var_new_str(vm, "crypto.subtle.digest: unsupported algorithm"));
        vm->gc.gc_defer--;
        var_t* p = web_promise(vm, "reject", reason);
        return (p != NULL) ? p : var_new_null(vm);
    }
    uint32_t len = 0;
    uint8_t* owned = NULL;
    const uint8_t* bytes = web_bytes(js_arg(env, 1), &len, &owned);
    if(outlen == 20)      sha1_hash(bytes, len, out);
    else if(outlen == 32) sha256_hash(bytes, len, out);
    else if(outlen == 48) { uint8_t full[64]; sha512_hash(bytes, len, full); memcpy(out, full, 48); }
    else                  sha512_hash(bytes, len, out);
    if(owned != NULL) mario_free(owned);
    var_t* ab = web_arraybuffer(vm, out, outlen);
    var_t* p = web_promise(vm, "resolve", ab);
    return (p != NULL) ? p : ab;
}

static var_t* native_crypto_subtle_unsupported(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    /* The key-bearing operations need a real key store and a constant-time
     * backend; rejecting with a clear reason beats a silent hang. */
    var_t* reason = var_new_obj_no_proto(vm, NULL, NULL);
    vm->gc.gc_defer++;
    var_add(reason, "name",    var_new_str(vm, "NotSupportedError"));
    var_add(reason, "message", var_new_str(vm, "crypto.subtle: only digest() is implemented"));
    vm->gc.gc_defer--;
    var_t* p = web_promise(vm, "reject", reason);
    return (p != NULL) ? p : var_new_null(vm);
}

/* ------------------------------------------------------------------ */
/* Global <-> window mirroring                                        */
/* ------------------------------------------------------------------ */

/* Publish one singleton on BOTH the global scope and `window`. mario keeps
 * them separate (window is an ordinary member of vm->root), but a page writes
 * `location.href` and `window.location.href` interchangeably. */
static void web_publish(vm_t* vm, var_t* window, const char* name, var_t* v) {
    if(v == NULL) return;
    var_add(vm->root, name, v);
    if(window != NULL && window != vm->root) var_add(window, name, v);
}

/* Link an already-installed global onto `window` too. Functions and accessors
 * both survive the copy: do_get() checks var_is_accessor() on the node it
 * finds, so an accessor var mirrored onto window still computes through its
 * getter (with `this` == window, which every getter here ignores). */
static void web_link_global(vm_t* vm, var_t* window, const char* name) {
    if(window == NULL || name == NULL) return;
    if(var_find_own_member_var(window, name) != NULL) return;
    var_t* v = var_find_own_member_var(vm->root, name);
    if(v == NULL) {
        node_t* n = vm_find(vm, name);
        if(n != NULL) v = n->var;
    }
    if(v == NULL) return;   /* engine built without this builtin: skip */
    var_add(window, name, v);
}

static void web_mirror_globals(vm_t* vm, var_t* window) {
    static const char* kNames[] = {
        /* dialogs + timers (alert/setTimeout come from the DOM bridge) */
        "alert", "confirm", "prompt",
        "setTimeout", "setInterval", "clearTimeout", "clearInterval",
        "requestAnimationFrame", "cancelAnimationFrame", "queueMicrotask",
        /* global functions this bridge installs */
        "parseInt", "parseFloat", "isNaN", "isFinite",
        "encodeURIComponent", "decodeURIComponent", "encodeURI", "decodeURI",
        "escape", "unescape", "atob", "btoa",
        "getComputedStyle", "matchMedia", "fetch",
        /* engine builtins, so `window.Object` and friends resolve */
        "console", "Object", "Array", "String", "Number", "Boolean",
        "Function", "Date", "Math", "JSON", "RegExp", "Error", "TypeError",
        "RangeError", "Promise", "Map", "Set", "Symbol", "Infinity", "NaN",
        /* classes, for `instanceof` checks against the globals */
        "Event", "CustomEvent", "MouseEvent", "KeyboardEvent",
        "Element", "Document", "Storage", "Location", "History",
        "Navigator", "Screen", "Performance", "XMLHttpRequest",
        "Response", "Headers", "URL", "URLSearchParams", "CSS"
    };
    /* A/B knob (MARIO_NOQMT): withhold the native queueMicrotask so polyfills
     * (core-js) install their own - on rokid.com the engine-backed microtask
     * loses React-flight reactions, the MutationObserver-backed one does not. */
    bool no_qmt = (getenv("MARIO_NOQMT") != NULL);
    for(size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        if(no_qmt && strcmp(kNames[i], "queueMicrotask") == 0) continue;
        web_link_global(vm, window, kNames[i]);
    }

    /* window's own value properties live on the global scope as accessors; the
     * mirror above would miss them because they are not in kNames. */
    static const char* kWinProps[] = {
        "innerWidth", "innerHeight", "outerWidth", "outerHeight",
        "screenX", "screenY", "scrollX", "scrollY",
        "pageXOffset", "pageYOffset", "devicePixelRatio", "length"
    };
    for(size_t i = 0; i < sizeof(kWinProps) / sizeof(kWinProps[0]); ++i)
        web_link_global(vm, window, kWinProps[i]);
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

void js_web_callbacks_init(js_web_callbacks_t* cb) {
    if(cb != NULL) memset(cb, 0, sizeof(*cb));
}

static var_t* storage_new(vm_t* vm) {
    var_t* s = new_obj(vm, CLS_STORAGE, 0);
    if(s == NULL) return NULL;
    /* The hidden entry array is created before it is rooted on the instance. */
    vm->gc.gc_defer++;
    var_t* items = var_new_array(vm);
    node_t* n = var_add(s, STORAGE_ITEMS, items);
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
    vm->gc.gc_defer--;
    return s;
}

bool js_register_web_natives(vm_t* vm, const js_web_callbacks_t* cb) {
    if(vm == NULL || vm->root == NULL) return false;
    /* The DOM bridge owns the embedder context, the window/document globals
     * and the Location class; without it there is nothing to attach to. */
    if(js_dom_callbacks(vm) == NULL) return false;

    js_web_state* st = (js_web_state*)mario_malloc(sizeof(js_web_state));
    if(st == NULL) return false;
    memset(st, 0, sizeof(*st));
    st->vm = vm;
    if(cb != NULL) st->cb = *cb;
    /* performance.timeOrigin is derived from this reading, so now() starts at
     * 0 for the first script the page runs. */
    st->t0_ms   = js_dom_monotonic_ms();
    st->window   = var_find_own_member_var(vm->root, "window");
    st->document = var_find_own_member_var(vm->root, "document");

    var_t* window = st->window;
    if(window != NULL) st->location = get_obj(window, "location");

    var_t* bridge = var_new_obj_no_proto(vm, st, web_state_free);
    if(bridge == NULL) {
        mario_free(st);
        return false;
    }
    var_add(vm->root, WEB_BRIDGE_KEY, bridge);
    /* Anchor the borrowed singletons so a GC pass can never free something the
     * state struct still points at. */
    vm->gc.gc_defer++;
    if(window != NULL)      var_add(bridge, "@@window",   window);
    if(st->document != NULL) var_add(bridge, "@@document", st->document);
    vm->gc.gc_defer--;

    var_t* cls = NULL;

    /* ---- global functions ----
     * Registered on vm->root exactly like the DOM bridge's alert(); the
     * mirror step below makes them reachable as window.<name> too. */
    vm_reg_static(vm, NULL, "parseInt(v, r)",           native_parseInt,           bridge);
    vm_reg_static(vm, NULL, "parseFloat(v)",            native_parseFloat,         bridge);
    vm_reg_static(vm, NULL, "isNaN(v)",                 native_isNaN,              bridge);
    vm_reg_static(vm, NULL, "isFinite(v)",              native_isFinite,           bridge);
    vm_reg_static(vm, NULL, "encodeURIComponent(v)",    native_encodeURIComponent, bridge);
    vm_reg_static(vm, NULL, "decodeURIComponent(v)",    native_decodeURIComponent, bridge);
    vm_reg_static(vm, NULL, "encodeURI(v)",             native_encodeURI,          bridge);
    vm_reg_static(vm, NULL, "decodeURI(v)",             native_decodeURI,          bridge);
    vm_reg_static(vm, NULL, "escape(v)",                native_escape,             bridge);
    vm_reg_static(vm, NULL, "unescape(v)",              native_unescape,           bridge);
    vm_reg_static(vm, NULL, "atob(v)",                  native_atob,               bridge);
    vm_reg_static(vm, NULL, "btoa(v)",                  native_btoa,               bridge);
    if(getenv("MARIO_NOQMT") == NULL)
        vm_reg_static(vm, NULL, "queueMicrotask(f)",        native_queueMicrotask,     bridge);
    vm_reg_static(vm, NULL, "getComputedStyle(e, p)",   native_getComputedStyle,   bridge);
    vm_reg_static(vm, NULL, "matchMedia(q)",            native_matchMedia,         bridge);
    vm_reg_static(vm, NULL, "fetch(i, init)",           native_fetch,              bridge);
    vm_reg_static(vm, NULL, "confirm(v)",               native_win_confirm,        bridge);
    vm_reg_static(vm, NULL, "prompt(v, d)",             native_win_prompt,         bridge);

    /* CSS namespace. CSS.supports() must be callable even when the queried
     * feature is unavailable; throwing here aborts framework initialization. */
    cls = vm_new_class(vm, "CSS");
    if(cls != NULL) {
        vm_reg_static(vm, cls, "supports(a, b)", native_css_supports, bridge);
        vm_reg_static(vm, cls, "escape(s)",      native_css_escape,   bridge);
    }

    /* ---- window methods ---- */
    vm_reg_static(vm, NULL, "scrollTo(a, b)",    native_win_scrollTo, bridge);
    vm_reg_static(vm, NULL, "scroll(a, b)",      native_win_scrollTo, bridge);
    vm_reg_static(vm, NULL, "scrollBy(a, b)",    native_win_scrollBy, bridge);
    vm_reg_static(vm, NULL, "open(u, n, f)",     native_win_open,     bridge);
    /* A browser refuses every one of these for a window the script did not
     * open, so accepting them silently is the spec-correct behaviour. */
    vm_reg_static(vm, NULL, "moveTo(a, b)",      native_win_noop, bridge);
    vm_reg_static(vm, NULL, "moveBy(a, b)",      native_win_noop, bridge);
    vm_reg_static(vm, NULL, "resizeTo(a, b)",    native_win_noop, bridge);
    vm_reg_static(vm, NULL, "resizeBy(a, b)",    native_win_noop, bridge);
    vm_reg_static(vm, NULL, "close()",           native_win_noop, bridge);
    vm_reg_static(vm, NULL, "stop()",            native_win_noop, bridge);
    vm_reg_static(vm, NULL, "focus()",           native_win_noop, bridge);
    vm_reg_static(vm, NULL, "blur()",            native_win_noop, bridge);
    vm_reg_static(vm, NULL, "print()",           native_win_noop, bridge);
    vm_reg_static(vm, NULL, "postMessage(m)",    native_win_noop, bridge);

    /* ---- window value properties ---- */
    {
        static const char* kWinProps[] = {
            "innerWidth", "innerHeight", "outerWidth", "outerHeight",
            "screenX", "screenY", "scrollX", "scrollY",
            "pageXOffset", "pageYOffset", "devicePixelRatio", "length"
        };
        for(int i = 0; i < (int)(sizeof(kWinProps) / sizeof(kWinProps[0])); ++i)
            js_acc_on(vm, vm->root, kWinProps[i], win_metric_get, NULL,
                      (void*)(intptr_t)i);
    }

    /* ---- console extras ----
     * The engine's Console only has write()/log(); everything else goes on the
     * same prototype so the global `console` instance picks it up. */
    cls = var_find_own_member_var(vm->root, CLS_CONSOLE);
    if(cls != NULL) {
        vm_reg_native(vm, cls, "warn(v)",           native_console_warn,       bridge);
        vm_reg_native(vm, cls, "error(v)",          native_console_error,      bridge);
        vm_reg_native(vm, cls, "info(v)",           native_console_info,       bridge);
        vm_reg_native(vm, cls, "debug(v)",          native_console_debug,      bridge);
        vm_reg_native(vm, cls, "trace(v)",          native_console_trace,      bridge);
        vm_reg_native(vm, cls, "dir(v)",            native_console_dir,        bridge);
        vm_reg_native(vm, cls, "assert(c, v)",      native_console_assert,     bridge);
        vm_reg_native(vm, cls, "clear()",           native_console_clear,      bridge);
        vm_reg_native(vm, cls, "group(v)",          native_console_noop,       bridge);
        vm_reg_native(vm, cls, "groupCollapsed(v)", native_console_noop,       bridge);
        vm_reg_native(vm, cls, "groupEnd()",        native_console_noop,       bridge);
        vm_reg_native(vm, cls, "count(l)",          native_console_count,      bridge);
        vm_reg_native(vm, cls, "countReset(l)",     native_console_countReset, bridge);
        vm_reg_native(vm, cls, "time(l)",           native_console_time,       bridge);
        vm_reg_native(vm, cls, "timeLog(l)",        native_console_timeLog,    bridge);
        vm_reg_native(vm, cls, "timeEnd(l)",        native_console_timeEnd,    bridge);
        /* The label tables live on the bridge var, which is rooted, so they
         * survive GC without a C-side allocation. */
        st->console_counters = var_new_obj_no_proto(vm, NULL, NULL);
        st->console_timers   = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(bridge, "@@counters", st->console_counters);
        var_add(bridge, "@@timers",   st->console_timers);
    }

    /* ---- Web Storage ---- */
    cls = vm_new_class(vm, CLS_STORAGE);
    if(cls != NULL) {
        vm_reg_native(vm, cls, "getItem(k)",     native_storage_getItem,     bridge);
        vm_reg_native(vm, cls, "setItem(k, v)",  native_storage_setItem,     bridge);
        vm_reg_native(vm, cls, "removeItem(k)",  native_storage_removeItem,  bridge);
        vm_reg_native(vm, cls, "clear()",        native_storage_clear,       bridge);
        vm_reg_native(vm, cls, "key(i)",         native_storage_key,         bridge);
        js_acc_cls(vm, cls, "length", native_storage_get_length, NULL, bridge);
    }
    st->storage_local   = storage_new(vm);
    st->storage_session = storage_new(vm);
    vm->gc.gc_defer++;
    if(st->storage_local   != NULL) var_add(bridge, "@@local",   st->storage_local);
    if(st->storage_session != NULL) var_add(bridge, "@@session", st->storage_session);
    vm->gc.gc_defer--;
    web_publish(vm, window, "localStorage",   st->storage_local);
    web_publish(vm, window, "sessionStorage", st->storage_session);
    /* Restore whatever the embedder persisted for this origin. */
    if(st->cb.storage_load != NULL) {
        char* blob = st->cb.storage_load(web_ctx(vm), false);
        if(blob != NULL) { js_web_storage_restore(vm, false, blob); mario_free(blob); }
        blob = st->cb.storage_load(web_ctx(vm), true);
        if(blob != NULL) { js_web_storage_restore(vm, true, blob); mario_free(blob); }
    }

    /* ---- location ----
     * js_dom.c created the class with a read-only href; the rest of the
     * surface (and href's setter) is filled in here. */
    cls = var_find_own_member_var(vm->root, CLS_LOCATION);
    if(cls != NULL) {
        static const char* kLocProps[] = {
            "href", "protocol", "host", "hostname", "port",
            "pathname", "search", "hash", "origin"
        };
        for(int i = 0; i < 9; ++i) {
            /* href/search/hash/pathname are writable (they navigate); the rest
             * are read-only, as in a browser. */
            native_func_t setter = NULL;
            if(i == 0) setter = loc_set_href;
            else if(i >= 5 && i <= 7) setter = loc_set_part;
            js_acc_cls(vm, cls, kLocProps[i], loc_get, setter, (void*)(intptr_t)i);
        }
        vm_reg_native(vm, cls, "assign(u)",   native_loc_assign,   bridge);
        vm_reg_native(vm, cls, "replace(u)",  native_loc_assign,   bridge);
        vm_reg_native(vm, cls, "reload()",    native_loc_reload,   bridge);
        vm_reg_native(vm, cls, "toString()",  native_loc_toString, bridge);
    }
    /* js_dom.c only linked location off `window`; a bare `location.href` in a
     * page script needs it on the global scope too. */
    web_publish(vm, window, "location", st->location);

    /* ---- history ---- */
    cls = vm_new_class(vm, CLS_HISTORY);
    if(cls != NULL) {
        js_acc_cls(vm, cls, "length", native_history_get_length, NULL, bridge);
        js_acc_cls(vm, cls, "state",  native_history_get_state,  NULL, bridge);
        vm_reg_native(vm, cls, "back()",                    native_history_back,       bridge);
        vm_reg_native(vm, cls, "forward()",                 native_history_forward,    bridge);
        vm_reg_native(vm, cls, "go(n)",                     native_history_go,         bridge);
        vm_reg_native(vm, cls, "pushState(s, t, u)",        native_history_pushState,  bridge);
        vm_reg_native(vm, cls, "replaceState(s, t, u)",     native_history_pushState,  bridge);
    }
    var_t* history = new_obj(vm, CLS_HISTORY, 0);
    web_publish(vm, window, "history", history);
    if(history != NULL) var_add(bridge, "@@history", history);

    /* ---- navigator ---- */
    cls = vm_new_class(vm, CLS_NAVIGATOR);
    if(cls != NULL) {
        static const char* kNavProps[] = {
            "userAgent", "appVersion", "platform", "language", "vendor",
            "product", "appCodeName", "appName", "productSub", "doNotTrack"
        };
        for(int i = 0; i < (int)(sizeof(kNavProps) / sizeof(kNavProps[0])); ++i)
            js_acc_cls(vm, cls, kNavProps[i], nav_get, NULL, (void*)(intptr_t)i);
        js_acc_cls(vm, cls, "hardwareConcurrency", metric_get, NULL, (void*)(intptr_t)0);
        js_acc_cls(vm, cls, "maxTouchPoints",      metric_get, NULL, (void*)(intptr_t)1);
        js_acc_cls(vm, cls, "deviceMemory",        metric_get, NULL, (void*)(intptr_t)2);
        js_acc_cls(vm, cls, "languages",  nav_get_languages, NULL, bridge);
        js_acc_cls(vm, cls, "userAgentData", nav_get_userAgentData, NULL, bridge);
        js_acc_cls(vm, cls, "onLine",        nav_get_bool, NULL, (void*)(intptr_t)0);
        js_acc_cls(vm, cls, "cookieEnabled", nav_get_bool, NULL, (void*)(intptr_t)1);
        /* Not const: pages do assign to these during feature detection and a
         * browser silently ignores the write rather than throwing. */
        vm_reg_var(vm, cls, "webdriver",       var_new_bool(vm, false), false);
        vm_reg_var(vm, cls, "pdfViewerEnabled", var_new_bool(vm, false), false);
        vm_reg_native(vm, cls, "javaEnabled()",      nav_javaEnabled, bridge);
        vm_reg_native(vm, cls, "sendBeacon(u, d)",   nav_sendBeacon,  bridge);
        vm_reg_native(vm, cls, "vibrate(n)",         nav_vibrate,     bridge);
    }
    var_t* navigator = new_obj(vm, CLS_NAVIGATOR, 0);
    web_publish(vm, window, "navigator", navigator);
    if(navigator != NULL) var_add(bridge, "@@navigator", navigator);

    /* ---- screen ---- */
    cls = vm_new_class(vm, CLS_SCREEN);
    if(cls != NULL) {
        js_acc_cls(vm, cls, "width",       metric_get, NULL, (void*)(intptr_t)3);
        js_acc_cls(vm, cls, "height",      metric_get, NULL, (void*)(intptr_t)4);
        js_acc_cls(vm, cls, "availWidth",  metric_get, NULL, (void*)(intptr_t)5);
        js_acc_cls(vm, cls, "availHeight", metric_get, NULL, (void*)(intptr_t)6);
        js_acc_cls(vm, cls, "colorDepth",  metric_get, NULL, (void*)(intptr_t)7);
        js_acc_cls(vm, cls, "pixelDepth",  metric_get, NULL, (void*)(intptr_t)8);
        js_acc_cls(vm, cls, "orientation", screen_get_orientation, NULL, bridge);
    }
    var_t* screen = new_obj(vm, CLS_SCREEN, 0);
    web_publish(vm, window, "screen", screen);
    if(screen != NULL) var_add(bridge, "@@screen", screen);

    /* ---- performance ---- */
    cls = vm_new_class(vm, CLS_PERF);
    if(cls != NULL) {
        vm_reg_native(vm, cls, "now()",           perf_now,            bridge);
        js_acc_cls(vm, cls, "timeOrigin", perf_timeOrigin_get, NULL, bridge);
        /* No performance timeline is kept; the methods exist so that
         * instrumentation code does not throw. */
        vm_reg_native(vm, cls, "mark(n)",           perf_noop, bridge);
        vm_reg_native(vm, cls, "measure(n, a, b)",  perf_noop, bridge);
        vm_reg_native(vm, cls, "clearMarks(n)",     perf_noop, bridge);
        vm_reg_native(vm, cls, "clearMeasures(n)",  perf_noop, bridge);
        vm_reg_native(vm, cls, "getEntries()",          perf_entries_empty, bridge);
        vm_reg_native(vm, cls, "getEntriesByType(t)",   perf_entries_empty, bridge);
        vm_reg_native(vm, cls, "getEntriesByName(n, t)", perf_entries_empty, bridge);
    }
    var_t* performance = new_obj(vm, CLS_PERF, 0);
    if(performance != NULL) {
        vm->gc.gc_defer++;
        var_t* nav = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(nav, "type",          var_new_int(vm, 0));   /* 0 = navigate */
        var_add(nav, "redirectCount", var_new_int(vm, 0));
        var_add(performance, "navigation", nav);
        var_t* mem = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(mem, "jsHeapSizeLimit", var_new_int(vm, 0));
        var_add(mem, "totalJSHeapSize", var_new_int(vm, 0));
        var_add(mem, "usedJSHeapSize",  var_new_int(vm, 0));
        var_add(performance, "memory", mem);
        vm->gc.gc_defer--;
        var_add(bridge, "@@performance", performance);
    }
    web_publish(vm, window, "performance", performance);

    /* ---- PerformanceObserver: see perfobs_supported_types() for why the
     * global must exist even though no observation is ever performed. ---- */
    cls = vm_new_class(vm, "PerformanceObserver");
    if(cls != NULL) {
        vm_reg_native(vm, cls, "constructor(cb)", native_perfobs_ctor, bridge);
        vm_reg_native(vm, cls, "observe(o)",      native_perfobs_noop, bridge);
        vm_reg_native(vm, cls, "disconnect()",    native_perfobs_noop, bridge);
        vm_reg_native(vm, cls, "takeRecords()",   native_perfobs_takeRecords, bridge);
        vm_reg_static(vm, cls, "getSupportedEntryTypes()", native_perfobs_supported, bridge);
        vm->gc.gc_defer++;
        var_add(cls, "supportedEntryTypes", perfobs_supported_types(vm));
        vm->gc.gc_defer--;
    }

    /* ---- document extras ---- */
    cls = var_find_own_member_var(vm->root, CLS_DOCUMENT);
    if(cls != NULL) {
        js_acc_cls(vm, cls, "cookie", native_doc_get_cookie, native_doc_set_cookie, bridge);
        js_acc_cls(vm, cls, "hidden",          doc_info_get, NULL, (void*)(intptr_t)0);
        js_acc_cls(vm, cls, "visibilityState", doc_info_get, NULL, (void*)(intptr_t)1);
        js_acc_cls(vm, cls, "compatMode",      doc_info_get, NULL, (void*)(intptr_t)2);
        js_acc_cls(vm, cls, "contentType",     doc_info_get, NULL, (void*)(intptr_t)3);
        js_acc_cls(vm, cls, "lastModified",    doc_info_get, NULL, (void*)(intptr_t)4);
        js_acc_cls(vm, cls, "activeElement", native_doc_get_activeElement, NULL, bridge);
        vm_reg_native(vm, cls, "hasFocus()", native_doc_hasFocus, bridge);
        /* Live-ish collections. index: 0 form, 1 img, 2 script, 3 a[href],
         * 4 a[name], 5 object. */
        js_acc_cls(vm, cls, "forms",   doc_collection_get, NULL, (void*)(intptr_t)0);
        js_acc_cls(vm, cls, "images",  doc_collection_get, NULL, (void*)(intptr_t)1);
        js_acc_cls(vm, cls, "scripts", doc_collection_get, NULL, (void*)(intptr_t)2);
        js_acc_cls(vm, cls, "links",   doc_collection_get, NULL, (void*)(intptr_t)3);
        js_acc_cls(vm, cls, "anchors", doc_collection_get, NULL, (void*)(intptr_t)4);
        js_acc_cls(vm, cls, "embeds",  doc_collection_get, NULL, (void*)(intptr_t)5);
        js_acc_cls(vm, cls, "plugins", doc_collection_get, NULL, (void*)(intptr_t)5);
    }

    /* ---- XMLHttpRequest ---- */
    cls = vm_new_class(vm, CLS_XHR);
    if(cls != NULL) {
        vm_reg_native(vm, cls, "constructor()",             native_xhr_ctor,                  bridge);
        vm_reg_native(vm, cls, "open(m, u, a)",             native_xhr_open,                  bridge);
        vm_reg_native(vm, cls, "send(b)",                   native_xhr_send,                  bridge);
        vm_reg_native(vm, cls, "abort()",                   native_xhr_abort,                 bridge);
        vm_reg_native(vm, cls, "setRequestHeader(k, v)",    native_xhr_setRequestHeader,      bridge);
        vm_reg_native(vm, cls, "getResponseHeader(k)",      native_xhr_getResponseHeader,     bridge);
        vm_reg_native(vm, cls, "getAllResponseHeaders()",   native_xhr_getAllResponseHeaders, bridge);
        vm_reg_native(vm, cls, "overrideMimeType(m)",       native_xhr_overrideMimeType,      bridge);
        vm_reg_var(vm, cls, "UNSENT",           var_new_int(vm, 0), true);
        vm_reg_var(vm, cls, "OPENED",           var_new_int(vm, 1), true);
        vm_reg_var(vm, cls, "HEADERS_RECEIVED", var_new_int(vm, 2), true);
        vm_reg_var(vm, cls, "LOADING",          var_new_int(vm, 3), true);
        vm_reg_var(vm, cls, "DONE",             var_new_int(vm, 4), true);
    }

    /* ---- Response / Headers (fetch) ---- */
    /* ---- MessageChannel / MessagePort: React's scheduler and the mtop
     * bundle's promise polyfill flush microtasks through a port pair. ---- */
    cls = vm_new_class(vm, CLS_MSGPORT);
    if(cls != NULL) {
        vm_reg_native(vm, cls, "postMessage(m)",            native_mc_postMessage,      bridge);
        vm_reg_native(vm, cls, "start()",                   native_mc_noop,             bridge);
        vm_reg_native(vm, cls, "close()",                   native_mc_close,            bridge);
        vm_reg_native(vm, cls, "addEventListener(t, f)",    native_mc_addEventListener, bridge);
        vm_reg_native(vm, cls, "removeEventListener(t, f)", native_mc_noop,             bridge);
    }
    cls = vm_new_class(vm, CLS_MSGCHAN);
    if(cls != NULL)
        vm_reg_native(vm, cls, "constructor()", native_mc_ctor, bridge);

    cls = vm_new_class(vm, CLS_RESPONSE);
    if(cls != NULL) {
        vm_reg_native(vm, cls, "text()", native_resp_text, bridge);
        vm_reg_native(vm, cls, "json()", native_resp_json, bridge);
        js_acc_cls(vm, cls, "headers", native_resp_get_headers, NULL, bridge);
    }
    cls = vm_new_class(vm, CLS_HEADERS);
    if(cls != NULL) {
        vm_reg_native(vm, cls, "get(k)", native_headers_get, bridge);
        vm_reg_native(vm, cls, "has(k)", native_headers_has, bridge);
    }

    /* ---- URL / URLSearchParams ---- */
    web_register_url(vm, bridge);

    /* ---- crypto ---- */
    var_t* crypto = var_new_obj_no_proto(vm, NULL, NULL);
    if(crypto != NULL) {
        vm_reg_native_on(vm, crypto, "getRandomValues(a)", native_crypto_getRandomValues, bridge);
        vm_reg_native_on(vm, crypto, "randomUUID()",       native_crypto_randomUUID,      bridge);
        /* SubtleCrypto: digest() only (see the SHA block above). The object has
         * to exist for `window.crypto.subtle` feature tests; a page that calls
         * an unimplemented operation gets a rejected promise, not a TypeError. */
        var_t* subtle = var_new_obj_no_proto(vm, NULL, NULL);
        if(subtle != NULL) {
            vm_reg_native_on(vm, subtle, "digest(a, d)",         native_crypto_subtle_digest,      bridge);
            vm_reg_native_on(vm, subtle, "encrypt(a, k, d)",     native_crypto_subtle_unsupported, bridge);
            vm_reg_native_on(vm, subtle, "decrypt(a, k, d)",     native_crypto_subtle_unsupported, bridge);
            vm_reg_native_on(vm, subtle, "sign(a, k, d)",        native_crypto_subtle_unsupported, bridge);
            vm_reg_native_on(vm, subtle, "verify(a, k, s, d)",   native_crypto_subtle_unsupported, bridge);
            vm_reg_native_on(vm, subtle, "generateKey(a, e, u)", native_crypto_subtle_unsupported, bridge);
            vm_reg_native_on(vm, subtle, "importKey(f, k, a, e, u)", native_crypto_subtle_unsupported, bridge);
            vm_reg_native_on(vm, subtle, "exportKey(f, k)",      native_crypto_subtle_unsupported, bridge);
            vm_reg_native_on(vm, subtle, "deriveBits(a, k, l)",  native_crypto_subtle_unsupported, bridge);
            vm_reg_native_on(vm, subtle, "deriveKey(a, k, d, e, u)", native_crypto_subtle_unsupported, bridge);
            vm_reg_native_on(vm, subtle, "wrapKey(f, k, w, a)",  native_crypto_subtle_unsupported, bridge);
            vm_reg_native_on(vm, subtle, "unwrapKey(f, k, w, a, d, e, u)", native_crypto_subtle_unsupported, bridge);
            var_add(crypto, "subtle", subtle);
        }
        var_add(bridge, "@@crypto", crypto);
    }
    web_publish(vm, window, "crypto", crypto);

    /* ---- self references ----
     * There is one window and no frames, so every one of these aliases it.
     * globalThis points at window rather than at vm->root: they differ in
     * mario, and window is the one carrying the mirrored global surface. The
     * visible consequence is that `globalThis.x = 1` is not readable as a
     * bare `x`, which is documented in js_web.h's gap list. */
    if(window != NULL) {
        vm->gc.gc_defer++;
        var_add(window, "window",     window);
        var_add(window, "self",       window);
        var_add(window, "top",        window);
        var_add(window, "parent",     window);
        var_add(window, "frames",     window);
        var_add(window, "globalThis", window);
        vm->gc.gc_defer--;
    }
    web_publish(vm, window, "self",       window);
    web_publish(vm, window, "top",        window);
    web_publish(vm, window, "parent",     window);
    web_publish(vm, window, "frames",     window);
    web_publish(vm, window, "globalThis", window);
    /* window.status is writable and ignored; window.closed is always false. */
    if(window != NULL) {
        var_add(window, "status", var_new_str(vm, ""));
        var_add(window, "closed", var_new_bool(vm, false));
        var_add(window, "name",   var_new_str(vm, ""));
        var_add(window, "opener", var_new_null(vm));
        var_add(window, "frameElement", var_new_null(vm));
        var_add(window, "isSecureContext", var_new_bool(vm, false));
    }

    /* window.origin mirrors location.origin, so it is a live getter reading the
     * document URL rather than a snapshot taken at registration time. */
    js_acc_on(vm, vm->root, "origin", loc_get, NULL, (void*)(intptr_t)8);
    if(window != NULL)
        js_acc_on(vm, window, "origin", loc_get, NULL, (void*)(intptr_t)8);

    /* ---- mirror the globals onto window ---- */
    web_mirror_globals(vm, window);

    return true;
}
