// Replaced <input> element for litehtml, drawn through the porting HAL.
//
// Ported from widget++'s el_input: the xwin/graph drawing is replaced by the
// eweb_gfx table, so the element knows nothing about the platform. The hdc
// litehtml hands to draw() is an eweb_surface_t* the engine is rendering into.

#pragma once

#include <litehtml.h>
#include <ewebview_port.h>

enum EWebInputType {
    EWEB_INPUT_TEXT,
    EWEB_INPUT_BUTTON
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
    virtual void     parse_styles(bool is_reparse) override;
    virtual void     on_click();

private:
    const eweb_port_t* m_port;
    EWebInputType m_inputType;

    static uint32_t make_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
};
