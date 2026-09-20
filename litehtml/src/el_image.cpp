#include "html.h"
#include "el_image.h"
#include "document.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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
	/* Re-read the element's own attributes first: a previous pass may have
	 * overwritten m_src with a <source> candidate, and the winner can change
	 * when the viewport (media) changes. */
	m_src = get_attr(_t("src"), _t(""));
	m_srcset = get_attr(_t("srcset"), _t(""));

	/* HTML spec: inside <picture>, the first <source> whose media attribute
	 * matches the current viewport overrides the <img>'s own srcset/src.
	 * Skipping this evaluation picks an asset with the wrong intrinsic
	 * ratio, and width:auto/height:100% boxes then come out too wide. */
	element::ptr p = parent();
	if(p && p->get_tagName() && !t_strcasecmp(p->get_tagName(), _t("picture")))
	{
		document* doc = get_document();
		media_features mf;
		if(doc && doc->container())
		{
			doc->container()->get_media_features(mf);
		}
		int cnt = (int) p->get_children_count();
		for(int k = 0; k < cnt; k++)
		{
			element::ptr ch = p->get_child(k);
			if(!ch || !ch->get_tagName() || t_strcasecmp(ch->get_tagName(), _t("source")))
			{
				continue;
			}
			const tchar_t* media = ch->get_attr(_t("media"));
			if(media && media[0] && doc)
			{
				media_query_list::ptr mql = media_query_list::create_from_string(media, doc);
				if(!mql)
				{
					continue;	// unparsable media: never matches
				}
				mql->apply_media_features(mf);
				if(!mql->is_used())
				{
					continue;
				}
			}
			const tchar_t* ss = ch->get_attr(_t("srcset"));
			if(ss && ss[0])
			{
				tstring cand = pick_srcset_candidate(ss);
				if(!cand.empty())
				{
					m_src = cand;
					return;
				}
			}
			const tchar_t* ssrc = ch->get_attr(_t("src"));
			if(ssrc && ssrc[0])
			{
				m_src = ssrc;
				return;
			}
		}
	}
	if(!m_srcset.empty())
	{
		tstring cand = pick_srcset_candidate(m_srcset);
		if(!cand.empty())
		{
			m_src = cand;
		}
	}
}

litehtml::el_image::el_image(litehtml::document* doc) : html_tag(doc)
{
	m_display = display_inline_block;
	m_attr_width = 0;
	m_attr_height = 0;
	m_resolved_mw = -1;
	m_resolved_mh = -1;
}

litehtml::el_image::~el_image( void )
{

}

