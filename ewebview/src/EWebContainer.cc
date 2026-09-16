// litehtml document_container for ewebview, driven entirely by the porting HAL.
//
// Ported from widget++'s XContainer.cc. Platform calls map onto eweb_port_t:
//   sys_tic_ms     -> clock.tic_ms
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
#include <math.h>
#include <vector>
#include <algorithm>

#include <plutovg.h>

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

/* Non-zero winding scanline fill over all subpaths at once (icon-font paths
 * rely on winding, not parity, for their counters). Geometry is already in
 * device space; spans are clipped to clip. Shared by draw_svg and the
 * transformed border quads, hence the extraction. */
static void fill_polys_nz(eweb_surface_t* s, const eweb_gfx_t* gfx,
                          const litehtml::position& clip,
                          const float* pts, const int* counts, int nsubs,
                          uint32_t argb)
{
    if (!s || !pts || !counts || nsubs <= 0 || !gfx->fill_rect)
        return;
    if ((argb >> 24) == 0)
        return;

    int total = 0;
    for (int i = 0; i < nsubs; i++)
        total += counts[i];
    if (total < 3)
        return;

    float miny = pts[1], maxy = pts[1];
    for (int i = 0; i < total; i++) {
        float yy = pts[i * 2 + 1];
        if (yy < miny) miny = yy;
        if (yy > maxy) maxy = yy;
    }
    int y0 = (int)floorf(miny);
    int y1 = (int)ceilf(maxy);
    if (y0 < clip.y) y0 = clip.y;
    if (y1 > clip.y + clip.height - 1) y1 = clip.y + clip.height - 1;

    struct xedge { float x; int dir; };
    std::vector<xedge> xs;
    xs.reserve(64);

    int base = 0;
    for (int y = y0; y <= y1; y++) {
        float fy = (float)y + 0.5f;
        xs.clear();
        base = 0;
        for (int sp = 0; sp < nsubs; sp++) {
            int n = counts[sp];
            for (int i = 0; i < n; i++) {
                float x1 = pts[(base + i) * 2];
                float y1p = pts[(base + i) * 2 + 1];
                float x2 = pts[(base + (i + 1) % n) * 2];
                float y2p = pts[(base + (i + 1) % n) * 2 + 1];
                if ((y1p <= fy && y2p > fy) || (y2p <= fy && y1p > fy)) {
                    float t = (fy - y1p) / (y2p - y1p);
                    xedge e;
                    e.x = x1 + t * (x2 - x1);
                    e.dir = (y2p > y1p) ? 1 : -1;
                    xs.push_back(e);
                }
            }
            base += n;
        }
        if (xs.size() < 2)
            continue;
        std::sort(xs.begin(), xs.end(), [](const xedge& a, const xedge& b) { return a.x < b.x; });

        /* Sweep: paint where the running winding number is non-zero. */
        int wind = 0;
        int span_start = 0;
        bool inside = false;
        for (size_t i = 0; i < xs.size(); i++) {
            wind += xs[i].dir;
            bool now_inside = (wind != 0);
            if (now_inside && !inside) {
                span_start = (int)(xs[i].x + 0.5f);
                inside = true;
            } else if (!now_inside && inside) {
                int span_end = (int)(xs[i].x + 0.5f);
                int xa = span_start < clip.x ? clip.x : span_start;
                int xb = span_end > clip.x + clip.width ? clip.x + clip.width : span_end;
                if (xb > xa)
                    gfx->fill_rect(gfx->ud, s, xa, y, xb - xa, 1, argb);
                inside = false;
            }
        }
    }
}

static inline int char_width_cache_slot(uint64_t key)
{
    return (int)(key & 8191ULL);
}

/* Anti-aliased counterpart of fill_polys_nz: rasterise the subpaths with
 * plutovg (the same FreeType smooth raster EwokOS' libsvg renders SVGs
 * with) into a bbox-sized offscreen surface, then composite the coverage
 * back through gfx->fill_rect, which blends per the HAL contract and
 * honours the current clip. The offscreen pixels are premultiplied, but
 * the paint is one uniform colour, so a pixel's alpha byte IS its
 * coverage and the un-premultiplied source colour for the blend is just
 * `argb`'s rgb - consecutive equal-alpha pixels share one fill_rect. */
