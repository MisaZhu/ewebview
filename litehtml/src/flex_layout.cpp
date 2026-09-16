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
		/* non-empty: anonymous flex item wrapping a contiguous text run
		 * (words + separating spaces); real CSS boxifies such runs into a
		 * single item, per-word items would overlap when the line shrinks */
		std::vector<element::ptr>	run;
		int				order;
		float			grow;
		float			shrink;
		int				base;		// outer main size before free-space distribution
		bool			has_main;	// base came from flex-basis / width
		int				ml, mr, mt, mb;
		bool			aml, amr;	// auto margins: container-resolved, see collection
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

/* Preferred (max-content-ish) width of a laid-out subtree. Block-level boxes
 * report their *available* width as their own box width (block semantics), so
 * when such a box is a flex item with flex-basis:auto its base size must be
 * recovered from the children the last render() placed inside it; otherwise
 * every auto-width block item (e.g. a <li> in a nav row) measures as the whole
 * row and the shrink step slices the row into equal strips.
 * Child positions are deliberately NOT consulted: a centered line box or a
 * justify-content:center flex row stores the centering lead in left(), which
 * would inflate the preferred width by that lead. Inline siblings share one
 * line (widths add up), block-level children stack (widths compete). */
/* Max-content width of a grid container; -1 when there is no column
 * template (the generic block loop's max-of-children is correct then). */
static int grid_max_content_width(litehtml::html_tag* el, int avail);

static int preferred_content_width(const litehtml::element::ptr& el)
{
	if(!el) return 0;
	litehtml::style_display d = el->get_display();
	if(el->is_replaced())
	{
		/* A replaced box with a flex display is a form widget styled as a flex
		 * container (w3.org's nav triggers are <button> styled display:flex):
		 * the widget places/paints its children itself at draw time, so they
		 * carry no laid-out width to sum (0 on the first pass, then whatever a
		 * draw-time placement left on a pseudo - a ratchet that collapsed the
		 * nav buttons to caret width), and width() is the FILL width of the
		 * last pass (the whole nav row), which inflates the owning li's flex
		 * base into a shrink/wrap ratchet. Its max-content is the widget
		 * intrinsic size, which is context-free. Any other replaced element
		 * (an <img> the page CSS scales down from a 512px source to 16px, say)
		 * must keep width(): its intrinsic size ignores the CSS scaling and
		 * would blow the row budget instead. */
		if(d == litehtml::display_flex || d == litehtml::display_inline_flex)
		{
			litehtml::size sz;
			sz.width = 0; sz.height = 0;
			el->get_content_size(sz, 0);
			return sz.width;
		}
		return el->width();
	}
	if(d == litehtml::display_grid || d == litehtml::display_inline_grid)
	{
		/* A fit-content cross size (column flex align-items:center) built from
		 * the generic block loop takes the MAX child width, squeezing a
		 * multi-column grid (apple.com's .tile-ctas CTA pair) to one item
		 * wide: the track group overflows right and the pair looks
		 * off-centre. Sum the columns instead. */
		int gw = grid_max_content_width(static_cast<litehtml::html_tag*>(el), 0x3fffffff);
		if(gw >= 0) return gw;
	}
	if(d == litehtml::display_inline_text)
	{
		/* width() is whatever the LAST layout left behind - zero on the first
		 * pass - so a fit-content cross size built from it collapses the box
		 * to one word per line (apple.com's hero headline inside
		 * .tile-content{align-items:center}). Measure the run unwrapped:
		 * max-content is by definition the single-line width. */
		litehtml::size sz;
		sz.width = 0;
		sz.height = 0;
		el->get_content_size(sz, 0x3fffffff);
		return sz.width;
	}
	/* A specified definite width IS the box's max-content contribution: a leaf
	 * box sized by width (the w3.org nav chevron ::after{inline-size:.4375rem},
	 * mapped onto width) contributes that width, not the sum of its (empty)
	 * children. Under-counting it shaves a few px off the owning flex item's
	 * base size, so the shrink step slices into the item's text and the nav
	 * row wraps/overlaps where real flexbox packs it on one line. Percentages
	 * stay content-based: against an unknown shrink target they are indefinite.
	 * border-box widths already include padding and borders. */
	{
		litehtml::css_length cw = el->get_css_width();
		if(!cw.is_predefined() && cw.units() != litehtml::css_units_none &&
		   cw.units() != litehtml::css_units_percentage)
		{
			int wv = el->get_document()->cvt_units(cw, el->get_font_size(), 0);
			if(wv > 0)
			{
				if(static_cast<litehtml::html_tag*>(el)->get_box_sizing() ==
				   litehtml::box_sizing_content_box)
					wv += el->padding_left() + el->padding_right() +
						  el->border_left() + el->border_right();
				return wv;
			}
		}
	}
	if(d == litehtml::display_inline_block)
	{
		return el->width();
	}
	if(d == litehtml::display_flex || d == litehtml::display_inline_flex)
	{
		/* row flex: the items sit side by side, so their widths add up */
		int w = 0;
		size_t n = el->get_children_count();
		for(size_t i = 0; i < n; i++)
		{
			litehtml::element::ptr c = el->get_child((int)i);
			if(!c || !c->is_visible()) continue;
			if(c->get_element_position() == litehtml::element_position_absolute ||
			   c->get_element_position() == litehtml::element_position_fixed) continue;
			w += c->margin_left() + c->margin_right() + preferred_content_width(c);
		}
		return w + el->padding_left() + el->padding_right() +
			   el->border_left() + el->border_right();
	}
	int w = 0;
	int line = 0;
	size_t n = el->get_children_count();
	for(size_t i = 0; i < n; i++)
	{
		litehtml::element::ptr c = el->get_child((int)i);
		if(!c || !c->is_visible()) continue;
		if(c->get_element_position() == litehtml::element_position_absolute ||
		   c->get_element_position() == litehtml::element_position_fixed) continue;
		int mw = c->margin_left() + c->margin_right();
		if(c->is_break())
		{
			if(line > w) w = line;
			line = 0;
			continue;
		}
		litehtml::style_display cd = c->get_display();
		if(cd == litehtml::display_inline || cd == litehtml::display_inline_text ||
		   cd == litehtml::display_inline_block || cd == litehtml::display_inline_flex ||
		   cd == litehtml::display_inline_grid || cd == litehtml::display_inline_table)
		{
			/* inline-level siblings flow into the same line */
			line += mw + preferred_content_width(c);
		}
		else
		{
			if(line > w) w = line;
			line = 0;
			int cw = mw + preferred_content_width(c);
			if(cw > w) w = cw;
		}
	}
	if(line > w) w = line;
	return w + el->padding_left() + el->padding_right() +
		   el->border_left() + el->border_right();
}

/* Height of a bare text flex item: line-height (number x font-size, or an
 * explicit length) instead of the raw font box, so a button with
 * line-height:1.5 gets the same cross size a line box would give it. */
static int flex_text_height(const litehtml::element::ptr& el, int fallback)
{
	const litehtml::tchar_t* lh = el->get_style_property(_t("line-height"), true, _t("normal"));
	if(!lh) return fallback;
	litehtml::css_length l;
	l.fromString(lh);
	if(l.is_predefined()) return fallback;
	int fs = el->get_font_size();
	if(fs <= 0 && el->parent()) fs = el->parent()->get_font_size();
	if(l.units() == litehtml::css_units_none)
		return (int)(l.val() * fs + 0.5f);
	return el->get_document()->cvt_units(l, fs, 0);
}

/* Greedy word wrap of an anonymous text run into inner px of main size.
 * With place set, every word node's m_pos is written relative to (ox, oy),
 * the flex container's content-box origin, exactly like the single-text
 * placement path. Returns the wrapped block height. */
