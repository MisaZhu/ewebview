// Replaced form-control elements for litehtml, drawn through the porting HAL.
//
// Ported from widget++'s el_input.cpp with the graph_t calls mapped onto the
// eweb_gfx table: graph_set() (a raw solid box) becomes fill_rect(), and the
// 3D-bevel button border graph_round_3d() is approximated by a filled rounded
// rect plus a darker rounded outline - visually close, and the HAL stays small.
//
// Every control the container factory maps here gets a recognisable widget:
// text/textarea a white bordered box with its value or placeholder, buttons a
// rounded face carrying the CSS background and a centred label, checkbox/radio
// a tick or dot, and select a bordered box with the chosen option plus a
// dropdown arrow. Labels come from the value/placeholder attribute, the
// selected <option>, or the element text, so controls are never blank boxes.

#include "eweb_el_input.h"

#include <cstdlib>
#include <cstring>

eweb_el_input::eweb_el_input(
    litehtml::document* doc,
    const eweb_port_t* port,
    EWebInputType inputType) :
    litehtml::html_tag(doc),
    m_port(port),
    m_inputType(inputType)
{
    m_display = litehtml::display_inline_block;
}

eweb_el_input::~eweb_el_input()
{
}

uint32_t eweb_el_input::make_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

std::string eweb_el_input::label(bool* is_placeholder)
{
    if (is_placeholder) {
        *is_placeholder = false;
    }
    switch (m_inputType) {
    case EWEB_INPUT_TEXT:
    case EWEB_INPUT_TEXTAREA: {
        const litehtml::tchar_t* v = get_attr(_t("value"));
        if (v && v[0]) {
            return std::string(v);
        }
        const litehtml::tchar_t* p = get_attr(_t("placeholder"));
        if (p && p[0]) {
            if (is_placeholder) {
                *is_placeholder = true;
            }
            return std::string(p);
        }
        if (m_inputType == EWEB_INPUT_TEXTAREA) {
            litehtml::tstring t;
            get_text(t);
            return std::string(t.c_str());
        }
        return std::string();
    }
    case EWEB_INPUT_BUTTON: {
        const litehtml::tchar_t* v = get_attr(_t("value"));
        if (v && v[0]) {
            return std::string(v);
        }
        litehtml::tstring t;
        get_text(t);
        return std::string(t.c_str());
    }
    case EWEB_INPUT_SELECT: {
        /* The selected <option>, falling back to the first one. */
        litehtml::element::ptr first;
        for (int i = 0; i < get_children_count(); i++) {
            litehtml::element::ptr ch = get_child(i);
            if (!ch || !ch->get_tagName() || t_strcasecmp(ch->get_tagName(), _t("option"))) {
                continue;
            }
            if (!first) {
                first = ch;
            }
            if (ch->get_attr(_t("selected"))) {
                litehtml::tstring t;
                ch->get_text(t);
                return std::string(t.c_str());
            }
        }
        if (first) {
            litehtml::tstring t;
            first->get_text(t);
            return std::string(t.c_str());
        }
        return std::string();
    }
    default:
        return std::string();
    }
}

int eweb_el_input::label_width(const std::string& text)
{
    if (text.empty()) {
        return 0;
    }
    litehtml::document* doc = get_document();
    litehtml::uint_ptr f = get_font();
    if (!doc || !doc->container() || !f) {
        return 0;
    }
    return doc->container()->text_width(text.c_str(), f);
}

