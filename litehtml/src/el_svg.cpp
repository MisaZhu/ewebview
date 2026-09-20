#include "html.h"
#include "el_svg.h"
#include "document.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* SVG path-data parser: tessellate the `d` attribute into flattened   */
/* polylines (user space). Curves are subdivided by a fixed step, arcs */
/* go through the standard endpoint->center parametrization.           */
/* ------------------------------------------------------------------ */

namespace litehtml {

struct svg_path_builder
{
	std::vector<std::vector<float> >& subs;	/* each sub: interleaved x,y */
	std::vector<float>*	cur;
	float cx, cy;			/* current point */
	float sx, sy;			/* start of the current subpath (for Z) */
	float lcx, lcy;			/* last control point (for S/T smoothness) */
	char  last_cmd;

	svg_path_builder(std::vector<std::vector<float> >& s)
		: subs(s), cur(0), cx(0), cy(0), sx(0), sy(0), lcx(0), lcy(0), last_cmd(0) {}

	void start_subpath(float x, float y)
	{
		subs.push_back(std::vector<float>());
		cur = &subs.back();
		cur->push_back(x);
		cur->push_back(y);
		cx = sx = x;
		cy = sy = y;
	}

	void line_to(float x, float y)
	{
		if(!cur) start_subpath(x, y);
		else { cur->push_back(x); cur->push_back(y); cx = x; cy = y; }
	}

	void close_path()
	{
		if(cur && !cur->empty())
		{
			cur->push_back(sx);
			cur->push_back(sy);
		}
		cx = sx;
		cy = sy;
		cur = 0;	/* a subsequent M opens a new subpath; L without M restarts one */
	}

	void cubic_to(float x1, float y1, float x2, float y2, float x, float y)
	{
		if(!cur) start_subpath(cx, cy);
		/* chord-length adaptive: enough segments to keep small icon curves
		 * smooth (a 5px bezier still needs ~10 steps to not look angular) */
		float extent = fabsf(x - cx) + fabsf(y - cy) + fabsf(x1 - cx) + fabsf(y1 - cy) +
					   fabsf(x2 - x1) + fabsf(y2 - y1);
		int steps = (int)(extent / 1.5f) + 2;
		if(steps > 64) steps = 64;
		if(steps < 4) steps = 4;
		for(int i = 1; i <= steps; i++)
		{
			float t = (float)i / (float)steps;
			float u = 1.0f - t;
			float px = u*u*u*cx + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x;
			float py = u*u*u*cy + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y;
			cur->push_back(px);
			cur->push_back(py);
		}
		lcx = x2; lcy = y2;
		cx = x; cy = y;
	}

	void quad_to(float x1, float y1, float x, float y)
	{
		if(!cur) start_subpath(cx, cy);
		float extent = fabsf(x - cx) + fabsf(y - cy) + fabsf(x1 - cx) + fabsf(y1 - cy);
		int steps = (int)(extent / 1.5f) + 2;
		if(steps > 64) steps = 64;
		if(steps < 4) steps = 4;
		for(int i = 1; i <= steps; i++)
		{
			float t = (float)i / (float)steps;
			float u = 1.0f - t;
			float px = u*u*cx + 2*u*t*x1 + t*t*x;
			float py = u*u*cy + 2*u*t*y1 + t*t*y;
			cur->push_back(px);
			cur->push_back(py);
		}
		lcx = x1; lcy = y1;
		cx = x; cy = y;
	}