static bool fill_polys_aa(eweb_surface_t* s, const eweb_gfx_t* gfx,
                          const litehtml::position& clip,
                          const float* pts, const int* counts, int nsubs,
                          uint32_t argb)
{
    if (!s || !pts || !counts || nsubs <= 0 || !gfx->fill_rect)
        return false;
    uint32_t alpha = argb >> 24;
    if (alpha == 0)
        return true;

    int total = 0;
    for (int i = 0; i < nsubs; i++)
        total += counts[i];
    if (total < 3)
        return false;

    float minx = pts[0], maxx = pts[0], miny = pts[1], maxy = pts[1];
    for (int i = 0; i < total; i++) {
        float xx = pts[i * 2], yy = pts[i * 2 + 1];
        if (xx < minx) minx = xx;
        if (xx > maxx) maxx = xx;
        if (yy < miny) miny = yy;
        if (yy > maxy) maxy = yy;
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < clip.x) x0 = clip.x;
    if (y0 < clip.y) y0 = clip.y;
    if (x1 > clip.x + clip.width) x1 = clip.x + clip.width;
    if (y1 > clip.y + clip.height) y1 = clip.y + clip.height;
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0)
        return true;
    /* A pathologically huge quad (degenerate transform) would allocate a
     * viewport-sized bitmap per call; the aliased filler handles those. */
    if ((int64_t)w * h > 4 * 1024 * 1024)
        return false;

    plutovg_surface_t* surf = plutovg_surface_create(w, h);
    if (!surf)
        return false;
    plutovg_canvas_t* cv = plutovg_canvas_create(surf);
    if (!cv) {
        plutovg_surface_destroy(surf);
        return false;
    }
    plutovg_canvas_set_fill_rule(cv, PLUTOVG_FILL_RULE_NON_ZERO);
    plutovg_canvas_set_rgba(cv,
                            (float)((argb >> 16) & 0xFF) / 255.0f,
                            (float)((argb >> 8) & 0xFF) / 255.0f,
                            (float)(argb & 0xFF) / 255.0f,
                            (float)alpha / 255.0f);
    plutovg_canvas_new_path(cv);
    int base = 0;
    for (int sp = 0; sp < nsubs; sp++) {
        int n = counts[sp];
        if (n >= 3) {
            plutovg_canvas_move_to(cv, pts[(base) * 2] - (float)x0,
                                   pts[(base) * 2 + 1] - (float)y0);
            for (int i = 1; i < n; i++)
                plutovg_canvas_line_to(cv, pts[(base + i) * 2] - (float)x0,
                                       pts[(base + i) * 2 + 1] - (float)y0);
            plutovg_canvas_close_path(cv);
        }
        base += n;
    }
    plutovg_canvas_fill(cv);

    const uint8_t* px = plutovg_surface_get_data(surf);
    int stride = plutovg_surface_get_stride(surf);
    uint32_t rgb = argb & 0xFFFFFFu;
    for (int y = 0; y < h; y++) {
        const uint32_t* row = (const uint32_t*)(px + (size_t)y * stride);
        int run = -1;
        uint8_t runa = 0;
        for (int x = 0; x <= w; x++) {
            uint8_t a = (x < w) ? (uint8_t)(row[x] >> 24) : 0;
            if (run >= 0 && a != runa) {
                if (runa)
                    gfx->fill_rect(gfx->ud, s, x0 + run, y0 + y, x - run, 1,
                                   ((uint32_t)runa << 24) | rgb);
                run = -1;
            }
            if (run < 0 && a) {
                run = x;
                runa = a;
            }
        }
    }
    plutovg_canvas_destroy(cv);
    plutovg_surface_destroy(surf);
    return true;
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
    m_paint_surf = 0;
    m_xform_on = false;
    memset(m_xform, 0, sizeof(m_xform));
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
    m_paint_surf = (void*)hdc;
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
    m_paint_surf = (void*)hdc;
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
                                const std::string& pageUrl, bool topLevel,
                                std::string* finalUrl)
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
            /* Resource loads are always plain GETs with no body. */
            if(!port->net.request(port->net.ud, cur_url.c_str(), "GET", NULL, 0,
                                  req_hdrs, req_hdr_count, &resp)) {
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
            /* Report the URL the body actually came from: redirect hops
             * replace cur_url, and the caller (a top-level navigation) must
             * publish the FINAL url so the address bar, session history and
             * relative-resource resolution all agree with what was fetched. */
            if(finalUrl != NULL)
                *finalUrl = cur_url;
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

/* CSS linear-gradient() background support. Tailwind-driven pages paint
 * hero fields and pill buttons with background:linear-gradient(...); the
 * HAL has no gradient primitive, so the gradient is rasterised into a temp
 * surface (0xAARRGGBB, same as every decoded image) and composited with the
 * ordinary src-over blit, masked by the element's own border-radius. */
struct GradientStop {
    float off;
    float r, g, b, a;
};

struct LinearGradient {
    float angle_deg;
    std::vector<GradientStop> stops;
};

static bool eweb_starts_with_ci(const std::string& s, const char* pre)
{
    size_t n = strlen(pre);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != pre[i])
            return false;
    }
    return true;
}

static std::string eweb_trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/* Split on commas that are not nested inside parentheses (rgba(0, 0, 0, .1)). */
static void split_top_level_commas(const std::string& s, std::vector<std::string>& out)
{
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        else if (s[i] == ',' && depth == 0) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(s.substr(start));
}

static bool parse_gradient_angle(const std::string& tok, float& deg)
{
    if (tok.size() > 3 && tok.compare(tok.size() - 3, 3, "deg") == 0) {
        deg = (float)atof(tok.c_str());
        return true;
    }
    if (tok.size() > 4 && tok.compare(tok.size() - 4, 4, "turn") == 0) {
        deg = (float)atof(tok.c_str()) * 360.0f;
        return true;
    }
    if (tok.size() > 3 && tok.compare(tok.size() - 3, 3, "rad") == 0) {
        deg = (float)atof(tok.c_str()) * 57.2957795f;
        return true;
    }
    if (tok.compare(0, 3, "to ") != 0)
        return false;
    std::string dir = eweb_trim(tok.substr(3));
    if (dir == "top") deg = 0.0f;
    else if (dir == "right") deg = 90.0f;
    else if (dir == "bottom") deg = 180.0f;
    else if (dir == "left") deg = 270.0f;
    else if (dir == "top right" || dir == "right top") deg = 45.0f;
    else if (dir == "bottom right" || dir == "right bottom") deg = 135.0f;
    else if (dir == "bottom left" || dir == "left bottom") deg = 225.0f;
    else if (dir == "top left" || dir == "left top") deg = 315.0f;
    else return false;
    return true;
}

