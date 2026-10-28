#include "html.h"
#include "el_text.h"
#include "document.h"
#include <stdint.h>
#include <cstdlib>
#include <new>

namespace litehtml {
bool style_prof_enabled();
void profile_text_parse(uint32_t transform_ms, uint32_t measure_ms, uint64_t start_ms);
}

/* UTF-8 helpers for letter-spacing: internal strings are UTF-8 (tchar_t is a
 * byte), so a glyph is a whole sequence, not a byte. Continuation bytes
 * (10xxxxxx) never start a code point. */
static int utf8_code_points(const char* s)
{
	int n = 0;
	for(; *s; s++)
	{
		if(((unsigned char)*s & 0xC0) != 0x80) n++;
	}
	return n;
}

static size_t utf8_seq_len(unsigned char b)
{
	if(b < 0x80) return 1;
	if((b & 0xE0) == 0xC0) return 2;
	if((b & 0xF0) == 0xE0) return 3;
	if((b & 0xF8) == 0xF0) return 4;
	return 1;
}

litehtml::el_text::el_text(const tchar_t* text, litehtml::document* doc) : element(doc)
{
	if(text)
	{
		m_text = text;
	}
	m_text_transform	= text_transform_none;
	m_use_transformed	= false;
	m_draw_spaces		= true;
	m_letter_spacing	= 0;
}

litehtml::el_text::~el_text()
{

}

litehtml::element::ptr litehtml::el_text::clone_node(bool deep)
{
	/* A text node has no children, so deep and shallow clones are identical.
	 * Built with malloc + placement-new exactly like litehtml_alloc and
	 * jsCreateTextNode, keeping the matching free() on teardown balanced. */
	void* mem = malloc(sizeof(el_text));
	if(!mem) return nullptr;
	return new (mem) el_text(m_text.c_str(), m_doc);
}

void litehtml::el_text::get_content_size( size& sz, int max_width )
{
	sz = m_size;
}

void litehtml::el_text::get_text( tstring& text )
{
	text += m_text;
}

const litehtml::tchar_t* litehtml::el_text::get_style_property( const tchar_t* name, bool inherited, const tchar_t* def /*= 0*/ )
{
	if(inherited)
	{
		element::ptr el_parent = parent();
		if (el_parent)
		{
			return el_parent->get_style_property(name, inherited, def);
		}
	}
	return def;
}

void litehtml::el_text::parse_styles(bool is_reparse)
{
	(void)is_reparse;
	{
		/* Chunked style-update gating: text nodes are leaves, so a node already
		 * measured in this epoch can be skipped outright, and a node reached
		 * after the slice expired waits for the next chunk. */
		document* step_doc = get_document();
		if(step_doc && step_doc->style_step_phase() == 2)
		{
			if(m_step_stamp == step_doc->style_step_epoch())
			{
				return;
			}
			if(step_doc->style_step_exhausted())
			{
				return;
			}
			step_doc->style_step_stamp(this);
		}
	}
	const bool sprof = litehtml::style_prof_fast();
	uint64_t start_ms = sprof ? sys_tic_ms(0) : 0;
	uint32_t transform_ms = 0;
	uint32_t measure_ms = 0;
	if(m_parent)
	{
		m_text_transform = m_parent->get_text_transform();
	}
	else
	{
		m_text_transform = text_transform_none;
	}
	if(m_text_transform != text_transform_none)
	{
		m_transformed_text	= m_text;
		m_use_transformed = true;
		document* doc = get_document();
		if (doc && doc->container())
		{
			uint64_t transform_start = sprof ? sys_tic_ms(0) : 0;
			doc->container()->transform_text(m_transformed_text, m_text_transform);
			if(sprof) transform_ms += (uint32_t)(sys_tic_ms(0) - transform_start);
		}
	}

	if(is_white_space())
	{
		m_transformed_text = _t(" ");
		m_use_transformed = true;
	} else
	{
		if(m_text == _t("\t"))
		{
			m_transformed_text = _t("    ");
			m_use_transformed = true;
		}
		if(m_text == _t("\n") || m_text == _t("\r"))
		{
			m_transformed_text = _t("");
			m_use_transformed = true;
		}
	}

	font_metrics fm;
	uint_ptr font = 0;
	element::ptr el_parent = parent();
	if (el_parent)
	{
		font = el_parent->get_font(&fm);
		m_letter_spacing = el_parent->letter_spacing();
	}
	else
	{
		m_letter_spacing = 0;
	}
	if(is_break())
	{
		m_size.height	= 0;
		m_size.width	= 0;
	} else
	{
		m_size.height	= fm.height;
		document* doc = get_document();
		if (doc && doc->container())
		{
			uint64_t measure_start = sprof ? sys_tic_ms(0) : 0;
			const tchar_t* drawn = m_use_transformed ? m_transformed_text.c_str() : m_text.c_str();
			m_size.width	= doc->container()->text_width(drawn, font);
			/* CSS text-3 letter-spacing: every glyph advance grows by the
			 * spacing, so the run's measured width grows by spacing times the
			 * code-point count. Line breaking and alignment read m_size.width,
			 * so widening it here propagates everywhere. */
			if(m_letter_spacing != 0)
			{
				m_size.width += m_letter_spacing * utf8_code_points(drawn);
			}
			if(sprof) measure_ms += (uint32_t)(sys_tic_ms(0) - measure_start);
		} else
		{
			m_size.width = 0;
		}
	}
	m_draw_spaces = fm.draw_spaces;
	litehtml::profile_text_parse(transform_ms, measure_ms, start_ms);
}

