// litehtml document_container for ewebview, driven entirely by the porting HAL.
//
// Ported from widget++'s XContainer: every xwin/graph/font/tinyhttpsc/vfs
// dependency is replaced by the eweb_port_t tables, so this file (like the
// rest of the core) knows nothing about EwokOS. The engine the container
// serves is reached through the small EWebContainerHost interface below, so
// the container never names the engine type either.

#pragma once

#include <litehtml.h>
#include <ewebview_port.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <unordered_map>

class eweb_el_input;

namespace eweb {

/* The engine-side services a container needs while litehtml parses/lays out a
 * document. Implemented by the ewebview engine; every method runs on the
 * engine thread. */
class EWebContainerHost {
public:
    /* Queue an async sub-resource fetch (returns false when the task table is
     * full; the caller then keeps the URL on its retry list). */
    virtual bool queueImageTask(const std::string& url) = 0;
    virtual void loadCSS(const std::string& url) = 0;
    /* Record a <link rel=stylesheet> media attribute against its resolved URL
     * so a sheet whose media never matches can be dropped when it lands. */
    virtual void setCSSMedia(const std::string& url, const std::string& media) = 0;
    /* Queue an in-page navigation (<a href> click). */
    virtual void queueNavigation(const std::string& url) = 0;
    /* Cross-thread build-abort flag polled by the hottest callbacks. */
    virtual bool buildAbortRequested() const = 0;
};

/* A font handle plus the size it was requested at (the HAL font handle itself
 * is size-independent; litehtml's uint_ptr font handle points at one of these
 * cache entries). */
struct EWebFontInfo {
    eweb_font_t* font;
    int size;
};

/* One cached decoded image. */
struct EWebImageInfo {
    eweb_surface_t* image;
    int ref_count;
};

class EWebContainer : public litehtml::document_container {
public:
    EWebContainer(const eweb_port_t* port, EWebContainerHost* host);
    virtual ~EWebContainer(void);

    virtual litehtml::uint_ptr         create_font(const litehtml::tchar_t* faceName, int size, int weight, litehtml::font_style italic, unsigned int decoration, litehtml::font_metrics* fm) override;
    virtual void                       delete_font(litehtml::uint_ptr hFont) override;
    virtual int                         text_width(const litehtml::tchar_t* text, litehtml::uint_ptr hFont) override;
    virtual void                       draw_text(litehtml::uint_ptr hdc, const litehtml::tchar_t* text, litehtml::uint_ptr hFont, litehtml::web_color color, const litehtml::position& pos) override;

    virtual int                        pt_to_px(int pt) override;
    virtual int                        get_default_font_size() const override;
    virtual const litehtml::tchar_t*   get_default_font_name() const override;
    virtual void                       draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override;
    virtual void                       load_image(const litehtml::tchar_t* src, const litehtml::tchar_t* baseurl, bool redraw_on_ready) override;
    virtual void                       get_image_size(const litehtml::tchar_t* src, const litehtml::tchar_t* baseurl, litehtml::size& sz) override;
    virtual void                       draw_background(litehtml::uint_ptr hdc, const litehtml::background_paint& bg) override;
    virtual void                       draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders, const litehtml::position& draw_pos, bool root) override;
    virtual void                       draw_svg(litehtml::uint_ptr hdc, const litehtml::position& pos,
                                                const litehtml::web_color& color,
                                                const float* pts, const int* counts, int nsubs) override;
    virtual void                       push_paint_transform(const float m[6]) override;
    virtual void                       pop_paint_transform() override;