static bool parse_linear_gradient(const std::string& val, LinearGradient& out)
{
    size_t open = val.find('(');
    size_t close = val.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open)
        return false;
    std::vector<std::string> parts;
    split_top_level_commas(val.substr(open + 1, close - open - 1), parts);
    out.angle_deg = 180.0f;
    out.stops.clear();
    size_t first_stop = 0;
    if (!parts.empty()) {
        float deg = 0.0f;
        std::string head = eweb_trim(parts[0]);
        if (parse_gradient_angle(head, deg)) {
            out.angle_deg = deg;
            first_stop = 1;
        }
    }
    for (size_t i = first_stop; i < parts.size(); i++) {
        std::string tok = eweb_trim(parts[i]);
        if (tok.empty())
            continue;
        std::string color_str;
        std::string rest;
        size_t paren = tok.find('(');
        if (paren != std::string::npos) {
            size_t cp = tok.rfind(')');
            if (cp == std::string::npos)
                continue;
            color_str = tok.substr(0, cp + 1);
            rest = eweb_trim(tok.substr(cp + 1));
        } else {
            size_t sp = tok.find(' ');
            if (sp == std::string::npos) {
                color_str = tok;
            } else {
                color_str = tok.substr(0, sp);
                rest = eweb_trim(tok.substr(sp + 1));
            }
        }
        GradientStop st;
        st.off = -1.0f;
        litehtml::web_color clr = litehtml::web_color::from_string(color_str.c_str(), 0);
        st.r = clr.red;
        st.g = clr.green;
        st.b = clr.blue;
        st.a = clr.alpha;
        if (!rest.empty())
            st.off = (float)atof(rest.c_str()) / 100.0f;
        out.stops.push_back(st);
    }
    if (out.stops.size() < 2)
        return false;
    /* Fill missing offsets: first 0, last 1, middles evenly between the
     * nearest specified neighbours, as the spec requires. */
    if (out.stops.front().off < 0.0f)
        out.stops.front().off = 0.0f;
    if (out.stops.back().off < 0.0f)
        out.stops.back().off = 1.0f;
    for (size_t i = 0; i < out.stops.size();) {
        if (out.stops[i].off >= 0.0f) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < out.stops.size() && out.stops[j].off < 0.0f)
            j++;
        float lo = out.stops[i - 1].off;
        float hi = (j < out.stops.size()) ? out.stops[j].off : 1.0f;
        for (size_t k = i; k < j; k++)
            out.stops[k].off = lo + (hi - lo) * (float)(k - i + 1) / (float)(j - i + 1);
        i = j;
    }
    for (size_t i = 1; i < out.stops.size(); i++)
        if (out.stops[i].off < out.stops[i - 1].off)
            out.stops[i].off = out.stops[i - 1].off;
    return true;
}

static uint32_t sample_linear_gradient(const LinearGradient& grad, float t)
{
    if (t <= 0.0f) t = 0.0f;
    if (t >= 1.0f) t = 1.0f;
    size_t i = 1;
    while (i + 1 < grad.stops.size() && grad.stops[i].off < t)
        i++;
    const GradientStop& a = grad.stops[i - 1];
    const GradientStop& b = grad.stops[i];
    float span = b.off - a.off;
    float f = (span > 1e-6f) ? (t - a.off) / span : 0.0f;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    uint32_t r = (uint32_t)(a.r + (b.r - a.r) * f);
    uint32_t g = (uint32_t)(a.g + (b.g - a.g) * f);
    uint32_t bl = (uint32_t)(a.b + (b.b - a.b) * f);
    uint32_t al = (uint32_t)(a.a + (b.a - a.a) * f);
    return (al << 24) | (r << 16) | (g << 8) | bl;
}