	void arc_to(float rx, float ry, float xrot_deg, bool large, bool sweep, float x, float y)
	{
		if(!cur) start_subpath(cx, cy);
		/* F.6.5 endpoint -> center parametrization */
		float x1 = cx, y1 = cy, x2 = x, y2 = y;
		if(fabsf(x1 - x2) < 1e-6f && fabsf(y1 - y2) < 1e-6f) { cx = x; cy = y; return; }
		if(rx <= 0 || ry <= 0) { line_to(x, y); return; }
		float phi = xrot_deg * 3.14159265358979f / 180.0f;
		float cph = cosf(phi), sph = sinf(phi);
		float dx2 = (x1 - x2) / 2.0f, dy2 = (y1 - y2) / 2.0f;
		float x1p = cph * dx2 + sph * dy2;
		float y1p = -sph * dx2 + cph * dy2;
		float rx2 = rx * rx, ry2 = ry * ry;
		float lam = (x1p * x1p) / rx2 + (y1p * y1p) / ry2;
		if(lam > 1.0f) { float s = sqrtf(lam); rx *= s; ry *= s; rx2 = rx * rx; ry2 = ry * ry; }
		float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
		float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
		float co = (den > 0) ? sqrtf(fabsf(num / den)) : 0.0f;
		if(large == sweep) co = -co;
		float cxp = co * (rx * y1p) / ry;
		float cyp = co * (-ry * x1p) / rx;
		float ccx = cph * cxp - sph * cyp + (x1 + x2) / 2.0f;
		float ccy = sph * cxp + cph * cyp + (y1 + y2) / 2.0f;
		float ux = (x1p - cxp) / rx,  uy = (y1p - cyp) / ry;
		float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
		auto ang = [](float ax, float ay, float bx, float by) {
			float d = ax * bx + ay * by;
			float n = sqrtf((ax*ax + ay*ay) * (bx*bx + by*by));
			float c = (n > 0) ? d / n : 1.0f;
			if(c > 1.0f) c = 1.0f; if(c < -1.0f) c = -1.0f;
			float a = acosf(c);
			if(ax * by - ay * bx < 0) a = -a;
			return a;
		};
		float th1 = ang(1, 0, ux, uy);
		float dth = ang(ux, uy, vx, vy);
		if(!sweep && dth > 0) dth -= 2 * 3.14159265358979f;
		if(sweep && dth < 0) dth += 2 * 3.14159265358979f;
		int steps = (int)(fabsf(dth) * sqrtf((rx + ry) / 2.0f) * 2.0f) + 4;
		if(steps > 64) steps = 64;
		if(steps < 6) steps = 6;
		for(int i = 1; i <= steps; i++)
		{
			float t = th1 + dth * (float)i / (float)steps;
			float ex = rx * cosf(t), ey = ry * sinf(t);
			cur->push_back(cph * ex - sph * ey + ccx);
			cur->push_back(sph * ex + cph * ey + ccy);
		}
		cx = x; cy = y;
	}
};

/* Number scanner for path data: handles signs, decimals and exponent-free
 * compact forms such as "1.5.2" (two numbers) and "-3-4". */
struct svg_num_scanner
{
	const char* p;
	svg_num_scanner(const char* s) : p(s ? s : "") {}

