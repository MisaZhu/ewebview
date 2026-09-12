/* EWebCanvas.cc - Canvas 2D backing store + drawing primitives.
 *
 * Every method maps 1:1 onto an eweb_port_t gfx/font callback (see
 * EWebCanvas.h for the full mapping). The pure-C bridge in the mario natives
 * (js_canvas.c) owns all the CanvasRenderingContext2D state and geometry; by
 * the time it calls us the coordinates are device-space and the op is a
 * single finished primitive, so we never re-implement tessellation, scanline
 * fill or blending here. */

#include "EWebCanvas.h"

#include <string.h>

namespace eweb {

/* ==================================================================
 * Lifecycle
 * ================================================================== */

EWebCanvas::EWebCanvas(const eweb_port_t* port, const std::string& id, int w, int h)
    : m_port(port), m_id(id), m_bmp(nullptr), m_font(nullptr), m_w(w), m_h(h)
{
    if(m_w <= 0) m_w = 300;   /* HTML canvas default */
    if(m_h <= 0) m_h = 150;
    if(m_port->gfx.surface_new == nullptr)
        return;
    m_bmp = m_port->gfx.surface_new(m_port->gfx.ud, m_w, m_h);
    if(m_bmp != nullptr && m_port->gfx.surface_clear)
        m_port->gfx.surface_clear(m_port->gfx.ud, m_bmp, 0xFF000000u);   /* surface_new leaves the buffer undefined */
}

EWebCanvas::~EWebCanvas()
{
    if(m_bmp != nullptr && m_port->gfx.surface_free)
        m_port->gfx.surface_free(m_port->gfx.ud, m_bmp);
    if(m_font != nullptr && m_port->font.destroy)
        m_port->font.destroy(m_port->font.ud, m_font);
}

eweb_font_t* EWebCanvas::ensureFont()
{
    if(m_font == nullptr && m_port->font.create)
        m_font = m_port->font.create(m_port->font.ud, "sans-serif");
    return m_font;
}

/* ==================================================================
 * Rects / lines
 * ================================================================== */

void EWebCanvas::fillRect(int x, int y, int w, int h, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.fill_rect == nullptr) return;
    m_port->gfx.fill_rect(m_port->gfx.ud, m_bmp, x, y, w, h, color);
}

/* gfx.rect draws a 1px outline only, so compose the lw-wide border from 4
 * wline edges, centered on the rect edge. */
void EWebCanvas::strokeRect(int x, int y, int w, int h, int lw, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.wline == nullptr) return;
    if(lw < 1) lw = 1;
    int half = lw / 2;
    int x0 = x - half, y0 = y - half;
    int x1 = x + w - 1 + (lw - half), y1 = y + h - 1 + (lw - half);
    m_port->gfx.wline(m_port->gfx.ud, m_bmp, x0, y0, x1, y0, lw, color);   /* top */
    m_port->gfx.wline(m_port->gfx.ud, m_bmp, x0, y1, x1, y1, lw, color);   /* bottom */
    m_port->gfx.wline(m_port->gfx.ud, m_bmp, x0, y0, x0, y1, lw, color);   /* left */
    m_port->gfx.wline(m_port->gfx.ud, m_bmp, x1, y0, x1, y1, lw, color);   /* right */
}

void EWebCanvas::drawLine(int x0, int y0, int x1, int y1, int w, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.wline == nullptr) return;
    m_port->gfx.wline(m_port->gfx.ud, m_bmp, x0, y0, x1, y1, w, color);
}

/* ==================================================================
 * Circles / arcs
 * ================================================================== */

void EWebCanvas::fillCircle(int cx, int cy, int r, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.fill_circle == nullptr) return;
    m_port->gfx.fill_circle(m_port->gfx.ud, m_bmp, cx, cy, r, color);
}

void EWebCanvas::strokeCircle(int cx, int cy, int r, int lw, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.circle == nullptr) return;
    if(lw < 1) lw = 1;
    m_port->gfx.circle(m_port->gfx.ud, m_bmp, cx, cy, r, lw, color);
}

/* Canvas arc angles are clockwise from +x in radians, the same convention the
 * gfx arc hooks use. */
void EWebCanvas::fillArc(int cx, int cy, int r, float a0, float a1, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.fill_arc == nullptr) return;
    m_port->gfx.fill_arc(m_port->gfx.ud, m_bmp, cx, cy, r, a0, a1, color);
}

void EWebCanvas::strokeArc(int cx, int cy, int r, int lw, float a0, float a1, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.arc == nullptr) return;
    if(lw < 1) lw = 1;
    m_port->gfx.arc(m_port->gfx.ud, m_bmp, cx, cy, r, lw, a0, a1, color);
}

/* ==================================================================
 * Round rects
 * ================================================================== */

void EWebCanvas::fillRoundRect(int x, int y, int w, int h, int r, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.fill_round == nullptr) return;
    m_port->gfx.fill_round(m_port->gfx.ud, m_bmp, x, y, w, h, r, color);
}

void EWebCanvas::strokeRoundRect(int x, int y, int w, int h, int r, int lw, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.round == nullptr) return;
    if(lw < 1) lw = 1;
    m_port->gfx.round(m_port->gfx.ud, m_bmp, x, y, w, h, r, lw, color);
}

/* ==================================================================
 * Bezier curves
 * The port flattens (adaptive de Casteljau) and strokes, so clipping + alpha
 * match the straight-line primitives. We hand it device-space control points
 * and never re-implement the subdivision. */

