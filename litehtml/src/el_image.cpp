#include "html.h"
#include "el_image.h"
#include "document.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* EWEB_IMGDBG (release-build image pipeline trace, default off): Pinterest's
 * "More winning ideas" carousel thumbs carry a valid i.pinimg.com/236x src yet
 * paint grey while sibling marketing imgs (736x) load - this surfaces, per
 * <img>, whether load_image fired and whether a decoded bitmap was cached by
 * paint time, separating "never fetched" from "fetched but not mounted". */
static bool eweb_imgdbg()
{
	static int v = -1;
	if(v < 0) { const char* e = getenv("EWEB_IMGDBG"); v = (e && e[0] == '1') ? 1 : 0; }
	return v == 1;
}

/* Parse a srcset/sizes length token ("100vw", "535px", "64em") into CSS px.
 * Returns false for anything unparseable (calc() etc.): the caller then
 * falls back to the full viewport width, the spec's default source size. */
namespace litehtml {
static bool srcset_source_size_px(const tstring& tok, int vw, int vh, int font_px, double& out)
{
	tstring s = tok;
	trim(s);
	if(s.empty())
	{
		return false;
	}
	char* end = NULL;
	double v = t_strtod(s.c_str(), &end);
	if(end == s.c_str())
	{
		return false;
	}
	tstring unit = end ? tstring(end) : tstring();
	trim(unit);
	if(unit.empty() || unit == _t("px"))
	{
		out = v;
	} else if(unit == _t("vw"))
	{
		out = v * (double)vw / 100.0;
	} else if(unit == _t("vh"))
	{
		out = v * (double)vh / 100.0;
	} else if(unit == _t("em") || unit == _t("rem"))
	{
		out = v * (double)font_px;
	} else
	{
		return false;
	}
	return out > 0.0;
}

/* HTML "select an image source": the source size is the first sizes entry
 * whose media condition matches (a bare length is the unconditional default);
 * without a sizes attribute it is 100vw. Commas inside calc()/media queries
 * are protected by a paren depth counter. */
static int srcset_sizes_width(const tstring& sizes, document* doc, const media_features& mf)
{
	int fallback = mf.width > 0 ? mf.width : 0;
	if(sizes.empty())
	{
		return fallback;
	}
	int font_px = doc ? doc->container()->get_default_font_size() : 16;
	size_t i = 0;
	int depth = 0;
	size_t start = 0;
	while(i <= sizes.length())
	{
		if(i < sizes.length())
		{
			tchar_t c = sizes[i];
			if(c == _t('(')) depth++;
			else if(c == _t(')') && depth > 0) depth--;
			else if(c != _t(',') || depth > 0) { i++; continue; }
		}
		tstring entry = sizes.substr(start, i - start);
		start = i + 1;
		i++;
		trim(entry);
		if(entry.empty())
		{
			continue;
		}
		/* Split the trailing <source-size-value> off the media condition at
		 * the last top-level space ("(min-width: 900px) 480px"). */
		size_t sp = entry.length();
		int d2 = 0;
		while(sp > 0)
		{
			sp--;
			tchar_t c = entry[sp];
			if(c == _t(')')) d2++;
			else if(c == _t('(') && d2 > 0) d2--;
			else if((c == _t(' ') || c == _t('\t')) && d2 == 0) break;
		}
		tstring cond, val;
		if(sp > 0 && (entry[sp] == _t(' ') || entry[sp] == _t('\t')))
		{
			cond = entry.substr(0, sp);
			val = entry.substr(sp + 1);
		} else
		{
			val = entry;
		}
		trim(cond);
		trim(val);
		if(!cond.empty() && doc)
		{
			media_query_list::ptr mql = media_query_list::create_from_string(cond, doc);
			if(!mql)
			{
				continue;
			}
			mql->apply_media_features(mf);
			if(!mql->is_used())
			{
				continue;
			}
		}
		double px = 0.0;
		if(srcset_source_size_px(val, mf.width, mf.height, font_px, px))
		{
			return (int)(px + 0.5);
		}
	}
	return fallback;
}

/* Pick one URL from a srcset attribute per the HTML density/width selection
 * rules at the device's real pixel ratio: x candidates take the smallest
 * density >= dpr (largest when all are below), w candidates take the smallest
 * width >= dpr * source-size (largest when all are below). The old picker
 * pinned 1x / narrowest-w, so a Retina viewport fetched next/image's 640w
 * hero instead of the 2048w/3840w asset and painted it blurry. */
static tstring pick_srcset_candidate(const tstring& srcset, const tstring& sizes,
									 document* doc, const media_features& mf)
{
	double dpr = (mf.resolution > 0) ? (double)mf.resolution / 96.0 : 1.0;
	int source_size = srcset_sizes_width(sizes, doc, mf);
	double target_w = dpr * (double)source_size;

	tstring first_url;
	tstring best_x_url, best_w_url;
	double best_x = 0.0, best_w = 0.0;
	bool have_x = false, have_w = false;
	size_t i = 0;
	int depth = 0;
	size_t start = 0;
	while(i <= srcset.length())
	{
		/* data: URLs carry commas - only split at top level. */
		if(i < srcset.length())
		{
			tchar_t c = srcset[i];
			if(c == _t('(')) depth++;
			else if(c == _t(')') && depth > 0) depth--;
			else if(c != _t(',') || depth > 0) { i++; continue; }
		}
		tstring cand = srcset.substr(start, i - start);
		start = i + 1;
		i++;
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
		double density = 1.0;	/* bare candidate: default 1x */
		double width = 0.0;
		bool is_w = false;
		if(!desc.empty())
		{
			tchar_t last = desc[desc.length() - 1];
			if(last == _t('x') || last == _t('X'))
			{
				density = t_strtod(desc.c_str(), NULL);
				if(density <= 0.0)
				{
					continue;
				}
			} else if(last == _t('w') || last == _t('W'))
			{
				width = t_strtod(desc.c_str(), NULL);
				if(width <= 0.0)
				{
					continue;
				}
				is_w = true;
			} else
			{
				continue;	/* invalid descriptor (h etc.): skip candidate */
			}
		}
		if(is_w)
		{
			/* Smallest width covering the density-scaled source size; when
			 * none covers it the largest available wins (spec fallback). */
			bool covers = width >= target_w;
			bool best_covers = have_w && best_w >= target_w;
			if(covers && (!best_covers || width < best_w))
			{
				best_w = width;
				best_w_url = url;
				have_w = true;
			} else if(!covers && !best_covers && (!have_w || width > best_w))
			{
				best_w = width;
				best_w_url = url;
				have_w = true;
			}
		} else
		{
			bool covers = density >= dpr;
			bool best_covers = have_x && best_x >= dpr;
			if(covers && (!best_covers || density < best_x))
			{
				best_x = density;
				best_x_url = url;
				have_x = true;
			} else if(!covers && !best_covers && (!have_x || density > best_x))
			{
				best_x = density;
				best_x_url = url;
				have_x = true;
			}
		}
	}
	if(have_x)
	{
		return best_x_url;
	}
	if(have_w)
	{
		return best_w_url;
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

	document* doc = get_document();
	media_features mf;
	memset(&mf, 0, sizeof(mf));
	mf.type = media_type_screen;
	mf.resolution = 96;
	if(doc && doc->container())
	{
		doc->container()->get_media_features(mf);
	}

	/* HTML spec: inside <picture>, the first <source> whose media attribute
	 * matches the current viewport overrides the <img>'s own srcset/src.
	 * Skipping this evaluation picks an asset with the wrong intrinsic
	 * ratio, and width:auto/height:100% boxes then come out too wide. */
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
				tstring cand = pick_srcset_candidate(ss, ch->get_attr(_t("sizes"), _t("")), doc, mf);
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
		tstring cand = pick_srcset_candidate(m_srcset, get_attr(_t("sizes"), _t("")), doc, mf);
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
	m_resolved_res = -1;
	m_dbg_logged_loaded = false;
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
	 * the live viewport differs from the snapshot taken at resolve time -
	 * including the device resolution, since a display change (dpr 1 -> 2)
	 * flips the srcset density winner without touching the CSS-px viewport. */
	{
		document* rdoc = get_document();
		if(rdoc && rdoc->container())
		{
			media_features mf;
			rdoc->container()->get_media_features(mf);
			if(mf.width != m_resolved_mw || mf.height != m_resolved_mh ||
			   mf.resolution != m_resolved_res)
			{
				m_resolved_mw = mf.width;
				m_resolved_mh = mf.height;
				m_resolved_res = mf.resolution;
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
			/* A percentage height whose containing-block chain is indefinite
			 * behaves as 'auto' (CSS 2.1 10.5); keep the intrinsic height rather
			 * than the raw percent value (100 for "100%") as pixels. */
			if (m_css_height.units() == css_units_percentage)
				m_pos.height = sz.height;
			else
				m_pos.height = (int)m_css_height.val();
			/* width stays auto here: the intrinsic-ratio recomputation below
			 * derives it from the resolved height. */
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
		/* A declared `aspect-ratio` is the preferred ratio: it overrides the
		 * intrinsic one and supplies one when the source has none, so an auto
		 * width follows the resolved height through it. */
		{
			double ar = aspect_ratio();
			if(ar > 0.0 && m_pos.height > 0)
			{
				m_pos.width = (int)(m_pos.height * ar + 0.5);
			}
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
		/* A declared `aspect-ratio` is the preferred ratio: it overrides the
		 * intrinsic one and supplies one when the source has none, so an auto
		 * height follows the resolved width through it. */
		{
			double ar = aspect_ratio();
			if(ar > 0.0 && m_pos.width > 0)
			{
				m_pos.height = (int)(m_pos.width / ar + 0.5);
			}
		}
	} else
	{
		m_pos.width		= (int) m_css_width.calc_percent(parent_width);
		m_pos.height	= 0;
		if (!get_predefined_height(m_pos.height))
		{
			/* Percentage height against an indefinite containing-block chain is
			 * 'auto' (CSS 2.1 10.5): derive height from the resolved width via the
			 * intrinsic ratio. The old fallback used the raw percent value as px
			 * (100 for "100%"), collapsing e.g. the pinterest hero collage to a
			 * 100px sliver. */
			if (m_css_height.units() == css_units_percentage)
				m_pos.height = sz.width ? (int)((float)m_pos.width * (float)sz.height / (float)sz.width) : sz.height;
			else
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

void litehtml::el_image::set_attr( const tchar_t* name, const tchar_t* val )
{
	html_tag::set_attr(name, val);
	if(!name) return;
	if(t_strcasecmp(name, _t("src")) != 0 && t_strcasecmp(name, _t("srcset")) != 0 &&
	   t_strcasecmp(name, _t("width")) != 0 && t_strcasecmp(name, _t("height")) != 0)
		return;
	/* Refresh the cached effective src from the new attribute bag; render()'s
	 * viewport-gated resolve would otherwise keep painting the stale src. */
	tstring old_src = m_src;
	parse_attributes();
	resolve_effective_src();
	document* doc = get_document();
	if(doc && doc->container() && !m_src.empty() && m_src != old_src)
	{
		doc->container()->load_image(m_src.c_str(), 0,
			!m_css_height.is_predefined() && !m_css_width.is_predefined());
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
			if(eweb_imgdbg() && iw > 0 && ih > 0 && !m_dbg_logged_loaded)
			{
				m_dbg_logged_loaded = true;
				fprintf(stderr, "[IMGDBG] draw LOADED src=%.90s nat=%dx%d box=%dx%d\n",
					m_src.c_str(), iw, ih, pos.width, pos.height);
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
		if(eweb_imgdbg())
			fprintf(stderr, "[IMGDBG] load_image src=%.90s cssw_pre=%d cssh_pre=%d\n",
				m_src.c_str(), (int)!m_css_width.is_predefined(), (int)!m_css_height.is_predefined());
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
