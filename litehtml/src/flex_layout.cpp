#include "html.h"
#include "html_tag.h"
#include "document.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <vector>

/*
 * Flexbox (row axis) layout for litehtml.
 *
 * Implements the subset modern sites actually lean on: flex-direction row /
 * row-reverse (column degrades to block flow, which is equivalent for a
 * single column), flex-wrap, flex-basis / flex-grow / flex-shrink plus the
 * "flex" shorthand, order, justify-content and align-items. Items are laid
 * out as block-level boxes at their resolved main size, so nested flex
 * containers and ordinary block content both work unchanged.
 */

namespace litehtml
{
	struct flex_item
	{
		element::ptr	el;
		int				order;
		float			grow;
		float			shrink;
		int				base;		// outer main size before free-space distribution
		bool			has_main;	// base came from flex-basis / width
		int				ml, mr, mt, mb;
		int				main;		// final outer main size
		int				cross;		// final outer cross size
	};
}

using namespace litehtml;

static bool flex_flag(const tchar_t* val, const tchar_t* a, const tchar_t* b = 0)
{
	if(!val) return false;
	if(!t_strcasecmp(val, a)) return true;
	if(b && !t_strcasecmp(val, b)) return true;
	return false;
}

static int flex_len(const tchar_t* v, int avail, int font_size, document* doc)
{
	if(!v || !v[0]) return 0;
	css_length l;
	l.fromString(v);
	if(l.is_predefined()) return 0;
	int px = doc->cvt_units(l, font_size, avail);
	return px > 0 ? px : 0;
}

/* gap / row-gap / column-gap: "gap: <row> [<column>]" */
static void flex_parse_gap(html_tag* el, int avail, int& row_gap, int& col_gap)
{
	row_gap = col_gap = 0;
	const tchar_t* gap_s = el->get_style_property(_t("gap"), false, 0);
	if(gap_s)
	{
		tstring gs = gap_s;
		string_vector toks;
		split_string(gs, toks, _t(" \t"));
		if(toks.size() >= 1)
			row_gap = flex_len(toks[0].c_str(), avail, el->get_font_size(), el->get_document());
		if(toks.size() >= 2)
			col_gap = flex_len(toks[1].c_str(), avail, el->get_font_size(), el->get_document());
		else
			col_gap = row_gap;
	}
	const tchar_t* cg = el->get_style_property(_t("column-gap"), false, 0);
	if(cg && !flex_flag(cg, "normal"))
		col_gap = flex_len(cg, avail, el->get_font_size(), el->get_document());
	const tchar_t* rg = el->get_style_property(_t("row-gap"), false, 0);
	if(rg && !flex_flag(rg, "normal"))
		row_gap = flex_len(rg, avail, el->get_font_size(), el->get_document());
	/* Legacy prefixed names (grid-gap: <row> [<column>]) -- still used by
	 * sites like w3.org; honoured unless the unprefixed props overrode. */
	const tchar_t* ggap = el->get_style_property(_t("grid-gap"), false, 0);
	if(ggap && !cg && !rg)
	{
		tstring gs = ggap;
		string_vector toks;
		split_string(gs, toks, _t(" \t"));
		if(toks.size() >= 1)
			row_gap = flex_len(toks[0].c_str(), avail, el->get_font_size(), el->get_document());
		if(toks.size() >= 2)
			col_gap = flex_len(toks[1].c_str(), avail, el->get_font_size(), el->get_document());
		else
			col_gap = row_gap;
	}
	const tchar_t* gcg = el->get_style_property(_t("grid-column-gap"), false, 0);
	if(gcg && !cg && !flex_flag(gcg, "normal"))
		col_gap = flex_len(gcg, avail, el->get_font_size(), el->get_document());
	const tchar_t* grg = el->get_style_property(_t("grid-row-gap"), false, 0);
	if(grg && !rg && !flex_flag(grg, "normal"))
		row_gap = flex_len(grg, avail, el->get_font_size(), el->get_document());
}