void EWebCanvas::strokeQuadratic(int x0, int y0, int cx, int cy, int x1, int y1,
                                 int lw, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.stroke_quadratic == nullptr) return;
    if(lw < 1) lw = 1;
    m_port->gfx.stroke_quadratic(m_port->gfx.ud, m_bmp, x0, y0, cx, cy, x1, y1, lw, color);
}

void EWebCanvas::strokeBezier(int x0, int y0, int cx1, int cy1, int cx2, int cy2,
                              int x1, int y1, int lw, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.stroke_bezier == nullptr) return;
    if(lw < 1) lw = 1;
    m_port->gfx.stroke_bezier(m_port->gfx.ud, m_bmp, x0, y0, cx1, cy1, cx2, cy2, x1, y1, lw, color);
}

/* ==================================================================
 * Pixels / blit
 * set_pixel alpha-blends; the bridge uses setPixel for gradient spans whose
 * alpha it has already resolved, so blending over the existing bitmap is the
 * right match. */

void EWebCanvas::setPixel(int x, int y, uint32_t color)
{
    if(m_bmp == nullptr || m_port->gfx.set_pixel == nullptr) return;
    m_port->gfx.set_pixel(m_port->gfx.ud, m_bmp, x, y, color);
}

uint32_t EWebCanvas::getPixel(int x, int y) const
{
    if(m_bmp == nullptr || m_port->gfx.get_pixel == nullptr) return 0;
    return m_port->gfx.get_pixel(m_port->gfx.ud, m_bmp, x, y);
}

/* drawImage / patterns. `src` is always an eweb_surface_t* - an anonymous
 * bitmap, a decoded <img>, or another EWebCanvas's bitmap - that the bridge
 * resolved before calling. blit_fit_alpha does the scaled blit with a global
 * alpha multiplier and respects our clip. */
void EWebCanvas::blit(eweb_surface_t* src, int sx, int sy, int sw, int sh,
                      int dx, int dy, int dw, int dh, uint8_t alpha)
{
    if(m_bmp == nullptr || src == nullptr || m_port->gfx.blit_fit_alpha == nullptr) return;
    if(sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    m_port->gfx.blit_fit_alpha(m_port->gfx.ud, src, sx, sy, sw, sh, m_bmp, dx, dy, dw, dh, alpha);
}

/* ==================================================================
 * Text
 * ================================================================== */

void EWebCanvas::drawText(int x, int y, const char* text, float size, uint32_t color)
{
    if(m_bmp == nullptr || text == nullptr || m_port->font.draw_text == nullptr) return;
    eweb_font_t* f = ensureFont();
    if(f == nullptr) return;
    uint32_t sz = (uint32_t)(size + 0.5f); if(sz < 1) sz = 1;
    m_port->font.draw_text(m_port->font.ud, m_bmp, x, y, text, f, (int)sz, color);
}

/* The font table has no glyph-outline stroke, so approximate with 4 offset
 * draws at +/-lw/2 plus the center (the same trick litehtml uses for
 * text-shadow). Correct-looking for the 1-3px lineWidths canvas text uses. */
void EWebCanvas::strokeText(int x, int y, const char* text, float size, int lw, uint32_t color)
{
    if(m_bmp == nullptr || text == nullptr || m_port->font.draw_text == nullptr) return;
    eweb_font_t* f = ensureFont();
    if(f == nullptr) return;
    uint32_t sz = (uint32_t)(size + 0.5f); if(sz < 1) sz = 1;
    if(lw < 1) lw = 1;
    int d = (lw + 1) / 2;
    m_port->font.draw_text(m_port->font.ud, m_bmp, x - d, y, text, f, (int)sz, color);
    m_port->font.draw_text(m_port->font.ud, m_bmp, x + d, y, text, f, (int)sz, color);
    m_port->font.draw_text(m_port->font.ud, m_bmp, x, y - d, text, f, (int)sz, color);
    m_port->font.draw_text(m_port->font.ud, m_bmp, x, y + d, text, f, (int)sz, color);
    m_port->font.draw_text(m_port->font.ud, m_bmp, x, y, text, f, (int)sz, color);
}

void EWebCanvas::textSize(const char* text, float size, int* w, int* h)
{
    if(w != nullptr) *w = 0;
    if(h != nullptr) *h = 0;
    if(text == nullptr || m_port->font.text_size == nullptr) return;
    eweb_font_t* f = ensureFont();
    if(f == nullptr) return;
    uint32_t sz = (uint32_t)(size + 0.5f); if(sz < 1) sz = 1;
    m_port->font.text_size(m_port->font.ud, f, (int)sz, text, w, h);
}

/* ==================================================================
 * Clip
 * surface_set_clip persists on the surface until surface_unset_clip; the
 * bridge tracks its own device-space clip rect and calls these on
 * save/restore, so we just forward. */

void EWebCanvas::setClip(int x, int y, int w, int h)
{
    if(m_bmp == nullptr || m_port->gfx.surface_set_clip == nullptr) return;
    m_port->gfx.surface_set_clip(m_port->gfx.ud, m_bmp, x, y, w, h);
}

void EWebCanvas::clearClip()
{
    if(m_bmp == nullptr || m_port->gfx.surface_unset_clip == nullptr) return;
    m_port->gfx.surface_unset_clip(m_port->gfx.ud, m_bmp);
}

}