static int flex_run_wrap(const std::vector<litehtml::element::ptr>& run, int inner,
		bool place, int ox, int oy)
{
	int lx = 0, ly = 0, lh = 0, pend = 0;
	for(size_t i = 0; i < run.size(); i++)
	{
		const litehtml::element::ptr& nd = run[i];
		if(nd->is_white_space())
		{
			if(lx > 0)
			{
				litehtml::size ssz;
				nd->get_content_size(ssz, inner);
				pend = ssz.width;
			}
			continue;
		}
		litehtml::size sz;
		nd->get_content_size(sz, inner);
		int nlh = flex_text_height(nd, sz.height);
		if(lx > 0 && lx + pend + sz.width > inner)
		{
			ly += lh;
			lx = 0;
			pend = 0;
			lh = 0;
		}
		int nx = (lx > 0) ? lx + pend : 0;
		if(place)
		{
			litehtml::position& np = nd->get_position();
			np.x		= ox + nx;
			np.y		= oy + ly;
			np.width	= sz.width;
			np.height	= nlh;
		}
		lx = nx + sz.width;
		pend = 0;
		if(nlh > lh) lh = nlh;
	}
	return ly + lh;
}

/* Automatic minimum size of a flex item (min-content): shrinking below the
 * widest unsplittable word makes glyphs overflow into the neighbour item,
 * which is exactly the nav-row overlap real flexbox never shows. */
static int flex_min_content_inner(const litehtml::element::ptr& el)
{
	if(!el) return 0;
	/* A specified definite width IS the box's min-content size. Falling back
	 * to children / replaced intrinsics instead clamped w3.org's .logo-link
	 * shrink to the SVG logo's 480px default width (its span carries
	 * width:clamp(...)), inflating the logo item fourfold and pushing the
	 * whole nav row off screen. Percentages stay content-based: against an
	 * unknown shrink target they are indefinite. */
	litehtml::css_length cw = el->get_css_width();
	if(!cw.is_predefined() && cw.units() != litehtml::css_units_none &&
	   cw.units() != litehtml::css_units_percentage)
	{
		int wv = el->get_document()->cvt_units(cw, el->get_font_size(), 0);
		if(wv > 0) return wv;
	}
	litehtml::style_display d = el->get_display();
	bool flexd = (d == litehtml::display_flex || d == litehtml::display_inline_flex);
	if(el->is_replaced() && !flexd)
	{
		/* Intrinsic, not the last laid-out width: reading m_pos here fed the
		 * previous pass' resolved size back into the shrink clamp and ratcheted
		 * the item wider on every relayout. But the raw source intrinsic is
		 * wrong too when the page scales the box through the other axis
		 * (.icon{height:16px} on a 512x512 source): the shrink floor then blew
		 * the item past the row budget and pushed the trailing nav items off
		 * screen. Resolve a definite CSS height through the aspect ratio, the
		 * way el_image::render does; a definite CSS width is handled above. */
		litehtml::size sz;
		sz.width = 0; sz.height = 0;
		el->get_content_size(sz, 0);
		litehtml::css_length ch = el->get_css_height();
		if(!ch.is_predefined() && ch.units() != litehtml::css_units_percentage &&
		   sz.width > 0 && sz.height > 0)
		{
			int hv = el->get_document()->cvt_units(ch, el->get_font_size(), 0);
			if(hv > 0)
				return (int)((float)hv * (float)sz.width / (float)sz.height);
		}
		return sz.width;
	}
	if(d == litehtml::display_inline_text)
	{
		/* min-content of a text run is its LONGEST WORD. Reading the laid-out
		 * run width made every shrink floor equal the current base, so an
		 * over-budget row could never shrink and overflowed the container
		 * instead (narrow windows pushed the trailing nav items off screen).
		 * With white-space:nowrap the run cannot split at all, so its
		 * min-content IS the full text width (apple.com's CTA pills). */
		litehtml::tstring t;
		el->get_text(t);
		litehtml::uint_ptr f = el->get_font();
		litehtml::document* doc = el->get_document();
		if(!f || !doc || !doc->container()) return el->width();
		litehtml::white_space ws = el->get_white_space();
		if(ws == litehtml::white_space_nowrap || ws == litehtml::white_space_pre)
		{
			return doc->container()->text_width(t.c_str(), f);
		}
		int w = 0;
		std::string cur;
		for(const char* p = t.c_str(); ; p++)
		{
			if(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == 0)
			{
				if(!cur.empty())
				{
					int cw = doc->container()->text_width(cur.c_str(), f);
					if(cw > w) w = cw;
					cur.clear();
				}
				if(*p == 0) break;
			}
			else cur += *p;
		}
		return w;
	}
	if(d == litehtml::display_inline_block)
	{
		return el->width();
	}
	int w = 0;
	size_t n = el->get_children_count();
	/* Under white-space:nowrap the whole inline run stays on ONE line, so the
	 * floor is the SUM of the inline children, not the widest of them. Splitting
	 * "Learn more" into text nodes and taking the max measured only the longest
	 * fragment, which shrank apple.com's min-content CTA tracks below the label
	 * and let the text spill out of the pill. */
	litehtml::white_space ws = el->get_white_space();
	bool one_line = flexd || ws == litehtml::white_space_nowrap || ws == litehtml::white_space_pre;
	for(size_t i = 0; i < n; i++)
	{
		litehtml::element::ptr c = el->get_child((int)i);
		if(!c || !c->is_visible()) continue;
		if(c->get_element_position() == litehtml::element_position_absolute ||
		   c->get_element_position() == litehtml::element_position_fixed) continue;
		int cw = flex_min_content_inner(c);
		if(one_line)
		{
			/* row flex: the items sit side by side even at min-content, so
			 * their floors add up (a replaced flex box - w3.org's nav button
			 * widget - reaches here: its label can clip, so its floor is the
			 * longest label word plus the caret, not the full label) */
			int ml = c->margin_left(), mr = c->margin_right();
			/* An automatic margin (w3.org account li margin-inline-start:auto)
			 * still holds the free space it absorbed on the last pack; feeding
			 * that back inflated the nav ul's min-content floor above the row
			 * budget so the row could never shrink and the account item landed
			 * past the right edge. Auto margins contribute 0 to intrinsics. */
			const litehtml::tchar_t* tn = c->get_tagName();
			if(tn && tn[0])
			{
				litehtml::html_tag* ct = static_cast<litehtml::html_tag*>(c);
				if(ct->margin_left_is_auto()) ml = 0;
				if(ct->margin_right_is_auto()) mr = 0;
			}
			w += ml + mr + cw;
		}
		else if(cw > w)
		{
			w = cw;
		}
	}
	return w;
}