int litehtml::html_tag::render_flex( int x, int y, int max_width, bool second_pass /*= false*/ )
{
	int parent_width = max_width;

	calc_outlines(parent_width);

	m_pos.clear();
	m_pos.move_to(x, y);
	m_pos.x += content_margins_left();
	m_pos.y += content_margins_top();

	int ret_width = 0;
	int avail = max_width;
	bool width_auto = true;

	if(!m_css_width.is_predefined())
	{
		int w = calc_width(parent_width);
		if(m_box_sizing == box_sizing_border_box)
		{
			w -= m_padding.width() + m_borders.width();
		}
		ret_width = avail = w;
		width_auto = false;
	}
	else if(avail)
	{
		avail -= content_margins_left() + content_margins_right();
	}

	if(!m_css_max_width.is_predefined() && !second_pass)
	{
		int mw = get_document()->cvt_units(m_css_max_width, m_font_size, parent_width);
		if(m_box_sizing == box_sizing_border_box)
		{
			mw -= m_padding.left + m_borders.left + m_padding.right + m_borders.right;
		}
		if(avail > mw)
		{
			avail = mw;
		}
	}
	if(avail < 0) avail = 0;

	const tchar_t* dir_s = get_style_property(_t("flex-direction"), false, _t("row"));
	if(flex_flag(dir_s, "column", "column-reverse"))
	{
		// a single flex column is block flow for the subset we support
		return render_box(x, y, max_width, second_pass);
	}
	bool row_reverse = flex_flag(dir_s, "row-reverse");

	const tchar_t* wrap_s	= get_style_property(_t("flex-wrap"), false, _t("nowrap"));
	bool do_wrap			= flex_flag(wrap_s, "wrap", "wrap-reverse");
	const tchar_t* jc_s		= get_style_property(_t("justify-content"), false, _t("flex-start"));
	const tchar_t* ai_s		= get_style_property(_t("align-items"), false, _t("stretch"));
	int row_gap = 0, col_gap = 0;
	flex_parse_gap(this, avail, row_gap, col_gap);

	std::vector<flex_item> items;
	for(auto& el : m_children)
	{
		if(!el || !el->is_visible()) continue;
		element_position ep = el->get_element_position();
		if(ep == element_position_absolute || ep == element_position_fixed) continue;
		if(el->is_white_space()) continue;

		/* CSS blockifies flex items: inline-level boxes become block-level */
		switch(el->get_display())
		{
		case display_inline:		el->set_display(display_block);		break;
		case display_inline_block:	el->set_display(display_block);		break;
		case display_inline_table:	el->set_display(display_table);		break;
		case display_inline_flex:	el->set_display(display_flex);		break;
		case display_inline_grid:	el->set_display(display_grid);		break;
		default:						break;
		}

		flex_item it;
		it.el		= el;
		it.grow		= 0;
		it.shrink	= 1;
		it.base		= 0;
		it.has_main	= false;
		it.main		= 0;
		it.cross	= 0;
		it.ml = el->margin_left();
		it.mr = el->margin_right();
		it.mt = el->margin_top();
		it.mb = el->margin_bottom();

		const tchar_t* v = el->get_style_property(_t("order"), false, _t("0"));
		it.order = v ? atoi(v) : 0;

		const tchar_t* sh = el->get_style_property(_t("flex"), false, 0);
		if(sh)
		{
			float g = 0, s = 1, b = -1;
			int n = sscanf(sh, "%f %f %f", &g, &s, &b);
			if(n >= 1) it.grow = g;
			if(n >= 2) it.shrink = s;
			if(n >= 3 && b >= 0)
			{
				it.base = (int)b + it.ml + it.mr;
				it.has_main = true;
			}
		}
		v = el->get_style_property(_t("flex-grow"), false, 0);
		if(v) it.grow = (float)atof(v);
		v = el->get_style_property(_t("flex-shrink"), false, 0);
		if(v) it.shrink = (float)atof(v);

		css_length cw = el->get_css_width();
		/* css_units_none means "no explicit width" (auto / unset). A default-constructed
		 * css_length has is_predefined()==false yet units none and value 0 — that is what
		 * text nodes and other never-width-styled items report. Treating it as an explicit
		 * 0 width collapsed them (e.g. the text inside a `display:flex` nav-link measured
		 * base=0 and was never rendered). Require a real unit before using width as base. */
		if(!cw.is_predefined() && cw.units() != css_units_none)
		{
			it.base = get_document()->cvt_units(cw, el->get_font_size(), avail) + it.ml + it.mr;
			it.has_main = true;
		}
		v = el->get_style_property(_t("flex-basis"), false, 0);
		if(v && !flex_flag(v, "auto", "content"))
		{
			css_length bl;
			bl.fromString(v);
			if(!bl.is_predefined())
			{
				it.base = get_document()->cvt_units(bl, el->get_font_size(), avail) + it.ml + it.mr;
				it.has_main = true;
			}
		}
		items.push_back(it);
	}

	if(items.empty())
	{
		m_pos.width = avail;
		m_pos.height = 0;
		calc_auto_margins(parent_width);
		return ret_width + content_margins_left() + content_margins_right();
	}

	std::stable_sort(items.begin(), items.end(),
			[](const flex_item& a, const flex_item& b){ return a.order < b.order; });

	// measure the preferred main size of flexible items
	for(auto& it : items)
	{
		if(!it.has_main)
		{
			if(it.el->get_display() == display_inline_text)
			{
				/* text nodes have no render() override (base element::render returns 0),
				 * so measure them via get_content_size exactly as place_element does;
				 * otherwise a bare text flex item collapses to base 0 */
				litehtml::size sz;
				it.el->get_content_size(sz, avail);
				it.base = sz.width + it.ml + it.mr;
			}
			else
			{
				it.base = it.el->render(0, 0, avail, second_pass);
			}
		}
		if(it.base < it.ml + it.mr) it.base = it.ml + it.mr;
	}

	// break into flex lines
	std::vector<std::vector<int> > lines;
	{
		std::vector<int> cur;
		int used = 0;
		for(size_t i = 0; i < items.size(); i++)
		{
			if(do_wrap && !cur.empty() &&
					used + items[i].base + (int)cur.size() * col_gap > avail)
			{
				lines.push_back(cur);
				cur.clear();
				used = 0;
			}
			cur.push_back((int)i);
			used += items[i].base;
		}
		if(!cur.empty()) lines.push_back(cur);
	}

	int bottom = 0;
	int used_width = 0;

	/* A flex container with a definite cross size (e.g. height:100vh) must give
	 * its single flex line that height, otherwise align-items centers within
	 * the tallest item and the container box (and its background) collapses. */
	int fixed_h = 0;
	bool has_fixed_h = !m_css_height.is_predefined() && m_css_height.units() != css_units_none;
	if(has_fixed_h)
	{
		fixed_h = get_document()->cvt_units(m_css_height, m_font_size, 0);
		if(m_box_sizing == box_sizing_border_box)
		{
			fixed_h -= m_padding.top + m_padding.bottom + m_borders.top + m_borders.bottom;
		}
		if(fixed_h < 0) fixed_h = 0;
	}

	for(size_t li = 0; li < lines.size(); li++)
	{
		std::vector<int>& line = lines[li];

		int sum = 0;
		float total_grow = 0, total_shrink = 0;
		for(size_t i = 0; i < line.size(); i++)
		{
			flex_item& it = items[line[i]];
			sum += it.base;
			total_grow += it.grow;
			total_shrink += it.shrink * (float)it.base;
		}

		int col_gaps = col_gap * (int)(line.size() ? line.size() - 1 : 0);
		int free = avail - sum - col_gaps;
		if(free > 0 && total_grow > 0)
		{
			for(size_t i = 0; i < line.size(); i++)
			{
				flex_item& it = items[line[i]];
				it.main = it.base + (int)((float)free * it.grow / total_grow);
			}
			free = 0;
		}
		else if(free < 0 && total_shrink > 0)
		{
			for(size_t i = 0; i < line.size(); i++)
			{
				flex_item& it = items[line[i]];
				int sh = (int)((float)(-free) * it.shrink * (float)it.base / total_shrink);
				it.main = it.base - sh;
				if(it.main < it.ml + it.mr) it.main = it.ml + it.mr;
			}
			free = 0;
		}
		else
		{
			for(size_t i = 0; i < line.size(); i++)
			{
				items[line[i]].main = items[line[i]].base;
			}
		}
		if(free < 0) free = 0;

		// lay the items out at their final main size to learn cross sizes
		int line_cross = 0;
		for(size_t i = 0; i < line.size(); i++)
		{
			flex_item& it = items[line[i]];
			if(it.el->get_display() == display_inline_text)
			{
				/* text has no render(); size it here so its height feeds line_cross */
				litehtml::size sz;
				it.el->get_content_size(sz, it.main);
				it.el->m_pos = sz;
				it.cross = sz.height + it.mt + it.mb;
			}
			else
			{
				it.el->render(0, 0, it.main, second_pass);
				it.cross = it.el->get_position().height + it.mt + it.mb;
			}
			if(it.cross > line_cross) line_cross = it.cross;
		}

		/* a single line in a definite-height container fills it, so align-items
		 * centers within the container rather than the tallest item */
		int line_h = line_cross;
		if(has_fixed_h && lines.size() == 1 && fixed_h > line_cross) line_h = fixed_h;

		// main-axis packing of the leftover space
		int lead = 0, jgap = 0;
		if(flex_flag(jc_s, "center"))
		{
			lead = free / 2;
		}
		else if(flex_flag(jc_s, "flex-end", "end"))
		{
			lead = free;
		}
		else if(flex_flag(jc_s, "space-between") && line.size() > 1)
		{
			jgap = free / (int)(line.size() - 1);
		}
		else if(flex_flag(jc_s, "space-around") && !line.empty())
		{
			jgap = free / (int)line.size();
			lead = jgap / 2;
		}

		std::vector<int> xs(line.size());
		{
			int xoff = lead;
			for(size_t i = 0; i < line.size(); i++)
			{
				xs[i] = xoff;
				xoff += items[line[i]].main + col_gap + jgap;
			}
			if(row_reverse)
			{
				int total = xoff - col_gap - jgap;
				for(size_t i = 0; i < line.size(); i++)
				{
					xs[i] = avail - total + (total - xs[i] - items[line[i]].main);
				}
			}
		}

		for(size_t i = 0; i < line.size(); i++)
		{
			flex_item& it = items[line[i]];
			int iy = 0;
			bool is_text = (it.el->get_display() == display_inline_text);
			css_length ch = it.el->get_css_height();
			if(!is_text && flex_flag(ai_s, "stretch", "normal") && ch.is_predefined())
			{
				iy = 0;
				it.el->get_position().height = line_h - it.mt - it.mb;
				it.cross = line_h;
			}
			else if(flex_flag(ai_s, "center"))
			{
				iy = (line_h - it.cross) / 2;
			}
			else if(flex_flag(ai_s, "flex-end", "end"))
			{
				iy = line_h - it.cross;
			}
			if(is_text)
			{
				/* position the text box directly; render() is a no-op for text.
				 * Coordinates are relative to this flex container's content-box
				 * origin (m_pos already holds that origin): adding m_pos.x again
				 * double-counted the container's own offset during the recursive
				 * draw pass, pushing items of any non-left-aligned flex box (e.g.
				 * grid tiles) far to the right. */
				it.el->m_pos.x = xs[i];
				it.el->m_pos.y = bottom + iy;
			}
			else
			{
				it.el->render(xs[i], bottom + iy, it.main, second_pass);
			}
		}

		int line_used = lead;
		for(size_t i = 0; i < line.size(); i++)
		{
			line_used += items[line[i]].main;
			if(i + 1 < line.size()) line_used += col_gap + jgap;
		}
		if(line_used > used_width) used_width = line_used;

		bottom += line_cross;
		if(li + 1 < lines.size()) bottom += row_gap;
	}

	/* width_auto: size to the container (block) or the used content (inline-flex).
	 * explicit width: avail already holds the resolved content width (calc_width
	 * minus padding/borders for border-box, capped by max-width). Assigning it here
	 * is essential — m_pos.clear() zeroed m_pos.width and the explicit branch above
	 * only set `avail`, so without this an explicit-width flex container renders 0
	 * wide (e.g. a nested `.global-nav ul { width:100% }`). */
	m_pos.width = width_auto ? (m_display == display_inline_flex ? std::min(avail, used_width) : avail) : avail;
	m_pos.height = has_fixed_h ? std::max(bottom, fixed_h) : bottom;
	calc_auto_margins(parent_width);

	int min_height = 0;
	if(!m_css_min_height.is_predefined())
	{
		min_height = (int)m_css_min_height.val();
	}
	if(min_height > m_pos.height)
	{
		m_pos.height = min_height;
	}

	m_pos.move_to(x, y);
	m_pos.x += content_margins_left();
	m_pos.y += content_margins_top();

	if(used_width > ret_width) ret_width = used_width;
	ret_width += content_margins_left() + content_margins_right();
	return ret_width;
}

