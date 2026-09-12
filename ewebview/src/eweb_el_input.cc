// Replaced <input> element for litehtml, drawn through the porting HAL.
//
// Ported from widget++'s el_input.cpp with the graph_t calls mapped onto the
// eweb_gfx table: graph_set() (a raw solid box) becomes fill_rect(), and the
// 3D-bevel button border graph_round_3d() is approximated by a filled rounded
// rect plus a darker rounded outline - visually close, and the HAL stays small.

#include "eweb_el_input.h"

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

void eweb_el_input::get_content_size(litehtml::size& sz, int max_width)
{
    sz.width = 100;
    sz.height = 24;

    if(m_inputType == EWEB_INPUT_BUTTON) {
        sz.width = 24;
    }
}

litehtml::style_display eweb_el_input::get_display() const
{
    return litehtml::display_inline_block;
}

litehtml::element_position eweb_el_input::get_element_position(litehtml::css_offsets* offsets) const
{
    return litehtml::element_position_relative;
}

void eweb_el_input::draw(litehtml::uint_ptr hdc, int x, int y, const litehtml::position* clip)
{
    eweb_surface_t* s = (eweb_surface_t*)hdc;
    if (!s || !m_port) {
        return;
    }
    const eweb_gfx_t* gfx = &m_port->gfx;

    litehtml::position pos = m_pos;
    pos.x += x;
    pos.y += y;

    uint32_t color = make_color(100, 100, 100, 255);

    switch (m_inputType) {
    case EWEB_INPUT_TEXT:
        if(gfx->fill_rect)
            gfx->fill_rect(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, color);
        break;
    case EWEB_INPUT_BUTTON:
        /* graph_round_3d(g, ..., 6, 1, color, false) approximation: a light
         * raised face with the base-colour rounded outline on top. */
        if(gfx->fill_round)
            gfx->fill_round(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 6,
                            make_color(180, 180, 180, 255));
        if(gfx->round)
            gfx->round(gfx->ud, s, pos.x, pos.y, pos.width, pos.height, 6, 1, color);
        break;
    }
}

int eweb_el_input::line_height() const
{
    int h = height();
    if (h <= 0) {
        return 24;
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
            if (sz.width)
            {
                m_pos.height = (int)((float)m_pos.width * (float)sz.height / (float)sz.width);
            }
            else
            {
                m_pos.height = sz.height;
            }
        }

        if (!m_css_max_height.is_predefined())
        {
            int max_height = doc->cvt_units(m_css_max_height, m_font_size);
            if (m_pos.height > max_height)
            {
                m_pos.height = max_height;
            }
            if (sz.height)
            {
                m_pos.width = (int)(m_pos.height * (float)sz.width / (float)sz.height);
            }
            else
            {
                m_pos.width = sz.width;
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

        if (sz.height)
        {
            m_pos.width = (int)(m_pos.height * (float)sz.width / (float)sz.height);
        }
        else
        {
            m_pos.width = sz.width;
        }
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

        if (sz.width)
        {
            m_pos.height = (int)((float)m_pos.width * (float)sz.height / (float)sz.width);
        }
        else
        {
            m_pos.height = sz.height;
        }
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
}

void eweb_el_input::on_click()
{
}
