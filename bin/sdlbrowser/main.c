/*
 * sdlbrowser - a minimal SDL2 desktop browser shell around ewebview.
 *
 * Layout (mirrors ~/work/ewokos/browser/apps/xBrowser):
 *
 *   +--------------------------------------------------------------+
 *   | [<] [X] [R] | https://address.bar/.........................  |  toolbar
 *   +--------------------------------------------------------------+
 *   |                                                              |
 *   |                     ewebview content                         |
 *   |                                                              |
 *   +--------------------------------------------------------------+
 *   | status text                                                  |  status bar
 *   +--------------------------------------------------------------+
 *
 * The ewebview engine is created with the SDL2 port (eweb_port_sdl2), which
 * maps every HAL table onto SDL2 / SDL2_ttf / SDL2_image / SDL2_gfx and
 * libtinyhttpsc.  Frames adopted from the engine are SDL_Surface* (ARGB8888);
 * we upload them into a streaming SDL_Texture and RenderCopy into the content
 * rect, shifted by (frameScroll - liveScroll) so fast wheel scrolling tracks
 * the gesture before the engine's refill frame lands.
 *
 * Build: see Makefile (links libewebview + SDL2 + companions).
 * Run:   ./sdlbrowser [url]   (default: res://html/default.html)
 *
 * Plain C99.
 */

#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <ewebview.h>
#include <ewebview_port.h>

/* The SDL2 port lives in libewebview.a (porting/src/sdl2/port_sdl2.c) but is
 * not declared in the public header; declare it here. */
extern void eweb_port_sdl2(eweb_port_t* port, void* ud);
extern void eweb_port_sdl2_set_dpr(float dpr);

/* ------------------------------------------------------------------ */
/* OS color scheme                                                     */
/* ------------------------------------------------------------------ */

/* Seed EWEB_COLOR_SCHEME from the OS appearance so pages that gate styles on
 * prefers-color-scheme (e.g. GitHub's dark theme) follow the desktop.  A
 * value already in the environment always wins. */
static void browser_detect_color_scheme(void) {
#if defined(__APPLE__)
    CFStringRef v;
    if(getenv("EWEB_COLOR_SCHEME")) return;
    /* The global preferences domain's identifier is literally the string
     * "kCFPreferencesGlobalDomain"; CoreFoundation ships no constant for it. */
    v = (CFStringRef)CFPreferencesCopyValue(CFSTR("AppleInterfaceStyle"),
            CFSTR("kCFPreferencesGlobalDomain"),
            kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    if(v) {
        int dark = (CFGetTypeID(v) == CFStringGetTypeID() &&
                    CFStringCompare(v, CFSTR("Dark"), 0) == kCFCompareEqualTo);
        setenv("EWEB_COLOR_SCHEME", dark ? "dark" : "light", 1);
        CFRelease(v);
    }
#endif
}

/* ------------------------------------------------------------------ */
/* UI geometry                                                         */
/* ------------------------------------------------------------------ */

/* Base chrome metrics in LOGICAL points. browser_apply_scale() multiplies each
 * by ui_scale (device px / logical point) so the toolbar, buttons, status bar
 * and UI font render crisp at their intended physical size on HiDPI displays.
 * The ewebview content canvas is sized in real device pixels and blit 1:1, so
 * it is never stretched - the engine reflows/reflows at the true pixel size. */
#define TOOLBAR_H     34
#define STATUSBAR_H   22
#define BUTTON_W      34
#define PAD           4
#define UI_FONT_PX    14
#define DEFAULT_W     1024
#define DEFAULT_H     768
#define MIN_W         320
#define MIN_H         240
#define ADDRESS_MAX   1023
#define STATUS_MAX    511
#define HISTORY_MAX   64
#define SCROLL_STEP   48      /* px per wheel click */

/* ------------------------------------------------------------------ */
/* Colors (0xAARRGGBB)                                                 */
/* ------------------------------------------------------------------ */

#define C_TOOLBAR_BG    0xFFE4E4E4u
#define C_TOOLBAR_BOT   0xFFB0B0B0u
#define C_BTN_FACE      0xFFD8D8D8u
#define C_BTN_HOVER     0xFFC4C4C4u
#define C_BTN_DOWN      0xFFA8A8A8u
#define C_BTN_BORDER    0xFF909090u
#define C_BTN_TEXT      0xFF202020u
#define C_EDIT_BG       0xFFFFFFFFu
#define C_EDIT_BORDER   0xFF808080u
#define C_EDIT_FOCUS    0xFF4A90E2u
#define C_EDIT_TEXT     0xFF101010u
#define C_EDIT_CARET    0xFF101010u
#define C_STATUS_BG     0xFFF0F0F0u
#define C_STATUS_TOP    0xFFC8C8C8u
#define C_STATUS_TEXT   0xFF404040u
#define C_CONTENT_BG    0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* Small UI widgets                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    SDL_Rect rect;
    char     label[8];
    bool     hovered;
    bool     down;
} ui_button_t;

typedef struct {
    SDL_Rect rect;
    char     text[ADDRESS_MAX + 1];
    int      cursor;          /* byte offset of caret in text[] */
    bool     focused;
    bool     select_all;      /* next click / focus selects all */
    int      scroll;          /* horizontal text scroll (px) */
} ui_edit_t;

