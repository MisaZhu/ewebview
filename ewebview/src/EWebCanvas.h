/* EWebCanvas - offscreen backing store for one HTML <canvas> element, plus
 * every Canvas 2D drawing primitive the pure-C bridge (mario natives
 * js_canvas.c) drives through js_canvas_callbacks_t.
 *
 * Ported from widget++'s JsCanvas: the EwokOS graph library calls are
 * replaced 1:1 by the eweb_port_t gfx/font tables, so this class is platform
 * independent. The bridge owns ALL CanvasRenderingContext2D state and
 * geometry - the CTM, path construction, curve/arc tessellation, even-odd
 * scanline fill, polyline stroke, clip stack, dash, gradient and shadow math.
 * By the time a callback reaches EWebCanvas the coordinates are already in
 * DEVICE space and the operation is a single finished primitive:
 *
 *   fillRect          -> gfx.fill_rect
 *   strokeRect        -> 4x gfx.wline (gfx.rect is 1px only)
 *   drawLine          -> gfx.wline
 *   fillCircle        -> gfx.fill_circle
 *   strokeCircle      -> gfx.circle(rw=lw)
 *   fillArc           -> gfx.fill_arc
 *   strokeArc         -> gfx.arc(rw=lw)
 *   fillRoundRect     -> gfx.fill_round
 *   strokeRoundRect   -> gfx.round(rw=lw)
 *   strokeQuadratic   -> gfx.stroke_quadratic
 *   strokeBezier      -> gfx.stroke_bezier
 *   setPixel          -> gfx.set_pixel
 *   getPixel          -> gfx.get_pixel
 *   blit              -> gfx.blit_fit_alpha
 *   drawText          -> font.draw_text
 *   strokeText        -> 5x font.draw_text (4 offsets + center)
 *   textSize          -> font.text_size
 *   setClip/clearClip -> gfx.surface_set_clip / surface_unset_clip
 *
 * Lifetime: owned by the engine (one per live <canvas>), created lazily on
 * the first getContext('2d') and freed per page load. All methods run
 * synchronously on the engine thread inside a VM run.
 */

#pragma once

#include <ewebview_port.h>
#include <string>
#include <stdint.h>

namespace eweb {

class EWebCanvas {
public:
    /* HTML canvas defaults to 300x150 when width/height are not positive. */
    EWebCanvas(const eweb_port_t* port, const std::string& id, int w, int h);
    ~EWebCanvas();

    /* Non-copyable: owns a surface and a lazily-created font handle. */
    EWebCanvas(const EWebCanvas&) = delete;
    EWebCanvas& operator=(const EWebCanvas&) = delete;

    const std::string& id()     const { return m_id; }
    int                width()  const { return m_w; }
    int                height() const { return m_h; }
    eweb_surface_t*    bitmap() const { return m_bmp; }
    bool               valid()  const { return m_bmp != nullptr; }

    /* --- rects / lines --- */
    void fillRect(int x, int y, int w, int h, uint32_t color);
    void strokeRect(int x, int y, int w, int h, int lw, uint32_t color);
    void drawLine(int x0, int y0, int x1, int y1, int w, uint32_t color);

    /* --- circles / arcs --- */
    void fillCircle(int cx, int cy, int r, uint32_t color);
    void strokeCircle(int cx, int cy, int r, int lw, uint32_t color);
    void fillArc(int cx, int cy, int r, float a0, float a1, uint32_t color);
    void strokeArc(int cx, int cy, int r, int lw, float a0, float a1, uint32_t color);

    /* --- round rects --- */
    void fillRoundRect(int x, int y, int w, int h, int r, uint32_t color);
    void strokeRoundRect(int x, int y, int w, int h, int r, int lw, uint32_t color);

    /* --- bezier curves (the port does the de Casteljau flattening) --- */
    void strokeQuadratic(int x0, int y0, int cx, int cy, int x1, int y1,
                         int lw, uint32_t color);
    void strokeBezier(int x0, int y0, int cx1, int cy1, int cx2, int cy2,
                      int x1, int y1, int lw, uint32_t color);

    /* --- pixels / blit --- */
    void     setPixel(int x, int y, uint32_t color);
    uint32_t getPixel(int x, int y) const;
    void     blit(eweb_surface_t* src, int sx, int sy, int sw, int sh,
                  int dx, int dy, int dw, int dh, uint8_t alpha);

    /* --- text --- */
    void drawText(int x, int y, const char* text, float size, uint32_t color);
    void strokeText(int x, int y, const char* text, float size, int lw, uint32_t color);
    void textSize(const char* text, float size, int* w, int* h);

    /* --- clip --- */
    void setClip(int x, int y, int w, int h);
    void clearClip();

private:
    /* Create the drawing font on first use (canvas text is rare enough that
     * paying for the font lazily keeps non-text canvases cheap). */
    eweb_font_t* ensureFont();

    const eweb_port_t* m_port;
    std::string     m_id;
    eweb_surface_t* m_bmp;    /* offscreen ARGB surface, m_w x m_h */
    eweb_font_t*    m_font;   /* lazily created on first text op */
    int             m_w;
    int             m_h;
};

}