/*
 * Grid (row-major) layout subset: explicit column tracks from
 * grid-template-columns (lengths, percentages, fr, auto, repeat(n, ...)),
 * auto rows sized to the tallest item, plus gap/row-gap/column-gap.
 * Anything else degrades to block flow, which is what an implicit
 * single-column grid would produce anyway.
 */

namespace litehtml
{
	struct grid_track
	{
		bool	is_fixed;
		int		fixed_w;
		float	fr;
		int		min_w;		// minmax() lower bound, used by repeat(auto-fill)
	};
}

/* the embedded STL string has no rfind */
static size_t str_rfind(const tstring& s, char c)
{
	for(int i = (int)s.length() - 1; i >= 0; i--)
	{
		if(s[(size_t)i] == c) return (size_t)i;
	}
	return tstring::npos;
}

static void grid_parse_tracks(const tchar_t* spec, int avail, int font_size, document* doc,
		std::vector<grid_track>& tracks, int col_gap = 0)
{
	// split on spaces outside parentheses
	std::vector<tstring> toks;
	tstring cur;
	int depth = 0;
	for(const tchar_t* p = spec; ; p++)
	{
		char c = *p;
		if(c == '(') depth++;
		if(c == ')') depth--;
		if(c == 0 || ((c == ' ' || c == '\t') && depth == 0))
		{
			if(!cur.empty())
			{
				toks.push_back(cur);
				cur.clear();
			}
			if(c == 0) break;
			continue;
		}
		cur += c;
	}

	for(size_t i = 0; i < toks.size(); i++)
	{
		tstring t = toks[i];
		/* Functional notation may contain spaces after the comma, e.g.
		 * 'repeat(auto-fill, minmax(10.625rem, 1fr))': the depth-0 split
		 * broke it into unbalanced fragments, so re-join until the parens
		 * balance again. */
		for(int depth = 0; ; )
		{
			int bal = 0;
			for(size_t k = 0; k < t.length(); k++)
			{
				if(t[k] == _t('(')) bal++;
				else if(t[k] == _t(')')) bal--;
			}
			depth = bal;
			if(depth <= 0 || i + 1 >= toks.size()) break;
			t += _t(' ');
			t += toks[++i];
		}
		if(t.substr(0, 7) == _t("repeat("))
		{
			int count = atoi(t.c_str() + 7);
			size_t cpos = t.find(_t(','));
			size_t epos = str_rfind(t, ')');
			if(cpos != tstring::npos && epos != tstring::npos && epos > cpos)
			{
				tstring inner = t.substr(cpos + 1, epos - cpos - 1);
				std::vector<grid_track> sub;
				grid_parse_tracks(inner.c_str(), avail, font_size, doc, sub, col_gap);
				if(count <= 0)
				{
					/* repeat(auto-fill | auto-fit, ...): fit as many columns of
					 * the track minimum as the row allows, like Chrome does for
					 * tile grids (minmax(L, 1fr) auto-fill). */
					if((t.substr(7, 9) == _t("auto-fill") || t.substr(7, 8) == _t("auto-fit")) &&
						(t.length() > 16 ? (t[16] == _t(',') || t[16] == _t(' ')) : true))
					{
						int minw = 0;
						if(!sub.empty())
						{
							minw = sub[0].min_w > 0 ? sub[0].min_w : (sub[0].is_fixed ? sub[0].fixed_w : 0);
						}
						if(minw <= 0) minw = 100;
						count = (avail + col_gap) / (minw + col_gap);
						if(count < 1) count = 1;
					}
				}
				for(int r = 0; r < count; r++)
					for(size_t s = 0; s < sub.size(); s++)
						tracks.push_back(sub[s]);
			}
			continue;
		}

		grid_track tr;
		tr.is_fixed = false;
		tr.fixed_w = 0;
		tr.fr = 1;
		tr.min_w = 0;
		size_t fl = str_rfind(t, 'r');
		if(fl != tstring::npos && fl == t.length() - 2 && t.substr(fl) == _t("fr"))
		{
			tr.fr = (float)atof(t.c_str());
			if(tr.fr <= 0) tr.fr = 1;
		}
		else if(t == _t("auto"))
		{
			tr.fr = 1;
		}
		else if(t.substr(0, 7) == _t("minmax("))
		{
			size_t cpos = t.find(_t(','));
			size_t epos = str_rfind(t, ')');
			if(cpos != tstring::npos && epos != tstring::npos)
			{
				/* Remember the lower bound so repeat(auto-fill) can count how
				 * many tracks fit; the upper bound still drives the width. */
				tstring mint = t.substr(7, cpos - 7);
				trim(mint);
				if(mint != _t("auto") && mint != _t("min-content") && mint != _t("max-content"))
				{
					css_length ml;
					ml.fromString(mint.c_str());
					if(!ml.is_predefined())
					{
						int mv = doc->cvt_units(ml, font_size, avail);
						if(mv > 0) tr.min_w = mv;
					}
				}
				tstring maxt = t.substr(cpos + 1, epos - cpos - 1);
				std::vector<grid_track> sub;
				grid_parse_tracks(maxt.c_str(), avail, font_size, doc, sub, col_gap);
				/* Keep the parsed lower bound: assigning the max-track below
				 * wholesale would drop it, and repeat(auto-fill, minmax(L,1fr))
				 * needs L to count how many columns fit (without it the count
				 * fell back to 100px and produced too many narrow tracks). */
				int lower = tr.min_w;
				if(!sub.empty())
				{
					tr = sub[0];
				}
				if(lower > 0)
					tr.min_w = lower;
				else if(tr.min_w <= 0 && tr.is_fixed)
					tr.min_w = tr.fixed_w;
			}
		}
		else if(!t.empty() && t[t.length() - 1] == _t('%'))
		{
			tr.is_fixed = true;
			tr.fixed_w = avail * atoi(t.c_str()) / 100;
			tr.min_w = tr.fixed_w;
		}
		else
		{
			css_length l;
			l.fromString(t.c_str());
			if(!l.is_predefined() && l.units() != css_units_none)
			{
				tr.is_fixed = true;
				tr.fixed_w = doc->cvt_units(l, font_size, avail);
				tr.min_w = tr.fixed_w;
			}
		}
		tracks.push_back(tr);
	}
}