static int flex_min_content(const litehtml::element::ptr& el)
{
	if(!el) return 0;
	return flex_min_content_inner(el) + el->padding_left() + el->padding_right() +
		   el->border_left() + el->border_right();
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
		/* Real column flex. Degrading to block flow lost BOTH axes' alignment:
		 * apple.com centres each tile's copy with align-items:center and pins
		 * the AirPods tile copy to the bottom with justify-content:flex-end,
		 * neither of which block flow can express (the copy rendered full-width
		 * at the top, so callouts sat left and overlaid art). Main axis is
		 * vertical here, cross axis horizontal. */
		bool col_reverse = flex_flag(dir_s, "column-reverse");
		const tchar_t* col_jc = get_style_property(_t("justify-content"), false, _t("flex-start"));
		const tchar_t* col_ai = get_style_property(_t("align-items"), false, _t("stretch"));

		std::vector<element::ptr> kids;
		std::vector<int> ord;
		for(auto& el : m_children)
		{
			if(!el || !el->is_visible()) continue;
			element_position ep = el->get_element_position();
			if(ep == element_position_absolute || ep == element_position_fixed) continue;
			if(el->is_white_space()) continue;
			if(el->get_display() == display_contents) continue;
			const tchar_t* o = el->get_style_property(_t("order"), false, _t("0"));
			ord.push_back(o ? atoi(o) : 0);
			kids.push_back(el);
		}
		{
			std::vector<size_t> idx(kids.size());
			for(size_t i = 0; i < idx.size(); i++) idx[i] = i;
			std::stable_sort(idx.begin(), idx.end(),
					[&ord](size_t a, size_t b){ return ord[a] < ord[b]; });
			std::vector<element::ptr> sorted;
			sorted.reserve(idx.size());
			for(size_t i = 0; i < idx.size(); i++) sorted.push_back(kids[idx[i]]);
			kids.swap(sorted);
			if(col_reverse) std::reverse(kids.begin(), kids.end());
		}

		int row_gap = 0, col_gap = 0;
		flex_parse_gap(this, avail, row_gap, col_gap);
		bool ai_stretch = flex_flag(col_ai, "stretch", "normal");

		int n = (int)kids.size();
		std::vector<int> cw(n, 0), chh(n, 0), mtv(n, 0), mbv(n, 0);
		int total = 0;
		for(int i = 0; i < n; i++)
		{
			mtv[i] = kids[i]->margin_top();
			mbv[i] = kids[i]->margin_bottom();
			int w;
			if(ai_stretch)
			{
				w = avail;
			}
			else
			{
				/* cross size is fit-content: max-content capped by the line.
				 * +1px slack: preferred widths truncate float text advances, and
				 * a box one pixel short of the one-line width wraps the headline
				 * (apple.com's "iPhone 18 Pro" hero). */
				w = preferred_content_width(kids[i]) + 1;
				if(w > avail) w = avail;
				if(w < 0) w = 0;
			}
			kids[i]->render(0, 0, w, second_pass);
			/* Main size is the child's OUTER height: boxes whose space comes from
			 * padding alone (the .l-frame aspect-ratio trick: content height 0 +
			 * padding-bottom, w3.org's card images) otherwise contribute nothing
			 * and the column collapses over them. */
			chh[i] = kids[i]->get_position().height + mtv[i] + mbv[i]
				+ kids[i]->padding_top() + kids[i]->padding_bottom()
				+ kids[i]->border_top() + kids[i]->border_bottom();
			/* Cross size is the child's ACTUAL outer width, not the width we
			 * offered: a child with a declared width of its own but no in-flow
			 * content (apple.com's gallery strip, .media-gallery-item-container
			 * with width:calc(var+var) and every tile position:absolute) probes
			 * preferred_content_width()==0, so the probe was 1px and
			 * align-items:center then "centred" a 701px strip at (avail-1)/2 -
			 * the left edge landed on the viewport middle. */
			cw[i] = kids[i]->get_position().width
				+ kids[i]->padding_left() + kids[i]->padding_right()
				+ kids[i]->border_left() + kids[i]->border_right()
				+ kids[i]->margin_left() + kids[i]->margin_right();
			if(cw[i] > avail) cw[i] = avail;
			total += chh[i];
			if(i + 1 < n) total += row_gap;
		}

		int fixed_h = 0;
		bool has_fixed_h = !m_css_height.is_predefined() && m_css_height.units() != css_units_none;
		if(has_fixed_h)
		{
			fixed_h = get_document()->cvt_units(m_css_height, m_font_size, 0);
			if(m_box_sizing == box_sizing_border_box)
				fixed_h -= m_padding.top + m_padding.bottom + m_borders.top + m_borders.bottom;
			if(fixed_h < 0) fixed_h = 0;
		}
		int cont_h = has_fixed_h ? std::max(total, fixed_h) : total;
		if(!m_css_min_height.is_predefined() && m_css_min_height.units() != css_units_none)
		{
			int mh = get_document()->cvt_units(m_css_min_height, m_font_size, 0);
			if(m_box_sizing == box_sizing_border_box)
				mh -= m_padding.top + m_padding.bottom + m_borders.top + m_borders.bottom;
			if(mh > cont_h) cont_h = mh;
		}

		/* main-axis (vertical) packing of leftover space */
		int lead = 0, jgap = 0;
		int free = cont_h - total;
		if(free > 0)
		{
			if(flex_flag(col_jc, "center")) lead = free / 2;
			else if(flex_flag(col_jc, "flex-end", "end")) lead = free;
			else if(flex_flag(col_jc, "space-between") && n > 1) jgap = free / (n - 1);
			else if(flex_flag(col_jc, "space-around") && n > 0) { jgap = free / n; lead = jgap / 2; }
			else if(flex_flag(col_jc, "space-evenly") && n > 0) { jgap = free / (n + 1); lead = jgap; }
		}

		int yy = lead;
		for(int i = 0; i < n; i++)
		{
			int ix = 0;
			if(!ai_stretch)
			{
				if(flex_flag(col_ai, "center")) ix = (avail - cw[i]) / 2;
				else if(flex_flag(col_ai, "flex-end", "end")) ix = avail - cw[i];
			}
			kids[i]->render(ix, yy, cw[i], second_pass);
			yy += chh[i] + row_gap + jgap;
		}

		int used_w = 0;
		for(int i = 0; i < n; i++) if(cw[i] > used_w) used_w = cw[i];
		m_pos.width = width_auto ? (m_display == display_inline_flex ? std::min(avail, used_w) : avail) : avail;
		m_pos.height = cont_h;
		calc_auto_margins(parent_width);
		/* No min-height re-clamp here: cont_h above already applied min-height
		 * unit-converted and, for box-sizing:border-box, shrunk by the padding
		 * and borders. Re-clamping with the raw value grows the CONTENT box by
		 * that padding again - apple.com's column-flex tiles came out too tall,
		 * shoving their bottom:0 artwork down and clipping it. */
		m_pos.move_to(x, y);
		m_pos.x += content_margins_left();
		m_pos.y += content_margins_top();
		return avail + content_margins_left() + content_margins_right();
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
		if(el->is_white_space())
		{
			/* Whitespace separates words *inside* an anonymous text run; on its
			 * own (source formatting between element siblings) it is no box. */
			if(!items.empty() && !items.back().run.empty())
			{
				items.back().run.push_back(el);
			}
			continue;
		}
		if(el->get_display() == display_contents) continue; /* box-less wrapper */
		if(el->get_display() == display_inline_text)
		{
			/* contiguous text boxifies into ONE anonymous flex item */
			if(!items.empty() && !items.back().run.empty())
			{
				items.back().run.push_back(el);
			}
			else
			{
				flex_item it;
				it.el		= el;
				it.run.push_back(el);
				it.grow		= 0;
				it.shrink	= 1;
				it.base		= 0;
				it.has_main	= false;
				it.main		= 0;
				it.cross	= 0;
				it.ml = it.mr = it.mt = it.mb = 0;
				it.aml = it.amr = false;
				it.order	= 0;
				items.push_back(it);
			}
			continue;
		}

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
		/* Auto margins on a flex item are the CONTAINER's to resolve (they
		 * absorb leftover free space after flexing). Left alone the item's own
		 * render() runs calc_auto_margins against its own width and the
		 * resolved value feeds the next pass' base size - a positive feedback
		 * loop that walked w3.org's nav button (margin-inline-start:auto)
		 * ~1000px further right on every relayout until the menu ul sat at
		 * x=15000, off screen. Track them and zero them for packing. */
		{
			html_tag* iht = static_cast<html_tag*>(el);
			it.aml = iht->m_css_margins.left.is_predefined();
			it.amr = iht->m_css_margins.right.is_predefined();
		}
		if(it.aml) it.ml = 0;
		if(it.amr) it.mr = 0;

		const tchar_t* v = el->get_style_property(_t("order"), false, _t("0"));
		it.order = v ? atoi(v) : 0;

		const tchar_t* sh = el->get_style_property(_t("flex"), false, 0);
		bool basis_set = false;
		if(sh)
		{
			/* Tokenise instead of sscanf: `%f %f %f` cannot tell `flex:0 0`
			 * (basis 0%) from `flex:0 0 auto` (basis auto, i.e. fall back to the
			 * specified width). The w3.org nav avatar is `flex:0 0 auto` +
			 * `width:2rem`; reading it as basis 0 collapsed the circle to 0px. */
			tstring ssh = sh;
			std::vector<tstring> tok;
			for(size_t p = 0; p < ssh.length();)
			{
				while(p < ssh.length() && (ssh[p] == _t(' ') || ssh[p] == _t('\t'))) p++;
				size_t q = p;
				while(q < ssh.length() && ssh[q] != _t(' ') && ssh[q] != _t('\t')) q++;
				if(q > p) tok.push_back(ssh.substr(p, q - p));
				p = q;
			}
			float g = 0, s = 1;
			if(tok.size() >= 1) g = (float)atof(tok[0].c_str());
			if(tok.size() >= 2) s = (float)atof(tok[1].c_str());
			it.grow = g;
			it.shrink = s;
			bool basis_auto = (tok.size() >= 3);
			if(tok.size() >= 3)
			{
				css_length bl;
				bl.fromString(tok[2].c_str());
				if(!bl.is_predefined())
				{
					it.base = get_document()->cvt_units(bl, el->get_font_size(), avail) + it.ml + it.mr;
					it.has_main = true;
					basis_set = true;
					basis_auto = false;
				}
			}
			if(!basis_set && !basis_auto)
			{
				/* The flex shorthand resets flex-basis to 0% when it is omitted
				 * (`flex:1` == `1 1 0%`), unlike the longhand's auto. Falling back
				 * to width made `.seed-row input{flex:1;width:100%}` measure a
				 * 100%-of-container base and overflow the row. */
				it.base = it.ml + it.mr;
				it.has_main = true;
				basis_set = true;
			}
		}
		v = el->get_style_property(_t("flex-grow"), false, 0);
		if(v) it.grow = (float)atof(v);
		v = el->get_style_property(_t("flex-shrink"), false, 0);
		if(v) it.shrink = (float)atof(v);
		v = el->get_style_property(_t("flex-basis"), false, 0);
		if(v && !flex_flag(v, "auto", "content"))
		{
			css_length bl;
			bl.fromString(v);
			if(!bl.is_predefined())
			{
				it.base = get_document()->cvt_units(bl, el->get_font_size(), avail) + it.ml + it.mr;
				it.has_main = true;
				basis_set = true;
			}
		}
		css_length cw = el->get_css_width();
		/* css_units_none means "no explicit width" (auto / unset). A default-constructed
		 * css_length has is_predefined()==false yet units none and value 0 — that is what
		 * text nodes and other never-width-styled items report. Treating it as an explicit
		 * 0 width collapsed them (e.g. the text inside a `display:flex` nav-link measured
		 * base=0 and was never rendered). Require a real unit before using width as base.
		 * A specified flex-basis (shorthand third component or longhand) wins over width
		 * for the base size, per spec; width only supplies it when the basis is auto —
		 * otherwise `flex:1 1 0` + `width:100%` items (primer PageLayout columns) would
		 * each fill a flex line and wrap into a vertical stack. */
		if(!basis_set && !cw.is_predefined() && cw.units() != css_units_none)
		{
			it.base = get_document()->cvt_units(cw, el->get_font_size(), avail) + it.ml + it.mr;
			it.has_main = true;
		}
		items.push_back(it);
	}

	/* drop trailing separators; a whitespace-only run is not an item */
	for(size_t i = items.size(); i-- > 0;)
	{
		if(items[i].run.empty()) continue;
		while(!items[i].run.empty() && items[i].run.back()->is_white_space())
		{
			items[i].run.pop_back();
		}
		if(items[i].run.empty()) items.erase(items.begin() + i);
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
			if(!it.run.empty())
			{
				/* max-content: all words plus separators on one line.
				 * Measure via get_content_size (m_size): m_pos.width is only
				 * written once the run is placed, so width() is stale here. */
				int w = 0;
				for(size_t k = 0; k < it.run.size(); k++)
				{
					litehtml::size sz;
					it.run[k]->get_content_size(sz, avail);
					w += sz.width;
				}
				it.base = w + it.ml + it.mr;
			}
			else
			{
				/* pin auto margins to their packed value so the item's own
				 * calc_auto_margins cannot re-resolve them against its own
				 * width (feedback loop, see collection note) */
				html_tag* mht = static_cast<html_tag*>(it.el);
				css_length sml = mht->m_css_margins.left, smr = mht->m_css_margins.right;
				if(it.aml){ css_length v; v = (float)it.ml; mht->m_css_margins.left = v; }
				if(it.amr){ css_length v; v = (float)it.mr; mht->m_css_margins.right = v; }
				it.base = it.el->render(0, 0, avail, second_pass);
				mht->m_css_margins.left = sml;
				mht->m_css_margins.right = smr;
				/* block-level containers fill the available width (block
				 * semantics); recover the content preferred width so the flex
				 * base size is max-content, as the spec requires */
				litehtml::style_display d = it.el->get_display();
				if(!it.el->is_replaced() &&
				   (d == litehtml::display_block || d == litehtml::display_list_item))
				{
					litehtml::css_length iw = it.el->get_css_width();
					if(iw.is_predefined() || iw.units() == litehtml::css_units_none)
					{
						int pw = preferred_content_width(it.el);
						if(pw > 0) it.base = pw + it.ml + it.mr;
					}
				}
			}
		}
		if(it.base < it.ml + it.mr) it.base = it.ml + it.mr;
	}

	/* An inline-flex atom sizes to its content (max-content), so resolve its
	 * own avail from the items before packing: packing first at the line's
	 * avail bakes the justify-content lead into the used width, which then
	 * widens the atom and shifts its label off the padding edge. */
	if(width_auto && m_display == display_inline_flex && !items.empty())
	{
		int intrinsic = 0;
		for(size_t i = 0; i < items.size(); i++) intrinsic += items[i].base;
		intrinsic += col_gap * (int)(items.size() - 1);
		avail = intrinsic;
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

	/* Render a non-run item at its resolved main size while telling the item
	 * that its percentage padding/margin/borders resolve against THIS
	 * container's content width: per spec a flex item's containing block is
	 * the flex container content box, never the item's own used size. Without
	 * the override w3.org's `.component--columns > ul li { padding:1.5% }`
	 * resolved against the shrunken item while the ul's matching
	 * `margin:-1.5%` resolved against the container, netting a negative lead
	 * that drifted the whole first column under the container's overflow
	 * clip. Only percentage outline lengths observe the override (cvt_units
	 * ignores the width base for absolute units), so the item's internal
	 * layout is untouched. */
	auto render_item = [&](flex_item& it, int ix, int iy)
	{
		html_tag* ht = static_cast<html_tag*>(it.el);
		int saved = ht->m_pct_cb_width;
		ht->m_pct_cb_width = avail;
		css_length sml = ht->m_css_margins.left, smr = ht->m_css_margins.right;
		if(it.aml){ css_length v; v = (float)it.ml; ht->m_css_margins.left = v; }
		if(it.amr){ css_length v; v = (float)it.mr; ht->m_css_margins.right = v; }
		it.el->render(ix, iy, it.main, second_pass);
		ht->m_css_margins.left = sml;
		ht->m_css_margins.right = smr;
		ht->m_pct_cb_width = saved;
	};

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
	/* min-height is just as definite a cross size as height for a single flex
	 * line: apple.com's .tile-wrapper{min-height:var(--hero-content-height)}
	 * must stretch .tile-content to the full tile so the column inside can
	 * push its copy to the bottom (justify-content:flex-end/space-between). */
	int cont_min_h = 0;
	if(!m_css_min_height.is_predefined() && m_css_min_height.units() != css_units_none)
	{
		cont_min_h = get_document()->cvt_units(m_css_min_height, m_font_size, 0);
		if(m_box_sizing == box_sizing_border_box)
		{
			cont_min_h -= m_padding.top + m_padding.bottom + m_borders.top + m_borders.bottom;
		}
		if(cont_min_h < 0) cont_min_h = 0;
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
				/* automatic minimum size: never below min-content */
				int minc = 0;
				if(!it.run.empty())
				{
					for(size_t k = 0; k < it.run.size(); k++)
					{
						litehtml::size sz;
						it.run[k]->get_content_size(sz, avail);
						if(sz.width > minc) minc = sz.width;
					}
				}
				else
				{
					minc = flex_min_content(it.el);
				}
				minc += it.ml + it.mr;
				if(it.main < minc) it.main = minc;
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

		/* auto margins absorb leftover free space before justify-content */
		{
			int na = 0;
			for(size_t i = 0; i < line.size(); i++)
			{
				if(items[line[i]].aml) na++;
				if(items[line[i]].amr) na++;
			}
			if(na > 0 && free > 0)
			{
				int share = free / na;
				for(size_t i = 0; i < line.size(); i++)
				{
					flex_item& it = items[line[i]];
					if(it.aml){ it.ml += share; it.main += share; }
					if(it.amr){ it.mr += share; it.main += share; }
				}
				free = 0;
			}
		}

		// lay the items out at their final main size to learn cross sizes
		int line_cross = 0;
		for(size_t i = 0; i < line.size(); i++)
		{
			flex_item& it = items[line[i]];
			if(!it.run.empty())
			{
				/* wrap the run at the resolved main size so its height feeds line_cross */
				int inner = it.main - it.ml - it.mr;
				if(inner < 0) inner = 0;
				it.cross = flex_run_wrap(it.run, inner, false, 0, 0) + it.mt + it.mb;
			}
			else
			{
				render_item(it, 0, 0);
				it.cross = it.el->get_position().height + it.mt + it.mb;
			}
			if(it.cross > line_cross) line_cross = it.cross;
		}

		/* a single line in a definite-height container fills it, so align-items
		 * centers within the container rather than the tallest item */
		int line_h = line_cross;
		if(lines.size() == 1)
		{
			if(has_fixed_h && fixed_h > line_cross) line_h = fixed_h;
			if(cont_min_h > line_h) line_h = cont_min_h;
		}

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
			bool is_run = !it.run.empty();
			bool placed = false;
			css_length ch = it.el->get_css_height();
			if(!is_run && flex_flag(ai_s, "stretch", "normal") && ch.is_predefined())
			{
				iy = 0;
				/* True stretch: give the item a definite height for the duration
				 * of its placement render so percentage-height children (w3.org's
				 * nav links carry height:100%) resolve against the stretched box
				 * and center their content in it. Writing m_pos.height directly
				 * either got wiped by the render or leaked into the next pass'
				 * measurement and ratcheted the row taller every relayout. */
				html_tag* sht = static_cast<html_tag*>(it.el);
				css_length shv = sht->m_css_height;
				int fh = line_h - it.mt - it.mb;
				if(fh < 0) fh = 0;
				css_length fv; fv = (float)fh;
				sht->m_css_height = fv;
				render_item(it, xs[i], bottom + iy);
				sht->m_css_height = shv;
				it.cross = line_h;
				/* The generic placement render below must NOT run again: it
				 * would re-layout the item without the forced height and undo
				 * everything the stretched render arranged (apple.com's column
				 * .tile-content lost its justify-content:flex-end anchoring). */
				placed = true;
			}
			else if(flex_flag(ai_s, "center"))
			{
				iy = (line_h - it.cross) / 2;
			}
			else if(flex_flag(ai_s, "flex-end", "end"))
			{
				iy = line_h - it.cross;
			}
			if(is_run)
			{
				/* place every word of the run; coordinates are relative to this
				 * flex container's content-box origin (m_pos already holds that
				 * origin): adding m_pos.x again would double-count the container's
				 * own offset during the recursive draw pass. */
				int inner = it.main - it.ml - it.mr;
				if(inner < 0) inner = 0;
				flex_run_wrap(it.run, inner, true, xs[i] + it.ml, bottom + iy + it.mt);
			}
			else if(!placed)
			{
				render_item(it, xs[i], bottom + iy);
			}
		}

		/* The line's contribution to the container's used width is the packed
		 * content only: the justify-content lead (and the extra space-around /
		 * space-evenly gaps) is free space inside THIS container, so reporting
		 * it would inflate every shrink-to-fit ancestor (rokid.com's absolutely
		 * positioned hero copy box sized to avail/2 + content/2 instead of to
		 * its content, throwing the centred button row off the logo axis). */
		int line_used = 0;
		for(size_t i = 0; i < line.size(); i++)
		{
			line_used += items[line[i]].main;
			if(i + 1 < line.size()) line_used += col_gap;
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
	/* A declared height is the box height: content that does not fit overflows
	 * (and is clipped by overflow:hidden), it must NOT stretch the box. Taking
	 * max(bottom, fixed_h) let apple.com's global nav links (height:44px,
	 * white-space:nowrap) grow to 90-117px around an oversized inner icon,
	 * which stacked the nav into overlapping bands instead of one 44px row. */
	m_pos.height = has_fixed_h ? fixed_h : bottom;
	calc_auto_margins(parent_width);

	/* m_pos.height is the CONTENT height, so min-height has to be unit-converted
	 * and - for box-sizing:border-box - shrunk by the padding and borders before
	 * it is compared. Clamping with the raw value made apple.com's .tile-wrapper
	 * (min-height:var(--hero-content-height); box-sizing:border-box;
	 * padding:61px 0 69px) 130px too tall: its absolutely positioned bottom:0
	 * hero art then sat 130px lower than in Safari and was clipped by the tile's
	 * overflow:clip, and every section below shifted down with it. */
	int min_height = 0;
	if(!m_css_min_height.is_predefined() && m_css_min_height.units() != css_units_none)
	{
		min_height = get_document()->cvt_units(m_css_min_height, m_font_size, 0);
		if(m_box_sizing == box_sizing_border_box)
		{
			min_height -= m_padding.top + m_padding.bottom + m_borders.top + m_borders.bottom;
		}
		if(min_height < 0) min_height = 0;
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
		bool	is_fixed = false;
		int		fixed_w = 0;
		float	fr = 1;
		int		min_w = 0;			// minmax() lower bound, used by repeat(auto-fill)
		bool	is_content = false;	// min-content / max-content track
		bool	content_min = false;	// true = min-content, false = max-content
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
		else if(t == _t("min-content") || t == _t("max-content"))
		{
			tr.is_fixed = false;
			tr.fixed_w = 0;
			tr.fr = 0;
			tr.is_content = true;
			tr.content_min = (t == _t("min-content"));
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

/* grid-column / grid-row explicit placement -------------------------------
 * Google's Grid_column places items with grid-column-start:<line> and
 * grid-column-end:span calc(<end> - <start> + 1); litehtml previously flowed
 * every item into a single 1fr track, collapsing multi-column text to one
 * word per line. These helpers recover (column, span, row) from the cascade. */
static bool grid_place_int(const tstring& in, int& out)
{
	tstring s = in;
	trim(s);
	if(s.empty())
	{
		return false;
	}
	size_t i = 0;
	bool neg = false;
	if(s[0] == _t('-')) { neg = true; i = 1; }
	else if(s[0] == _t('+')) { i = 1; }
	int v = 0;
	bool any = false;
	for(; i < s.length(); i++)
	{
		if(s[i] < _t('0') || s[i] > _t('9'))
		{
			return false;
		}
		v = v * 10 + (s[i] - _t('0'));
		any = true;
	}
	if(!any)
	{
		return false;
	}
	out = neg ? -v : v;
	return true;
}

/* Additive integer expression, optionally wrapped in calc(): "calc(6 - 2 + 1)". */
static bool grid_place_calc(const tstring& in, int& out)
{
	tstring s = in;
	trim(s);
	if(s.find(_t("calc(")) == 0)
	{
		s = s.substr(5);
		if(!s.empty() && s[s.length() - 1] == _t(')'))
		{
			s = s.substr(0, s.length() - 1);
		}
	}
	int total = 0, sign = 1;
	tstring term;
	bool any = false;
	for(size_t i = 0; i <= s.length(); i++)
	{
		if(i == s.length() || s[i] == _t('+') || s[i] == _t('-'))
		{
			if(!term.empty())
			{
				int v;
				if(!grid_place_int(term, v)) return false;
				total += sign * v;
				any = true;
				term.clear();
			}
			if(i < s.length())
			{
				sign = (s[i] == _t('+')) ? 1 : -1;
			}
		}
		else if(s[i] != _t(' '))
		{
			term += s[i];
		}
	}
	if(!any)
	{
		return false;
	}
	out = total;
	return true;
}

static void grid_item_placement(html_tag* el, int n, int& col, int& span, int& row)
{
	col = -1; span = 1; row = -1;
	if(!el)
	{
		return;
	}
	tstring start_s, end_s;
	/* grid-area: row-start / col-start / row-end / col-end. apple.com stacks a
	 * card's artwork and its overlay with "grid-area:1/1" on both; without the
	 * shorthand the overlay auto-flows into a second row and the clip hides
	 * it. Explicit lines win over the longhands parsed below. */
	if(const tchar_t* ga = el->get_style_property(_t("grid-area"), false, 0))
	{
		string_vector tk;
		split_string(ga, tk, _t("/"));
		for(size_t i = 0; i < tk.size(); i++) trim(tk[i]);
		if(tk.size() >= 1)
		{
			int rv = 0;
			if(tk[0].find(_t("span")) != 0 && grid_place_calc(tk[0], rv) && rv >= 1) row = rv - 1;
		}
		if(tk.size() >= 2 && tk[1].find(_t("span")) != 0) start_s = tk[1];
		if(tk.size() >= 4 && tk[3].find(_t("span")) != 0) end_s = tk[3];
	}
	const tchar_t* gc = el->get_style_property(_t("grid-column"), false, 0);
	if(gc)
	{
		tstring g = gc;
		size_t slash = g.find(_t('/'));
		if(slash == tstring::npos)
		{
			start_s = g;
		}
		else
		{
			start_s = g.substr(0, slash);
			end_s = g.substr(slash + 1);
		}
	}
	if(const tchar_t* v = el->get_style_property(_t("grid-column-start"), false, 0)) start_s = v;
	if(const tchar_t* v = el->get_style_property(_t("grid-column-end"), false, 0)) end_s = v;

	int sl = 0, eln = 0;
	bool has_start = false, end_is_span = false, has_end_line = false;
	tstring st = start_s; trim(st);
	tstring en = end_s; trim(en);
	if(st.find(_t("span")) == 0)
	{
		int v; if(grid_place_calc(st.substr(4), v) && v > 0) { span = v; }
	}
	else if(grid_place_calc(st, sl))
	{
		if(sl >= 1) { col = sl - 1; has_start = true; }
	}
	if(en.find(_t("span")) == 0)
	{
		int v; if(grid_place_calc(en.substr(4), v) && v > 0) { span = v; end_is_span = true; }
	}
	else if(!en.empty() && grid_place_calc(en, eln))
	{
		has_end_line = true;
	}
	if(has_start)
	{
		if(!end_is_span && has_end_line)
		{
			span = (eln == -1) ? (n - col) : (eln - sl);
		}
		if(span < 1) span = 1;
		if(col + span > n) span = n - col;
	}
	else
	{
		if(span > n) span = n;
	}
	if(const tchar_t* v = el->get_style_property(_t("grid-row-start"), false, 0))
	{
		tstring rs = v; trim(rs);
		int rv;
		if(rs.find(_t("span")) != 0 && grid_place_calc(rs, rv) && rv >= 1)
		{
			row = rv - 1;
		}
	}
}

static int grid_max_content_width(litehtml::html_tag* el, int avail)
{
using namespace litehtml;
int row_gap = 0, col_gap = 0;
flex_parse_gap(el, avail, row_gap, col_gap);
const tchar_t* tc = el->get_style_property(_t("grid-template-columns"), false, 0);
if(!tc) return -1;
std::vector<grid_track> tracks;
grid_parse_tracks(tc, avail, el->get_font_size(), el->get_document(), tracks, col_gap);
int n = (int)tracks.size();
if(n < 2) return -1;

std::vector<element::ptr> items;
for(size_t i = 0; i < el->get_children_count(); i++)
{
element::ptr c = el->get_child((int)i);
if(!c || !c->is_visible()) continue;
element_position ep = c->get_element_position();
if(ep == element_position_absolute || ep == element_position_fixed) continue;
if(c->is_white_space()) continue;
if(c->get_display() == display_contents) continue;
items.push_back(c);
}

/* Same auto-flow cursor as render_grid so items land in the columns the
 * layout pass will actually use. */
std::vector<std::vector<char>> occ;
auto fits = [&](int r, int c, int sp) -> bool {
if(c < 0 || c + sp > n) return false;
if(r >= (int)occ.size()) return true;
for(int cc = c; cc < c + sp; cc++)
if(occ[r][cc]) return false;
return true;
};
auto mark = [&](int r, int c, int sp) {
while((int)occ.size() <= r) occ.push_back(std::vector<char>(n, 0));
for(int cc = c; cc < c + sp; cc++) occ[r][cc] = 1;
};
std::vector<int> colw(n, 0);
int cursor_r = 0, cursor_c = 0;
for(size_t k = 0; k < items.size(); k++)
{
int col = -1, span = 1, row = -1;
if(items[k]->get_display() != display_inline_text)
grid_item_placement(static_cast<html_tag*>(items[k]), n, col, span, row);
if(span < 1) span = 1;
if(span > n) span = n;
int r;
if(col >= 0)
{
r = (row >= 0) ? row : 0;
while(!fits(r, col, span)) r++;
cursor_r = r;
cursor_c = col + span;
if(cursor_c >= n) cursor_c = 0;
}
else
{
r = cursor_r;
int c = cursor_c;
while(true)
{
if(c + span > n) { r++; c = 0; }
if(fits(r, c, span)) break;
c++;
if(c >= n) { r++; c = 0; }
}
col = c;
cursor_r = r;
cursor_c = c + span;
if(cursor_c >= n) cursor_c = 0;
}
mark(r, col, span);
int iw = preferred_content_width(items[k]) + items[k]->margin_left() + items[k]->margin_right();
if(span == 1)
{
if(iw > colw[col]) colw[col] = iw;
}
else
{
int per = iw / span;
for(int c = col; c < col + span && c < n; c++)
if(per > colw[c]) colw[c] = per;
}
}
int total = 0;
for(int i = 0; i < n; i++)
{
int w = colw[i];
if(tracks[i].is_fixed && tracks[i].fixed_w > w) w = tracks[i].fixed_w;
total += w;
}
total += col_gap * (n - 1);
total += el->padding_left() + el->padding_right() +
 el->border_left() + el->border_right();
return total;
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
	{
		/* A grid with no grid-template-columns still gets ONE implicit column,
		 * but only when a child actually places itself: apple.com's gallery
		 * cards are display:grid with no template and stack artwork + overlay
		 * via grid-area:1/1, which block flow pushes below the clip. A template
		 * less grid whose children do NOT place themselves keeps the block
		 * fallback: apple's .tile-ctas gets its two button columns from
		 * ":has(:nth-child(2))", which the selector engine cannot match, and
		 * strict single-column auto-flow would stack the pills vertically. */
		bool placed = false;
		for(auto& el : m_children)
		{
			if(!el || !el->is_visible() || el->get_display() == display_inline_text) continue;
			if(el->get_style_property(_t("grid-area"), false, 0) ||
			   el->get_style_property(_t("grid-row"), false, 0) ||
			   el->get_style_property(_t("grid-row-start"), false, 0) ||
			   el->get_style_property(_t("grid-column"), false, 0) ||
			   el->get_style_property(_t("grid-column-start"), false, 0))
			{
				placed = true;
				break;
			}
		}
		if(!placed)
			return render_box(x, y, max_width, second_pass);
		grid_track one;
		one.is_fixed = false;
		one.fixed_w = 0;
		one.fr = 1;
		one.is_content = false;
		one.content_min = false;
		tracks.push_back(one);
	}

	int n = (int)tracks.size();
	int gaps = col_gap * (n - 1);
	bool have_content_track = false;
	for(int i = 0; i < n; i++)
		if(tracks[i].is_content) have_content_track = true;

	std::vector<element::ptr> items;
	for(auto& el : m_children)
	{
		if(!el || !el->is_visible()) continue;
		element_position ep = el->get_element_position();
		if(ep == element_position_absolute || ep == element_position_fixed) continue;
		if(el->is_white_space()) continue;
		if(el->get_display() == display_contents) continue; /* box-less wrapper */
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

	/* grid-auto-rows: minmax(L, ...) floors every row height; items then
	 * stretch to the row height (align-self:stretch default), which is what
	 * centers captions vertically inside the square member tiles. */
	int row_min = 0;
	const tchar_t* ar = get_style_property(_t("grid-auto-rows"), false, 0);
	if(ar)
	{
		std::vector<grid_track> rt;
		grid_parse_tracks(ar, avail, m_font_size, get_document(), rt, col_gap);
		if(!rt.empty())
		{
			row_min = rt[0].min_w > 0 ? rt[0].min_w : (rt[0].is_fixed ? rt[0].fixed_w : 0);
		}
	}

	/* Author-specified heights, saved so the stretch pass below can force a
	 * row height and a later relayout measures from the clean state again. */
	std::vector<css_length> orig_h(items.size());
	std::vector<css_length> orig_w(items.size());
	std::vector<int> crossv(items.size(), 0);
	for(size_t i = 0; i < items.size(); i++)
	{
		if(items[i]->get_display() != display_inline_text)
		{
			orig_h[i] = static_cast<html_tag*>(items[i])->m_css_height;
			orig_w[i] = static_cast<html_tag*>(items[i])->m_css_width;
		}
	}

	/* Explicit placement: honour grid-column-start/end + span. Google's
	 * Grid_column puts every block on a specific track range; packing items
	 * sequentially into one 1fr track collapsed multi-column text to a word
	 * per line. Items with no explicit column auto-flow into the next free
	 * cell. An occupancy map tracks filled cells so each row can be sized by
	 * its tallest occupant. */
	struct grid_cell { int row; int col; int span; };
	std::vector<grid_cell> place(items.size());
	std::vector<std::vector<char>> occ;	/* occ[row][col] */

	auto fits = [&](int r, int c, int sp) -> bool {
		if(c < 0 || c + sp > n) return false;
		if(r >= (int)occ.size()) return true;
		for(int cc = c; cc < c + sp; cc++)
			if(occ[r][cc]) return false;
		return true;
	};
	auto mark = [&](int r, int c, int sp) {
		while((int)occ.size() <= r) occ.push_back(std::vector<char>(n, 0));
		for(int cc = c; cc < c + sp; cc++) occ[r][cc] = 1;
	};

	int row_count = 0;
	int cursor_r = 0, cursor_c = 0;
	for(size_t k = 0; k < items.size(); k++)
	{
		int col = -1, span = 1, row = -1;
		if(items[k]->get_display() != display_inline_text)
			grid_item_placement(static_cast<html_tag*>(items[k]), n, col, span, row);
		if(span < 1) span = 1;
		if(span > n) span = n;

		int r;
		if(col >= 0)
		{
			/* An explicit row is taken verbatim: grid items MAY overlap (that
			 * is how grid-area:1/1 stacks a card and its overlay); only auto
			 * placement searches for a free row. */
			if(row >= 0)
			{
				r = row;
			}
			else
			{
				r = 0;
				while(!fits(r, col, span)) r++;
			}
			cursor_r = r;
			cursor_c = col + span;
			if(cursor_c >= n) cursor_c = 0;
		}
		else
		{
			r = cursor_r;
			int c = cursor_c;
			while(true)
			{
				if(c + span > n) { r++; c = 0; }
				if(fits(r, c, span)) break;
				c++;
				if(c >= n) { r++; c = 0; }
			}
			col = c;
			cursor_r = r;
			cursor_c = c + span;
			if(cursor_c >= n) cursor_c = 0;
		}
		mark(r, col, span);
		if(r + 1 > row_count) row_count = r + 1;
		place[k].row = r; place[k].col = col; place[k].span = span;
	}
	if(row_count == 0) row_count = 1;

	/* Track sizing happens AFTER placement: a min-content/max-content track is
	 * sized from the items actually placed in it, so the occupancy map must be
	 * complete first. apple.com's CTA row is
	 * 'grid-template-columns:min-content min-content' - previously the tokens
	 * fell through to fr=1 and the two pills each got half the container. */
	int fixed_sum = 0, content_sum = 0;
	float fr_sum = 0;
	for(int i = 0; i < n; i++)
	{
		if(tracks[i].is_fixed) fixed_sum += tracks[i].fixed_w;
		else if(tracks[i].is_content) content_sum += 0;	/* measured below */
		else fr_sum += tracks[i].fr;
	}
	std::vector<int> col_w(n, 0);
	for(int i = 0; i < n; i++)
		if(tracks[i].is_fixed) col_w[i] = tracks[i].fixed_w;
	if(have_content_track)
	{
		for(int i = 0; i < n; i++)
		{
			if(!tracks[i].is_content) continue;
			int w = 0;
			for(size_t k = 0; k < items.size(); k++)
			{
				if(place[k].col != i || place[k].span != 1) continue;
				element::ptr el = items[k];
				int iw = 0;
				if(el->get_display() == display_inline_text)
				{
					litehtml::size sz;
					el->get_content_size(sz, avail);
					iw = sz.width;
				}
				else
				{
					/* Measuring at the full available width lets a wrapping
					 * item lay out once instead of iteratively growing the
					 * track; the preferred width is then read back from the
					 * children (positions ignored, see preferred_content_width).
					 * With white-space:nowrap (apple's .button) the laid-out
					 * width already IS the max-content width. */
					static_cast<html_tag*>(el)->m_css_height = orig_h[k];
					static_cast<html_tag*>(el)->m_css_width = orig_w[k];
					el->render(0, 0, avail, second_pass);
					iw = tracks[i].content_min ? flex_min_content(el)
												: preferred_content_width(el);
					if(iw < (int)el->get_position().width && !tracks[i].content_min)
						iw = (int)el->get_position().width;
				}
				iw += el->margin_left() + el->margin_right();
				if(iw > w) w = iw;
			}
			/* Items spanning several tracks distribute their contribution over
			 * the spanned content tracks (rare; keeps the tracks from
			 * collapsing to zero under a spanning item). */
			for(size_t k = 0; k < items.size(); k++)
			{
				if(place[k].span < 2) continue;
				if(i < place[k].col || i >= place[k].col + place[k].span) continue;
				int outer = 0, ctracks = 0;
				for(int c = place[k].col; c < place[k].col + place[k].span && c < n; c++)
				{
					if(c + 1 < place[k].col + place[k].span) outer += col_gap;
					if(tracks[c].is_content) ctracks++;
				}
				if(ctracks == 0) continue;
				element::ptr el = items[k];
				int iw = 0;
				if(el->get_display() != display_inline_text)
				{
					el->render(0, 0, avail, second_pass);
					iw = tracks[i].content_min ? flex_min_content(el)
												: preferred_content_width(el);
				}
				iw += el->margin_left() + el->margin_right() - outer;
				if(iw < 0) iw = 0;
				iw /= ctracks;
				if(iw > w) w = iw;
			}
			col_w[i] = w;
			content_sum += w;
		}
	}
	int flex_avail = avail - gaps - fixed_sum - content_sum;
	if(flex_avail < 0) flex_avail = 0;
	for(int i = 0; i < n; i++)
	{
		if(tracks[i].is_fixed || tracks[i].is_content) continue;
		col_w[i] = fr_sum > 0 ? (int)((float)flex_avail * tracks[i].fr / fr_sum) : 0;
	}

	/* justify-content shifts/grows the whole track group inside the container:
	 * apple.com centres its CTA pills with 'justify-content:center'. Without
	 * this the track group always hugged the inline-start edge. */
	int lead = 0, jgap = 0;
	int used = content_sum + fixed_sum + gaps;
	for(int i = 0; i < n; i++)
		if(!tracks[i].is_fixed && !tracks[i].is_content) used += col_w[i];
	int leftover = avail - used;
	if(leftover > 0)
	{
		const tchar_t* jc = get_style_property(_t("justify-content"), false, 0);
		if(jc)
		{
			tstring j = jc;
			trim(j);
			if(j == _t("center")) lead = leftover / 2;
			else if(j == _t("flex-end") || j == _t("end") || j == _t("right")) lead = leftover;
			else if(j == _t("space-between") && n > 1) jgap = leftover / (n - 1);
			else if(j == _t("space-around")) { jgap = leftover / n; lead = jgap / 2; }
			else if(j == _t("space-evenly")) { jgap = leftover / (n + 1); lead = jgap; }
		}
	}
	std::vector<int> col_xpos(n, 0);
	{
		int ix = lead;
		for(int i = 0; i < n; i++)
		{
			col_xpos[i] = ix;
			ix += col_w[i] + col_gap + jgap;
		}
	}

	auto col_x = [&](int col) -> int {
		return col_xpos[col];
	};
	auto col_span_w = [&](int col, int span) -> int {
		int w = 0;
		for(int c = col; c < col + span && c < n; c++)
		{
			w += col_w[c];
			if(c + 1 < col + span) w += col_gap;
		}
		return w;
	};

	/* Form controls report is_replaced(), so litehtml sizes them to their
	 * intrinsic width and they left-align inside the cell, leaving the rest
	 * empty (a two-button .segmented control showed each button at ~76px in a
	 * ~128px 1fr track). Real browsers blockify grid items and, with the
	 * default justify-self:stretch, fill the cell. Mirror that for auto-width
	 * form widgets by pinning the CSS width to the cell box for this render. */
	auto stretch_item_width = [&](const element::ptr& el, int outer) {
		if(el->eweb_form_widget() == 0) return;
		html_tag* t = static_cast<html_tag*>(el);
		if(!t->m_css_width.is_predefined()) return;
		int w = outer - el->margin_left() - el->margin_right();
		if(w < 0) w = 0;
		css_length cw;
		cw = (float)w;
		t->m_css_width = cw;
	};

	for(int r = 0; r < row_count; r++)
	{
		int cur_row_h = 0;
		for(size_t k = 0; k < items.size(); k++)
		{
			if(place[k].row != r) continue;

			/* Item coordinates are relative to this grid container's content-box
			 * origin (m_pos already holds that origin). Seeding ix with m_pos.x and
			 * y with m_pos.y double-counted the container offset during the draw
			 * pass, shifting every tile of a non-left-aligned grid to the right. */
			int ix = col_x(place[k].col);
			int outer = col_span_w(place[k].col, place[k].span);

			element::ptr el = items[k];
			int cross = 0;
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
				html_tag* git = static_cast<html_tag*>(el);
				git->m_css_height = orig_h[k];
				git->m_css_width = orig_w[k];
				stretch_item_width(el, outer);
				el->render(ix, bottom, outer, second_pass);
				/* A row track is sized by the item's OUTER box. Measuring the
				 * content height alone dropped each tile's vertical padding -
				 * apple.com's .section grids (gap:12px, one tile per row,
				 * .tile-wrapper padding up to 69px) then stacked every row inside
				 * the previous tile, overlapping the whole page top to bottom. */
				cross = el->get_position().height + el->padding_top() + el->padding_bottom()
					+ el->border_top() + el->border_bottom()
					+ el->margin_top() + el->margin_bottom();
			}
			crossv[k] = cross;
			if(cross > cur_row_h) cur_row_h = cross;
		}

		int row_h = cur_row_h > row_min ? cur_row_h : row_min;
		/* stretch: the default align-self fills the row height */
		for(size_t k = 0; k < items.size(); k++)
		{
			if(place[k].row != r) continue;
			element::ptr el = items[k];
			if(el->get_display() == display_inline_text) continue;
			if(crossv[k] >= row_h) continue;
			int ix = col_x(place[k].col);
			int outer = col_span_w(place[k].col, place[k].span);
			html_tag* gst = static_cast<html_tag*>(el);
			int inner = row_h - el->margin_top() - el->margin_bottom();
			css_length h;
			/* The forced height is interpreted per the item's box-sizing: a
			 * border-box item keeps its padding inside the row, a content-box
			 * item outside it - one raw value made stretched tiles either fall
			 * short of the row or overflow it by their padding. */
			if(gst->m_box_sizing != box_sizing_border_box)
			{
				inner -= el->padding_top() + el->padding_bottom() + el->border_top() + el->border_bottom();
			}
			if(inner < 0) inner = 0;
			h = (float)inner;
			gst->m_css_height = h;
			stretch_item_width(el, outer);
			el->render(ix, bottom, outer, second_pass);
		}

		bottom += row_h;
		if(r + 1 < row_count) bottom += row_gap;
	}

	/* Drop the forced row heights again: m_pos already carries the stretched
	 * boxes, and the next relayout must measure the author's heights. */
	for(size_t k = 0; k < items.size(); k++)
	{
		if(items[k]->get_display() != display_inline_text)
		{
			static_cast<html_tag*>(items[k])->m_css_height = orig_h[k];
			static_cast<html_tag*>(items[k])->m_css_width = orig_w[k];
		}
	}

	m_pos.width = width_auto ? avail : avail;
	m_pos.height = bottom;
	calc_auto_margins(parent_width);

	m_pos.move_to(x, y);
	m_pos.x += content_margins_left();
	m_pos.y += content_margins_top();

	ret_width = avail + content_margins_left() + content_margins_right();
	return ret_width;
}