	bool next(float& out)
	{
		while(*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
		const char* start = p;
		if(*p == '+' || *p == '-') p++;
		bool digits = false;
		while(*p >= '0' && *p <= '9') { p++; digits = true; }
		if(*p == '.')
		{
			p++;
			while(*p >= '0' && *p <= '9') { p++; digits = true; }
		}
		if(!digits) { p = start; return false; }
		if(*p == 'e' || *p == 'E')
		{
			const char* save = p;
			p++;
			if(*p == '+' || *p == '-') p++;
			if(*p >= '0' && *p <= '9') { while(*p >= '0' && *p <= '9') p++; }
			else p = save;
		}
		out = (float)atof(start);
		return true;
	}

	/* Peek whether the next non-space char is a number (flag pairs in A use
	 * single-digit flags packed without separators: "0 0 1" or "001"). */
	bool peek_flag(bool& out)
	{
		while(*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
		if(*p == '0') { out = false; p++; return true; }
		if(*p == '1') { out = true;  p++; return true; }
		return false;
	}
};

static bool svg_cmd_letter(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void parse_svg_path(const char* d, std::vector<std::vector<float> >& subs)
{
	if(!d || !*d) return;
	svg_path_builder b(subs);
	svg_num_scanner sc(d);
	char cmd = 0;
	float v[7] = {0, 0, 0, 0, 0, 0, 0};
	for(;;)
	{
		const char* iter_start = sc.p;
		/* Next command letter, or repeat of the previous one. */
		const char* save = sc.p;
		while(*sc.p && (*sc.p == ' ' || *sc.p == ',' || *sc.p == '\t' || *sc.p == '\n' || *sc.p == '\r')) sc.p++;
		if(!*sc.p) break;	/* end of path data (a trailing Z must not repeat) */
		if(svg_cmd_letter(*sc.p)) { cmd = *sc.p; sc.p++; }
		else if(cmd) { sc.p = save; }
		else break;
		if(!cmd) break;

		if(cmd == 'Z' || cmd == 'z') { b.close_path(); b.last_cmd = cmd; continue; }

		int nargs = 0;
		switch(cmd)
		{
		case 'M': case 'm': nargs = 2; break;
		case 'L': case 'l': case 'T': case 't': nargs = 2; break;
		case 'H': case 'h': case 'V': case 'v': nargs = 1; break;
		case 'C': case 'c': nargs = 6; break;
		case 'S': case 's': case 'Q': case 'q': nargs = 4; break;
		case 'A': case 'a': nargs = 7; break;
		default: return;	/* unknown command: bail out with what we have */
		}

		bool got = true;
		if(cmd == 'A' || cmd == 'a')
		{
			/* flags arrive either packed single digits ("0 0 1" / "001") or as
			 * ordinary numbers; never consult v[] when peek_flag consumed them */
			bool large = false, sweep = false;
			got = sc.next(v[0]) && sc.next(v[1]) && sc.next(v[2]);
			if(got && !sc.peek_flag(large))
			{
				got = sc.next(v[3]);
				large = (v[3] != 0.0f);
			}
			if(got && !sc.peek_flag(sweep))
			{
				got = sc.next(v[4]);
				sweep = (v[4] != 0.0f);
			}
			got = got && sc.next(v[5]) && sc.next(v[6]);
			v[3] = large ? 1.0f : 0.0f;
			v[4] = sweep ? 1.0f : 0.0f;
		}
		else
		{
			for(int i = 0; i < nargs && got; i++) got = sc.next(v[i]);
		}
		if(!got) break;

		bool rel = (cmd >= 'a' && cmd <= 'z');
		float ox = rel ? b.cx : 0.0f;
		float oy = rel ? b.cy : 0.0f;
		switch(cmd)
		{
		case 'M': case 'm':
			b.start_subpath(v[0] + ox, v[1] + oy);
			/* subsequent coordinate pairs after M act as implicit L */
			cmd = rel ? 'l' : 'L';
			break;
		case 'L': case 'l': b.line_to(v[0] + ox, v[1] + oy); break;
		case 'H': case 'h': b.line_to(v[0] + ox, b.cy); break;
		case 'V': case 'v': b.line_to(b.cx, v[0] + oy); break;
		case 'C': case 'c':
			b.cubic_to(v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy, v[4] + ox, v[5] + oy);
			break;
		case 'S': case 's':
		{
			/* reflected control point when the previous command was C/S */
			float x1 = b.cx, y1 = b.cy;
			char pc = b.last_cmd;
			if(pc == 'C' || pc == 'c' || pc == 'S' || pc == 's')
			{ x1 = 2 * b.cx - b.lcx; y1 = 2 * b.cy - b.lcy; }
			b.cubic_to(x1, y1, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy);
			break;
		}
		case 'Q': case 'q':
			b.quad_to(v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy);
			break;
		case 'T': case 't':
		{
			float x1 = b.cx, y1 = b.cy;
			char pc = b.last_cmd;
			if(pc == 'Q' || pc == 'q' || pc == 'T' || pc == 't')
			{ x1 = 2 * b.cx - b.lcx; y1 = 2 * b.cy - b.lcy; }
			b.quad_to(x1, y1, v[0] + ox, v[1] + oy);
			break;
		}
		case 'A': case 'a':
		{
			bool large = (v[3] != 0);
			bool sweep = (v[4] != 0);
			b.arc_to(fabsf(v[0]), fabsf(v[1]), v[2], large, sweep, v[5] + ox, v[6] + oy);
			break;
		}
		}
		b.last_cmd = cmd;
		/* Guard: a well-formed iteration always consumes input. */
		if(sc.p == iter_start) break;
	}
}

} // namespace litehtml

/* ------------------------------------------------------------------ */
/* Element glue                                                        */
/* ------------------------------------------------------------------ */

litehtml::el_svg::el_svg(litehtml::document* doc) : html_tag(doc)
{
	m_display = display_inline_block;
	m_shapes_ready = false;
	m_vb_x = m_vb_y = 0;
	m_vb_w = m_vb_h = 0;
	m_intrinsic_w = m_intrinsic_h = 0;
}

litehtml::el_svg::~el_svg()
{
}

void litehtml::el_svg::collect_shape(const element::ptr& el)
{
	if(!el || !el->get_tagName()) return;
	tstring tag = el->get_tagName();
	lcase(tag);

	if(tag == _t("g") || tag == _t("svg") || tag == _t("a"))
	{
		int cnt = el->get_children_count();
		for(int i = 0; i < cnt; i++)
			collect_shape(el->get_child(i));
		return;
	}

	/* remember the owning shape: fill/stroke are resolved per paint from its
	 * attributes, computed style and the SVG paint-inheritance chain. */
	#define SVG_PUSH_SHAPE(sp) \
		(sp).shape = el;

	if(tag == _t("path"))
	{
		const tchar_t* d = el->get_attr(_t("d"));
		if(d && *d)
		{
			std::vector<std::vector<float> > subs;
			parse_svg_path(d, subs);
			for(size_t i = 0; i < subs.size(); i++)
			{
				if(subs[i].size() >= 6)	/* at least 3 points */
				{
					svg_subpath sp;
					sp.pts = subs[i];
					sp.stroke_pts = subs[i];
					SVG_PUSH_SHAPE(sp)
					m_subpaths.push_back(sp);
				}
			}
		}
		return;
	}
	if(tag == _t("rect"))
	{
		const tchar_t* ax = el->get_attr(_t("x"));
		const tchar_t* ay = el->get_attr(_t("y"));
		const tchar_t* aw = el->get_attr(_t("width"));
		const tchar_t* ah = el->get_attr(_t("height"));
		float x = ax ? (float)atof(ax) : 0;
		float y = ay ? (float)atof(ay) : 0;
		float w = aw ? (float)atof(aw) : 0;
		float h = ah ? (float)atof(ah) : 0;
		if(w > 0 && h > 0)
		{
			svg_subpath sp;
			sp.pts.push_back(x);     sp.pts.push_back(y);
			sp.pts.push_back(x + w); sp.pts.push_back(y);
			sp.pts.push_back(x + w); sp.pts.push_back(y + h);
			sp.pts.push_back(x);     sp.pts.push_back(y + h);
			sp.stroke_pts = sp.pts;
			sp.stroke_closed = true;
			SVG_PUSH_SHAPE(sp)
			m_subpaths.push_back(sp);
		}
		return;
	}
	if(tag == _t("circle") || tag == _t("ellipse"))
	{
		const tchar_t* acx = el->get_attr(_t("cx"));
		const tchar_t* acy = el->get_attr(_t("cy"));
		float cx = acx ? (float)atof(acx) : 0;
		float cy = acy ? (float)atof(acy) : 0;
		float rx, ry;
		if(tag == _t("circle"))
		{
			const tchar_t* ar = el->get_attr(_t("r"));
			rx = ry = ar ? (float)atof(ar) : 0;
		}
		else
		{
			const tchar_t* arx = el->get_attr(_t("rx"));
			const tchar_t* ary = el->get_attr(_t("ry"));
			rx = arx ? (float)atof(arx) : 0;
			ry = ary ? (float)atof(ary) : 0;
		}
		if(rx > 0 && ry > 0)
		{
			svg_subpath sp;
			const int steps = 24;
			for(int i = 0; i < steps; i++)
			{
				float a = (float)i * 2.0f * 3.14159265358979f / (float)steps;
				sp.pts.push_back(cx + rx * cosf(a));
				sp.pts.push_back(cy + ry * sinf(a));
			}
			sp.stroke_pts = sp.pts;
			sp.stroke_closed = true;
			SVG_PUSH_SHAPE(sp)
			m_subpaths.push_back(sp);
		}
		return;
	}
	#undef SVG_PUSH_SHAPE
	/* line/text/defs: unsupported in this minimal path. */
}

/* SVG paint value for a shape: own attribute, own computed style, then the
 * same pair up the ancestor chain (fill/stroke inherit in SVG), stopping at
 * the <svg> root. Returns null when nothing specifies it. */
static const litehtml::tchar_t* svg_paint_value(const litehtml::element::ptr& shape, const litehtml::tchar_t* prop)
{
	for(litehtml::element::ptr el = shape; el; el = el->parent())
	{
		const litehtml::tchar_t* v = el->get_attr(prop);
		if(!v || !*v)
		{
			v = el->get_style_property(prop, false, 0);
		}
		if(v && *v)
		{
			return v;
		}
		if(!t_strcasecmp(el->get_tagName(), _t("svg")))
		{
			break;
		}
	}
	return nullptr;
}

/* Per-paint paint resolution: fill (attribute -> computed style -> inherited
 * chain, so CSS like `.brand-mark rect{fill:var(--accent)}` or an svg-level
 * `fill:none` reaches the shape as soon as the document style walk has run)
 * and stroke (+stroke-width). "none" leaves the subpath unpainted;
 * "currentColor"/unparseable fill falls back to the svg's inherited text
 * colour at draw time. */
void litehtml::el_svg::resolve_subpath_colors()
{
	document_container* cont = get_document() ? get_document()->container() : nullptr;
	size_t n = m_subpaths.size();
	m_subpath_colors.assign(n, web_color(0, 0, 0, 0));
	m_subpath_has_color.assign(n, 0);
	m_subpath_strokes.assign(n, web_color(0, 0, 0, 0));
	m_subpath_has_stroke.assign(n, 0);
	m_subpath_stroke_w.assign(n, 1.0f);
	for(size_t i = 0; i < n; i++)
	{
		const svg_subpath& sp = m_subpaths[i];
		if(!sp.shape)
		{
			continue;
		}
		const tchar_t* fill = svg_paint_value(sp.shape, _t("fill"));
		if(fill)
		{
			if(!t_strcasecmp(fill, _t("none")))
			{
				m_subpath_has_color[i] = 2;
			}
			else if(t_strcasecmp(fill, _t("currentColor")) && t_strcasecmp(fill, _t("inherit")) && cont)
			{
				web_color c = web_color::from_string(fill, cont);
				if(c.alpha != 0 || c.red != 0 || c.green != 0 || c.blue != 0)
				{
					m_subpath_colors[i] = c;
					m_subpath_has_color[i] = 1;
				}
			}
		}
		const tchar_t* stroke = svg_paint_value(sp.shape, _t("stroke"));
		if(stroke && t_strcasecmp(stroke, _t("none")) && cont)
		{
			web_color c = web_color::from_string(stroke, cont);
			if(c.alpha != 0 || c.red != 0 || c.green != 0 || c.blue != 0)
			{
				m_subpath_strokes[i] = c;
				m_subpath_has_stroke[i] = 1;
				const tchar_t* sw = svg_paint_value(sp.shape, _t("stroke-width"));
				if(sw)
				{
					float wv = (float)atof(sw);
					if(wv > 0) m_subpath_stroke_w[i] = wv;
				}
			}
		}
	}
}

void litehtml::el_svg::parse_attributes()
{
	const tchar_t* attr_w = get_attr(_t("width"));
	const tchar_t* attr_h = get_attr(_t("height"));
	if(attr_w && *attr_w)
	{
		m_intrinsic_w = atoi(attr_w);
		m_style.add_property(_t("width"), attr_w, 0, false);
	}
	if(attr_h && *attr_h)
	{
		m_intrinsic_h = atoi(attr_h);
		m_style.add_property(_t("height"), attr_h, 0, false);
	}

	/* get_attr folds the key to lower case (attributes are stored folded). */
	const tchar_t* vb = get_attr(_t("viewbox"));
	if(vb && *vb)
	{
		float f[4] = {0, 0, 0, 0};
		int n = sscanf(vb, "%f %f %f %f", &f[0], &f[1], &f[2], &f[3]);
		if(n >= 4 && f[2] > 0 && f[3] > 0)
		{
			m_vb_x = f[0]; m_vb_y = f[1]; m_vb_w = f[2]; m_vb_h = f[3];
		}
	}
	if(m_intrinsic_w <= 0 && m_vb_w > 0) m_intrinsic_w = (int)m_vb_w;
	if(m_intrinsic_h <= 0 && m_vb_h > 0) m_intrinsic_h = (int)m_vb_h;
}

void litehtml::el_svg::get_content_size(size& sz, int /*max_width*/)
{
	sz.width = m_intrinsic_w > 0 ? m_intrinsic_w : 0;
	sz.height = m_intrinsic_h > 0 ? m_intrinsic_h : 0;
}

int litehtml::el_svg::line_height() const
{
	return height();
}

bool litehtml::el_svg::is_replaced() const
{
	return true;
}

int litehtml::el_svg::render(int x, int y, int max_width, bool /*second_pass*/)
{
	calc_outlines(max_width);
	m_pos.move_to(x, y);

	size sz;
	get_content_size(sz, max_width);
	m_pos.width = sz.width;
	m_pos.height = sz.height;

	if(m_css_height.is_predefined() && !m_css_width.is_predefined())
	{
		m_pos.width = (int)m_css_width.calc_percent(max_width);
		if(sz.width > 0 && sz.height > 0)
			m_pos.height = (int)((float)m_pos.width * (float)sz.height / (float)sz.width);
	}
	else if(!m_css_height.is_predefined() && m_css_width.is_predefined())
	{
		if(!get_predefined_height(m_pos.height))
			m_pos.height = (int)m_css_height.val();
		if(sz.width > 0 && sz.height > 0)
			m_pos.width = (int)((float)m_pos.height * (float)sz.width / (float)sz.height);
	}
	else if(!m_css_height.is_predefined() && !m_css_width.is_predefined())
	{
		m_pos.width = (int)m_css_width.calc_percent(max_width);
		if(!get_predefined_height(m_pos.height))
			m_pos.height = (int)m_css_height.val();
	}
	else
	{
		/* Auto width AND auto height: an SVG with a viewBox has an intrinsic
		 * size, and like any replaced element it is used as-is, only constrained
		 * by the available width (ratio-preserving). Filling max_width instead
		 * blew up apple.com's global nav: every section link carries a bare
		 * <svg viewBox> wordmark icon whose span is sized by the icon, so the
		 * icon grew to the link's offered width and stacked the nav row into
		 * overlapping bands. The gallery service logos still come out right
		 * because their 449px-wide viewBox is constrained to the card width. */
		if(sz.width > 0 && max_width > 0 && sz.width > max_width)
		{
			m_pos.width = max_width;
			if(sz.height > 0)
				m_pos.height = (int)((float)max_width * (float)sz.height / (float)sz.width);
		}
	}

	calc_auto_margins(max_width);
	m_pos.x += content_margins_left();
	m_pos.y += content_margins_top();

	return m_pos.width + content_margins_left() + content_margins_right();
}

void litehtml::el_svg::draw(uint_ptr hdc, int x, int y, const position* clip)
{
	document* doc = get_document();
	if(!doc || !doc->container()) return;

	/* The shape children only exist once the whole subtree has been
	 * parsed, so tessellation happens lazily on first paint (parse_attributes
	 * runs at creation time, when <path> siblings are not attached yet).
	 * Only the GEOMETRY latches here; fill colours are resolved every paint
	 * (resolve_subpath_colors) because the page sheets may land after the
	 * first progressive frame. */
	if(!m_shapes_ready)
	{
		m_shapes_ready = true;
		m_subpaths.clear();
		for(auto& c : m_children)
			collect_shape(c);
	}
	if(m_subpaths.empty()) return;
	resolve_subpath_colors();

	position pos = m_pos;
	pos.x += x;
	pos.y += y;
	if(pos.width <= 0 || pos.height <= 0) return;
	if(clip && !pos.does_intersect(clip)) return;

	/* viewBox -> element box (aspect preserved, centered - "meet") */
	float vbw = m_vb_w > 0 ? m_vb_w : (float)pos.width;
	float vbh = m_vb_h > 0 ? m_vb_h : (float)pos.height;
	float s = (float)pos.width / vbw;
	float s2 = (float)pos.height / vbh;
	if(s2 < s) s = s2;	/* meet: fit inside */
	float dx = (pos.width - vbw * s) / 2.0f - m_vb_x * s;
	float dy = (pos.height - vbh * s) / 2.0f - m_vb_y * s;

	/* CSS transform on the <svg> itself (GitHub's nav chevrons are a
	 * chevron-right icon with transform:rotate(90deg)). The container's paint
	 * transform only bends border quads, so the matrix is applied here to every
	 * emitted point instead; it is computed about the border box, like
	 * html_tag::draw_background does. */
	position border_box = pos;
	border_box += m_padding;
	border_box += m_borders;
	float xf[6];
	bool have_xf = compute_transform_matrix(border_box, xf);
	auto emit = [&](std::vector<float>& out, float px, float py)
	{
		float X = px * s + dx + pos.x;
		float Y = py * s + dy + pos.y;
		if(have_xf)
		{
			float tx = xf[0] * X + xf[2] * Y + xf[4];
			float ty = xf[1] * X + xf[3] * Y + xf[5];
			X = tx;
			Y = ty;
		}
		out.push_back(X);
		out.push_back(Y);
	};

	/* Paint consecutive same-colour subpaths together (one draw_svg call each)
	 * so nonzero-winding counters inside a shape survive, while differently
	 * filled shapes (accent rect + bg-coloured dot) layer in document order.
	 * has_color: 0 = currentColor fallback, 1 = resolved colour, 2 = "none"
	 * (skip). */
	web_color cc = get_color(_t("color"), true, web_color(0, 0, 0));
	auto run_color = [&](size_t i) -> web_color
	{
		return m_subpath_has_color[i] == 1 ? m_subpath_colors[i] : cc;
	};
	size_t run_start = 0;
	bool run_started = false;
	auto flush_run = [&](size_t begin, size_t end)
	{
		if(!run_started) return;
		std::vector<float> pts;
		std::vector<int> counts;
		for(size_t si = begin; si < end; si++)
		{
			if(m_subpath_has_color[si] == 2) continue;
			const svg_subpath& sp = m_subpaths[si];
			int n = (int)sp.pts.size() / 2;
			if(n < 3) continue;
			counts.push_back(n);
			for(int i = 0; i < n; i++)
			{
				emit(pts, sp.pts[i * 2], sp.pts[i * 2 + 1]);
			}
		}
		if(counts.empty()) return;
		doc->container()->draw_svg(hdc, pos, run_color(begin), pts.data(), counts.data(), (int)counts.size());
	};
	for(size_t i = 0; i < m_subpaths.size(); i++)
	{
		if(m_subpath_has_color[i] == 2)
		{
			/* "none" ends the current run and paints nothing itself. */
			flush_run(run_start, i);
			run_started = false;
			continue;
		}
		if(!run_started)
		{
			run_start = i;
			run_started = true;
			continue;
		}
		web_color a = run_color(run_start);
		web_color b = run_color(i);
		if(a.red != b.red || a.green != b.green || a.blue != b.blue || a.alpha != b.alpha)
		{
			flush_run(run_start, i);
			run_start = i;
		}
	}
	flush_run(run_start, m_subpaths.size());

	/* Strokes: expand every outline segment into a width-quad and fill the
	 * batch (the port has no line primitive). Painted after the fills, in
	 * document order, grouped by colour. */
	std::vector<float> spts;
	std::vector<int> scounts;
	web_color scol(0, 0, 0, 0);
	bool srun = false;
	auto flush_strokes = [&]()
	{
		if(!srun || scounts.empty())
		{
			spts.clear();
			scounts.clear();
			srun = false;
			return;
		}
		doc->container()->draw_svg(hdc, pos, scol, spts.data(), scounts.data(), (int)scounts.size());
		spts.clear();
		scounts.clear();
		srun = false;
	};
	auto same_stroke = [&](const web_color& a, const web_color& b)
	{
		return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
	};
	for(size_t i = 0; i < m_subpaths.size(); i++)
	{
		if(!m_subpath_has_stroke[i])
		{
			continue;
		}
		if(srun && !same_stroke(scol, m_subpath_strokes[i]))
		{
			flush_strokes();
		}
		if(!srun)
		{
			scol = m_subpath_strokes[i];
			srun = true;
		}
		const svg_subpath& sp = m_subpaths[i];
		float hw = m_subpath_stroke_w[i] * 0.5f;
		size_t pn = sp.stroke_pts.size() / 2;
		size_t segs = sp.stroke_closed ? pn : (pn > 0 ? pn - 1 : 0);
		for(size_t k = 0; k < segs; k++)
		{
			float x0 = sp.stroke_pts[k * 2], y0 = sp.stroke_pts[k * 2 + 1];
			size_t k1 = (k + 1) % pn;
			float x1 = sp.stroke_pts[k1 * 2], y1 = sp.stroke_pts[k1 * 2 + 1];
			float dxs = x1 - x0, dys = y1 - y0;
			float len = sqrtf(dxs * dxs + dys * dys);
			if(len < 1e-4f)
			{
				continue;
			}
			float nx = -dys / len * hw, ny = dxs / len * hw;
			scounts.push_back(4);
			emit(spts, x0 + nx, y0 + ny);
			emit(spts, x1 + nx, y1 + ny);
			emit(spts, x1 - nx, y1 - ny);
			emit(spts, x0 - nx, y0 - ny);
		}
	}
	flush_strokes();
}

void litehtml::el_svg::draw_children(uint_ptr /*hdc*/, int /*x*/, int /*y*/,
	const position* /*clip*/, draw_flag /*flag*/, int /*zindex*/)
{
	/* atomic: shape children are painted by draw(), never by the tree walk */
}