void eweb_el_input::draw_text_box(eweb_surface_t* s, const litehtml::position& box,
                                  const std::string& text, uint32_t color, bool center)
{
    if (text.empty() || !s || !m_port) {
        return;
    }
    litehtml::document* doc = get_document();
    if (!doc || !doc->container()) {
        return;
    }
    litehtml::font_metrics fm;
    litehtml::uint_ptr f = get_font(&fm);
    if (!f) {
        return;
    }
    int tw = doc->container()->text_width(text.c_str(), f);
    int x = center ? box.x + (box.width - tw) / 2 : box.x + 4;
    if (x < box.x) {
        x = box.x;
    }
    /* The HAL anchors text at its top-left; centre the font box vertically. */
    int y = box.y + (box.height - fm.height) / 2;
    if (y < box.y) {
        y = box.y;
    }

    const eweb_gfx_t* gfx = &m_port->gfx;
    bool clipped = false;
    if (gfx->surface_set_clip) {
        gfx->surface_set_clip(gfx->ud, s, box.x, box.y, box.width, box.height);
        clipped = true;
    }
    litehtml::web_color wc((litehtml::byte)((color >> 16) & 0xFF),
                           (litehtml::byte)((color >> 8) & 0xFF),
                           (litehtml::byte)(color & 0xFF),
                           (litehtml::byte)((color >> 24) & 0xFF));
    litehtml::position tp;
    tp.x = x;
    tp.y = y;
    tp.width = tw;
    tp.height = fm.height;
    doc->container()->draw_text((litehtml::uint_ptr)s, text.c_str(), f, wc, tp);
    if (clipped && gfx->surface_unset_clip) {
        gfx->surface_unset_clip(gfx->ud, s);
    }
}

void eweb_el_input::get_content_size(litehtml::size& sz, int max_width)
{
    switch (m_inputType) {
    case EWEB_INPUT_TEXT:
        sz.width = 100;
        sz.height = 24;
        break;
    case EWEB_INPUT_TEXTAREA:
        sz.width = 160;
        sz.height = 48;
        break;
    case EWEB_INPUT_BUTTON:
        sz.width = label_width(label()) + 24;
        sz.height = 24;
        if (sz.width < 24) {
            sz.width = 24;
        }
        break;
    case EWEB_INPUT_CHECKBOX:
    case EWEB_INPUT_RADIO:
        sz.width = 13;
        sz.height = 13;
        break;
    case EWEB_INPUT_SELECT:
        sz.width = label_width(label()) + 30;
        sz.height = 24;
        if (sz.width < 40) {
            sz.width = 40;
        }
        break;
    case EWEB_INPUT_RANGE:
        /* UA intrinsic 129x20; the page normally sizes the slider itself
         * (input[type=range]{width:100%;height:22px}). */
        sz.width = 129;
        sz.height = 20;
        break;
    default:
        sz.width = 0;
        sz.height = 0;
        break;
    }
    /* The intrinsic size is the BORDER box (render() assigns it straight to
     * m_pos): without the page padding/borders every control came out at the
     * bare UA size - .ghost-btn{padding:6px 14px} buttons rendered 12px too
     * short, the 8px-padded field rule could not reach its ~40px height. */
    extra_box_size(sz.width, sz.height);
}

void eweb_el_input::extra_box_size(int& w, int& h) const
{
    switch (m_inputType) {
    case EWEB_INPUT_TEXT:
    case EWEB_INPUT_TEXTAREA:
    case EWEB_INPUT_BUTTON:
    case EWEB_INPUT_SELECT:
    case EWEB_INPUT_RANGE:
        break;
    default:
        /* checkbox/radio/hidden keep their fixed UA chrome */
        return;
    }
    w += m_padding.left + m_padding.right + m_borders.left + m_borders.right;
    h += m_padding.top + m_padding.bottom + m_borders.top + m_borders.bottom;
}

litehtml::style_display eweb_el_input::get_display() const
{
    if (m_inputType == EWEB_INPUT_HIDDEN) {
        return litehtml::display_none;
    }
    /* Honour a CSS display:none resolved by parse_styles (m_display), e.g.
     * Google's hidden password twin .Hvu6D{display:none}. The old override
     * hard-coded inline-block for every non-hidden control, so a field the
     * page explicitly hid still painted as an empty box. */
    if (m_display == litehtml::display_none) {
        return litehtml::display_none;
    }
    return litehtml::display_inline_block;
}