/* ------------------------------------------------------------------ */
/* Browser state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    /* SDL */
    SDL_Window*   window;
    SDL_Renderer* renderer;
    TTF_Font*     ui_font;
    int           win_w, win_h;   /* renderer pixel space (device px under HiDPI) */
    float         ui_scale;       /* device px per logical point (HiDPI factor) */
    int           toolbar_h, statusbar_h, button_w, pad;  /* chrome metrics scaled to device px */
    char          font_path[512]; /* resolved UI font; re-opened at the scaled size */
    int           font_px;        /* current ui_font pixel size (device px) */

    /* ewebview */
    ewebview_t*   view;
    eweb_port_t   port;             /* kept so we can call surface_native() */
    eweb_surface_t* frame;        /* adopted frame (owned until released) */
    eweb_surface_t* frame_prev;   /* previous frame, released only AFTER its
                                   * replacement has been uploaded: the engine
                                   * recycles a released buffer immediately and
                                   * would overwrite it mid-upload (torn rows /
                                   * ghost text / half-drawn images). */
    SDL_Texture*  frame_tex;      /* streaming texture, viewport-sized */
    int           frame_sx, frame_sy;   /* scroll the frame was rendered at */
    int           doc_w, doc_h;         /* full document size */
    int           scroll_x, scroll_y;   /* live UI scroll offset */
    int           view_w, view_h;       /* ewebview viewport in LOGICAL CSS px */
    int           tex_w, tex_h;         /* frame texture size in DEVICE px */
    bool          pool_flush;           /* dpr changed: force frame pool turnover */

    /* toolbar widgets */
    ui_button_t   btn_back, btn_stop, btn_refresh;
    ui_edit_t     address;

    /* status */
    char          status[STATUS_MAX + 1];
    char          current_url[ADDRESS_MAX + 1];

    /* session history (back stack) */
    char          history[HISTORY_MAX][ADDRESS_MAX + 1];
    int           history_count;
    bool          history_nav;    /* suppress push for programmatic nav */

    /* misc */
    bool          running;
    bool          content_dirty;  /* re-upload frame texture */
    uint32_t      last_tick_ms;

    /* headless screenshot mode: `sdlbrowser <url> --shot out.bmp [settle_ms]`
     * saves one frame of the page surface and exits (regression / CI use). */
    const char*   shot_path;
    uint32_t      shot_settle_ms;
    uint32_t      shot_start_ms;
    int           shot_scroll_y;   /* -1 = capture at top; else scroll first */
    bool          shot_scrolled;
} browser_t;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static void browser_load(browser_t* b, const char* url);
static void browser_back(browser_t* b);
static void browser_stop(browser_t* b);
static void browser_refresh(browser_t* b);
static void browser_resize(browser_t* b, int w, int h);
static void browser_render(browser_t* b);
static void browser_handle_event(browser_t* b, const SDL_Event* ev);
static void browser_ui_scroll(browser_t* b, int dx, int dy);

/* ------------------------------------------------------------------ */
/* ewebview listener callbacks (all on the UI thread, inside tick)     */
/* ------------------------------------------------------------------ */

static void cb_frame(void* ud, eweb_surface_t* frame,
                     int fsx, int fsy, int dw, int dh) {
    browser_t* b = (browser_t*)ud;
    eweb_surface_t* old = b->frame;

    b->frame    = frame;
    b->frame_sx = fsx;
    b->frame_sy = fsy;
    b->doc_w    = dw;
    b->doc_h    = dh;
    b->content_dirty = true;

    /* Do NOT release the outgoing frame here: upload_frame() still has to
     * copy its pixels this tick, and a released buffer goes straight back
     * into the engine's pool where the next render overwrites it - racing
     * the upload and tearing the picture. Park it in frame_prev instead;
     * upload_frame() releases it once the new frame is safely in the
     * texture (and browser_destroy releases it at shutdown). */
    if(b->frame_prev && b->view)
        ewebview_release_frame(b->view, b->frame_prev);
    b->frame_prev = old;
}

static void cb_scroll(void* ud, int x, int y, int dw, int dh) {
    browser_t* b = (browser_t*)ud;
    b->scroll_x = x;
    b->scroll_y = y;
    b->doc_w    = dw;
    b->doc_h    = dh;
}

static void cb_url(void* ud, const char* url) {
    browser_t* b = (browser_t*)ud;
    if(!url) url = "";
    snprintf(b->current_url, sizeof(b->current_url), "%s", url);

    /* history bookkeeping (mirrors xBrowser::onHtmlUrlChanged) */
    if(b->history_nav) {
        b->history_nav = false;
    } else if(b->history_count == 0 ||
              strcmp(b->history[b->history_count - 1], url) != 0) {
        if(b->history_count < HISTORY_MAX) {
            snprintf(b->history[b->history_count], ADDRESS_MAX + 1, "%s", url);
            b->history_count++;
        }
    }

    /* keep the address bar in sync unless the user is editing it */
    if(!b->address.focused) {
        snprintf(b->address.text, sizeof(b->address.text), "%s", url);
        b->address.cursor = (int)strlen(b->address.text);
        b->address.scroll = 0;
    }
}

static void cb_status(void* ud, const char* text, int progress) {
    browser_t* b = (browser_t*)ud;
    (void)progress;
    snprintf(b->status, sizeof(b->status), "%s", text ? text : "");
}

static void cb_build_status(void* ud, const char* text, int progress, bool overlay) {
    browser_t* b = (browser_t*)ud;
    if(overlay && text && text[0])
        snprintf(b->status, sizeof(b->status), "%s (%d%%)", text, progress);
    else if(!overlay)
        b->status[0] = '\0';
}

static void cb_dialog(void* ud, const char* text) {
    browser_t* b = (browser_t*)ud;
    snprintf(b->status, sizeof(b->status), "[dialog] %s", text ? text : "");
}

static void cb_task_start(void* ud, const char* url, int type) {
    browser_t* b = (browser_t*)ud;
    (void)type;
    snprintf(b->status, sizeof(b->status), "loading %s", url ? url : "");
}

static void cb_task_end(void* ud, const char* url, int type) {
    browser_t* b = (browser_t*)ud;
    (void)type; (void)url;
    b->status[0] = '\0';
}

static void cb_task_failed(void* ud, const char* url, int type) {
    browser_t* b = (browser_t*)ud;
    (void)type;
    snprintf(b->status, sizeof(b->status), "FAILED: %s", url ? url : "");
}

