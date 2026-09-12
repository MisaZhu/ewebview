// litehtml document_container for ewebview, driven entirely by the porting HAL.
//
// Ported from widget++'s XContainer.cc. Platform calls map onto eweb_port_t:
//   kernel_tic_ms     -> clock.tic_ms
//   font_*            -> font.*
//   graph_*           -> gfx.*
//   vfs_readfile      -> net.read_file
//   tinyhttpsc        -> net.request / net.free_response (redirects followed here)
//   X::getResFullName / x_get_res_name -> net.resolve_resource (res://)
//   webp/graph_image  -> image.decode
//   CookieJar         -> EWebCookieJar
//   WidgetWebview     -> EWebContainerHost
// Debug traces compile in with -DEWEBVIEW_DEBUG (see EWebLog.h).

#include "EWebContainer.h"
#include "EWebCookies.h"
#include "EWebLog.h"
#include "eweb_el_input.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>

using namespace litehtml;

namespace eweb {
namespace {

static uint64_t make_char_width_key(const EWebFontInfo* fontInfo, uint32_t codepoint)
{
    uintptr_t font_ptr = reinterpret_cast<uintptr_t>(fontInfo->font);
    uint64_t font_hash = ((uint64_t)(font_ptr >> 4)) & 0xFFFFFFULL;
    return (((uint64_t)(fontInfo->size & 0xFFFF)) << 48) |
           (((uint64_t)(codepoint & 0xFFFFFF)) << 24) |
           font_hash;
}

static bool next_utf8_codepoint(const char*& p, uint32_t& codepoint)
{
    unsigned char c0 = (unsigned char)*p;
    if(c0 == 0) {
        return false;
    }

    if(c0 < 0x80) {
        codepoint = c0;
        ++p;
        return true;
    }

    if((c0 & 0xE0) == 0xC0) {
        unsigned char c1 = (unsigned char)p[1];
        if((c1 & 0xC0) != 0x80) {
            codepoint = c0;
            ++p;
            return true;
        }
        codepoint = ((uint32_t)(c0 & 0x1F) << 6) | (uint32_t)(c1 & 0x3F);
        p += 2;
        return true;
    }

    if((c0 & 0xF0) == 0xE0) {
        unsigned char c1 = (unsigned char)p[1];
        unsigned char c2 = (unsigned char)p[2];
        if((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) {
            codepoint = c0;
            ++p;
            return true;
        }
        codepoint = ((uint32_t)(c0 & 0x0F) << 12) |
                    ((uint32_t)(c1 & 0x3F) << 6) |
                    (uint32_t)(c2 & 0x3F);
        p += 3;
        return true;
    }

    if((c0 & 0xF8) == 0xF0) {
        unsigned char c1 = (unsigned char)p[1];
        unsigned char c2 = (unsigned char)p[2];
        unsigned char c3 = (unsigned char)p[3];
        if((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
            codepoint = c0;
            ++p;
            return true;
        }
        codepoint = ((uint32_t)(c0 & 0x07) << 18) |
                    ((uint32_t)(c1 & 0x3F) << 12) |
                    ((uint32_t)(c2 & 0x3F) << 6) |
                    (uint32_t)(c3 & 0x3F);
        p += 4;
        return true;
    }

    codepoint = c0;
    ++p;
    return true;
}

static std::string trim_request_url(const std::string& url)
{
    size_t begin = 0;
    size_t end = url.size();

    while(begin < end) {
        char ch = url[begin];
        if(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            ++begin;
            continue;
        }
        break;
    }

    while(end > begin) {
        char ch = url[end - 1];
        if(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            --end;
            continue;
        }
        break;
    }

    if(end > begin + 1) {
        char first = url[begin];
        char last = url[end - 1];
        if((first == '"' && last == '"') ||
           (first == '\'' && last == '\'') ||
           (first == '`' && last == '`')) {
            ++begin;
            --end;
        }
    }

    return url.substr(begin, end - begin);
}

/* Case-insensitive header-name compare, local so this file needs no libc
 * extension (strcasecmp lives in <strings.h>, not <string.h>). */
static bool header_name_is(const char* key, const char* want)
{
    if(key == NULL)
        return false;
    size_t i = 0;
    for(; key[i] != 0 && want[i] != 0; i++) {
        char a = key[i];
        char b = want[i];
        if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if(b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if(a != b)
            return false;
    }
    return key[i] == 0 && want[i] == 0;
}

/* Every Set-Cookie of one response, in wire order. A response may carry any
 * number of them (one per cookie), so the by-key lookup - which returns the
 * first match only - cannot be used here; the indexed walk over the header
 * list can. The returned strings are copies: the response owns the header
 * text and frees it with itself. */
static void collect_set_cookies(const eweb_http_response_t* resp, std::vector<std::string>& out)
{
    if(resp == NULL)
        return;
    for(int i = 0; i < resp->header_count; i++) {
        const char* key = resp->headers[i].key;
        const char* val = resp->headers[i].value;
        if(val == NULL || !header_name_is(key, "set-cookie"))
            continue;
        out.push_back(std::string(val));
    }
}

/* First value of one response header, or NULL. */
static const char* find_header(const eweb_http_response_t* resp, const char* name)
{
    if(resp == NULL)
        return NULL;
    for(int i = 0; i < resp->header_count; i++) {
        if(header_name_is(resp->headers[i].key, name))
            return resp->headers[i].value;
    }
    return NULL;
}

/* Same cap the original container applied internally. */
static const int kMaxRedirects = 5;

static bool is_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

} /* anonymous namespace */

static inline int char_width_cache_slot(uint64_t key)
{
    return (int)(key & 8191ULL);
}

EWebContainer::EWebContainer(const eweb_port_t* port, EWebContainerHost* host)
{
    m_port = port;
    m_host = host;
    m_client_width = 640;
    m_client_height = 480;
    m_text_width_calls = 0;
    m_text_width_ms = 0;
    m_draw_text_calls = 0;
    m_draw_text_ms = 0;
    m_text_width_hits = 0;
    m_text_width_misses = 0;
    m_char_width_hits = 0;
    m_char_width_misses = 0;
    m_create_font_calls = 0;
    m_create_font_ms = 0;
    m_defer_image_load = false;
    m_abort = false;
    memset(m_char_width_keys, 0, sizeof(m_char_width_keys));
    memset(m_char_width_vals, 0, sizeof(m_char_width_vals));
}

EWebContainer::~EWebContainer(void)
{
    for (auto& pair : m_fonts) {
        if (pair.second.font && m_port->font.destroy) {
            m_port->font.destroy(m_port->font.ud, pair.second.font);
        }
    }
    m_fonts.clear();

    for (auto& pair : m_images) {
        if (pair.second.image && m_port->gfx.surface_free) {
            m_port->gfx.surface_free(m_port->gfx.ud, pair.second.image);
        }
    }
    m_images.clear();
}

uint32_t EWebContainer::web_color_to_argb(const litehtml::web_color& c)
{
    return ((uint32_t)c.alpha << 24) | ((uint32_t)c.red << 16) |
           ((uint32_t)c.green << 8) | (uint32_t)c.blue;
}

litehtml::uint_ptr EWebContainer::create_font(const litehtml::tchar_t* faceName, int size, int weight, litehtml::font_style italic, unsigned int decoration, litehtml::font_metrics* fm)
{
    uint64_t start_ms = m_port->clock.tic_ms(m_port->clock.ud);
    /* The CSS family is passed through to the port, which maps it onto a
     * concrete face (the reference port ships one CJK face and treats the
     * family as advisory). */
    std::string fontName = (faceName != NULL && faceName[0] != 0) ? faceName : "sans-serif";
    std::string key = fontName;
    key += "-" + std::to_string(size) + "px";

    EWebFontInfo fontInfo;
    fontInfo.font = NULL;
    fontInfo.size = size;

    if (m_fonts.find(key) != m_fonts.end()) {
        fontInfo = m_fonts[key];
    } else {
        if(m_port->font.create)
            fontInfo.font = m_port->font.create(m_port->font.ud, fontName.c_str());
        fontInfo.size = size;
        m_fonts[key] = fontInfo;
        if (fontInfo.font == NULL) {
            m_create_font_calls++;
            m_create_font_ms += (uint32_t)(m_port->clock.tic_ms(m_port->clock.ud) - start_ms);
            return 0;
        }
    }

    if(fontInfo.font == NULL) {
        m_create_font_calls++;
        m_create_font_ms += (uint32_t)(m_port->clock.tic_ms(m_port->clock.ud) - start_ms);
        return 0;
    }

    if (fm && m_port->font.metrics) {
        eweb_font_metrics_t m;
        m_port->font.metrics(m_port->font.ud, fontInfo.font, size, &m);
        fm->ascent = m.ascent;
        fm->descent = m.descent;
        fm->height = m.height;
        fm->x_height = m.x_height;
        fm->draw_spaces = italic == fontStyleItalic || decoration;
    }

    m_create_font_calls++;
    m_create_font_ms += (uint32_t)(m_port->clock.tic_ms(m_port->clock.ud) - start_ms);
    return (uint_ptr)&m_fonts[key];
}

void EWebContainer::delete_font(litehtml::uint_ptr hFont)
{
    return;
}

int EWebContainer::text_width(const litehtml::tchar_t* text, litehtml::uint_ptr hFont)
{
    EWebFontInfo* fontInfo = (EWebFontInfo*)hFont;

    if(!fontInfo || !fontInfo->font || !text) {
        return 0;
    }

    if(!text[0]) {
        return 0;
    }

    /* text_width is the hottest callback of every long litehtml pass (doc
     * creation, style walk, render). Those passes run on the ENGINE thread,
     * so there is no UI loop to pump; instead short-circuit the instant a
     * termination is requested (STOP/NAVIGATE/RELOAD raise the volatile
     * buildAbort flag from the UI thread, which the single-threaded engine
     * cannot otherwise notice mid-parse). Return a cheap dummy width once
     * aborted - the half-built document is discarded, correctness does not
     * matter. */
    if(m_abort || (m_host && m_host->buildAbortRequested()))
        return 1;

    m_text_width_calls++;
    uint64_t start_ms = m_port->clock.tic_ms(m_port->clock.ud);
    const char* p = text;
    uint32_t w = 0;
    bool cache_hit_only = true;
    while(*p) {
        uint32_t codepoint = 0;
        if(!next_utf8_codepoint(p, codepoint)) {
            break;
        }

        uint64_t char_key = make_char_width_key(fontInfo, codepoint);
        int slot = char_width_cache_slot(char_key);
        if(m_char_width_keys[slot] == char_key) {
            m_char_width_hits++;
            w += (uint32_t)m_char_width_vals[slot];
            continue;
        }

        cache_hit_only = false;
        m_char_width_misses++;
        int cw = m_port->font.char_width(m_port->font.ud, fontInfo->font,
                                         fontInfo->size, codepoint);
        if(cw < 0)
            cw = 0;
        m_char_width_keys[slot] = char_key;
        m_char_width_vals[slot] = cw;
        w += (uint32_t)cw;
    }

    m_text_width_ms += (uint32_t)(m_port->clock.tic_ms(m_port->clock.ud) - start_ms);
    if(cache_hit_only) {
        m_text_width_hits++;
    } else {
        m_text_width_misses++;
    }
    return w;
}

void EWebContainer::draw_text(litehtml::uint_ptr hdc, const litehtml::tchar_t* text, litehtml::uint_ptr hFont, litehtml::web_color color, const litehtml::position& pos)
{
    EWebFontInfo* fontInfo = (EWebFontInfo*)hFont;
    if (!fontInfo || !fontInfo->font || !text) {
        return;
    }

    eweb_surface_t* s = (eweb_surface_t*)hdc;
    if (!s || !m_port->font.draw_text) {
        return;
    }

    uint32_t argb = web_color_to_argb(color);

    // Force opaque color if alpha is 0
    if ((argb >> 24) == 0) {
        argb = 0xFF000000 | (argb & 0xFFFFFF);
    }

    uint64_t start_ms = m_port->clock.tic_ms(m_port->clock.ud);
    m_port->font.draw_text(m_port->font.ud, s, pos.x, pos.y, text,
                           fontInfo->font, fontInfo->size, argb);
    m_draw_text_calls++;
    m_draw_text_ms += (uint32_t)(m_port->clock.tic_ms(m_port->clock.ud) - start_ms);
}

void EWebContainer::resetPerfStats()
{
    m_text_width_calls = 0;
    m_text_width_ms = 0;
    m_draw_text_calls = 0;
    m_draw_text_ms = 0;
    m_text_width_hits = 0;
    m_text_width_misses = 0;
    m_char_width_hits = 0;
    m_char_width_misses = 0;
    m_create_font_calls = 0;
    m_create_font_ms = 0;
}

void EWebContainer::getPerfStats(uint32_t& textWidthCalls, uint32_t& textWidthMs,
                                 uint32_t& drawTextCalls, uint32_t& drawTextMs,
                                 uint32_t& textWidthHits, uint32_t& textWidthMisses,
                                 uint32_t& charWidthHits, uint32_t& charWidthMisses,
                                 uint32_t& createFontCalls, uint32_t& createFontMs) const
{
    textWidthCalls = m_text_width_calls;
    textWidthMs = m_text_width_ms;
    drawTextCalls = m_draw_text_calls;
    drawTextMs = m_draw_text_ms;
    textWidthHits = m_text_width_hits;
    textWidthMisses = m_text_width_misses;
    charWidthHits = m_char_width_hits;
    charWidthMisses = m_char_width_misses;
    createFontCalls = m_create_font_calls;
    createFontMs = m_create_font_ms;
}

int EWebContainer::pt_to_px(int pt)
{
    return pt;
}

int EWebContainer::get_default_font_size() const
{
    return 16;
}

const litehtml::tchar_t* EWebContainer::get_default_font_name() const
{
    return _t("sans-serif");
}

void EWebContainer::draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker)
{
    eweb_surface_t* s = (eweb_surface_t*)hdc;
    if (!s)
        return;
    const eweb_gfx_t* gfx = &m_port->gfx;

    if (!marker.image.empty())
    {
    }
    else
    {
        uint32_t color = web_color_to_argb(marker.color);

        switch (marker.marker_type)
        {
        case litehtml::list_style_type_circle:
            if(gfx->circle)
                gfx->circle(gfx->ud, s, marker.pos.x, marker.pos.y, marker.pos.width, 1, color);
            break;
        case litehtml::list_style_type_disc:
            if(gfx->fill_circle)
                gfx->fill_circle(gfx->ud, s, marker.pos.x, marker.pos.y, marker.pos.width, color);
            break;
        case litehtml::list_style_type_square:
            if(gfx->fill_rect)
                gfx->fill_rect(gfx->ud, s, marker.pos.x, marker.pos.y, marker.pos.width, marker.pos.height, color);
            break;
        default:
            /* numbered/alpha markers are laid out as text by litehtml itself;
             * nothing to draw here. */
            break;
        }
    }
}

static std::string url_origin(const std::string& url) {
    size_t scheme_end = url.find("://");
    if(scheme_end == std::string::npos)
        return std::string();
    size_t path_start = url.find('/', scheme_end + 3);
    if(path_start == std::string::npos)
        return url;
    return url.substr(0, path_start);
}

const std::string EWebContainer::getFullURL(const eweb_port_t* port, const std::string& src, const std::string& baseurl) {
    std::string clean_src = trim_request_url(src);
    std::string clean_baseurl = trim_request_url(baseurl);
    std::string path = clean_src;
    if(clean_src.compare(0, 7, "file://") == 0 ||
            clean_src.compare(0, 7, "http://") == 0 ||
            clean_src.compare(0, 8, "https://") == 0) {
        return clean_src;
    }
    else if(clean_src.compare(0, 6, "res://") == 0) {
        /* The port's private resource scheme: resolve to a real path, then
         * dress it as file:// so the rest of the pipeline treats it as a
         * local read. */
        if(port && port->net.resolve_resource) {
            char buf[1024];
            const char* full = port->net.resolve_resource(port->net.ud,
                    clean_src.substr(6).c_str(), buf, (int)sizeof(buf));
            if(full != NULL) {
                path = "file:/";
                path += full;
                return path;
            }
        }
        return clean_src;
    }
    else if(clean_src.compare(0, 2, "//") == 0) {
        // Protocol-relative URL: //example.com/path
        // Use https by default, or http if baseurl uses http
        if(clean_baseurl.compare(0, 7, "http://") == 0) {
            path = "http:" + clean_src;
        } else {
            path = "https:" + clean_src;
        }
        return path;
    }

    /* Root-relative path (/foo/bar.css): resolve against the base origin,
     * otherwise "/_next/static/css/x.css" would be fetched as-is and fail. */
    if(!clean_src.empty() && clean_src[0] == '/') {
        std::string origin = url_origin(clean_baseurl);
        if(!origin.empty())
            return origin + clean_src;
        return clean_src;
    }
    if(clean_src.compare(0, 2, "./") == 0)
        clean_src.erase(0, 2);

    if (!clean_baseurl.empty()) {
        size_t slash = clean_baseurl.find_last_of('/');
        std::string base_dir = slash == std::string::npos ? clean_baseurl : clean_baseurl.substr(0, slash + 1);
        if(!base_dir.empty())
            path = base_dir + clean_src;
    }
    return path;
}

std::string EWebContainer::normalizeURL(const eweb_port_t* port, const std::string& url, const std::string& baseurl)
{
    return getFullURL(port, url, baseurl);
}

uint8_t* EWebContainer::loadURL(const eweb_port_t* port, const std::string& url, int* sz,
                                const std::string& pageUrl, bool topLevel)
{
    uint8_t* ret = NULL;
    if(sz != NULL)
        *sz = 0;
    if(port == NULL)
        return NULL;

    std::string full_url = getFullURL(port, url, "");
    EWEB_LOG("[ewebview] loadURL: %s -> %s\n", url.c_str(), full_url.c_str());
    if(full_url.compare(0, 7, "file://") == 0) {
        std::string path = full_url.substr(6);
        if(!path.empty() && port->net.read_file) { //local file
            uint64_t start_ms = port->clock.tic_ms(port->clock.ud);
            ret = port->net.read_file(port->net.ud, path.c_str(), sz);
            uint32_t cost_ms = (uint32_t)(port->clock.tic_ms(port->clock.ud) - start_ms);
            EWEB_LOG("[ewebview] loadURL file: path=%s ok=%d size=%d cost=%u ms\n",
                path.c_str(), ret != NULL ? 1 : 0, sz != NULL ? *sz : 0, cost_ms);
            return ret;
        }
    }
    else if(full_url.compare(0, 7, "http://") == 0 || full_url.compare(0, 8, "https://") == 0) {
        if(!port->net.request || !port->net.free_response)
            return NULL;
        uint64_t start_ms = port->clock.tic_ms(port->clock.ud);
        /* One request per redirect hop, followed here instead of inside the
         * port. A library-internal redirect loop would ship the first host's
         * Cookie: header to every later host, and would drop every Set-Cookie
         * but the last one. Doing the hops here also lets a relative Location
         * be resolved against the URL it came from. */
        std::string cur_url = trim_request_url(full_url);
        for(int hop = 0; hop <= kMaxRedirects; hop++) {
            /* Same-site cookies for THIS hop only: recomputed per hop so a
             * redirect to another site never carries the previous site's jar,
             * and only what the jar's domain/path/Secure/SameSite rules allow
             * for this URL. */
            std::string cookie_header = EWebCookieJar::instance().requestHeader(cur_url, pageUrl, topLevel);
            eweb_http_header_t req_hdr;
            req_hdr.key = "Cookie";
            req_hdr.value = cookie_header.c_str();
            const eweb_http_header_t* req_hdrs = cookie_header.empty() ? NULL : &req_hdr;
            int req_hdr_count = cookie_header.empty() ? 0 : 1;
            if(!cookie_header.empty()) {
                EWEB_LOG("[ewebview] cookie header: url=%s page=%s top=%d len=%d\n",
                    cur_url.c_str(), pageUrl.c_str(), topLevel ? 1 : 0, (int)cookie_header.size());
            }

            eweb_http_response_t resp;
            if(!port->net.request(port->net.ud, cur_url.c_str(), req_hdrs, req_hdr_count, &resp)) {
                EWEB_LOG("[ewebview] loadURL http: request failed url=%s cost=%u ms\n",
                    cur_url.c_str(), (uint32_t)(port->clock.tic_ms(port->clock.ud) - start_ms));
                return NULL;
            }

            /* Store Set-Cookie before anything else: a login flow sets its
             * session cookie on the 302, and an error status may carry one
             * too. The header strings belong to the response, so they are
             * copied out here. */
            std::vector<std::string> set_cookies;
            collect_set_cookies(&resp, set_cookies);
            if(!set_cookies.empty()) {
                EWebCookieJar::instance().storeResponseCookies(cur_url, set_cookies);
                EWEB_LOG("[ewebview] cookies stored: url=%s count=%d jar=%d\n",
                    cur_url.c_str(), (int)set_cookies.size(), (int)EWebCookieJar::instance().count());
            }

            const char* location = find_header(&resp, "location");

            if(!resp.error && is_redirect_status(resp.status) &&
               location != NULL && hop < kMaxRedirects) {
                std::string next_url = getFullURL(port, trim_request_url(location), cur_url);
                port->net.free_response(port->net.ud, &resp);
                if(next_url.empty() || next_url == cur_url) {
                    EWEB_LOG("[ewebview] loadURL http: unusable redirect url=%s\n", cur_url.c_str());
                    return NULL;
                }
                EWEB_LOG("[ewebview] loadURL redirect: hop=%d status=%d -> %s\n",
                    hop, resp.status, next_url.c_str());
                cur_url = next_url;
                continue;
            }

            if(resp.error) {
                EWEB_LOG("[ewebview] loadURL http error: url=%s status=%d body=%d cost=%u ms\n",
                    cur_url.c_str(), resp.status, resp.body_size,
                    (uint32_t)(port->clock.tic_ms(port->clock.ud) - start_ms));
                port->net.free_response(port->net.ud, &resp);
                return NULL;
            }

            if(resp.status != 200) {
                EWEB_LOG("[ewebview] loadURL http status: url=%s status=%d body=%d cost=%u ms\n",
                    cur_url.c_str(), resp.status, resp.body_size,
                    (uint32_t)(port->clock.tic_ms(port->clock.ud) - start_ms));
                port->net.free_response(port->net.ud, &resp);
                return NULL;
            }

            if(resp.body == NULL || resp.body_size <= 0) {
                EWEB_LOG("[ewebview] loadURL http empty body: url=%s status=%d cost=%u ms\n",
                    cur_url.c_str(), resp.status,
                    (uint32_t)(port->clock.tic_ms(port->clock.ud) - start_ms));
                port->net.free_response(port->net.ud, &resp);
                return NULL;
            }

            // Allocate memory and copy data
            ret = (uint8_t*)malloc((size_t)resp.body_size + 1);
            if(ret == NULL) {
                EWEB_LOG("[ewebview] loadURL http: alloc failed url=%s body=%d\n",
                    cur_url.c_str(), resp.body_size);
                port->net.free_response(port->net.ud, &resp);
                return NULL;
            }

            memcpy(ret, resp.body, (size_t)resp.body_size);
            int body_size = resp.body_size;
            port->net.free_response(port->net.ud, &resp);
            ret[body_size] = 0;

            if(sz != NULL)
                *sz = body_size;
            EWEB_LOG("[ewebview] loadURL http: url=%s status=%d size=%d cost=%u ms\n",
                cur_url.c_str(), resp.status, body_size,
                (uint32_t)(port->clock.tic_ms(port->clock.ud) - start_ms));
            return ret;
        }
    }
    EWEB_LOG("[ewebview] loadURL failed: %s\n", full_url.c_str());
    return NULL;
}


void EWebContainer::load_image(const litehtml::tchar_t* src, const litehtml::tchar_t* baseurl, bool redraw_on_ready)
{
    (void)redraw_on_ready;
    if (src == NULL || src[0] == 0)
        return;
    /* Aborted build: do not queue fetches for a page that will never show. */
    if (m_abort)
        return;

    std::string img_path = std::string(src);
    std::string base_url = baseurl ? std::string(baseurl) : std::string();
    if(base_url.empty())
        base_url = m_base_url;
    std::string full_url = getFullURL(m_port, img_path, base_url);
    if(full_url.empty())
        return;

    auto it = m_images.find(full_url);
    if (it != m_images.end()) {
        it->second.ref_count++;
        return;
    }

    if(m_defer_image_load) {
        for(const auto& pending_url : m_pending_image_urls) {
            if(pending_url == full_url) {
                return;
            }
        }
        m_pending_image_urls.push_back(full_url);
        return;
    }

    if(m_host)
        m_host->queueImageTask(full_url);
}

void EWebContainer::setDeferImageLoad(bool defer)
{
    m_defer_image_load = defer;
    EWEB_LOG("[ewebview] image defer: %d pending=%d\n", defer ? 1 : 0, (int)m_pending_image_urls.size());
}

void EWebContainer::flushPendingImages()
{
    if(m_pending_image_urls.empty()) {
        EWEB_LOG("[ewebview] flush pending images: 0\n");
        return;
    }

    std::vector<std::string> pending_urls = m_pending_image_urls;
    EWEB_LOG("[ewebview] flush pending images: %d\n", (int)pending_urls.size());
    std::vector<std::string> retry_urls;
    int queued_count = 0;
    for(const auto& url : pending_urls) {
        if(m_host && m_host->queueImageTask(url)) {
            queued_count++;
        } else {
            retry_urls.push_back(url);
        }
    }
    m_pending_image_urls = retry_urls;
    EWEB_LOG("[ewebview] flush pending images result: queued=%d retry=%d left=%d\n",
        queued_count, (int)retry_urls.size(), (int)m_pending_image_urls.size());
}

eweb_surface_t* EWebContainer::decodeImageData(const eweb_port_t* port, const uint8_t* data, int sz)
{
    if (port == NULL || data == NULL || sz <= 0 || !port->image.decode)
        return NULL;
    return port->image.decode(port->image.ud, data, sz);
}

bool EWebContainer::loadImageData(const std::string& url, uint8_t* data, int sz)
{
    if (data == NULL || sz <= 0 || url.empty())
        return false;

    eweb_surface_t* img = decodeImageData(m_port, data, sz);
    if (img == NULL) {
        EWEB_LOG("[ewebview] image decode failed: url=%s size=%d\n", url.c_str(), sz);
        return false;
    }
    return mountImage(url, img);
}

bool EWebContainer::mountImage(const std::string& url, eweb_surface_t* img)
{
    if (img == NULL || url.empty())
        return false;

    /* Free the previous bitmap for this url (if any) before overwriting the
     * cache slot, so re-decodes of the same url do not leak. */
    auto it = m_images.find(url);
    if (it != m_images.end()) {
        if (it->second.image && it->second.image != img && m_port->gfx.surface_free) {
            m_port->gfx.surface_free(m_port->gfx.ud, it->second.image);
        }
        it->second.image = img;
        it->second.ref_count = 1;
    } else {
        EWebImageInfo info;
        info.image = img;
        info.ref_count = 1;
        m_images[url] = info;
    }
    if(m_port->gfx.surface_dims) {
        int iw = 0, ih = 0;
        m_port->gfx.surface_dims(m_port->gfx.ud, img, &iw, &ih);
        EWEB_LOG("[ewebview] image cached: url=%s dim=%dx%d ref=1\n", url.c_str(), iw, ih);
    }
    return true;
}

eweb_surface_t* EWebContainer::getImage(const std::string& url) const
{
    if (url.empty()) return NULL;
    auto it = m_images.find(url);
    if (it == m_images.end()) return NULL;
    return it->second.image;
}

std::string EWebContainer::resolveUrl(const std::string& src) const
{
    return getFullURL(m_port, src, m_base_url);
}

void EWebContainer::get_image_size(const litehtml::tchar_t* src, const litehtml::tchar_t* baseurl, litehtml::size& sz)
{
    sz.width = 0;
    sz.height = 0;

    if (src == NULL || src[0] == 0)
        return;

    std::string img_path = std::string(src);
    std::string base_url = baseurl ? std::string(baseurl) : std::string();
    if(base_url.empty())
        base_url = m_base_url;
    std::string full_url = getFullURL(m_port, img_path, base_url);
    if(full_url.empty())
        return;

    auto it = m_images.find(full_url);
    if (it != m_images.end() && it->second.image != NULL && m_port->gfx.surface_dims) {
        int iw = 0, ih = 0;
        m_port->gfx.surface_dims(m_port->gfx.ud, it->second.image, &iw, &ih);
        sz.width = iw;
        sz.height = ih;
    }
}

void EWebContainer::draw_background(litehtml::uint_ptr hdc, const litehtml::background_paint& bg)
{
    eweb_surface_t* s = (eweb_surface_t*)hdc;
    if (!s)
        return;
    const eweb_gfx_t* gfx = &m_port->gfx;

    bool do_image = false;
    if (!bg.image.empty() && bg.image_size.width > 0 && bg.image_size.height > 0) {
        std::string full_url = getFullURL(m_port, bg.image, m_base_url);
        if (!full_url.empty()) {
            auto it = m_images.find(full_url);
            eweb_surface_t* img = NULL;
            if (it != m_images.end() && it->second.image != NULL) {
                img = it->second.image;
            }

            if (img != NULL && gfx->blit_fit_alpha && gfx->surface_dims) {
                int iw = 0, ih = 0;
                gfx->surface_dims(gfx->ud, img, &iw, &ih);
                gfx->blit_fit_alpha(gfx->ud, img, 0, 0, iw, ih,
                    s, bg.clip_box.x, bg.clip_box.y, bg.clip_box.width, bg.clip_box.height, 0xFF);
                return;
            }
        }
        do_image = true;
    }

    uint32_t color = web_color_to_argb(bg.color);
    uint8_t alpha = color >> 24;

    if(!do_image) {
        if(alpha == 0)
            return;
        if(gfx->fill_rect)
            gfx->fill_rect(gfx->ud, s, bg.clip_box.x, bg.clip_box.y, bg.clip_box.width, bg.clip_box.height, color);
    } else {
        // Keep image boxes visible before the real bitmap arrives so HTML can
        // render immediately and swap the image in on a later repaint.
        uint32_t fill = alpha != 0 ? color : 0xFFE8E8E8;
        uint32_t stroke = alpha != 0 ? color : 0xFFB0B0B0;
        if(gfx->fill_rect)
            gfx->fill_rect(gfx->ud, s, bg.clip_box.x, bg.clip_box.y, bg.clip_box.width, bg.clip_box.height, fill);
        if(gfx->rect)
            gfx->rect(gfx->ud, s, bg.clip_box.x, bg.clip_box.y, bg.clip_box.width, bg.clip_box.height, stroke);
    }
}

void EWebContainer::draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders, const litehtml::position& draw_pos, bool root)
{
    eweb_surface_t* s = (eweb_surface_t*)hdc;
    if (!s)
        return;

    if (borders.top.width != 0 && borders.top.style > litehtml::border_style_hidden) {
        uint32_t color = web_color_to_argb(borders.top.color);
        if(m_port->gfx.rect)
            m_port->gfx.rect(m_port->gfx.ud, s, draw_pos.x, draw_pos.y, draw_pos.width, draw_pos.height, color);
    }
}

void EWebContainer::transform_text(litehtml::tstring& text, litehtml::text_transform tt)
{
}

void EWebContainer::set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius, bool valid_x, bool valid_y)
{
}

void EWebContainer::del_clip()
{
}

void EWebContainer::clear_images()
{
    for (auto& pair : m_images) {
        if (pair.second.image && m_port->gfx.surface_free) {
            m_port->gfx.surface_free(m_port->gfx.ud, pair.second.image);
        }
    }
    m_images.clear();
}

void EWebContainer::clear_inputs()
{
    // Inputs are DOM-owned elements. EWebContainer only keeps non-owning references.
    m_vecInput.clear();
}

void EWebContainer::get_client_rect(litehtml::position& client) const
{
    client.width = m_client_width;
    client.height = m_client_height;
}

void EWebContainer::set_client_size(int width, int height)
{
    m_client_width = width;
    m_client_height = height;
}

void EWebContainer::on_anchor_click(const litehtml::tchar_t* url, const litehtml::element::ptr& el)
{
    (void)el;
    if(url == nullptr || url[0] == 0 || m_host == nullptr)
        return;
    /* Called from the engine's anchor-click handling on the engine thread, so
     * this only queues the navigation; queueNavigation resolves the href
     * against the current page and the engine loop performs the actual
     * navigation once the dispatch unwinds. */
    m_host->queueNavigation(std::string(url));
}

void EWebContainer::set_cursor(const litehtml::tchar_t* cursor)
{
}

void EWebContainer::import_css(litehtml::tstring& text, const litehtml::tstring& url, litehtml::tstring& baseurl)
{
    if (url.empty())
        return;

    std::string css_path = std::string(url);
    std::string base_url = std::string(baseurl);
    /* el_link passes an empty baseurl; fall back to the page URL so that
     * root-relative stylesheet hrefs (/_next/static/css/...) resolve. */
    if(base_url.empty())
        base_url = m_base_url;
    std::string full_url = getFullURL(m_port, css_path, base_url);
    if(full_url.empty())
        return;
    if(m_host)
        m_host->loadCSS(full_url);
}

void EWebContainer::set_caption(const litehtml::tchar_t* caption)
{
}

void EWebContainer::set_base_url(const litehtml::tchar_t* base_url)
{
    if (base_url != NULL) {
        m_base_url = normalizeURL(m_port, std::string(base_url), "");
    } else {
        m_base_url.clear();
    }
}

litehtml::element* EWebContainer::create_element(const litehtml::tchar_t* tag_name,
                                    const litehtml::string_map& attributes,
                                    litehtml::document* doc)
{
    /* Called once per parsed tag during createFromString: the other reliable
     * abort checkpoint for pages that are markup-heavy but text-light (the
     * engine cannot interrupt itself mid-parse, so it notices a STOP/NAVIGATE
     * here via buildAbortRequested). On abort return the default element - the
     * document is about to be thrown away. */
    if(m_abort || (m_host && m_host->buildAbortRequested()))
        return NULL;
    if (!t_strcasecmp(tag_name, _t("input"))) {
        auto iter = attributes.find(_t("type"));
        if (iter != attributes.end()) {
            if (!t_strcasecmp(iter->second.c_str(), _t("text"))) {
                auto input = new eweb_el_input(doc, m_port, EWEB_INPUT_TEXT);
                m_vecInput.push_back(input);
                return input;
            }
            else if (!t_strcasecmp(iter->second.c_str(), _t("button"))) {
                auto input = new eweb_el_input(doc, m_port, EWEB_INPUT_BUTTON);
                m_vecInput.push_back(input);
                return input;
            }
        }
    }
    return NULL;
}

void EWebContainer::get_media_features(litehtml::media_features& media) const
{
    litehtml::position client;
    get_client_rect(client);
    media.type       = litehtml::media_type_screen;
    media.width      = client.width;
    media.height     = client.height;
    media.device_width  = 640;
    media.device_height = 480;
    media.color     = 8;
    media.monochrome  = 0;
    media.color_index = 256;
    media.resolution  = 96;
}

void EWebContainer::get_language(litehtml::tstring& language, litehtml::tstring& culture) const
{
    language = _t("en");
    culture = _t("");
}

void EWebContainer::link(litehtml::document* ptr, const litehtml::element::ptr& el)
{
    /* el_link lands here because import_css never supplies sheet text
     * synchronously in this async container. Record the <link> media
     * attribute against the queued URL so the engine can drop sheets whose
     * media never matches this device (media="print"): master-sheet
     * selectors carry no media list, so a print-only sheet would otherwise
     * apply its rules on screen. */
    if(!el || !m_host)
        return;
    const tchar_t* rel = el->get_attr(_t("rel"));
    if(!rel || t_strcasecmp(rel, _t("stylesheet")))
        return;
    const tchar_t* href = el->get_attr(_t("href"));
    if(!href || !href[0])
        return;
    std::string full_url = getFullURL(m_port, std::string(href), m_base_url);
    if(full_url.empty())
        return;
    const tchar_t* media = el->get_attr(_t("media"));
    m_host->setCSSMedia(full_url, media ? std::string(media) : std::string());
}

}