bool eweb_el_input::page_styled_box() const
{
    /* Any box-level declaration in the flattened cascade means the page owns
     * this control's chrome; defer to litehtml::draw_background rather than
     * stamping the neutral UA widget box on top. Shorthands (border, background)
     * are stored under their own names in m_style, so checking each covers both
     * the shorthand and longhand forms. */
    static const litehtml::tchar_t* const kProps[] = {
        _t("background"), _t("background-color"), _t("background-image"),
        _t("border"), _t("border-top"), _t("border-right"),
        _t("border-bottom"), _t("border-left"), _t("border-style"),
        _t("border-width"), _t("border-color"), _t("border-radius")
    };
    for (size_t i = 0; i < sizeof(kProps) / sizeof(kProps[0]); i++) {
        const litehtml::tchar_t* v = get_style_property_own(kProps[i]);
        if (v && v[0]) {
            return true;
        }
    }
    return false;
}

litehtml::element_position eweb_el_input::get_element_position(litehtml::css_offsets* offsets) const
{
    return litehtml::element_position_relative;
}

void eweb_el_input::add_widget_part_style(const litehtml::tstring& part, const litehtml::style& st)
{
    /* Only replaced controls with painted parts (the range slider) keep these;
     * accumulating on every control would grow the map with rules like
     * input::placeholder that draw() never reads. */
    if (m_inputType != EWEB_INPUT_RANGE) {
        return;
    }
    m_widget_styles[std::string(part.c_str())] = st;
}

void eweb_el_input::resolve_widget_parts()
{
    if (m_inputType != EWEB_INPUT_RANGE || m_widget_styles.empty()) {
        return;
    }
    static const char* const kThumb[] = { "-webkit-slider-thumb", "-moz-range-thumb" };
    static const char* const kTrack[] = { "-webkit-slider-runnable-track", "-moz-range-track" };
    for (int which = 0; which < 2; which++) {
        litehtml::element::ptr* slot = which ? &m_part_track : &m_part_thumb;
        const char* const* names = which ? kTrack : kThumb;
        if (*slot) {
            continue;  /* built on an earlier parse; styles accumulate below */
        }
        for (int i = 0; i < 2; i++) {
            auto it = m_widget_styles.find(names[i]);
            if (it == m_widget_styles.end()) {
                continue;
            }
            /* Childless html_tag, parented to this control so custom-property
             * inheritance and var() resolution follow the real tree - the same
             * trick the ::before/::after elements use. Never appended to
             * m_children, so layout/draw never see it. */
            litehtml::element::ptr el = new litehtml::html_tag(get_document());
            el->parent(this);
            el->add_style(it->second);
            el->parse_styles(false);
            *slot = el;
            break;
        }
    }
}

const litehtml::element::ptr& eweb_el_input::widget_part(bool thumb) const
{
    return thumb ? m_part_thumb : m_part_track;
}

bool eweb_el_input::part_color(bool thumb, litehtml::web_color& c) const
{
    const litehtml::element::ptr& p = widget_part(thumb);
    if (!p) {
        return false;
    }
    const litehtml::tchar_t* v = p->get_style_property(_t("background-color"), false);
    if (!v || !v[0] || !litehtml::web_color::is_color(v)) {
        return false;
    }
    c = litehtml::web_color::from_string(v, get_document()->container());
    return c.alpha != 0;
}

int eweb_el_input::part_dim(bool thumb, const char* prop, int defval) const
{
    const litehtml::element::ptr& p = widget_part(thumb);
    if (!p) {
        return defval;
    }
    const litehtml::tchar_t* v = p->get_style_property(prop, false);
    if (!v || !v[0]) {
        return defval;
    }
    char* end = 0;
    double d = strtod(v, &end);
    if (end == v || d <= 0) {
        return defval;
    }
    if (end && !strncmp(end, "px", 2)) {
        return (int)(d + 0.5);
    }
    return defval;
}

float eweb_el_input::range_fraction()
{
    const litehtml::tchar_t* v = get_attr(_t("value"));
    const litehtml::tchar_t* mn = get_attr(_t("min"));
    const litehtml::tchar_t* mx = get_attr(_t("max"));
    double dv = v ? atof(v) : 0.0;
    double dmin = mn ? atof(mn) : 0.0;
    double dmax = mx ? atof(mx) : 100.0;
    if (dmax <= dmin) {
        return 0.0f;
    }
    double f = (dv - dmin) / (dmax - dmin);
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return (float)f;
}

