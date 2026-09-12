#include "html.h"
#include "el_image.h"
#include "document.h"

/* Pick one URL from a srcset attribute: an exact 1x candidate wins, then the
 * narrowest w descriptor (this device is a low-density viewport), then the
 * first candidate. Descriptors other than x/w are ignored. */
namespace litehtml {
static tstring pick_srcset_candidate(const tstring& srcset)
{
	tstring first_url;
	tstring best_url;
	int best_w = 0;
	bool have_w = false;
	size_t i = 0;
	while(i <= srcset.length())
	{
		size_t comma = srcset.find(_t(','), i);
		if(comma == tstring::npos)
		{
			comma = srcset.length();
		}
		tstring cand = srcset.substr(i, comma - i);
		i = comma + 1;
		trim(cand);
		if(cand.empty())
		{
			continue;
		}
		size_t sp = 0;
		while(sp < cand.length() && cand[sp] != _t(' ') && cand[sp] != _t('\t'))
		{
			sp++;
		}
		tstring url = cand.substr(0, sp);
		tstring desc = cand.substr(sp);
		trim(desc);
		if(url.empty())
		{
			continue;
		}
		if(first_url.empty())
		{
			first_url = url;
		}
		if(desc.empty())
		{
			return url;	// bare candidate: default 1x
		}
		tchar_t last = desc[desc.length() - 1];
		if(last == _t('x') || last == _t('X'))
		{
			if(t_atoi(desc.c_str()) == 1)
			{
				return url;	// exact 1x match
			}
		} else if(last == _t('w') || last == _t('W'))
		{
			int w = t_atoi(desc.c_str());
			if(w > 0 && (!have_w || w < best_w))
			{
				best_w = w;
				best_url = url;
				have_w = true;
			}
		}
	}
	if(have_w)
	{
		return best_url;
	}
	return first_url;
}
} // namespace litehtml

void litehtml::el_image::resolve_effective_src()
{
	if(!m_srcset.empty())
	{
		tstring cand = pick_srcset_candidate(m_srcset);
		if(!cand.empty())
		{
			m_src = cand;
		}
	}
	if(m_src.empty())
	{
		element::ptr p = parent();
		if(p && p->get_tagName() && !t_strcasecmp(p->get_tagName(), _t("picture")))
		{
			int cnt = (int) p->get_children_count();
			for(int k = 0; k < cnt; k++)
			{
				element::ptr ch = p->get_child(k);
				if(!ch || !ch->get_tagName() || t_strcasecmp(ch->get_tagName(), _t("source")))
				{
					continue;
				}
				const tchar_t* ss = ch->get_attr(_t("srcset"));
				if(ss && ss[0])
				{
					tstring cand = pick_srcset_candidate(ss);
					if(!cand.empty())
					{
						m_src = cand;
						break;
					}
				}
				const tchar_t* ssrc = ch->get_attr(_t("src"));
				if(ssrc && ssrc[0])
				{
					m_src = ssrc;
					break;
				}
			}
		}
	}
}

litehtml::el_image::el_image(litehtml::document* doc) : html_tag(doc)
{
	m_display = display_inline_block;
}

litehtml::el_image::~el_image( void )
{

}

void litehtml::el_image::get_content_size( size& sz, int max_width )
{
	document* doc = get_document();
	if (doc && doc->container())
	{
		doc->container()->get_image_size(m_src.c_str(), 0, sz);
	} else
	{
		sz.width = 0;
		sz.height = 0;
	}
}

int litehtml::el_image::line_height() const
{
	return height();
}

bool litehtml::el_image::is_replaced() const
{
	return true;
}

