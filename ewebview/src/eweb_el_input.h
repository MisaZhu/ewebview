// Replaced form-control elements for litehtml, drawn through the porting HAL.
//
// Ported from widget++'s el_input: the xwin/graph drawing is replaced by the
// eweb_gfx table, so the element knows nothing about the platform. The hdc
// litehtml hands to draw() is an eweb_surface_t* the engine is rendering into.
//
// Covers the whole form-control family the container factory maps here:
// text-like <input>/<textarea>, <input type=button|submit|reset>/<button>,
// <input type=checkbox|radio>, <select> and <input type=hidden>. Each draws a
// recognisable widget (box, border, label text, arrow / tick / dot) instead of
// the old featureless solid rectangle.

#pragma once

#include <litehtml.h>
#include <ewebview_port.h>

#include <map>
#include <string>

enum EWebInputType {
    EWEB_INPUT_TEXT,      /* <input type=text|password|search|...>            */
    EWEB_INPUT_TEXTAREA,  /* <textarea>                                       */
    EWEB_INPUT_BUTTON,    /* <input type=button|submit|reset>, <button>       */
    EWEB_INPUT_CHECKBOX,  /* <input type=checkbox>                            */
    EWEB_INPUT_RADIO,     /* <input type=radio>                               */
    EWEB_INPUT_SELECT,    /* <select>                                         */
    EWEB_INPUT_RANGE,     /* <input type=range>                               */
    EWEB_INPUT_HIDDEN     /* <input type=hidden>                              */
};

class eweb_el_input : public litehtml::html_tag
{
public:
    eweb_el_input(
        litehtml::document* doc,
        const eweb_port_t* port,
        EWebInputType inputType);
    virtual ~eweb_el_input(void);

    virtual int      line_height() const override;
    virtual bool     is_replaced() const override;
    virtual void     get_content_size(litehtml::size& sz, int max_width) override;
    virtual litehtml::style_display    get_display() const override;
    virtual litehtml::element_position get_element_position(litehtml::css_offsets* offsets = 0) const override;
    virtual int      render(int x, int y, int max_width, bool second_pass = false) override;
    virtual void     draw(litehtml::uint_ptr hdc, int x, int y, const litehtml::position* clip) override;
    virtual void     draw_stacking_context(litehtml::uint_ptr hdc, int x, int y, const litehtml::position* clip, bool with_positioned) override;
    virtual void     parse_styles(bool is_reparse) override;
    virtual void     add_widget_part_style(const litehtml::tstring& part, const litehtml::style& st) override;
    virtual void     on_click();

private:
    const eweb_port_t* m_port;
    EWebInputType m_inputType;

    /* Raw declaration blocks matched by widget part pseudo-element rules
     * (input[type=range]::-webkit-slider-thumb {...}), keyed by the lowercased
     * part name. parse_styles turns each into a childless html_tag so the
     * values resolve (var(), inherited custom properties) exactly like the
     * ::before/::after elements do, and draw() reads the computed look. */
    std::map<std::string, litehtml::style> m_widget_styles;
    litehtml::element::ptr m_part_thumb;
    litehtml::element::ptr m_part_track;

    static uint32_t make_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

    void resolve_widget_parts();
    const litehtml::element::ptr& widget_part(bool thumb) const;
    bool part_color(bool thumb, litehtml::web_color& c) const;
    int  part_dim(bool thumb, const char* prop, int defval) const;
    float range_fraction();
    void extra_box_size(int& w, int& h) const;

    /* The user-visible label: value/placeholder attribute for <input>, the
     * selected <option> text for <select>, the element text otherwise. */
    std::string label(bool* is_placeholder = 0);
    int label_width(const std::string& text);
    void draw_text_box(eweb_surface_t* s, const litehtml::position& box,
                       const std::string& text, uint32_t color, bool center);
    /* Paint the page-authored box of a styled control: CSS background via
     * init_background_paint plus the border box (width/colour/radius) the
     * html_tag::draw path would have drawn - draw() is overridden here, so
     * neither comes for free. Mirrors el_image::draw. */
    void draw_page_box(litehtml::uint_ptr hdc, int x, int y, const litehtml::position* clip);
    /* True when the page (cascade) declares any box styling for this control
     * - background/border/border-radius. When set, draw() defers the box look
     * to litehtml::draw_background (CSS colours, borders, opacity) instead of
     * painting the neutral UA white-fill/gray-outline widget chrome, so styled
     * fields (Google's transparent border:none input, Material buttons) don't
     * get a spurious extra rectangle. */
    bool page_styled_box() const;
};