void litehtml::el_image::attr_size( size& sz ) const
{
	/* The attribute pair sizes the box when no author rule does; a single
	 * attribute scales the other axis through the natural aspect ratio,
	 * exactly like a browser's intrinsic-size computation. */
	if(m_attr_width > 0 && m_attr_height > 0)
	{
		sz.width = m_attr_width;
		sz.height = m_attr_height;
	} else if(m_attr_width > 0)
	{
		if(sz.width > 0) sz.height = (int)((float)sz.height * (float)m_attr_width / (float)sz.width);
		sz.width = m_attr_width;
	} else if(m_attr_height > 0)
	{
		if(sz.height > 0) sz.width = (int)((float)sz.width * (float)m_attr_height / (float)sz.height);
		sz.height = m_attr_height;
	}
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
	attr_size(sz);
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
	/* <picture>/srcset selection is media-dependent; the first style pass can
	 * run while the client viewport is still 0x0 (max-height:775px then
	 * matches and the wrong asset ratio sizes the box). Re-resolve whenever
	 * the live viewport differs from the snapshot taken at resolve time. */
	{
		document* rdoc = get_document();
		if(rdoc && rdoc->container())
		{
			media_features mf;
			rdoc->container()->get_media_features(mf);
			if(mf.width != m_resolved_mw || mf.height != m_resolved_mh)
			{
				m_resolved_mw = mf.width;
				m_resolved_mh = mf.height;
				tstring old_src = m_src;
				resolve_effective_src();
				if(m_src != old_src && !m_src.empty())
				{
					rdoc->container()->load_image(m_src.c_str(), 0,
						!m_css_height.is_predefined() && !m_css_width.is_predefined());
				}
			}
		}
	}
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
	attr_size(sz);

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
			/* A percentage max-width against an indefinite available width (the
			 * flex base-size pass hands in 0) resolves to 0; per CSS it then
			 * behaves as 'none', so never clamp the intrinsic size down to 0.
			 * Without this, core.css's global img{max-width:100%} collapsed the
			 * w3.org member logos to zero-width boxes. */
			if(max_width > 0 && m_pos.width > max_width)
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
			/* A percentage max-width against an indefinite available width (the
			 * flex base-size pass hands in 0) resolves to 0; per CSS it then
			 * behaves as 'none', so never clamp the intrinsic size down to 0.
			 * Without this, core.css's global img{max-width:100%} collapsed the
			 * w3.org member logos to zero-width boxes. */
			if(max_width > 0 && m_pos.width > max_width)
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
			/* A percentage max-width against an indefinite available width (the
			 * flex base-size pass hands in 0) resolves to 0; per CSS it then
			 * behaves as 'none', so never clamp the intrinsic size down to 0.
			 * Without this, core.css's global img{max-width:100%} collapsed the
			 * w3.org member logos to zero-width boxes. */
			if(max_width > 0 && m_pos.width > max_width)
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
		m_attr_height = atoi(attr_height);
	}
	const tchar_t* attr_width = get_attr(_t("width"));
	if(attr_width)
	{
		m_attr_width = atoi(attr_width);
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

	/* Replaced elements honour transform too: apple.com centres its hero art
	 * with "inset-inline-start:50%; transform:translate(-50%)" on an absolutely
	 * positioned <img>. The port's paint transform only skews border quads, it
	 * never moves a background bitmap, so a pure translation is folded into the
	 * destination box itself: bitmap, background, borders and the culling test
	 * below then all land on the transformed position. Non-translational
	 * matrices (rotate/scale) still go through push_paint_transform for the
	 * border quads, and culling is skipped for them because the clip test uses
	 * the untransformed box. */
	float xform[6];
	bool have_xform = compute_transform_matrix(el_pos, xform);
	if(have_xform && xform[0] == 1.0f && xform[1] == 0.0f &&
	   xform[2] == 0.0f && xform[3] == 1.0f)
	{
		int tx = (int)lroundf(xform[4]);
		int ty = (int)lroundf(xform[5]);
		pos.x += tx;
		pos.y += ty;
		el_pos.x += tx;
		el_pos.y += ty;
		have_xform = false;
	}
	if(have_xform)
	{
		doc->container()->push_paint_transform(xform);
	}

	// draw standard background here
	if (have_xform || el_pos.does_intersect(clip))
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
	if(have_xform || pos.does_intersect(clip))
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
			/* object-fit: the content box is the destination viewport. Size the
			 * bitmap inside it (cover/contain/none/scale-down) and centre it;
			 * draw_background clips the overflowing tile back to clip_box and
			 * back-computes the source sub-rect, which is exactly the centre
			 * crop browsers show for object-fit:cover (history thumbs).
			 * The default 'fill' keeps stretching to the whole box. */
			int fw = pos.width, fh = pos.height, fx = pos.x, fy = pos.y;
			litehtml::size nat;
			int iw = 0, ih = 0;
			if (doc->container())
			{
				doc->container()->get_image_size(m_src.c_str(), 0, nat);
				iw = nat.width; ih = nat.height;
			}
			if (iw > 0 && ih > 0)
			{
				const tchar_t* of = get_style_property(_t("object-fit"), false, 0);
				bool is_cover = false, is_contain = false, is_none = false, is_sdown = false;
				if (of)
				{
					if (!t_strcasecmp(of, _t("cover"))) is_cover = true;
					else if (!t_strcasecmp(of, _t("contain"))) is_contain = true;
					else if (!t_strcasecmp(of, _t("none"))) is_none = true;
					else if (!t_strcasecmp(of, _t("scale-down"))) is_sdown = true;
				}
				if (is_cover || is_contain || is_none || is_sdown)
				{
					float sx = (float)pos.width / (float)iw;
					float sy = (float)pos.height / (float)ih;
					float s;
					if (is_cover) s = sx > sy ? sx : sy;
					else if (is_contain) s = sx < sy ? sx : sy;
					else if (is_none) s = 1.0f;
					else { s = sx < sy ? sx : sy; if (s > 1.0f) s = 1.0f; }
					fw = (int)(iw * s); fh = (int)(ih * s);
					fx = pos.x + (pos.width - fw) / 2;
					fy = pos.y + (pos.height - fh) / 2;
					/* object-position: where the (possibly larger) fitted bitmap sits
					 * inside the box. apple.com anchors card art with "bottom" so a
					 * 284px image in a shorter object-fit:none box shows its bottom. */
					const tchar_t* op = get_style_property(_t("object-position"), false, 0);
					if (op && *op)
					{
						string_vector toks;
						split_string(op, toks, _t(" \t"));
						float px = 50.f, py = 50.f;		/* percentages */
						bool px_abs = false, py_abs = false;
						int  ax = 0, ay = 0;				/* absolute px offsets */
						int  idx = 0;
						bool horiz_set = false, vert_set = false;
						for (size_t k = 0; k < toks.size() && idx < 2; k++)
						{
							const tstring& tk = toks[k];
							if (tk.empty()) continue;
							if (!t_strcasecmp(tk.c_str(), _t("left")))   { px = 0;   horiz_set = true; idx++; continue; }
							if (!t_strcasecmp(tk.c_str(), _t("right")))  { px = 100; horiz_set = true; idx++; continue; }
							if (!t_strcasecmp(tk.c_str(), _t("top")))    { py = 0;   vert_set = true;  idx++; continue; }
							if (!t_strcasecmp(tk.c_str(), _t("bottom"))) { py = 100; vert_set = true;  idx++; continue; }
							if (!t_strcasecmp(tk.c_str(), _t("center"))) { idx++; continue; }
							css_length L;
							L.fromString(tk, _t(""), -1);
							/* first numeric token is horizontal unless a keyword already
							 * fixed that axis ("bottom 10px" is invalid, but "10px bottom"
							 * and "left 20%" are common). */
							bool to_vert = horiz_set || (idx == 1 && !vert_set);
							if (!to_vert)
							{
								if (L.units() == css_units_percentage) px = L.val();
								else { px_abs = true; ax = doc->cvt_units(L, get_font_size()); }
								horiz_set = true;
							}
							else
							{
								if (L.units() == css_units_percentage) py = L.val();
								else { py_abs = true; ay = doc->cvt_units(L, get_font_size()); }
								vert_set = true;
							}
							idx++;
						}
						fx = pos.x + (px_abs ? ax : (int)((pos.width  - fw) * px / 100.f));
						fy = pos.y + (py_abs ? ay : (int)((pos.height - fh) * py / 100.f));
					}
				}
			}
			bg.image_size.width		= fw;
			bg.image_size.height	= fh;
			bg.border_radius		= m_css_borders.radius.calc_percents(bg.border_box.width, bg.border_box.height);
			bg.position_x			= fx;
			bg.position_y			= fy;
			doc->container()->draw_background(hdc, bg);
		}
	}

	// draw borders
	if (have_xform || el_pos.does_intersect(clip))
	{
		position border_box = pos;
		border_box += m_padding;
		border_box += m_borders;

		borders bdr = m_css_borders;
		bdr.radius = m_css_borders.radius.calc_percents(border_box.width, border_box.height);

		doc->container()->draw_borders(hdc, bdr, border_box, have_parent() ? false : true);
	}
	if(have_xform)
	{
		doc->container()->pop_paint_transform();
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

litehtml::el_video::el_video(litehtml::document* doc) : el_image(doc)
{
}

litehtml::el_video::~el_video( void )
{
}

void litehtml::el_video::parse_attributes()
{
	/* No decoder: the poster frame IS the element's payload (a preload=none
	 * video shows exactly this in a real browser). srcset/picture do not
	 * apply to <video>. */
	m_src = get_attr(_t("poster"), _t(""));

	const tchar_t* attr_height = get_attr(_t("height"));
	if(attr_height)
	{
		m_attr_height = atoi(attr_height);
	}
	const tchar_t* attr_width = get_attr(_t("width"));
	if(attr_width)
	{
		m_attr_width = atoi(attr_width);
	}
}

void litehtml::el_video::parse_styles( bool is_reparse /*= false*/ )
{
	el_image::parse_styles(is_reparse);
	/* The UA sheet hides <video> with !important (no playback, no fallback
	 * text spill). With a poster to show, lift the element back into flow
	 * here, after the cascade; without one keep the UA verdict. An author
	 * display that is not 'none' is respected as written. */
	if(!m_src.empty() && m_display == display_none)
	{
		m_display = display_inline_block;
	}
}
