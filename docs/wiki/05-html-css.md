# Chapter 5 · HTML & CSS: Parsing, Layout, and the Container

> Language: **English** | [中文](05-html-css.zh.md)

The HTML and CSS subsystem is built entirely on the vendored **litehtml + gumbo** (`litehtml/`). This chapter covers three things: litehtml's "container" inverted-dependency model, how ewebview's container implementation `EWebContainer` maps each callback onto the porting layer, and the complete path of a style from loading to painting.

## 5.1 litehtml's Inverted Dependency: document_container

litehtml itself **paints nothing**, nor does it fetch network resources or measure fonts. It defines a pure-virtual interface `litehtml::document_container` (`litehtml/html.h`) declaring all environment capabilities as ~25 callbacks; during parsing, style computation, layout, and painting it **calls back** into it whenever it needs the environment. ewebview's `EWebContainer` (`src/EWebContainer.{h,cc}`) is the implementation of this interface:

```
litehtml engine (platform-agnostic)
   │  calls back during parse/render/draw
   ▼
EWebContainer : document_container (platform-agnostic, holds eweb_port_t*)
   │  translates each callback into a porting-table call
   ▼
eweb_port_t.gfx / font / image / net (platform implementation)
```

The data flow is therefore: gumbo parses HTML text into a node tree → litehtml builds the `element` tree and applies stylesheets → `render(width)` computes each element's geometry → during the `draw(hdc, ...)` traversal it calls back the container's `draw_text`/`draw_background`/`draw_borders` → the container calls `gfx.*`/`font.*` to paint onto the `eweb_surface_t`. The `hdc` here is the surface handle passed in by the engine (the porting layer may cast it back to the native type).

## 5.2 EWebContainer Callback Groups

| Group | Callbacks | Lands on the porting table |
| --- | --- | --- |
| Font | `create_font` / `delete_font` / `text_width` / `draw_text` / `pt_to_px` / `get_default_font_size` / `get_default_font_name` | `font.create/metrics/char_width/draw_text` |
| Graphics | `draw_background` / `draw_borders` / `draw_list_marker` / `set_clip` / `del_clip` | `gfx.blit_fit_alpha/fill_rect/...`, `gfx.surface_set_clip` |
| Image | `load_image` / `get_image_size` | queues a download task via the host; `image.decode` runs on the download thread |
| CSS loading | `import_css` / `link` (records media) | queues a CSS task via the host |
| Document services | `create_element` (custom elements), `get_media_features`, `get_language`, `get_client_rect`, `transform_text` | viewport size / constants |
| Interaction | `on_anchor_click` / `set_cursor` / `set_caption` | notifies the UI via the host/event queue |