void eweb_el_input::draw_page_box(litehtml::uint_ptr hdc, int x, int y, const litehtml::position* clip)
{
    litehtml::document* doc = get_document();
    if (!doc || !doc->container()) {
        return;
    }
    litehtml::position pos = m_pos;
    pos.x += x;
    pos.y += y;
    if (clip && !pos.does_intersect(clip)) {
        return;
    }
    /* Mirrors el_image::draw, but m_pos here is already the BORDER box (the
     * intrinsic size folds in padding+borders), so init_background_paint -
     * which treats its argument as the CONTENT box and re-adds them - gets the
     * shrunk content rect, and the border box is m_pos verbatim. Adding them
     * again painted every styled control a padding+border slab taller than its
     * layout box, which bled into the next flex-wrap line. */
    int bx = m_padding.left + m_borders.left + m_padding.right + m_borders.right;
    int by = m_padding.top + m_borders.top + m_padding.bottom + m_borders.bottom;
    const litehtml::background* bg = get_background();
    if (bg) {
        litehtml::position content = pos;
        content.x += m_padding.left + m_borders.left;
        content.y += m_padding.top + m_borders.top;
        content.width -= bx;
        content.height -= by;
        if (content.width < 0) content.width = 0;
        if (content.height < 0) content.height = 0;
        litehtml::background_paint bg_paint;
        init_background_paint(content, bg_paint, bg);
        doc->container()->draw_background(hdc, bg_paint);
    }
    litehtml::borders bdr = m_css_borders;
    bdr.radius = m_css_borders.radius.calc_percents(pos.width, pos.height);
    doc->container()->draw_borders(hdc, bdr, pos, false);
}

/* The computed 'color' of the control as HAL ARGB. */
static uint32_t widget_fg_argb(eweb_el_input* el)
{
    litehtml::web_color fgc = el->get_color(_t("color"), true, litehtml::web_color(0, 0, 0, 255));
    return ((uint32_t)(fgc.alpha ? fgc.alpha : 255) << 24) |
           ((uint32_t)fgc.red << 16) | ((uint32_t)fgc.green << 8) | fgc.blue;
}