void EWebContainer::draw_background(litehtml::uint_ptr hdc, const litehtml::background_paint& bg)
{
    eweb_surface_t* s = (eweb_surface_t*)hdc;
    m_paint_surf = (void*)hdc;
    if (!s)
        return;
    const eweb_gfx_t* gfx = &m_port->gfx;

    if (eweb_starts_with_ci(bg.image, "linear-gradient(")) {
        const litehtml::position& cb = bg.clip_box;
        LinearGradient grad;
        if (cb.width <= 0 || cb.height <= 0 || !parse_linear_gradient(bg.image, grad))
            return;
        if (!gfx->surface_new || !gfx->surface_pixels || !gfx->surface_free || !gfx->blit_fit_alpha)
            return;
        eweb_surface_t* tmp = gfx->surface_new(gfx->ud, cb.width, cb.height);
        if (!tmp)
            return;
        int tw = 0, th = 0;
        uint32_t* px = gfx->surface_pixels(gfx->ud, tmp, &tw, &th);
        if (px && tw > 0 && th > 0) {
            const float kPi = 3.14159265f;
            float ang = grad.angle_deg * kPi / 180.0f;
            float dx = sinf(ang), dy = -cosf(ang);
            float W = (float)cb.width, H = (float)cb.height;
            float L = fabsf(W * dx) + fabsf(H * dy);
            if (L <= 1e-6f) L = 1.0f;
            float sx = W * 0.5f - dx * L * 0.5f;
            float sy = H * 0.5f - dy * L * 0.5f;
            float kx = W / (float)tw, ky = H / (float)th;
            /* CSS border-radius clamping: overlapping corner radii shrink
             * proportionally so a 30rem radius on a 55rem-tall pill becomes
             * the stadium shape the button shows in a real browser. */
            float rtl_x = (float)bg.border_radius.top_left_x, rtl_y = (float)bg.border_radius.top_left_y;
            float rtr_x = (float)bg.border_radius.top_right_x, rtr_y = (float)bg.border_radius.top_right_y;
            float rbl_x = (float)bg.border_radius.bottom_left_x, rbl_y = (float)bg.border_radius.bottom_left_y;
            float rbr_x = (float)bg.border_radius.bottom_right_x, rbr_y = (float)bg.border_radius.bottom_right_y;
            float f = 1.0f;
            if (rtl_x + rtr_x > 0) f = std::min(f, W / (rtl_x + rtr_x));
            if (rbl_x + rbr_x > 0) f = std::min(f, W / (rbl_x + rbr_x));
            if (rtl_y + rbl_y > 0) f = std::min(f, H / (rtl_y + rbl_y));
            if (rtr_y + rbr_y > 0) f = std::min(f, H / (rtr_y + rbr_y));
            if (f < 1.0f) {
                rtl_x *= f; rtl_y *= f; rtr_x *= f; rtr_y *= f;
                rbl_x *= f; rbl_y *= f; rbr_x *= f; rbr_y *= f;
            }
            bool rounded = (rtl_x > 0 || rtl_y > 0 || rtr_x > 0 || rbr_x > 0 || rbl_x > 0);
            for (int yy = 0; yy < th; yy++) {
                float Y = (yy + 0.5f) * ky;
                for (int xx = 0; xx < tw; xx++) {
                    float X = (xx + 0.5f) * kx;
                    uint32_t c = sample_linear_gradient(grad, ((X - sx) * dx + (Y - sy) * dy) / L);
                    if (rounded) {
                        float ex = 0, ey = 0, rx = 0, ry = 0;
                        bool in_corner = false;
                        if (X < rtl_x && Y < rtl_y) { ex = rtl_x; ey = rtl_y; rx = rtl_x; ry = rtl_y; in_corner = true; }
                        else if (X > W - rtr_x && Y < rtr_y) { ex = W - rtr_x; ey = rtr_y; rx = rtr_x; ry = rtr_y; in_corner = true; }
                        else if (X < rbl_x && Y > H - rbl_y) { ex = rbl_x; ey = H - rbl_y; rx = rbl_x; ry = rbl_y; in_corner = true; }
                        else if (X > W - rbr_x && Y > H - rbr_y) { ex = W - rbr_x; ey = H - rbr_y; rx = rbr_x; ry = rbr_y; in_corner = true; }
                        if (in_corner && rx > 0 && ry > 0) {
                            float nx = (X - ex) / rx, ny = (Y - ey) / ry;
                            if (nx * nx + ny * ny > 1.0f)
                                c = 0;
                        }
                    }
                    px[yy * tw + xx] = c;
                }
            }
            /* src rect lives in the SOURCE surface's logical space (the HAL
             * scales it by tmp's own dpr); tmp was created at cb.width x
             * cb.height logical, so passing the device-pixel tw/th here
             * double-scaled the src rect on HiDPI (dpr=2) and SDL's clip of
             * the oversized src shrank the dst to half the clip box - the
             * half-size hero gradient and clipped pill buttons on Retina. */
            gfx->blit_fit_alpha(gfx->ud, tmp, 0, 0, cb.width, cb.height, s, cb.x, cb.y, cb.width, cb.height, 0xFF);
        }
        gfx->surface_free(gfx->ud, tmp);
        return;
    }

    bool do_image = false;
    if (!bg.image.empty() && bg.image_size.width > 0 && bg.image_size.height > 0) {
        std::string full_url = getFullURL(m_port, bg.image, m_base_url);
        if (!full_url.empty()) {
            auto it = m_images.find(full_url);
            eweb_surface_t* img = NULL;
            if (it != m_images.end() && it->second.image != NULL) {
                img = it->second.image;
            }

            if (img != NULL && gfx->surface_dims && (gfx->blit || gfx->blit_fit_alpha)) {
                int iw = 0, ih = 0;
                gfx->surface_dims(gfx->ud, img, &iw, &ih);
                if (iw > 0 && ih > 0) {
                    /* Tile the image at its resolved background-size starting
                     * at (position_x, position_y), honouring background-repeat
                     * and clipping to clip_box. Stretching the whole bitmap
                     * into the clip box (the old blit_fit_alpha shortcut)
                     * smeared sprite sheets across every small box that
                     * references them - the garbled-icon noise on dense
                     * pages. */
                    int tw = bg.image_size.width  > 0 ? bg.image_size.width  : iw;
                    int th = bg.image_size.height > 0 ? bg.image_size.height : ih;
                    if (tw <= 0 || th <= 0)
                        return;
                    const litehtml::position& cb = bg.clip_box;
                    bool tile_x = (bg.repeat == litehtml::background_repeat_repeat ||
                                   bg.repeat == litehtml::background_repeat_repeat_x);
                    bool tile_y = (bg.repeat == litehtml::background_repeat_repeat ||
                                   bg.repeat == litehtml::background_repeat_repeat_y);
                    int x0 = bg.position_x;
                    int y0 = bg.position_y;
                    if (tile_x) {
                        while (x0 > cb.x) x0 -= tw;
                        while (x0 + tw <= cb.x) x0 += tw;
                    }
                    if (tile_y) {
                        while (y0 > cb.y) y0 -= th;
                        while (y0 + th <= cb.y) y0 += th;
                    }
                    int y_end = cb.y + cb.height;
                    int x_end = cb.x + cb.width;
                    for (int ty = y0; ty < y_end; ty += th) {
                        if (!tile_y && ty != y0) break;
                        for (int tx = x0; tx < x_end; tx += tw) {
                            if (!tile_x && tx != x0) break;
                            /* dst = tile rect intersected with the clip box */
                            int dx = tx > cb.x ? tx : cb.x;
                            int dy = ty > cb.y ? ty : cb.y;
                            int dr = (tx + tw) < x_end ? (tx + tw) : x_end;
                            int db = (ty + th) < y_end ? (ty + th) : y_end;
                            int dw = dr - dx;
                            int dh = db - dy;
                            if (dw <= 0 || dh <= 0)
                                continue;
                            /* matching src sub-rect in natural image pixels */
                            int sx = (int)((long)(dx - tx) * iw / tw);
                            int sy = (int)((long)(dy - ty) * ih / th);
                            int sw = (int)((long)dw * iw / tw);
                            int sh = (int)((long)dh * ih / th);
                            if (sw <= 0) sw = 1;
                            if (sh <= 0) sh = 1;
                            if (sx + sw > iw) sw = iw - sx;
                            if (sy + sh > ih) sh = ih - sy;
                            if (sw <= 0 || sh <= 0)
                                continue;
                            /* Decoded images must composite src-over: the HAL
                             * blit is a straight copy (BLENDMODE_NONE) that
                             * writes transparent pixels' black RGB verbatim -
                             * the black field behind alpha images painted at
                             * natural size (w3.org hero illustration). */
                            bool masked = false;
                            int crad = top_clip_radius();
                            if (crad > 0 && gfx->surface_new && gfx->surface_pixels &&
                                gfx->surface_free && gfx->blit_fit_alpha) {
                                /* Rounded clip (.avatar border-radius:50% with
                                 * overflow:hidden): the HAL clip is rectangular,
                                 * so mask the tile against the round box in a
                                 * temp surface and composite that with alpha. */
                                eweb_surface_t* tmp = gfx->surface_new(gfx->ud, dw, dh);
                                if (tmp) {
                                    gfx->surface_clear(gfx->ud, tmp, 0);
                                    gfx->blit_fit_alpha(gfx->ud, img, sx, sy, sw, sh, tmp, 0, 0, dw, dh, 0xFF);
                                    int tw2 = 0, th2 = 0;
                                    uint32_t* px = gfx->surface_pixels(gfx->ud, tmp, &tw2, &th2);
                                    if (px && tw2 > 0 && th2 > 0) {
                                        const litehtml::position& cr = m_clips.back().r;
                                        /* tmp covers exactly the dst tile, but its
                                         * buffer may sit at device resolution
                                         * (dpr>1): map each native pixel back to
                                         * the logical point of the tile it is. */
                                        float kx = (float)dw / (float)tw2;
                                        float ky = (float)dh / (float)th2;
                                        /* border-radius:50% rounds to an ELLIPSE
                                         * on a non-square box; the clamp-to-corner
                                         * SDF below would degenerate to a stadium
                                         * and poke out of the border ring. */
                                        int cside = cr.width < cr.height ? cr.width : cr.height;
                                        bool ellipse = (crad >= cside / 2 - 1);
                                        float ex = cr.x + cr.width * 0.5f;
                                        float ey = cr.y + cr.height * 0.5f;
                                        float erx = cr.width * 0.5f;
                                        float ery = cr.height * 0.5f;
                                        for (int yy = 0; yy < th2; yy++) {
                                            float Y = dy + (yy + 0.5f) * ky;
                                            float cy = Y < cr.y + crad ? cr.y + crad :
                                                       (Y > cr.y + cr.height - crad ? cr.y + cr.height - crad : Y);
                                            float ny = ellipse ? (Y - ey) / ery : 0.0f;
                                            for (int xx = 0; xx < tw2; xx++) {
                                                float X = dx + (xx + 0.5f) * kx;
                                                bool out;
                                                if (ellipse) {
                                                    float nx = (X - ex) / erx;
                                                    out = (nx * nx + ny * ny) > 1.0f;
                                                } else {
                                                    float cx = X < cr.x + crad ? cr.x + crad :
                                                               (X > cr.x + cr.width - crad ? cr.x + cr.width - crad : X);
                                                    float ddx = X - cx, ddy = Y - cy;
                                                    out = (ddx * ddx + ddy * ddy) > (float)crad * crad;
                                                }
                                                if (out)
                                                    px[yy * tw2 + xx] = 0;
                                            }
                                        }
                                        gfx->blit_fit_alpha(gfx->ud, tmp, 0, 0, dw, dh, s, dx, dy, dw, dh, 0xFF);
                                        masked = true;
                                    }
                                    gfx->surface_free(gfx->ud, tmp);
                                }
                            }
                            if (!masked) {
                                if (gfx->blit_fit_alpha) {
                                    gfx->blit_fit_alpha(gfx->ud, img, sx, sy, sw, sh, s, dx, dy, dw, dh, 0xFF);
                                } else if (gfx->blit) {
                                    gfx->blit(gfx->ud, img, sx, sy, sw, sh, s, dx, dy, sw, sh);
                                }
                            }
                        }
                    }
                    return;
                }
            }
        }
        do_image = true;
    }

    uint32_t color = web_color_to_argb(bg.color);
    uint8_t alpha = color >> 24;

    /* The root background propagates to the canvas in CSS: it must cover the
     * whole viewport, not just the html box. Painting only the html box left
     * any window margin the layout did not reach untouched, so a stale frame
     * showed through at the left/bottom edges (leaf photo bleed). */
    if(bg.is_root && alpha > 0 && gfx->fill_rect)
        gfx->fill_rect(gfx->ud, s, 0, 0, m_client_width, m_client_height, color);

    if(!do_image) {
        if(alpha == 0)
            return;
        if(m_xform_on && !bg.is_root) {
            /* CSS transform: the software HAL has no transformed blit, so fill
             * the (rounded) box through the paint matrix as a polygon. Rounded
             * corners are approximated with arc segments; gradient/image
             * backgrounds are not transformed in this path (rare combination).
             * Content (text/children) is painted separately and stays
             * untransformed in this phase. */
            const float* M = m_xform;
            const litehtml::position& cb = bg.clip_box;
            float X = (float)cb.x, Y = (float)cb.y;
            float W = (float)cb.width, H = (float)cb.height;
            if(W > 0.0f && H > 0.0f) {
                float rtl = (float)bg.border_radius.top_left_x,     rty = (float)bg.border_radius.top_left_y;
                float rtr = (float)bg.border_radius.top_right_x,    rry = (float)bg.border_radius.top_right_y;
                float rbr = (float)bg.border_radius.bottom_right_x, rby = (float)bg.border_radius.bottom_right_y;
                float rbl = (float)bg.border_radius.bottom_left_x,  rly = (float)bg.border_radius.bottom_left_y;
                /* Clamp opposing corners so arcs never overlap (CSS rule). */
                float f = 1.0f;
                if(rtl + rtr > 0) f = std::min(f, W / (rtl + rtr));
                if(rbl + rbr > 0) f = std::min(f, W / (rbl + rbr));
                if(rty + rly > 0) f = std::min(f, H / (rty + rly));
                if(rry + rby > 0) f = std::min(f, H / (rry + rby));
                rtl *= f; rtr *= f; rbr *= f; rbl *= f;
                rty *= f; rry *= f; rby *= f; rly *= f;
                std::vector<float> local;
                local.reserve(64);
                const int SEG = 6;
                auto arc = [&](float cx, float cy, float rx, float ry, float a0, float a1) {
                    for(int i = 0; i <= SEG; i++) {
                        float a = a0 + (a1 - a0) * (float)i / (float)SEG;
                        local.push_back(cx + rx * cosf(a));
                        local.push_back(cy + ry * sinf(a));
                    }
                };
                const float kPi = 3.14159265358979f;
                /* Clockwise from the top-left corner. */
                local.push_back(X + rtl); local.push_back(Y);
                local.push_back(X + W - rtr); local.push_back(Y);
                if(rtr > 0 && rry > 0) arc(X + W - rtr, Y + rry, rtr, rry, -kPi * 0.5f, 0.0f);
                local.push_back(X + W); local.push_back(Y + H - rby);
                if(rbr > 0 && rby > 0) arc(X + W - rbr, Y + H - rby, rbr, rby, 0.0f, kPi * 0.5f);
                local.push_back(X + rbl); local.push_back(Y + H);
                if(rbl > 0 && rly > 0) arc(X + rbl, Y + H - rly, rbl, rly, kPi * 0.5f, kPi);
                local.push_back(X); local.push_back(Y + rty);
                if(rtl > 0 && rty > 0) arc(X + rtl, Y + rty, rtl, rty, kPi, kPi * 1.5f);
                /* Transform every point and compute the bbox clip. */
                size_t n = local.size() / 2;
                std::vector<float> pts(n * 2);
                float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
                for(size_t i = 0; i < n; i++) {
                    float lx = local[i * 2], ly = local[i * 2 + 1];
                    float dx = M[0] * lx + M[2] * ly + M[4];
                    float dy = M[1] * lx + M[3] * ly + M[5];
                    pts[i * 2] = dx; pts[i * 2 + 1] = dy;
                    if(dx < minx) minx = dx;
                    if(dx > maxx) maxx = dx;
                    if(dy < miny) miny = dy;
                    if(dy > maxy) maxy = dy;
                }
                litehtml::position cp;
                cp.x = (int)floorf(minx); cp.y = (int)floorf(miny);
                cp.width = (int)ceilf(maxx) - cp.x + 1;
                cp.height = (int)ceilf(maxy) - cp.y + 1;
                std::vector<int> counts(1, (int)n);
                if(!fill_polys_aa(s, gfx, cp, pts.data(), counts.data(), 1, color))
                    fill_polys_nz(s, gfx, cp, pts.data(), counts.data(), 1, color);
                return;
            }
        }
        int rad_x = bg.border_radius.top_left_x;
        int rad_y = bg.border_radius.top_left_y;
        bool rounded = (rad_x > 0 || rad_y > 0) &&
            bg.border_radius.top_right_x == rad_x &&
            bg.border_radius.bottom_left_x == rad_x &&
            bg.border_radius.bottom_right_x == rad_x &&
            bg.border_radius.top_left_y == rad_y &&
            bg.border_radius.top_right_y == rad_y &&
            bg.border_radius.bottom_left_y == rad_y &&
            bg.border_radius.bottom_right_y == rad_y;
        if(rounded && gfx->fill_round)
            gfx->fill_round(gfx->ud, s, bg.clip_box.x, bg.clip_box.y, bg.clip_box.width, bg.clip_box.height,
                rad_x > rad_y ? rad_x : rad_y, color);
        else if(gfx->fill_rect)
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
    m_paint_surf = (void*)hdc;
    if (!s)
        return;
    (void)root;

    const eweb_gfx_t* gfx = &m_port->gfx;
    const litehtml::border& t = borders.top;
    const litehtml::border& r = borders.right;
    const litehtml::border& b = borders.bottom;
    const litehtml::border& l = borders.left;

    bool has_top    = t.width > 0 && t.style > litehtml::border_style_hidden;
    bool has_right  = r.width > 0 && r.style > litehtml::border_style_hidden;
    bool has_bottom = b.width > 0 && b.style > litehtml::border_style_hidden;
    bool has_left   = l.width > 0 && l.style > litehtml::border_style_hidden;
    if (!has_top && !has_right && !has_bottom && !has_left)
        return;

    int rad = borders.radius.top_left_x;
    /* border-radius:50% on a box whose sides differ by a pixel or two (line
     * box rounding) yields x/y radii that differ by one; tolerate that. */
    bool uniform_radius =
                          borders.radius.top_right_x >= rad - 1 && borders.radius.top_right_x <= rad + 1 &&
                          borders.radius.bottom_left_x >= rad - 1 && borders.radius.bottom_left_x <= rad + 1 &&
                          borders.radius.bottom_right_x >= rad - 1 && borders.radius.bottom_right_x <= rad + 1 &&
                          borders.radius.top_left_y >= rad - 1 && borders.radius.top_left_y <= rad + 1 &&
                          borders.radius.top_right_y >= rad - 1 && borders.radius.top_right_y <= rad + 1 &&
                          borders.radius.bottom_left_y >= rad - 1 && borders.radius.bottom_left_y <= rad + 1 &&
                          borders.radius.bottom_right_y >= rad - 1 && borders.radius.bottom_right_y <= rad + 1;

    /* Rounded outline with one shared width/color: single stroked round-rect
     * (the .button pill case). Otherwise paint each side independently with
     * its own width/color, which also fixes border-bottom-only rules that
     * used to be drawn as a full 1px outline. */
    int minside = draw_pos.width < draw_pos.height ? draw_pos.width : draw_pos.height;
    /* Only a square box with radius >= half its side is a true circle (.avatar).
     * A wide pill (border-radius:999px) must NOT take this path or it collapses
     * to a ring; let it fall through to the rounded-rect stroke below. */
    bool square = (draw_pos.width - minside) <= 2 && (draw_pos.height - minside) <= 2;
    if (square && rad > 0 && uniform_radius && minside > 0 && rad >= minside / 2 - 1) {
        /* Fully round box (border-radius:50%): stroke an ellipse ring, not four
         * rect edges - the w3.org .avatar circle. The box can be a pixel or two
         * off square (line-box rounding), so the ring follows the box ellipse
         * exactly - a plain circle would leave the masked image poking out. */
        int bw = 0; uint32_t col = 0;
        if (has_top)         { bw = t.width; col = web_color_to_argb(t.color); }
        else if (has_right)  { bw = r.width; col = web_color_to_argb(r.color); }
        else if (has_bottom) { bw = b.width; col = web_color_to_argb(b.color); }
        else if (has_left)   { bw = l.width; col = web_color_to_argb(l.color); }
        if (bw > 0) {
            const int N = 48;
            float cx = draw_pos.x + draw_pos.width * 0.5f;
            float cy = draw_pos.y + draw_pos.height * 0.5f;
            float rx = draw_pos.width * 0.5f;
            float ry = draw_pos.height * 0.5f;
            std::vector<float> pts;
            std::vector<int> counts;
            pts.reserve(N * 4);
            for (int i = 0; i < N; i++) {
                float a = 6.28318530718f * (float)i / (float)N;
                pts.push_back(cx + rx * cosf(a));
                pts.push_back(cy + ry * sinf(a));
            }
            counts.push_back(N);
            float rx2 = rx - (float)bw, ry2 = ry - (float)bw;
            if (rx2 > 0.0f && ry2 > 0.0f) {
                for (int i = N - 1; i >= 0; i--) {
                    float a = 6.28318530718f * (float)i / (float)N;
                    pts.push_back(cx + rx2 * cosf(a));
                    pts.push_back(cy + ry2 * sinf(a));
                }
                counts.push_back(N);
            }
            litehtml::position cp = draw_pos;
            cp.x -= 1; cp.y -= 1; cp.width += 2; cp.height += 2;
            fill_polys_nz(s, gfx, cp, pts.data(), counts.data(), (int)counts.size(), col);
        }
        return;
    }

    if (m_xform_on) {
        /* CSS transform: push each border edge through the paint matrix as a
         * quad and fill with the winding scanline filler (border-trick
         * chevrons: a 2px L rotated 45 degrees). */
        const float* M = m_xform;
        auto quad = [&](float x0, float y0, float x1, float y1,
                        float x2, float y2, float x3, float y3, uint32_t col) {
            float px[4] = {x0, x1, x2, x3}, py[4] = {y0, y1, y2, y3};
            std::vector<float> pts(8);
            float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
            for (int i = 0; i < 4; i++) {
                float dx = M[0] * px[i] + M[2] * py[i] + M[4];
                float dy = M[1] * px[i] + M[3] * py[i] + M[5];
                pts[i * 2] = dx; pts[i * 2 + 1] = dy;
                if (dx < minx) minx = dx;
                if (dx > maxx) maxx = dx;
                if (dy < miny) miny = dy;
                if (dy > maxy) maxy = dy;
            }
            litehtml::position cp;
            cp.x = (int)floorf(minx); cp.y = (int)floorf(miny);
            cp.width = (int)ceilf(maxx) - cp.x + 1;
            cp.height = (int)ceilf(maxy) - cp.y + 1;
            std::vector<int> counts(1, 4);
            fill_polys_nz(s, gfx, cp, pts.data(), counts.data(), (int)counts.size(), col);
        };
        float bx = (float)draw_pos.x, by = (float)draw_pos.y;
        float bw = (float)draw_pos.width, bh = (float)draw_pos.height;
        if (has_top) {
            float x0 = bx + (has_left ? l.width : 0), x1 = bx + bw - (has_right ? r.width : 0);
            quad(x0, by, x1, by, x1, by + t.width, x0, by + t.width, web_color_to_argb(t.color));
        }
        if (has_bottom) {
            float x0 = bx + (has_left ? l.width : 0), x1 = bx + bw - (has_right ? r.width : 0);
            quad(x0, by + bh - b.width, x1, by + bh - b.width, x1, by + bh, x0, by + bh, web_color_to_argb(b.color));
        }
        if (has_left)
            quad(bx, by, bx + l.width, by, bx + l.width, by + bh, bx, by + bh, web_color_to_argb(l.color));
        if (has_right)
            quad(bx + bw - r.width, by, bx + bw, by, bx + bw, by + bh, bx + bw - r.width, by + bh, web_color_to_argb(r.color));
        return;
    }
    if (rad > 0 && uniform_radius && has_top && t.width == r.width && t.width == b.width && t.width == l.width &&
        web_color_to_argb(t.color) == web_color_to_argb(r.color) &&
        web_color_to_argb(t.color) == web_color_to_argb(b.color) &&
        web_color_to_argb(t.color) == web_color_to_argb(l.color) && gfx->round) {
        gfx->round(gfx->ud, s, draw_pos.x, draw_pos.y, draw_pos.width, draw_pos.height,
                   rad, t.width, web_color_to_argb(t.color));
        return;
    }

    if (has_top && gfx->fill_rect) {
        int x = draw_pos.x + (has_left ? l.width : 0);
        int w = draw_pos.width - (has_left ? l.width : 0) - (has_right ? r.width : 0);
        if (w > 0)
            gfx->fill_rect(gfx->ud, s, x, draw_pos.y, w, t.width, web_color_to_argb(t.color));
    }
    if (has_bottom && gfx->fill_rect) {
        int x = draw_pos.x + (has_left ? l.width : 0);
        int w = draw_pos.width - (has_left ? l.width : 0) - (has_right ? r.width : 0);
        if (w > 0)
            gfx->fill_rect(gfx->ud, s, x, draw_pos.y + draw_pos.height - b.width, w, b.width, web_color_to_argb(b.color));
    }
    if (has_left && gfx->fill_rect)
        gfx->fill_rect(gfx->ud, s, draw_pos.x, draw_pos.y, l.width, draw_pos.height, web_color_to_argb(l.color));
    if (has_right && gfx->fill_rect)
        gfx->fill_rect(gfx->ud, s, draw_pos.x + draw_pos.width - r.width, draw_pos.y, r.width, draw_pos.height, web_color_to_argb(r.color));
}

void EWebContainer::draw_svg(litehtml::uint_ptr hdc, const litehtml::position& pos,
                             const litehtml::web_color& color,
                             const float* pts, const int* counts, int nsubs)
{
    eweb_surface_t* s = (eweb_surface_t*)hdc;
    m_paint_surf = (void*)hdc;
    if (!s || !pts || !counts || nsubs <= 0)
        return;
    const eweb_gfx_t* gfx = &m_port->gfx;
    uint32_t argb = web_color_to_argb(color);
    /* Anti-aliased path first; the plain scanline filler stays as the
     * fallback for the (rare) cases the offscreen raster cannot serve. */
    if (!fill_polys_aa(s, gfx, pos, pts, counts, nsubs, argb))
        fill_polys_nz(s, gfx, pos, pts, counts, nsubs, argb);
}

void EWebContainer::transform_text(litehtml::tstring& text, litehtml::text_transform tt)
{
}

void EWebContainer::push_paint_transform(const float m[6])
{
    /* Record the box's CSS transform; the border edges drawn between this push
     * and the matching pop go through the matrix as quads (border-trick
     * chevrons rotated 45 degrees). Backgrounds/text stay untransformed. */
    if(!m) { m_xform_on = false; return; }
    for(int i = 0; i < 6; i++) m_xform[i] = m[i];
    m_xform_on = true;
}

void EWebContainer::pop_paint_transform()
{
    m_xform_on = false;
    memset(m_xform, 0, sizeof(m_xform));
}

int EWebContainer::top_clip_radius() const
{
    if(m_clips.empty()) return 0;
    const clip_entry& e = m_clips.back();
    /* A radius only rounds the box if it stays inside it */
    if(e.radius <= 0) return 0;
    int half = (e.r.width < e.r.height ? e.r.width : e.r.height) / 2;
    return e.radius < half ? e.radius : half;
}

litehtml::position EWebContainer::top_clip_rect() const
{
    litehtml::position r;
    if(!m_clips.empty()) r = m_clips.back().r;
    return r;
}

void EWebContainer::set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius, bool valid_x, bool valid_y)
{
    litehtml::position r = pos;
    if(!valid_x) { r.x = -100000; r.width = 200000; }
    if(!valid_y) { r.y = -100000; r.height = 200000; }
    if(!m_clips.empty())
    {
        const litehtml::position& p = m_clips.back().r;
        int x1 = r.x > p.x ? r.x : p.x;
        int y1 = r.y > p.y ? r.y : p.y;
        int x2 = r.right() < p.right() ? r.right() : p.right();
        int y2 = r.bottom() < p.bottom() ? r.bottom() : p.bottom();
        r.x = x1; r.y = y1;
        r.width = x2 > x1 ? x2 - x1 : 0;
        r.height = y2 > y1 ? y2 - y1 : 0;
    }
    /* keep a uniform radius with the entry so image compositing under a
     * rounded clip can mask to the round box; mixed corners stay rect */
    int rad = 0;
    if(bdr_radius.top_left_x > 0 &&
       bdr_radius.top_left_x == bdr_radius.top_right_x &&
       bdr_radius.top_left_x == bdr_radius.bottom_left_x &&
       bdr_radius.top_left_x == bdr_radius.bottom_right_x)
    {
        rad = bdr_radius.top_left_x;
    }
    m_clips.push_back(clip_entry{r, rad});
    if(m_paint_surf && m_port && m_port->gfx.surface_set_clip)
    {
        m_port->gfx.surface_set_clip(m_port->gfx.ud, (eweb_surface_t*)m_paint_surf,
                                     r.x, r.y, r.width, r.height);
    }
}