int litehtml::el_text::get_base_line()
{
	element::ptr el_parent = parent();
	if (el_parent)
	{
		return el_parent->get_base_line();
	}
	return 0;
}

void litehtml::el_text::draw( uint_ptr hdc, int x, int y, const position* clip )
{
	if(is_white_space() && !m_draw_spaces)
	{
		return;
	}

	position pos = m_pos;
	pos.x	+= x;
	pos.y	+= y;

	if(pos.does_intersect(clip))
	{
		element::ptr el_parent = parent();
		if (el_parent)
		{
			document* doc = get_document();
			if (doc && doc->container())
			{
				uint_ptr font = el_parent->get_font();
				litehtml::web_color color = el_parent->get_color(_t("color"), true, doc->get_def_color());
				const tchar_t* drawn = m_use_transformed ? m_transformed_text.c_str() : m_text.c_str();
				if(m_letter_spacing != 0)
				{
					/* Space the glyphs by re-advancing per code point: the port
					 * draws whole strings at natural advance, so each glyph is
					 * emitted at a running x offset that adds the spacing. */
					int xoff = 0;
					for(const tchar_t* s = drawn; *s; )
					{
						size_t L = utf8_seq_len((unsigned char)*s);
						tstring one(s, L);
						position p = pos;
						p.x += xoff;
						p.width = doc->container()->text_width(one.c_str(), font);
						if(p.does_intersect(clip))
						{
							doc->container()->draw_text(hdc, one.c_str(), font, color, p);
						}
						xoff += p.width + m_letter_spacing;
						s += L;
					}
				} else
				{
					doc->container()->draw_text(hdc, drawn, font, color, pos);
				}
			}
		}
	}
}

int litehtml::el_text::line_height() const
{
	element::ptr el_parent = parent();
	if (el_parent)
	{
		return el_parent->line_height();
	}
	return 0;
}

litehtml::uint_ptr litehtml::el_text::get_font( font_metrics* fm /*= 0*/ )
{
	element::ptr el_parent = parent();
	if (el_parent)
	{
		return el_parent->get_font(fm);
	}
	return 0;
}

litehtml::style_display litehtml::el_text::get_display() const
{
	return display_inline_text;
}

litehtml::white_space litehtml::el_text::get_white_space() const
{
	element::ptr el_parent = parent();
	if (el_parent) return el_parent->get_white_space();
	return white_space_normal;
}

litehtml::element_position litehtml::el_text::get_element_position(css_offsets* offsets) const
{
	element::ptr p = parent();
	while(p && p->get_display() == display_inline)
	{
		if(p->get_element_position() == element_position_relative)
		{
			if(offsets)
			{
				*offsets = p->get_css_offsets();
			}
			return element_position_relative;
		}
		p = p->parent();
	}
	return element_position_static;
}

litehtml::css_offsets litehtml::el_text::get_css_offsets() const
{
	element::ptr p = parent();
	while(p && p->get_display() == display_inline)
	{
		if(p->get_element_position() == element_position_relative)
		{
			return p->get_css_offsets();
		}
		p = p->parent();
	}
	return css_offsets();
}