void eweb_el_input::draw(litehtml::uint_ptr hdc, int x, int y, const litehtml::position* clip)
{
    eweb_surface_t* s = (eweb_surface_t*)hdc;
    if (!s || !m_port) {
        return;
    }
    if (m_inputType == EWEB_INPUT_HIDDEN) {
        return;
    }
    /* opacity:0 (own or from an ancestor) hides the whole widget; the box draw
     * paths in litehtml already honour this, mirror it here since draw() is
     * overridden and does not funnel through html_tag::draw(). */
    if (get_opacity_cum() <= 0.004f) {
        return;
    }
    const eweb_gfx_t* gfx = &m_port->gfx;

    litehtml::position pos = m_pos;
    pos.x += x;
    pos.y += y;
    if (clip && !pos.does_intersect(clip)) {
        return;
    }

    switch (m_inputType) {
    case EWEB_INPUT_TEXT:
    case EWEB_INPUT_TEXTAREA: {
        /* When the page styles the field (Google's .whsOnd is transparent with
         * border:none, its underline drawn by a sibling; the H3 workbench
         * fields carry border+radius+bg-input) paint exactly that CSS box -
         * background AND borders, which the overridden draw() never got from
         * html_tag::draw. Only an unstyled control gets the neutral UA white
         * fill + gray outline. */
        if (page_styled_box()) {
            draw_page_box(hdc, x, y, clip);
        } else {
            if (gfx->fill_rect) {
                gfx->fill_rect(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 0xFFFFFFFF);
            }
            if (gfx->rect) {
                gfx->rect(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 0xFF767676);
            }
        }
        /* Value/placeholder text: inside the padding box, in the computed
         * 'color' (the page sets color:var(--text)); the placeholder fades it
         * (input::placeholder{color:var(--text-faint)} is not resolvable). */
        litehtml::position tb = pos;
        int il = m_padding.left + m_borders.left;
        int it = m_padding.top + m_borders.top;
        int ir = m_padding.right + m_borders.right;
        int ib = m_padding.bottom + m_borders.bottom;
        tb.x += il;
        tb.y += it;
        tb.width -= il + ir;
        tb.height -= it + ib;
        if (tb.width < 0) tb.width = 0;
        if (tb.height < 0) tb.height = 0;
        bool placeholder = false;
        std::string t = label(&placeholder);
        uint32_t fg = widget_fg_argb(this);
        uint32_t tcol = fg;
        if (placeholder) {
            tcol = ((((fg >> 16) & 0xFF) / 2 + 64) << 16) |
                   ((((fg >> 8) & 0xFF) / 2 + 64) << 8) |
                   (((fg & 0xFF) / 2 + 64)) | (fg & 0xFF000000);
        }
        /* number fields keep the right edge clear for the spinner arrows */
        const litehtml::tchar_t* tyattr = get_attr(_t("type"));
        bool is_number = (m_inputType == EWEB_INPUT_TEXT && tyattr &&
                          !t_strcasecmp(tyattr, _t("number")));
        if (is_number) {
            int aw = 14;
            if (tb.width > aw) {
                tb.width -= aw;
            }
        }
        draw_text_box(s, tb, t, tcol, false);
        if (is_number && gfx->fill_rect) {
            /* UA spinner: two small stacked triangles at the right edge */
            int sx = pos.x + pos.width - ir - 9;
            int cy = pos.y + pos.height / 2;
            for (int i = 0; i < 4; i++) {
                gfx->fill_rect(gfx->ud, s, sx + i, cy - 6 + i, 7 - 2 * i, 1, fg);
            }
            for (int i = 0; i < 4; i++) {
                gfx->fill_rect(gfx->ud, s, sx + i, cy + 5 - i, 7 - 2 * i, 1, fg);
            }
        }
        break;
    }
    case EWEB_INPUT_BUTTON: {
        /* Always paint the element's own CSS background/borders first (colour,
         * sprite, rounded Material face - now opacity-aware). Only a control
         * the page left unstyled falls back to the neutral face + gray outline;
         * otherwise that chrome doubled up over the styled button (the black
         * box on Next, the gray slab on Create account). */
        if (page_styled_box()) {
            draw_page_box(hdc, x, y, clip);
        } else {
            draw_background(hdc, x, y, clip);
            if (gfx->fill_round) {
                gfx->fill_round(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 4, 0xFFEFEFEF);
            } else if (gfx->fill_rect) {
                gfx->fill_rect(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 0xFFEFEFEF);
            }
            if (gfx->round) {
                gfx->round(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 4, 1, 0xFF8F8F8F);
            }
        }
        draw_text_box(s, pos, label(), widget_fg_argb(this), true);
        break;
    }
    case EWEB_INPUT_CHECKBOX: {
        if (gfx->fill_rect) {
            gfx->fill_rect(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 0xFFFFFFFF);
        }
        if (gfx->rect) {
            gfx->rect(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 0xFF767676);
        }
        if (get_attr(_t("checked")) && gfx->wline) {
            gfx->wline(gfx->ud, s, pos.x + 2, pos.y + pos.height / 2,
                       pos.x + pos.width / 2 - 1, pos.y + pos.height - 3, 2, 0xFF222222);
            gfx->wline(gfx->ud, s, pos.x + pos.width / 2 - 1, pos.y + pos.height - 3,
                       pos.x + pos.width - 3, pos.y + 2, 2, 0xFF222222);
        }
        break;
    }
    case EWEB_INPUT_RADIO: {
        int cx = pos.x + pos.width / 2;
        int cy = pos.y + pos.height / 2;
        int r = pos.width / 2;
        if (gfx->fill_circle) {
            gfx->fill_circle(gfx->ud, s, cx, cy, r, 0xFFFFFFFF);
        }
        if (gfx->circle) {
            gfx->circle(gfx->ud, s, cx, cy, r, 1, 0xFF767676);
        }
        if (get_attr(_t("checked")) && gfx->fill_circle) {
            gfx->fill_circle(gfx->ud, s, cx, cy, r > 2 ? r / 2 : 1, 0xFF222222);
        }
        break;
    }
    case EWEB_INPUT_SELECT: {
        uint32_t fg = widget_fg_argb(this);
        if (page_styled_box()) {
            /* The page dresses the select (border/radius/bg-input like the
             * other fields) - no white UA box on top. */
            draw_page_box(hdc, x, y, clip);
        } else {
            if (gfx->fill_rect) {
                gfx->fill_rect(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 0xFFFFFFFF);
            }
            if (gfx->rect) {
                gfx->rect(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 0xFF767676);
            }
        }
        /* dropdown arrow: a small filled down-triangle at the right edge, in
         * the control's own foreground colour */
        if (gfx->fill_rect) {
            int ax = pos.x + pos.width - (m_padding.right + m_borders.right) - 12;
            int ay = pos.y + pos.height / 2 - 2;
            for (int i = 0; i < 4; i++) {
                gfx->fill_rect(gfx->ud, s, ax + i, ay + i, 8 - 2 * i, 1, fg);
            }
        }
        litehtml::position tb = pos;
        int il = m_padding.left + m_borders.left;
        tb.x += il;
        tb.width -= il + m_padding.right + m_borders.right + 18;
        if (tb.width < 0) tb.width = 0;
        draw_text_box(s, tb, label(), fg, false);
        break;
    }
    case EWEB_INPUT_RANGE: {
        resolve_widget_parts();
        /* Track: geometry/colour from the page's part rules (height:3px,
         * background:var(--line-strong)), UA defaults otherwise. */
        int th = part_dim(false, "height", 4);
        if (th > pos.height) th = pos.height;
        litehtml::web_color tc;
        uint32_t track_col = part_color(false, tc)
            ? make_color(tc.red, tc.green, tc.blue, tc.alpha)
            : 0xFF5A5A5A;
        int ty = pos.y + (pos.height - th) / 2;
        int trad = part_dim(false, "border-radius", th / 2);
        if (gfx->fill_round && trad > 0) {
            gfx->fill_round(gfx->ud, s, pos.x, ty, pos.width, th, trad, track_col);
        } else if (gfx->fill_rect) {
            gfx->fill_rect(gfx->ud, s, pos.x, ty, pos.width, th, track_col);
        }
        /* Thumb: 14px accent dot (::-webkit-slider-thumb) at the value
         * fraction, with the traversed part of the track tinted to match -
         * the filled-progress look Chrome renders for accent-colored thumbs. */
        int tw = part_dim(true, "width", 14);
        int thh = part_dim(true, "height", tw);
        litehtml::web_color hc;
        uint32_t thumb_col = part_color(true, hc)
            ? make_color(hc.red, hc.green, hc.blue, hc.alpha)
            : 0xFF3F3F3F;
        float f = range_fraction();
        int span = pos.width - tw;
        if (span < 0) span = 0;
        int tx = pos.x + (int)(span * f + 0.5f);
        int tcy = pos.y + pos.height / 2;
        if (gfx->fill_rect && tx > pos.x) {
            gfx->fill_rect(gfx->ud, s, pos.x, ty, tx - pos.x, th, thumb_col);
        }
        if (gfx->fill_circle) {
            gfx->fill_circle(gfx->ud, s, tx + tw / 2, tcy, thh / 2, thumb_col);
        } else if (gfx->fill_rect) {
            gfx->fill_rect(gfx->ud, s, tx, tcy - thh / 2, tw, thh, thumb_col);
        }
        break;
    }
    default:
        break;
    }
}