void EWebContainer::del_clip()
{
    if(m_clips.empty()) return;
    m_clips.pop_back();
    if(!m_paint_surf || !m_port) return;
    if(m_clips.empty())
    {
        if(m_port->gfx.surface_unset_clip)
            m_port->gfx.surface_unset_clip(m_port->gfx.ud, (eweb_surface_t*)m_paint_surf);
    }
    else
    {
        const litehtml::position& r = m_clips.back().r;
        if(m_port->gfx.surface_set_clip)
            m_port->gfx.surface_set_clip(m_port->gfx.ud, (eweb_surface_t*)m_paint_surf,
                                         r.x, r.y, r.width, r.height);
    }
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
        const litehtml::tchar_t* type = _t("text");
        auto iter = attributes.find(_t("type"));
        if (iter != attributes.end()) {
            type = iter->second.c_str();
        }
        EWebInputType it;
        if (!t_strcasecmp(type, _t("button")) ||
            !t_strcasecmp(type, _t("submit")) ||
            !t_strcasecmp(type, _t("reset"))) {
            it = EWEB_INPUT_BUTTON;
        }
        else if (!t_strcasecmp(type, _t("checkbox"))) {
            it = EWEB_INPUT_CHECKBOX;
        }
        else if (!t_strcasecmp(type, _t("radio"))) {
            it = EWEB_INPUT_RADIO;
        }
        else if (!t_strcasecmp(type, _t("hidden"))) {
            it = EWEB_INPUT_HIDDEN;
        }
        else if (!t_strcasecmp(type, _t("range"))) {
            it = EWEB_INPUT_RANGE;
        }
        else {
            /* text, password, search, email, url, tel, number, and the
             * type-less default all render as a single-line text box. */
            it = EWEB_INPUT_TEXT;
        }
        auto input = new eweb_el_input(doc, m_port, it);
        m_vecInput.push_back(input);
        return input;
    }
    if (!t_strcasecmp(tag_name, _t("button"))) {
        auto input = new eweb_el_input(doc, m_port, EWEB_INPUT_BUTTON);
        m_vecInput.push_back(input);
        return input;
    }
    if (!t_strcasecmp(tag_name, _t("select"))) {
        auto input = new eweb_el_input(doc, m_port, EWEB_INPUT_SELECT);
        m_vecInput.push_back(input);
        return input;
    }
    if (!t_strcasecmp(tag_name, _t("textarea"))) {
        auto input = new eweb_el_input(doc, m_port, EWEB_INPUT_TEXTAREA);
        m_vecInput.push_back(input);
        return input;
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
    /* Preferred color scheme, "dark;light" encoding (0 = dark, 1 = light).
     * The shell seeds EWEB_COLOR_SCHEME from the OS appearance at startup;
     * an explicit value always wins so pages can be forced either way. */
    const char* cs = getenv("EWEB_COLOR_SCHEME");
    media.color_scheme = (cs && !strcasecmp(cs, "dark")) ? 0 : 1;
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