static void cb_tasks_end(void* ud) {
    browser_t* b = (browser_t*)ud;
    b->status[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                         */
/* ------------------------------------------------------------------ */

/* If the user typed a bare domain, prepend https:// (mirrors xBrowser). */
static void normalize_url(const char* in, char* out, int outsz) {
    if(!in || !in[0]) { out[0] = '\0'; return; }
    if(strstr(in, "://") || strncmp(in, "file:", 5) == 0 ||
       strncmp(in, "about:", 6) == 0 || strncmp(in, "data:", 5) == 0) {
        snprintf(out, outsz, "%s", in);
    } else if(in[0] == '/') {
        snprintf(out, outsz, "file:%s", in);
    } else {
        snprintf(out, outsz, "https://%s", in);
    }
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */

static void browser_load(browser_t* b, const char* url) {
    char full[ADDRESS_MAX + 1];
    if(!b->view || !url || !url[0]) return;
    normalize_url(url, full, sizeof(full));
    if(!full[0]) return;

    /* update address bar */
    snprintf(b->address.text, sizeof(b->address.text), "%s", full);
    b->address.cursor = (int)strlen(b->address.text);
    b->address.scroll = 0;

    ewebview_load(b->view, full);
}

static void browser_back(browser_t* b) {
    if(b->history_count < 2) return;
    b->history_count--;                    /* drop current */
    b->history_nav = true;                 /* the pop IS the bookkeeping */
    browser_load(b, b->history[b->history_count - 1]);
}

static void browser_stop(browser_t* b) {
    if(b->view) ewebview_stop(b->view);
    snprintf(b->status, sizeof(b->status), "stopped");
}

static void browser_refresh(browser_t* b) {
    const char* url = b->current_url[0] ? b->current_url : b->address.text;
    if(!url[0]) return;
    b->history_nav = true;                 /* same page: no duplicate push */
    if(b->view) ewebview_reload(b->view);
}

/* ------------------------------------------------------------------ */
/* Scroll (UI-local, mirrors WidgetWebview::uiLocalScroll)             */
/* ------------------------------------------------------------------ */

static void browser_ui_scroll(browser_t* b, int dx, int dy) {
    int nx = b->scroll_x + dx;
    int ny = b->scroll_y + dy;
    int maxx = b->doc_w - b->view_w; if(maxx < 0) maxx = 0;
    int maxy = b->doc_h - b->view_h; if(maxy < 0) maxy = 0;
    if(nx < 0) nx = 0; else if(nx > maxx) nx = maxx;
    if(ny < 0) ny = 0; else if(ny > maxy) ny = maxy;
    if(nx == b->scroll_x && ny == b->scroll_y) return;
    b->scroll_x = nx;
    b->scroll_y = ny;
    if(b->view) ewebview_scroll(b->view, nx, ny);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static void layout_toolbar(browser_t* b) {
    int x = b->pad;
    int y = b->pad;
    int bh = b->toolbar_h - b->pad * 2;
    if(bh < 1) bh = 1;

    b->btn_back.rect    = (SDL_Rect){ x, y, b->button_w, bh }; x += b->button_w + b->pad;
    b->btn_stop.rect    = (SDL_Rect){ x, y, b->button_w, bh }; x += b->button_w + b->pad;
    b->btn_refresh.rect = (SDL_Rect){ x, y, b->button_w, bh }; x += b->button_w + b->pad;

    int aw = b->win_w - x - b->pad;
    if(aw < 1) aw = 1;
    b->address.rect = (SDL_Rect){ x, y, aw, bh };
}

static SDL_Rect content_rect(browser_t* b) {
    SDL_Rect r;
    r.x = 0;
    r.y = b->toolbar_h;
    r.w = b->win_w;
    r.h = b->win_h - b->toolbar_h - b->statusbar_h;
    if(r.h < 1) r.h = 1;
    return r;
}

static SDL_Rect status_rect(browser_t* b) {
    SDL_Rect r;
    r.x = 0;
    r.y = b->win_h - b->statusbar_h;
    r.w = b->win_w;
    r.h = b->statusbar_h;
    if(r.h < 1) r.h = 1;
    return r;
}

/* ------------------------------------------------------------------ */
/* Frame texture management                                            */
/* ------------------------------------------------------------------ */

static void ensure_frame_texture(browser_t* b) {
    if(b->frame_tex && b->tex_w > 0 && b->tex_h > 0) {
        int tw, th;
        SDL_QueryTexture(b->frame_tex, NULL, NULL, &tw, &th);
        if(tw == b->tex_w && th == b->tex_h)
            return;   /* already the right size */
        SDL_DestroyTexture(b->frame_tex);
        b->frame_tex = NULL;
    }
    if(b->tex_w <= 0 || b->tex_h <= 0) return;
    b->frame_tex = SDL_CreateTexture(b->renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        b->tex_w, b->tex_h);
    /* Pre-fill with the content background (opaque white = 0xFFFFFFFF in
     * ARGB8888) so the transient window right after a resize - when the previous,
     * differently-sized frame is uploaded before the engine's refilled frame
     * lands - shows white in any region the old frame does not cover, instead of
     * uninitialised texture memory. */
    if(b->frame_tex) {
        void* px; int pt;
        if(SDL_LockTexture(b->frame_tex, NULL, &px, &pt) == 0) {
            for(int y = 0; y < b->tex_h; y++)
                memset((uint8_t*)px + (size_t)y * pt, 0xFF, (size_t)pt);
            SDL_UnlockTexture(b->frame_tex);
        }
    }
}

static void upload_frame(browser_t* b) {
    if(!b->frame || !b->frame_tex) return;

    /* The SDL2 port's surface_native() hands back the concrete SDL_Surface*. */
    SDL_Surface* surf = NULL;
    if(b->port.gfx.surface_native)
        surf = (SDL_Surface*)b->port.gfx.surface_native(b->port.gfx.ud, b->frame);
    if(!surf) return;

    void* pixels;
    int pitch;
    if(SDL_LockTexture(b->frame_tex, NULL, &pixels, &pitch) < 0) return;

    /* Copy row by row (src pitch may differ from dst pitch). */
    uint8_t* dst = (uint8_t*)pixels;
    uint8_t* src = (uint8_t*)surf->pixels;
    int copy_pitch = surf->pitch < pitch ? surf->pitch : pitch;
    int rows = surf->h < b->tex_h ? surf->h : b->tex_h;
    for(int y = 0; y < rows; y++)
        memcpy(dst + y * pitch, src + y * surf->pitch, (size_t)copy_pitch);

    SDL_UnlockTexture(b->frame_tex);
    b->content_dirty = false;

    /* The new frame's pixels are now in the texture; the parked previous
     * frame is no longer referenced anywhere and can go back to the pool. */
    if(b->frame_prev && b->view) {
        ewebview_release_frame(b->view, b->frame_prev);
        b->frame_prev = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing helpers                                                     */
/* ------------------------------------------------------------------ */

static void draw_rect(SDL_Renderer* r, int x, int y, int w, int h, uint32_t c) {
    SDL_SetRenderDrawColor(r, (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, (c>>24)&0xFF);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(r, &rc);
}

static void draw_rect_outline(SDL_Renderer* r, int x, int y, int w, int h, uint32_t c) {
    SDL_SetRenderDrawColor(r, (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, (c>>24)&0xFF);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderDrawRect(r, &rc);
}

static void draw_button(SDL_Renderer* r, TTF_Font* font, ui_button_t* btn) {
    uint32_t bg = btn->down ? C_BTN_DOWN : btn->hovered ? C_BTN_HOVER : C_BTN_FACE;
    SDL_Rect rc = btn->rect;

    draw_rect(r, rc.x, rc.y, rc.w, rc.h, bg);
    draw_rect_outline(r, rc.x, rc.y, rc.w, rc.h, C_BTN_BORDER);

    /* center the label */
    SDL_Color fg = { (C_BTN_TEXT>>16)&0xFF, (C_BTN_TEXT>>8)&0xFF,
                     C_BTN_TEXT&0xFF, 0xFF };
    SDL_Surface* txt = TTF_RenderUTF8_Blended(font, btn->label, fg);
    if(txt) {
        SDL_Texture* tt = SDL_CreateTextureFromSurface(r, txt);
        int tx = rc.x + (rc.w - txt->w) / 2;
        int ty = rc.y + (rc.h - txt->h) / 2;
        SDL_Rect dr = { tx, ty, txt->w, txt->h };
        SDL_RenderCopy(r, tt, NULL, &dr);
        SDL_DestroyTexture(tt);
        SDL_FreeSurface(txt);
    }
}

/* Byte-length of the UTF-8 character starting at s[i]. */
static int utf8_char_len(const char* s, int i) {
    unsigned char c = (unsigned char)s[i];
    if(c < 0x80) return 1;
    if((c & 0xE0) == 0xC0) return 2;
    if((c & 0xF0) == 0xE0) return 3;
    if((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Move cursor left/right by one UTF-8 character. */
static void edit_move_cursor(ui_edit_t* e, int dir) {
    if(dir < 0) {
        /* walk backwards: find the start of the previous char */
        int i = e->cursor - 1;
        while(i > 0 && ((unsigned char)e->text[i] & 0xC0) == 0x80) i--;
        if(i < 0) i = 0;
        e->cursor = i;
    } else {
        int len = utf8_char_len(e->text, e->cursor);
        e->cursor += len;
        if(e->cursor > (int)strlen(e->text))
            e->cursor = (int)strlen(e->text);
    }
}

static void draw_edit(SDL_Renderer* r, TTF_Font* font, ui_edit_t* e) {
    SDL_Rect rc = e->rect;

    /* background + border */
    draw_rect(r, rc.x, rc.y, rc.w, rc.h, C_EDIT_BG);
    draw_rect_outline(r, rc.x, rc.y, rc.w, rc.h,
                      e->focused ? C_EDIT_FOCUS : C_EDIT_BORDER);

    /* text (clipped to the edit rect, scrolled horizontally) */
    int inner_x = rc.x + 5;
    int inner_w = rc.w - 10;
    if(inner_w <= 0) return;

    SDL_Color fg = { (C_EDIT_TEXT>>16)&0xFF, (C_EDIT_TEXT>>8)&0xFF,
                     C_EDIT_TEXT&0xFF, 0xFF };
    SDL_Surface* txt = TTF_RenderUTF8_Blended(font, e->text, fg);
    if(txt) {
        /* auto-scroll so the caret stays visible */
        int caret_x = 0;
        if(e->cursor > 0) {
            char saved = e->text[e->cursor];
            e->text[e->cursor] = '\0';
            int cw = 0, ch = 0;
            TTF_SizeUTF8(font, e->text, &cw, &ch);
            e->text[e->cursor] = saved;
            caret_x = cw;
        }
        if(caret_x - e->scroll > inner_w - 4)
            e->scroll = caret_x - inner_w + 4;
        if(caret_x - e->scroll < 0)
            e->scroll = caret_x;
        if(e->scroll < 0) e->scroll = 0;

        SDL_Rect clip = { inner_x, rc.y, inner_w, rc.h };
        SDL_RenderSetClipRect(r, &clip);
        SDL_Texture* tt = SDL_CreateTextureFromSurface(r, txt);
        int ty = rc.y + (rc.h - txt->h) / 2;
        SDL_Rect dr = { inner_x - e->scroll, ty, txt->w, txt->h };
        SDL_RenderCopy(r, tt, NULL, &dr);
        SDL_DestroyTexture(tt);
        SDL_RenderSetClipRect(r, NULL);

        /* caret */
        if(e->focused) {
            int cx = inner_x + caret_x - e->scroll;
            int cy = rc.y + 4;
            int ch2 = rc.h - 8;
            draw_rect(r, cx, cy, 1, ch2, C_EDIT_CARET);
        }
        SDL_FreeSurface(txt);
    }
}

static void draw_statusbar(SDL_Renderer* r, TTF_Font* font, browser_t* b) {
    SDL_Rect rc = status_rect(b);
    draw_rect(r, rc.x, rc.y, rc.w, rc.h, C_STATUS_BG);
    /* top border line */
    SDL_SetRenderDrawColor(r, (C_STATUS_TOP>>16)&0xFF, (C_STATUS_TOP>>8)&0xFF,
                           C_STATUS_TOP&0xFF, 0xFF);
    SDL_RenderDrawLine(r, rc.x, rc.y, rc.x + rc.w, rc.y);

    if(b->status[0]) {
        SDL_Color fg = { (C_STATUS_TEXT>>16)&0xFF, (C_STATUS_TEXT>>8)&0xFF,
                         C_STATUS_TEXT&0xFF, 0xFF };
        SDL_Surface* txt = TTF_RenderUTF8_Blended(font, b->status, fg);
        if(txt) {
            SDL_Texture* tt = SDL_CreateTextureFromSurface(r, txt);
            int ty = rc.y + (rc.h - txt->h) / 2;
            SDL_Rect dr = { rc.x + 6, ty, txt->w, txt->h };
            SDL_RenderCopy(r, tt, NULL, &dr);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(txt);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main render                                                         */
/* ------------------------------------------------------------------ */

static void browser_render(browser_t* b) {
    SDL_Renderer* r = b->renderer;

    /* clear */
    SDL_SetRenderDrawColor(r, 0xFF, 0xFF, 0xFF, 0xFF);
    SDL_RenderClear(r);

    /* toolbar background */
    draw_rect(r, 0, 0, b->win_w, b->toolbar_h, C_TOOLBAR_BG);
    SDL_SetRenderDrawColor(r, (C_TOOLBAR_BOT>>16)&0xFF, (C_TOOLBAR_BOT>>8)&0xFF,
                           C_TOOLBAR_BOT&0xFF, 0xFF);
    SDL_RenderDrawLine(r, 0, b->toolbar_h - 1, b->win_w, b->toolbar_h - 1);

    /* buttons */
    draw_button(r, b->ui_font, &b->btn_back);
    draw_button(r, b->ui_font, &b->btn_stop);
    draw_button(r, b->ui_font, &b->btn_refresh);

    /* address bar */
    draw_edit(r, b->ui_font, &b->address);

    /* content area */
    SDL_Rect cr = content_rect(b);
    if(b->content_dirty)
        upload_frame(b);

    if(b->frame_tex) {
        /* Blit shifted by (frameScroll - liveScroll) so already-rendered
         * content tracks a fast wheel/drag before the refill frame lands. */
        /* Scroll offsets are LOGICAL CSS px; the blit runs in device px. */
        int dx = cr.x + (int)lroundf((float)(b->frame_sx - b->scroll_x) * b->ui_scale);
        int dy = cr.y + (int)lroundf((float)(b->frame_sy - b->scroll_y) * b->ui_scale);

        SDL_RenderSetClipRect(r, &cr);
        SDL_Rect dr = { dx, dy, b->tex_w, b->tex_h };
        SDL_RenderCopy(r, b->frame_tex, NULL, &dr);
        SDL_RenderSetClipRect(r, NULL);
    } else {
        draw_rect(r, cr.x, cr.y, cr.w, cr.h, C_CONTENT_BG);
    }

    /* status bar */
    draw_statusbar(r, b->ui_font, b);

    SDL_RenderPresent(r);
}

/* ------------------------------------------------------------------ */
/* Hit testing                                                         */
/* ------------------------------------------------------------------ */

static bool point_in_rect(int px, int py, const SDL_Rect* r) {
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

static ui_button_t* hit_button(browser_t* b, int x, int y) {
    if(point_in_rect(x, y, &b->btn_back.rect))    return &b->btn_back;
    if(point_in_rect(x, y, &b->btn_stop.rect))    return &b->btn_stop;
    if(point_in_rect(x, y, &b->btn_refresh.rect)) return &b->btn_refresh;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Address bar editing                                                 */
/* ------------------------------------------------------------------ */

static void edit_insert_text(ui_edit_t* e, const char* text) {
    if(e->select_all) {
        e->text[0] = '\0';
        e->cursor = 0;
        e->select_all = false;
    }
    int len = (int)strlen(e->text);
    int ins = (int)strlen(text);
    if(len + ins > ADDRESS_MAX) return;
    /* shift tail right */
    memmove(e->text + e->cursor + ins, e->text + e->cursor, (size_t)(len - e->cursor + 1));
    memcpy(e->text + e->cursor, text, (size_t)ins);
    e->cursor += ins;
}

static void edit_backspace(ui_edit_t* e) {
    if(e->select_all) {
        e->text[0] = '\0'; e->cursor = 0; e->select_all = false; return;
    }
    if(e->cursor <= 0) return;
    /* find start of previous char */
    int i = e->cursor - 1;
    while(i > 0 && ((unsigned char)e->text[i] & 0xC0) == 0x80) i--;
    int len = e->cursor - i;
    memmove(e->text + i, e->text + e->cursor, strlen(e->text + e->cursor) + 1);
    e->cursor = i;
    (void)len;
}

static void edit_delete(ui_edit_t* e) {
    if(e->select_all) {
        e->text[0] = '\0'; e->cursor = 0; e->select_all = false; return;
    }
    int len = (int)strlen(e->text);
    if(e->cursor >= len) return;
    int clen = utf8_char_len(e->text, e->cursor);
    memmove(e->text + e->cursor, e->text + e->cursor + clen,
            (size_t)(len - e->cursor - clen + 1));
}

static void edit_select_all(ui_edit_t* e) {
    e->select_all = true;
    e->cursor = (int)strlen(e->text);
}

/* ------------------------------------------------------------------ */
/* Event handling                                                      */
/* ------------------------------------------------------------------ */

/* Mouse coordinate space (measured on macOS/Metal HighDPI): SDL delivers mouse
 * EVENT coords (ev->motion/ev->button .x/.y) already in DEVICE PIXELS, while
 * SDL_GetMouseState() reports LOGICAL POINTS. To avoid mixing the two spaces we
 * always read the position from SDL_GetMouseState (points) and scale by ui_scale
 * into the device-pixel space all layout/hit-testing here uses. */
/* Device-pixel offset -> LOGICAL CSS px (the space ewebview events use). */
static int dev_to_logical(const browser_t* b, int d) {
    float s = (b->ui_scale > 0.0f) ? b->ui_scale : 1.0f;
    return (int)((float)d / s);
}

static void browser_mouse_pos(const browser_t* b, int* dx, int* dy) {
    int px = 0, py = 0;
    SDL_GetMouseState(&px, &py);
    float s = (b->ui_scale > 0.0f) ? b->ui_scale : 1.0f;
    *dx = (int)((float)px * s);
    *dy = (int)((float)py * s);
}

static void browser_handle_event(browser_t* b, const SDL_Event* ev) {
    switch(ev->type) {

    case SDL_QUIT:
        b->running = false;
        break;

    case SDL_WINDOWEVENT:
        /* SIZE_CHANGED: the window was resized. DISPLAY_CHANGED: it moved to a
         * display with a different DPI scale. Both re-derive ui_scale, the
         * chrome metrics and the device-pixel canvas (browser_apply_scale also
         * re-opens the UI font when the scale changes). */
        if(ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
           ev->window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
            browser_resize(b, ev->window.data1, ev->window.data2);
        }
        break;

    case SDL_MOUSEMOTION: {
        int mx, my; browser_mouse_pos(b, &mx, &my);
        /* button hover */
        b->btn_back.hovered    = point_in_rect(mx, my, &b->btn_back.rect);
        b->btn_stop.hovered    = point_in_rect(mx, my, &b->btn_stop.rect);
        b->btn_refresh.hovered = point_in_rect(mx, my, &b->btn_refresh.rect);

        /* forward move to ewebview when inside the content area */
        SDL_Rect cr = content_rect(b);
        if(b->view && point_in_rect(mx, my, &cr)) {
            eweb_event_t wev;
            memset(&wev, 0, sizeof(wev));
            wev.mouse_state = EWEB_MOUSE_MOVE;
            wev.button = EWEB_BUTTON_NONE;
            wev.cx = dev_to_logical(b, mx - cr.x);
            wev.cy = dev_to_logical(b, my - cr.y);
            ewebview_post_event(b->view, &wev);
        }
        break;
    }

    case SDL_MOUSEBUTTONDOWN: {
        int mx, my; browser_mouse_pos(b, &mx, &my);

        /* toolbar buttons */
        ui_button_t* btn = hit_button(b, mx, my);
        if(btn && ev->button.button == SDL_BUTTON_LEFT) {
            btn->down = true;
            break;
        }

        /* address bar focus */
        if(point_in_rect(mx, my, &b->address.rect)) {
            if(!b->address.focused) {
                b->address.focused = true;
                SDL_StartTextInput();
            }
            /* click positions the caret (simplified: end of text) */
            if(b->address.select_all) {
                /* keep selection; a second click deselects */
                b->address.select_all = false;
            }
            break;
        } else {
            if(b->address.focused) {
                b->address.focused = false;
                SDL_StopTextInput();
            }
        }

        /* content area: forward to ewebview */
        SDL_Rect cr = content_rect(b);
        if(b->view && point_in_rect(mx, my, &cr)) {
            eweb_event_t wev;
            memset(&wev, 0, sizeof(wev));
            wev.mouse_state = EWEB_MOUSE_DOWN;
            wev.button = (ev->button.button == SDL_BUTTON_LEFT)  ? EWEB_BUTTON_LEFT :
                         (ev->button.button == SDL_BUTTON_MIDDLE)? EWEB_BUTTON_MIDDLE :
                         (ev->button.button == SDL_BUTTON_RIGHT) ? EWEB_BUTTON_RIGHT :
                                                                   EWEB_BUTTON_NONE;
            wev.cx = dev_to_logical(b, mx - cr.x);
            wev.cy = dev_to_logical(b, my - cr.y);
            ewebview_post_event(b->view, &wev);
        }
        break;
    }

    case SDL_MOUSEBUTTONUP: {
        int mx, my; browser_mouse_pos(b, &mx, &my);

        /* button click (release inside) */
        ui_button_t* btn = hit_button(b, mx, my);
        if(btn) btn->down = false;
        if(btn && ev->button.button == SDL_BUTTON_LEFT) {
            if(btn == &b->btn_back)         browser_back(b);
            else if(btn == &b->btn_stop)    browser_stop(b);
            else if(btn == &b->btn_refresh) browser_refresh(b);
            break;
        }

        /* content area: forward UP + CLICK to ewebview */
        SDL_Rect cr = content_rect(b);
        if(b->view && point_in_rect(mx, my, &cr)) {
            eweb_event_t wev;
            memset(&wev, 0, sizeof(wev));
            int btn2 = (ev->button.button == SDL_BUTTON_LEFT)  ? EWEB_BUTTON_LEFT :
                       (ev->button.button == SDL_BUTTON_MIDDLE)? EWEB_BUTTON_MIDDLE :
                       (ev->button.button == SDL_BUTTON_RIGHT) ? EWEB_BUTTON_RIGHT :
                                                                 EWEB_BUTTON_NONE;
            wev.cx = dev_to_logical(b, mx - cr.x);
            wev.cy = dev_to_logical(b, my - cr.y);
            wev.button = btn2;

            wev.mouse_state = EWEB_MOUSE_UP;
            ewebview_post_event(b->view, &wev);

            wev.mouse_state = EWEB_MOUSE_CLICK;
            ewebview_post_event(b->view, &wev);
        }
        break;
    }

    case SDL_MOUSEWHEEL: {
        /* wheel over the content area scrolls the page; the step is in LOGICAL
         * CSS px because the engine's scroll offsets are logical. */
        int mx, my; browser_mouse_pos(b, &mx, &my);
        SDL_Rect cr = content_rect(b);
        if(point_in_rect(mx, my, &cr)) {
            int step = SCROLL_STEP;
            int dy = -ev->wheel.y * step;
            int dx = -ev->wheel.x * step;
            browser_ui_scroll(b, dx, dy);
        }
        break;
    }

    case SDL_TEXTINPUT:
        if(b->address.focused)
            edit_insert_text(&b->address, ev->text.text);
        break;

    case SDL_KEYDOWN: {
        SDL_Keycode k = ev->key.keysym.sym;
        Uint16 mod = ev->key.keysym.mod;

        /* address bar editing */
        if(b->address.focused) {
            if(k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                b->address.focused = false;
                SDL_StopTextInput();
                browser_load(b, b->address.text);
                break;
            }
            if(k == SDLK_ESCAPE) {
                b->address.focused = false;
                SDL_StopTextInput();
                /* restore the current URL */
                snprintf(b->address.text, sizeof(b->address.text), "%s", b->current_url);
                b->address.cursor = (int)strlen(b->address.text);
                break;
            }
            if(k == SDLK_BACKSPACE) { edit_backspace(&b->address); break; }
            if(k == SDLK_DELETE)    { edit_delete(&b->address); break; }
            if(k == SDLK_LEFT)      { edit_move_cursor(&b->address, -1); break; }
            if(k == SDLK_RIGHT)     { edit_move_cursor(&b->address, +1); break; }
            if(k == SDLK_HOME)      { b->address.cursor = 0; break; }
            if(k == SDLK_END)       { b->address.cursor = (int)strlen(b->address.text); break; }
            if(k == SDLK_a && (mod & KMOD_CTRL)) { edit_select_all(&b->address); break; }
            /* let other keys fall through to shortcuts */
        }

        /* global shortcuts */
        if(k == SDLK_F5) { browser_refresh(b); break; }
        if(k == SDLK_ESCAPE) { browser_stop(b); break; }
        if(k == SDLK_l && (mod & KMOD_CTRL)) {
            b->address.focused = true;
            b->address.select_all = true;
            SDL_StartTextInput();
            break;
        }
        if(k == SDLK_LEFT && (mod & KMOD_ALT)) { browser_back(b); break; }
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Resize                                                              */
/* ------------------------------------------------------------------ */

/* Refresh the HiDPI scale and every metric derived from it. Under
 * SDL_WINDOW_ALLOW_HIGHDPI the renderer works in native DEVICE PIXELS (2x on a
 * Retina panel) while SDL_GetWindowSize still reports LOGICAL POINTS; ui_scale
 * bridges the two. The chrome metrics and the UI font are scaled by ui_scale so
 * they render crisp at their intended physical size, and win_w/win_h hold the
 * renderer's pixel size so the content canvas below is sized in real pixels. */
static void browser_apply_scale(browser_t* b) {
    int pts_w = 0, pts_h = 0, out_w = 0, out_h = 0;
    if(b->window) SDL_GetWindowSize(b->window, &pts_w, &pts_h);
    if(b->renderer && SDL_GetRendererOutputSize(b->renderer, &out_w, &out_h) == 0
       && out_w > 0 && out_h > 0) {
        b->win_w = out_w;
        b->win_h = out_h;
    } else {
        b->win_w = pts_w;
        b->win_h = pts_h;
    }

    float old_scale = b->ui_scale;
    b->ui_scale = (pts_w > 0 && b->win_w > 0) ? ((float)b->win_w / (float)pts_w) : 1.0f;
    if(b->ui_scale < 0.5f) b->ui_scale = 1.0f;   /* guard: unknown -> treat as 1x */
    /* The SDL2 port rasterises the page at this ratio: logical CSS-px layout at
     * native device-px resolution (crisp text at normal physical size). */
    eweb_port_sdl2_set_dpr(b->ui_scale);
    if(old_scale > 0.0f && fabsf(old_scale - b->ui_scale) > 0.001f && b->view)
        b->pool_flush = true;   /* display change: pooled frames carry the old dpr */

    b->toolbar_h   = (int)(TOOLBAR_H   * b->ui_scale + 0.5f);
    b->statusbar_h = (int)(STATUSBAR_H * b->ui_scale + 0.5f);
    b->button_w    = (int)(BUTTON_W    * b->ui_scale + 0.5f);
    b->pad         = (int)(PAD         * b->ui_scale + 0.5f);
    if(b->toolbar_h   < 1) b->toolbar_h   = 1;
    if(b->statusbar_h < 1) b->statusbar_h = 1;
    if(b->button_w    < 1) b->button_w    = 1;

    /* (Re)open the UI font at the scaled pixel size so chrome text is crisp.
     * Also covers a scale change when the window moves between displays. */
    int want_px = (int)(UI_FONT_PX * b->ui_scale + 0.5f);
    if(want_px < 1) want_px = 1;
    if(b->font_path[0] && (b->ui_font == NULL || want_px != b->font_px)) {
        if(b->ui_font) { TTF_CloseFont(b->ui_font); b->ui_font = NULL; }
        b->ui_font = TTF_OpenFont(b->font_path, want_px);
        if(b->ui_font) b->font_px = want_px;
    }
}

/* Recompute every derived rectangle from the renderer's true (device-pixel)
 * output size and resize the ewebview viewport to match (in logical CSS px; the
 * port rasterises it at device resolution), so the engine re-lays-out and
 * re-renders the document at exactly that size (window.onresize fires there
 * too). The frame is then blit 1:1 - the picture is never stretched or deformed,
 * it is genuinely re-rendered to fit. Also recreates the frame texture at the
 * new device-pixel size. fb_w/fb_h are the fallback if the query fails. */
static void browser_set_size(browser_t* b, int fb_w, int fb_h) {
    browser_apply_scale(b);
    if(b->win_w <= 0) b->win_w = fb_w;
    if(b->win_h <= 0) b->win_h = fb_h;
    if(b->win_w < MIN_W) b->win_w = MIN_W;
    if(b->win_h < MIN_H) b->win_h = MIN_H;

    layout_toolbar(b);

    /* Content canvas: the engine lays out in LOGICAL CSS px; the SDL2 port
     * rasterises that layout at view * dpr device pixels, so the frame texture
     * (device px) still blits 1:1 into the drawable - crisp, never stretched. */
    SDL_Rect cr = content_rect(b);
    int vw = (int)ceilf((float)cr.w / b->ui_scale);
    int vh = (int)ceilf((float)cr.h / b->ui_scale);
    if(vw < 1) vw = 1;
    if(vh < 1) vh = 1;
    b->view_w = vw;
    b->view_h = vh;
    b->tex_w = (int)lroundf((float)vw * b->ui_scale);
    b->tex_h = (int)lroundf((float)vh * b->ui_scale);
    if(b->view) {
        if(b->pool_flush) {
            /* dpr changed (window moved between displays): pooled frames still
             * carry the old device resolution while their logical dims match the
             * new viewport, so the pool would keep them. A one-off +1 resize
             * forces the engine to drop and reallocate them at the new dpr. */
            ewebview_set_viewport(b->view, vw + 1, vh + 1);
            b->pool_flush = false;
        }
        ewebview_set_viewport(b->view, vw, vh);
    }

    /* recreate the frame texture at the new canvas size */
    if(b->frame_tex) { SDL_DestroyTexture(b->frame_tex); b->frame_tex = NULL; }
    ensure_frame_texture(b);
    b->content_dirty = true;
}

static void browser_resize(browser_t* b, int w, int h) {
    (void)w; (void)h;   /* authoritative size comes from the renderer output */
    browser_set_size(b, w, h);
}

/* ------------------------------------------------------------------ */
/* Init / teardown                                                     */
/* ------------------------------------------------------------------ */

static bool browser_init(browser_t* b, int argc, char** argv) {
    memset(b, 0, sizeof(*b));
    b->running = true;

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    if(TTF_Init() < 0) {
        SDL_Log("TTF_Init failed: %s", TTF_GetError());
        return false;
    }

    /* Initial geometry override: EWEB_WINDOW=WxH (logical points). Lets headless
     * shots reproduce a specific viewport, e.g. a wide desktop layout. */
    int init_w = DEFAULT_W, init_h = DEFAULT_H;
    {
        const char* ws = SDL_getenv("EWEB_WINDOW");
        if(ws && ws[0]) {
            int w = 0, h = 0;
            if(sscanf(ws, "%dx%d", &w, &h) == 2 && w >= MIN_W && h >= MIN_H) {
                init_w = w;
                init_h = h;
            }
        }
    }

    /* Request a native-resolution drawable with SDL_WINDOW_ALLOW_HIGHDPI so the
     * renderer works in real device pixels. The content canvas (ewebview
     * viewport) is then sized in those device pixels and its frame is blit 1:1 -
     * crisp, never stretched or deformed - and a resize genuinely re-renders the
     * page at the new pixel size. SDL_GetWindowSize reports logical points while
     * the renderer output is device pixels; browser_apply_scale() derives
     * ui_scale from the two and scales the chrome + font to match. */
    b->window = SDL_CreateWindow("sdlbrowser",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        init_w, init_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if(!b->window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    b->renderer = SDL_CreateRenderer(b->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if(!b->renderer) {
        b->renderer = SDL_CreateRenderer(b->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if(!b->renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(b->renderer, SDL_BLENDMODE_BLEND);

    /* UI font path: reuse the same resolution logic as the SDL2 port. The font
     * itself is opened by browser_apply_scale() at the HiDPI-scaled pixel size. */
    const char* font_path = SDL_getenv("EWEBVIEW_SDL2_FONT");
    if(!font_path || !font_path[0]) {
        /* CJK-first, same ordering as the SDL2 port: the default face must
         * render Chinese/Japanese/Korean glyphs from UTF-8 pages. */
        static const char* const paths[] = {
            "/System/Library/Fonts/PingFang.ttc",
            "/System/Library/Fonts/Hiragino Sans GB.ttc",
            "/System/Library/Fonts/STHeiti Medium.ttc",
            "/System/Library/Fonts/STHeiti Light.ttc",
            "/System/Library/Fonts/Supplemental/Songti.ttc",
            "/Library/Fonts/Arial Unicode.ttf",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "C:\\Windows\\Fonts\\msyh.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
            NULL
        };
        for(int i = 0; paths[i]; i++) {
            SDL_RWops* rw = SDL_RWFromFile(paths[i], "rb");
            if(rw) { SDL_RWclose(rw); font_path = paths[i]; break; }
        }
    }
    if(font_path)
        snprintf(b->font_path, sizeof(b->font_path), "%s", font_path);
    if(!b->ui_font && !b->font_path[0]) {
        SDL_Log("warning: no UI font found");
    }

    /* button labels */
    snprintf(b->btn_back.label,    sizeof(b->btn_back.label),    "<");
    snprintf(b->btn_stop.label,    sizeof(b->btn_stop.label),    "X");
    snprintf(b->btn_refresh.label, sizeof(b->btn_refresh.label), "R");

    /* initial geometry. b->view is still NULL here, so browser_set_size only
     * lays out the chrome and sizes the frame texture; the viewport is pushed to
     * the engine right after it is created below. */
    browser_set_size(b, init_w, init_h);

    /* create the ewebview engine with the SDL2 port */
    eweb_port_sdl2(&b->port, NULL);
    b->view = ewebview_create(&b->port);
    if(!b->view) {
        SDL_Log("ewebview_create failed");
        return false;
    }

    eweb_listener_t lis;
    eweb_listener_init(&lis);
    lis.ud              = b;
    lis.on_frame        = cb_frame;
    lis.on_scroll       = cb_scroll;
    lis.on_url          = cb_url;
    lis.on_status       = cb_status;
    lis.on_build_status = cb_build_status;
    lis.on_dialog       = cb_dialog;
    lis.on_task_start   = cb_task_start;
    lis.on_task_end     = cb_task_end;
    lis.on_task_failed  = cb_task_failed;
    lis.on_tasks_end    = cb_tasks_end;
    ewebview_set_listener(b->view, &lis);

    ewebview_set_viewport(b->view, b->view_w, b->view_h);

    /* default CSS: res:// resolves to <program-dir>/res via the SDL2 port, so
     * the shell no longer depends on the current working directory. */
    ewebview_set_default_css(b->view, "res://html/default.css");

    /* initial URL: first positional argument (flags like --shot are skipped) */
    const char* url = "res://html/default.html";
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--shot") == 0) { i++; continue; }
        if(strcmp(argv[i], "--scroll") == 0) { i++; continue; }
        url = argv[i];
        break;
    }
    char initial[ADDRESS_MAX + 1];
    normalize_url(url, initial, sizeof(initial));
    snprintf(b->address.text, sizeof(b->address.text), "%s", initial);
    b->address.cursor = (int)strlen(b->address.text);

    /* --shot <out.bmp> [settle_ms] */
    b->shot_path = NULL;
    b->shot_settle_ms = 8000;
    b->shot_start_ms = 0;
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            b->shot_path = argv[i + 1];
            if(i + 2 < argc) b->shot_settle_ms = (uint32_t)atoi(argv[i + 2]);
            break;
        }
    }

    /* --scroll <y>: jump the viewport to a document offset before capturing,
     * so below-the-fold regions (e.g. the w3.org member grid) can be shot. */
    b->shot_scroll_y = -1;
    b->shot_scrolled = false;
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--scroll") == 0 && i + 1 < argc) { b->shot_scroll_y = atoi(argv[i + 1]); break; }
    }

    /* nojs flag */
    for(int i = 1; i < argc; i++)
        if(strcmp(argv[i], "nojs") == 0) { ewebview_set_js_enabled(b->view, false); break; }

    ewebview_load(b->view, initial);
    b->shot_start_ms = SDL_GetTicks();

    return true;
}

static void browser_destroy(browser_t* b) {
    if(b->view) {
        if(b->frame_prev) {
            ewebview_release_frame(b->view, b->frame_prev);
            b->frame_prev = NULL;
        }
        if(b->frame) {
            ewebview_release_frame(b->view, b->frame);
            b->frame = NULL;
        }
        ewebview_destroy(b->view);
        b->view = NULL;
    }
    if(b->frame_tex) { SDL_DestroyTexture(b->frame_tex); b->frame_tex = NULL; }
    if(b->ui_font)   { TTF_CloseFont(b->ui_font); b->ui_font = NULL; }
    if(b->renderer)  { SDL_DestroyRenderer(b->renderer); b->renderer = NULL; }
    if(b->window)    { SDL_DestroyWindow(b->window); b->window = NULL; }
    TTF_Quit();
    SDL_Quit();
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv) {
    browser_t browser;
    browser_detect_color_scheme();
    if(!browser_init(&browser, argc, argv)) {
        browser_destroy(&browser);
        return 1;
    }

    browser_t* b = &browser;
    const uint32_t TICK_MS = 16;   /* ~60 Hz UI pump */

    while(b->running) {
        uint32_t t0 = SDL_GetTicks();

        /* drain SDL events */
        SDL_Event ev;
        while(SDL_PollEvent(&ev))
            browser_handle_event(b, &ev);

        /* pump the engine (fires listener callbacks on this thread) */
        if(b->view)
            ewebview_tick(b->view);

        /* render */
        browser_render(b);

        /* headless screenshot: once the page has settled, save one frame and exit */
        if(b->shot_path && (SDL_GetTicks() - b->shot_start_ms) >= b->shot_settle_ms) {
            if(b->shot_scroll_y >= 0 && !b->shot_scrolled) {
                ewebview_scroll(b->view, 0, b->shot_scroll_y);
                b->shot_scrolled = true;
                b->shot_start_ms = SDL_GetTicks();
                b->shot_settle_ms = 1000;   /* let the new offset render a frame */
            } else {
                SDL_Surface* surf = NULL;
                if(b->frame && b->port.gfx.surface_native)
                    surf = (SDL_Surface*)b->port.gfx.surface_native(b->port.gfx.ud, b->frame);
                if(surf) SDL_SaveBMP(surf, b->shot_path);
                b->running = false;
            }
        }

        /* frame pacing */
        uint32_t elapsed = SDL_GetTicks() - t0;
        if(elapsed < TICK_MS)
            SDL_Delay(TICK_MS - elapsed);
    }

    browser_destroy(b);
    return 0;
}