    virtual void                       transform_text(litehtml::tstring& text, litehtml::text_transform tt) override;
    virtual void                       set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius, bool valid_x, bool valid_y) override;
    virtual void                       del_clip() override;
    virtual litehtml::element*  create_element(const litehtml::tchar_t* tag_name, const litehtml::string_map& attributes, litehtml::document* doc) override;
    virtual void                       get_media_features(litehtml::media_features& media) const override;
    virtual void                       get_language(litehtml::tstring& language, litehtml::tstring& culture) const override;
    virtual void                       link(litehtml::document* doc, const litehtml::element::ptr& el) override;

    void                               clear_images();
    void                               clear_inputs();

    void                               get_client_rect(litehtml::position& client) const;
    void                               set_client_size(int width, int height);
    void                               on_anchor_click(const litehtml::tchar_t* url, const litehtml::element::ptr& el);
    void                               set_cursor(const litehtml::tchar_t* cursor);
    void                               import_css(litehtml::tstring& text, const litehtml::tstring& url, litehtml::tstring& baseurl);
    void                               set_caption(const litehtml::tchar_t* caption);
    void                               set_base_url(const litehtml::tchar_t* base_url);
    void                               setDeferImageLoad(bool defer);
    void                               flushPendingImages();

    void                               resetPerfStats();
    void                               getPerfStats(uint32_t& textWidthCalls, uint32_t& textWidthMs,
                                                    uint32_t& drawTextCalls, uint32_t& drawTextMs,
                                                    uint32_t& textWidthHits, uint32_t& textWidthMisses,
                                                    uint32_t& charWidthHits, uint32_t& charWidthMisses,
                                                    uint32_t& createFontCalls, uint32_t& createFontMs) const;
    /* Fetch `url` (http/https/file) through the port's net table and return a
     * malloc'd body, or NULL. `pageUrl` is the document that initiated the
     * fetch and `topLevel` marks the main-document navigation itself; together
     * they decide which of the cookie jar's entries may go out on the request
     * (see EWebCookieJar::requestHeader). Both default to "no initiator
     * known", which counts as same-site. Runs on the download worker thread. */
    static uint8_t*                    loadURL(const eweb_port_t* port,
                                               const std::string& url, int* sz,
                                               const std::string& pageUrl = std::string(),
                                               bool topLevel = false);
    static std::string                 normalizeURL(const eweb_port_t* port,
                                                    const std::string& url,
                                                    const std::string& baseurl);
    /* Decode-only path: pure heap work (the port's image.decode), safe to run
     * on the download worker thread; no engine state is touched. */
    static eweb_surface_t*             decodeImageData(const eweb_port_t* port,
                                                       const uint8_t* data, int sz);
    bool                               loadImageData(const std::string& url, uint8_t* data, int sz);
    /* Engine-thread O(1) mount of a bitmap decoded by decodeImageData(); takes
     * ownership on success (replacing any cached image for the same url). */
    bool                               mountImage(const std::string& url, eweb_surface_t* img);
    /* Look up a decoded bitmap by URL. Returns the cached surface (owned by
     * the container, caller must NOT free it) or NULL if not cached. Used by
     * the Canvas 2D bridge's bitmap_from_element callback so drawImage(<img>)
     * resolves through the same cache litehtml uses for layout. */
    eweb_surface_t*                    getImage(const std::string& url) const;
    /* Resolve a possibly-relative src to the absolute URL key the cache uses.
     * Mirrors getFullURL but exposed publicly for the canvas bridge. */
    std::string                        resolveUrl(const std::string& src) const;
    /* Resolve `src` against an explicit `baseurl` with no container instance -
     * a pure function of (port, src, baseurl). Public so the engine can resolve
     * a static <script src> while the build container is not yet alive, and a
     * dynamically injected <script src> against the document URL. */
    static const std::string           getFullURL(const eweb_port_t* port,
                                                  const std::string& src,
                                                  const std::string& baseurl);

    /* Build abort: once set, the hottest litehtml callbacks (text_width,
     * create_element, load_image) short-circuit so an in-flight
     * createFromString/render unwinds fast. Set on the build container only;
     * the half-built document is discarded afterwards, so bogus metrics are
     * fine. */
    void                               setAbort(bool abort) { m_abort = abort; }
    bool                               aborted() const { return m_abort; }

private:
    enum { CHAR_WIDTH_CACHE_SIZE = 8192 };

    const eweb_port_t* m_port;
    EWebContainerHost* m_host;
    std::unordered_map<std::string, EWebFontInfo> m_fonts;
    std::unordered_map<std::string, EWebImageInfo> m_images;
    int m_client_width;
    int m_client_height;
    std::string m_base_url;
    uint32_t m_text_width_calls;
    uint32_t m_text_width_ms;
    uint32_t m_draw_text_calls;
    uint32_t m_draw_text_ms;
    uint32_t m_text_width_hits;
    uint32_t m_text_width_misses;
    uint32_t m_char_width_hits;
    uint32_t m_char_width_misses;
    uint32_t m_create_font_calls;
    uint32_t m_create_font_ms;
    bool m_defer_image_load;
    bool m_abort;
    uint64_t m_char_width_keys[CHAR_WIDTH_CACHE_SIZE];
    int m_char_width_vals[CHAR_WIDTH_CACHE_SIZE];
    std::vector<std::string> m_pending_image_urls;

    /* overflow/clip support: litehtml pushes a clip rectangle around the
     * children of any box with overflow != visible; we mirror the stack onto
     * the port surface clip so text, blits and fills all honour it. The border
     * radius travels with the entry so image compositing under a rounded clip
     * (the .avatar border-radius:50% + overflow:hidden case) can mask the blit
     * to the round box; the HAL clip itself stays rectangular. */
    struct clip_entry { litehtml::position r; int radius; };
    std::vector<clip_entry> m_clips;
    int                        top_clip_radius() const;
    void* m_paint_surf;

    /* Paint-time affine transform (CSS transform of the box whose borders are
     * being drawn), see litehtml document_container::push_paint_transform. */
    bool m_xform_on;
    float m_xform[6];

    std::vector<eweb_el_input*> m_vecInput;

    static uint32_t web_color_to_argb(const litehtml::web_color& c);
};

}