int litehtml::html_tag::render_grid( int x, int y, int max_width, bool second_pass /*= false*/ )
{
	int parent_width = max_width;

	calc_outlines(parent_width);

	m_pos.clear();
	m_pos.move_to(x, y);
	m_pos.x += content_margins_left();
	m_pos.y += content_margins_top();

	int ret_width = 0;
	int avail = max_width;
	bool width_auto = true;

	if(!m_css_width.is_predefined())
	{
		int w = calc_width(parent_width);
		if(m_box_sizing == box_sizing_border_box)
		{
			w -= m_padding.width() + m_borders.width();
		}
		ret_width = avail = w;
		width_auto = false;
	}
	else if(avail)
	{
		avail -= content_margins_left() + content_margins_right();
	}
	if(avail < 0) avail = 0;

	std::vector<grid_track> tracks;
	int row_gap = 0, col_gap = 0;
	flex_parse_gap(this, avail, row_gap, col_gap);
	const tchar_t* tc = get_style_property(_t("grid-template-columns"), false, 0);
	if(tc)
		grid_parse_tracks(tc, avail, m_font_size, get_document(), tracks, col_gap);
	if(tracks.empty())
		return render_box(x, y, max_width, second_pass);

	int n = (int)tracks.size();
	int gaps = col_gap * (n - 1);
	int fixed_sum = 0;
	float fr_sum = 0;
	for(int i = 0; i < n; i++)
	{
		if(tracks[i].is_fixed) fixed_sum += tracks[i].fixed_w;
		else fr_sum += tracks[i].fr;
	}
	int flex_avail = avail - gaps - fixed_sum;
	if(flex_avail < 0) flex_avail = 0;
	std::vector<int> col_w(n);
	for(int i = 0; i < n; i++)
	{
		if(tracks[i].is_fixed)
			col_w[i] = tracks[i].fixed_w;
		else
			col_w[i] = fr_sum > 0 ? (int)((float)flex_avail * tracks[i].fr / fr_sum) : 0;
		if(col_w[i] < 0) col_w[i] = 0;
	}

	std::vector<element::ptr> items;
	for(auto& el : m_children)
	{
		if(!el || !el->is_visible()) continue;
		element_position ep = el->get_element_position();
		if(ep == element_position_absolute || ep == element_position_fixed) continue;
		if(el->is_white_space()) continue;
		switch(el->get_display())
		{
		case display_inline:		el->set_display(display_block);		break;
		case display_inline_block:	el->set_display(display_block);		break;
		case display_inline_flex:	el->set_display(display_flex);		break;
		default:						break;
		}
		items.push_back(el);
	}

	int bottom = 0;
	int cur_row_h = 0;
	for(size_t i = 0; i < items.size(); i++)
	{
		int col = (int)(i % (size_t)n);

		if(col == 0 && i != 0)
		{
			bottom += cur_row_h + row_gap;
			cur_row_h = 0;
		}

		int ix = 0;
		for(int c = 0; c < col; c++)
			ix += col_w[c] + col_gap;

		element::ptr el = items[i];
		int outer = col_w[col];
		int cross = 0;
		/* Item coordinates are relative to this grid container's content-box
		 * origin (m_pos already holds that origin). Seeding ix with m_pos.x and
		 * y with m_pos.y double-counted the container offset during the draw
		 * pass, shifting every tile of a non-left-aligned grid to the right. */
		if(el->get_display() == display_inline_text)
		{
			litehtml::size sz;
			el->get_content_size(sz, outer);
			el->m_pos = sz;
			el->m_pos.x = ix;
			el->m_pos.y = bottom;
			cross = sz.height;
		}
		else
		{
			el->render(ix, bottom, outer, second_pass);
			cross = el->get_position().height + el->margin_top() + el->margin_bottom();
		}
		if(cross > cur_row_h) cur_row_h = cross;
	}
	bottom += cur_row_h;

	m_pos.width = width_auto ? avail : avail;
	m_pos.height = bottom;
	calc_auto_margins(parent_width);

	m_pos.move_to(x, y);
	m_pos.x += content_margins_left();
	m_pos.y += content_margins_top();

	ret_width = avail + content_margins_left() + content_margins_right();
	return ret_width;
}