`set_caption` is an empty implementation (the title now goes through the JS bridge's `set_title`); `delete_font` is also empty — fonts are cached by key and destroyed together with the whole container.

### Container ↔ Engine: EWebContainerHost

The container does not directly reference engine classes; instead it faces a small interface (`EWebContainer.h`):

```cpp
class EWebContainerHost {
    virtual bool queueImageTask(const std::string& url) = 0;  // queue an image download
    virtual void loadCSS(const std::string& url) = 0;         // queue a CSS download
    virtual void setCSSMedia(const std::string& url, const std::string& media) = 0;
    virtual void queueNavigation(const std::string& url) = 0; // <a href> click
    virtual bool buildAbortRequested() const = 0;             // build-abort polling
};
```

The engine (`EWebEngine`) implements it. The container therefore does not directly reference engine types and can be tested and replaced independently of the engine.

## 5.3 The Font Pipeline & Caching

Text layout is litehtml's hottest callback path; `EWebContainer` adds two layers of caching:

1. **Font-handle cache** `m_fonts`: key = `"family-NNpx"`. The CSS family is passed to the porting layer as-is (the reference ports have only one CJK system font, so family is merely a hint); the handle returned by `font.create` carries no size — `EWebFontInfo{font, size}` records the requested size, and the `uint_ptr` litehtml receives is the address of this cache entry. `create_font` also back-fills ascent/descent/height/x_height to litehtml's layout via `font.metrics`.
2. **Char-width cache** (8192-slot direct-mapped): `text_width` measures width per UTF-8 code point, each code point looking up the `make_char_width_key(font,size,codepoint)` slot and accumulating on a hit; only a miss calls `font.char_width`. On a hit the whole-string width **never enters the porting layer** — this is the main speedup for long-text layout.

`text_width`/`create_element` are also **build-abort checkpoints**: on detecting `m_abort || host->buildAbortRequested()` they return dummy values (the parse is going to be discarded anyway), letting a single STOP interrupt a whole-page parse within milliseconds.

`getPerfStats()` aggregates these counters (text_width count/time/hit-rate, create_font time); at the end of the `BUILD_RENDER_DOC` stage it prints to `sys.log` (debug builds), giving first-hand data for layout-performance regressions.

## 5.4 The Four Sources of CSS & the Cascade

A document's styles come from:

1. **UA default stylesheet** `master.css`: **no longer compiled into litehtml**; instead the embedder specifies it via `ewebview_set_default_css(url)` before the first page loads (e.g. sdlbrowser uses `res://html/default.css`). The engine treats it as an ordinary `EWEB_TASK_CSS` task, downloads it during `BUILD_PRELOAD_CSS`, and once in hand feeds it into the context's master style set via `loadCSSContent()`. It provides browser-default behaviors like `<div>` being block-level and `<b>` being bold. **If no default CSS is set, `BUILD_PRELOAD_CSS` is skipped entirely** (going straight to `CREATE_DOC`, with no UA stylesheet).
2. **External stylesheets** `<link rel="stylesheet">`: when `el_link` is parsed, the `link()` + `import_css()` callbacks run → the container resolves to an absolute URL then `host->loadCSS(url)` queues the download. `link()` also records the `media` attribute to the engine (`setCSSMedia`) — sheets like `media="print"` that never match are discarded on landing, preventing print styles from polluting screen rendering.
3. **Embedded style blocks** `<style>`: handled by litehtml itself.
4. **Inline styles** `style="..."` attributes: same as above.

**Asynchrony is the core design here**: the `import_css` callback cannot get the CSS text (the download is not finished), it only records the URL; after the CSS text arrives via the download thread, the engine's `loadCSSContent()` feeds it into the context's master style set, marks dirty, and the next engine-loop round recomputes styles uniformly. The same URL is deduplicated with `m_seenCssUrls`.

Media queries (`@media`) depend on the return of `get_media_features()`: viewport width/height come from the container's client area; `device 640x480`, `resolution 96`, `color 8` are constants (adjustable per port).

## 5.5 The Image Pipeline: Defer, Download, Mount

The three paths of `load_image(src, baseurl, redraw_on_ready)`:

- **Already cached** in `m_images`: reference count +1, return directly;
- **During construction** (`m_defer_image_load == true`): only record the URL into `m_pending_image_urls`, issue no task — a half-finished page does not deserve to consume bandwidth; after `BUILD_SWAP_DOC`, `m_flushDeferredImages` is set and step 8 of the engine loop calls `flushPendingImages()` to queue them all;
- **Live phase**: `host->queueImageTask(url)` queues immediately.

Image **decoding happens on the download thread** (`decodeImageData` → `image.decode`, pure heap operations); the engine thread only does the O(1) mount (`mountImage` into `m_images`). `get_image_size` queries the cache during layout: 0x0 if not yet downloaded, real size when relayout happens after download. `draw_background` uses `gfx.blit_fit_alpha` to scale and blend the background image into the background box.

The same cache is reused by the Canvas bridge: `drawImage(<img>)` reads the element `src` via `bitmap_from_element`, resolves to an absolute URL, and queries this cache (see Ch. 8).

## 5.6 Custom Element: eweb_el_input

litehtml has only a generic element for `<input>`. The `create_element` hook intercepts `type="text"` / `type="button"` and constructs `eweb_el_input` (`src/eweb_el_input.{h,cc}`) — a **replaced element** (`is_replaced()` returns true): it carries its own intrinsic size (`get_content_size`), draws itself (`draw()` — painting the border, background color, text, and centered button label via the port's gfx/font), and gives click feedback (`on_click`). It is ported from the widget++-era `el_input`; all painting goes through the porting tables and knows nothing of EwokOS.

This is an example of a litehtml extension point: **any tag needing custom typographic behavior can be intercepted in `create_element`**.

## 5.7 Painting: How the draw Stage Lands Pixels

In `engineRenderFrame()`, the engine calls `m_doc->draw(hdc, x, y, clip)` on the current document (`drawPageToCacheLocked`); litehtml depth-traverses the element tree, each element calling back the container:

- background color/background image → `draw_background` → `gfx.fill_rect` / `gfx.blit_fit_alpha` (with `border-radius` clipping assisted by `set_clip`);
- borders → `draw_borders` → `gfx.fill_rect` (four sides), etc.;
- text → `draw_text` → `font.draw_text(surface, x, y, utf8, font, size, argb)`;
- list markers → `draw_list_marker` → draw a bullet/number text.

Colors are always `0xAARRGGBB` (`web_color_to_argb` converts from litehtml's `web_color`); coordinates are device pixels in the document coordinate system. The draw stage is **pure painting** — all geometry was fixed in the render stage, which is why a repaint (scrolling, content dirty) does not relayout.

## 5.8 Known Boundaries of HTML/CSS

- `<script src="...">` external scripts are **now supported** (downloaded as `EWEB_TASK_SCRIPT` and executed in document order, see Ch. 6);
- litehtml itself is CSS2.1 + partial CSS3 (flex via `flex_layout`, no grid);
- `position:fixed` participates in hit testing (`get_element_by_point`'s client coordinates match); other behaviors are simplified;
- among forms, only the `text`/`button` kinds of `<input>` are replaced elements; the rest render as ordinary tags;
- no iframe/worker/popup — `window.postMessage` accepts the call but does nothing (see Ch. 7's "known gaps").

Most of these boundaries are deliberate trade-offs: the engine's goal is practical rendering on embedded devices, not a complete web platform.
