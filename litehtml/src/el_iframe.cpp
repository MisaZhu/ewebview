#include "html.h"
#include "el_iframe.h"
#include "document.h"
#include <stdlib.h>

litehtml::el_iframe::el_iframe(litehtml::document* doc) : el_image(doc)
{
}

litehtml::el_iframe::~el_iframe(void)
{
}

void litehtml::el_iframe::parse_styles(bool is_reparse /*= false*/)
{
	/* Same cascade as <img>, but the payload is an HTML document: route the
	 * fetch through load_iframe so the container never tries to decode the
	 * frame URL as a bitmap. */
	resolve_effective_src();
	html_tag::parse_styles(is_reparse);

	document* doc = get_document();
	if(!m_src.empty() && doc && doc->container())
	{
		doc->container()->load_iframe(m_src.c_str(), 0,
			!m_css_height.is_predefined() && !m_css_width.is_predefined());
	}
}

void litehtml::el_iframe::set_attr(const tchar_t* name, const tchar_t* val)
{
	html_tag::set_attr(name, val);
	if(!name || t_strcasecmp(name, _t("src")) != 0)
		return;
	tstring old_src = m_src;
	parse_attributes();
	document* doc = get_document();
	if(doc && doc->container() && !m_src.empty() && m_src != old_src)
	{
		doc->container()->load_iframe(m_src.c_str(), 0,
			!m_css_height.is_predefined() && !m_css_width.is_predefined());
	}
}

void litehtml::el_iframe::get_content_size(size& sz, int max_width)
{
	/* No bitmap natural size: the HTML default iframe intrinsic is 300x150.
	 * Author CSS (wx's iframe{width:100%;height:100%}) overrides it in
	 * render(); this only anchors the auto/auto case. */
	(void)max_width;
	sz.width  = 300;
	sz.height = 150;
}

void litehtml::el_iframe::draw(uint_ptr hdc, int x, int y, const position* clip)
{
	document* doc = get_document();
	if(!doc || !doc->container())
		return;

	/* Background/borders ride the replaced-element path; the frame URL is
	 * not in the image cache so the base bitmap pass paints nothing. */
	el_image::draw(hdc, x, y, clip);

	position pos = m_pos;
	pos.x += x;
	pos.y += y;
	if(pos.width > 0 && pos.height > 0 && pos.does_intersect(clip))
	{
		doc->container()->draw_iframe(hdc, m_src.c_str(), 0, pos, clip);
	}
}