int litehtml::el_image::render( int x, int y, int max_width, bool second_pass )
{
	int parent_width = max_width;

	calc_outlines(parent_width);

	m_pos.move_to(x, y);

	document* doc = get_document();

	litehtml::size sz;
	if (doc && doc->container())
	{
		doc->container()->get_image_size(m_src.c_str(), 0, sz);
	} else
	{
		sz.width = 0;
		sz.height = 0;
	}

	m_pos.width		= sz.width;
	m_pos.height	= sz.height;

	if(m_css_height.is_predefined() && m_css_width.is_predefined())
	{
		m_pos.height	= sz.height;
		m_pos.width		= sz.width;

		// check for max-height
		if(!m_css_max_width.is_predefined() && doc)
		{
			int max_width = doc->cvt_units(m_css_max_width, m_font_size, parent_width);
			if(m_pos.width > max_width)
			{
				m_pos.width = max_width;
			}
			if(sz.width)
			{
				m_pos.height = (int) ((float) m_pos.width * (float) sz.height / (float)sz.width);
			} else
			{
				m_pos.height = sz.height;
			}
		}

		// check for max-height
		if(!m_css_max_height.is_predefined() && doc)
		{
			int max_height = doc->cvt_units(m_css_max_height, m_font_size);
			if(m_pos.height > max_height)
			{
				m_pos.height = max_height;
			}
			if(sz.height)
			{
				m_pos.width = (int) (m_pos.height * (float)sz.width / (float)sz.height);
			} else
			{
				m_pos.width = sz.width;
			}
		}
	} else if(!m_css_height.is_predefined() && m_css_width.is_predefined())
	{
		if (!get_predefined_height(m_pos.height))
		{
			m_pos.height = (int)m_css_height.val();
		}

		// check for max-height
		if(!m_css_max_height.is_predefined() && doc)
		{
			int max_height = doc->cvt_units(m_css_max_height, m_font_size);
			if(m_pos.height > max_height)
			{
				m_pos.height = max_height;
			}
		}

		if(sz.height)
		{
			m_pos.width = (int) (m_pos.height * (float)sz.width / (float)sz.height);
		} else
		{
			m_pos.width = sz.width;
		}
	} else if(m_css_height.is_predefined() && !m_css_width.is_predefined())
	{
		m_pos.width = (int) m_css_width.calc_percent(parent_width);

		// check for max-width
		if(!m_css_max_width.is_predefined() && doc)
		{
			int max_width = doc->cvt_units(m_css_max_width, m_font_size, parent_width);
			if(m_pos.width > max_width)
			{
				m_pos.width = max_width;
			}
		}

		if(sz.width)
		{
			m_pos.height = (int) ((float) m_pos.width * (float) sz.height / (float)sz.width);
		} else
		{
			m_pos.height = sz.height;
		}
	} else
	{
		m_pos.width		= (int) m_css_width.calc_percent(parent_width);
		m_pos.height	= 0;
		if (!get_predefined_height(m_pos.height))
		{
			m_pos.height = (int)m_css_height.val();
		}

		// check for max-height
		if(!m_css_max_height.is_predefined() && doc)
		{
			int max_height = doc->cvt_units(m_css_max_height, m_font_size);
			if(m_pos.height > max_height)
			{
				m_pos.height = max_height;
			}
		}

		// check for max-height
		if(!m_css_max_width.is_predefined() && doc)
		{
			int max_width = doc->cvt_units(m_css_max_width, m_font_size, parent_width);
			if(m_pos.width > max_width)
			{
				m_pos.width = max_width;
			}
		}
	}

	calc_auto_margins(parent_width);

	m_pos.x	+= content_margins_left();
	m_pos.y += content_margins_top();

	return m_pos.width + content_margins_left() + content_margins_right();
}

void litehtml::el_image::parse_attributes()
{
	m_src = get_attr(_t("src"), _t(""));
	m_srcset = get_attr(_t("srcset"), _t(""));

	const tchar_t* attr_height = get_attr(_t("height"));
	if(attr_height)
	{
		m_style.add_property(_t("height"), attr_height, 0, false);
	}
	const tchar_t* attr_width = get_attr(_t("width"));
	if(attr_width)
	{
		m_style.add_property(_t("width"), attr_width, 0, false);
	}
}

void litehtml::el_image::draw( uint_ptr hdc, int x, int y, const position* clip )
{
	document* doc = get_document();
	if (!doc || !doc->container())
	{
		return;
	}

	position pos = m_pos;
	pos.x += x;
	pos.y += y;

	position el_pos = pos;
	el_pos += m_padding;
	el_pos += m_borders;

	// draw standard background here
	if (el_pos.does_intersect(clip))
	{
		const background* bg = get_background();
		if (bg)
		{
			background_paint bg_paint;
			init_background_paint(pos, bg_paint, bg);

			doc->container()->draw_background(hdc, bg_paint);
		}
	}

	// draw image as background
	if(pos.does_intersect(clip))
	{
		if (pos.width > 0 && pos.height > 0) {
			background_paint bg;
			bg.image				= m_src;
			bg.clip_box				= pos;
			bg.origin_box			= pos;
			bg.border_box			= pos;
			bg.border_box			+= m_padding;
			bg.border_box			+= m_borders;
			bg.repeat				= background_repeat_no_repeat;
			bg.image_size.width		= pos.width;
			bg.image_size.height	= pos.height;
			bg.border_radius		= m_css_borders.radius.calc_percents(bg.border_box.width, bg.border_box.height);
			bg.position_x			= pos.x;
			bg.position_y			= pos.y;
			doc->container()->draw_background(hdc, bg);
		}
	}

	// draw borders
	if (el_pos.does_intersect(clip))
	{
		position border_box = pos;
		border_box += m_padding;
		border_box += m_borders;

		borders bdr = m_css_borders;
		bdr.radius = m_css_borders.radius.calc_percents(border_box.width, border_box.height);

		doc->container()->draw_borders(hdc, bdr, border_box, have_parent() ? false : true);
	}
}

void litehtml::el_image::parse_styles( bool is_reparse /*= false*/ )
{
	resolve_effective_src();
	html_tag::parse_styles(is_reparse);

	document* doc = get_document();
	if(!m_src.empty() && doc && doc->container())
	{
		if(!m_css_height.is_predefined() && !m_css_width.is_predefined())
		{
			doc->container()->load_image(m_src.c_str(), 0, true);
		} else
		{
			doc->container()->load_image(m_src.c_str(), 0, false);
		}
	}
}