void eweb_el_input::draw_stacking_context(litehtml::uint_ptr hdc, int x, int y, const litehtml::position* clip, bool with_positioned)
{
    /* Replaced control: draw() paints the whole widget, label included. A
     * <button>'s text child is the label SOURCE, not inline content - letting
     * html_tag recurse into it painted the label a second time (in the page
     * colour, uncentred) on top of the widget face. */
    (void) hdc;
    (void) x;
    (void) y;
    (void) clip;
    (void) with_positioned;
}

int eweb_el_input::line_height() const
{
    int h = height();
    if (h <= 0) {
        return (m_inputType == EWEB_INPUT_CHECKBOX || m_inputType == EWEB_INPUT_RADIO) ? 13 : 24;
    }
    return h;
}

bool eweb_el_input::is_replaced() const
{
    return true;
}

int eweb_el_input::render(int x, int y, int max_width, bool second_pass)
{
    using namespace litehtml;

    int parent_width = max_width;

    calc_outlines(parent_width);

    m_pos.move_to(x, y);

    document* doc = get_document();

    litehtml::size sz;
    get_content_size(sz, max_width);

    m_pos.width = sz.width;
    m_pos.height = sz.height;

    if (m_css_height.is_predefined() && m_css_width.is_predefined())
    {
        m_pos.height = sz.height;
        m_pos.width = sz.width;

        if (!m_css_max_width.is_predefined())
        {
            int max_width_val = doc->cvt_units(m_css_max_width, m_font_size, parent_width);
            if (m_pos.width > max_width_val)
            {
                m_pos.width = max_width_val;
            }
        }

        if (!m_css_max_height.is_predefined())
        {
            int max_height = doc->cvt_units(m_css_max_height, m_font_size);
            if (m_pos.height > max_height)
            {
                m_pos.height = max_height;
            }
        }
    }
    else if (!m_css_height.is_predefined() && m_css_width.is_predefined())
    {
        if (!get_predefined_height(m_pos.height))
        {
            m_pos.height = (int)m_css_height.val();
        }

        if (!m_css_max_height.is_predefined())
        {
            int max_height = doc->cvt_units(m_css_max_height, m_font_size);
            if (m_pos.height > max_height)
            {
                m_pos.height = max_height;
            }
        }

        /* Form controls have no intrinsic aspect ratio: a CSS height must not
         * rescale the width the way it does for images. */
        m_pos.width = sz.width;
    }
    else if (m_css_height.is_predefined() && !m_css_width.is_predefined())
    {
        m_pos.width = (int)m_css_width.calc_percent(parent_width);

        if (!m_css_max_width.is_predefined())
        {
            int max_width_val = doc->cvt_units(m_css_max_width, m_font_size, parent_width);
            if (m_pos.width > max_width_val)
            {
                m_pos.width = max_width_val;
            }
        }

        /* width:100% must not stretch the control vertically (the old aspect
         * math turned a 100%-wide select into a ~120px slab). */
        m_pos.height = sz.height;
    }
    else
    {
        m_pos.width = (int)m_css_width.calc_percent(parent_width);
        m_pos.height = 0;
        if (!get_predefined_height(m_pos.height))
        {
            m_pos.height = (int)m_css_height.val();
        }

        if (!m_css_max_height.is_predefined())
        {
            int max_height = doc->cvt_units(m_css_max_height, m_font_size);
            if (m_pos.height > max_height)
            {
                m_pos.height = max_height;
            }
        }

        if (!m_css_max_width.is_predefined())
        {
            int max_width_val = doc->cvt_units(m_css_max_width, m_font_size, parent_width);
            if (m_pos.width > max_width_val)
            {
                m_pos.width = max_width_val;
            }
        }
    }

    calc_auto_margins(parent_width);

    m_pos.x += content_margins_left();
    m_pos.y += content_margins_top();

    return m_pos.width + content_margins_left() + content_margins_right();
}

void eweb_el_input::parse_styles(bool is_reparse)
{
    litehtml::html_tag::parse_styles(is_reparse);
    /* Widget part blocks (::-webkit-slider-thumb/track rules) arrive during
     * apply_stylesheet, before this runs; turn them into resolved pseudo-part
     * elements now that the cascade (and the parent custom properties) are in
     * place. */
    resolve_widget_parts();
}

void eweb_el_input::on_click()
{
}
