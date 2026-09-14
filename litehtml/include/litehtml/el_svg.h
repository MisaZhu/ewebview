#pragma once

#include "html_tag.h"

namespace litehtml
{
	/* Minimal inline <svg> support: a replaced element whose intrinsic size
	 * comes from the width/height attributes (viewBox as fallback) and whose
	 * content is tessellated once at parse time into flattened polylines.
	 * Supported shapes: <path> (M/L/H/V/C/S/Q/T/A/Z, absolute + relative),
	 * <rect>, <circle>/<ellipse>; <g> is traversed. Fill and stroke are
	 * resolved per paint, not at tessellation: async containers mount the
	 * page sheets after the first progressive frame, so a shape's CSS paint
	 * (e.g. `.mark rect{fill:var(--accent)}`) only exists once the document
	 * style walk has run - geometry latches once, paint follows the cascade.
	 * Resolution order per shape: own attribute, computed style, then the
	 * same pair up the ancestor chain (SVG paint inheritance), then the
	 * <svg> currentColor for fill. Stroke is painted by expanding each
	 * outline segment into a width-quad and filling it (the port has no
	 * line primitive); fill is non-zero-winding scanline through draw_svg.
	 * Consecutive same-colour subpaths share one call so winding counters
	 * survive.
	 * Enough for icon-font SVGs such as apple.com's globalnav labels. */
	class el_svg : public html_tag
	{
		struct svg_subpath
		{
			std::vector<float>	pts;	/* flattened x,y pairs (user space) */
			std::vector<float>	stroke_pts;	/* outline polyline to stroke */
			bool				stroke_closed;
			element::ptr		shape;	/* owning shape element (paint source) */
			svg_subpath() : stroke_closed(false) {}
		};

		std::vector<svg_subpath>	m_subpaths;
		std::vector<web_color>	m_subpath_colors;	/* per-draw resolved fills */
		std::vector<char>		m_subpath_has_color;
		std::vector<web_color>	m_subpath_strokes;	/* per-draw resolved strokes */
		std::vector<char>		m_subpath_has_stroke;
		std::vector<float>	m_subpath_stroke_w;	/* stroke width, user space */
		bool	m_shapes_ready;					/* tessellated on first paint */
		float	m_vb_x, m_vb_y, m_vb_w, m_vb_h;	/* viewBox */
		int		m_intrinsic_w, m_intrinsic_h;		/* width/height attrs */

		void	collect_shape(const element::ptr& el);
		void	resolve_subpath_colors();
	public:
		el_svg(litehtml::document* doc);
		virtual ~el_svg(void);

		virtual int		line_height() const override;
		virtual bool	is_replaced() const override;
		virtual int		render(int x, int y, int max_width, bool second_pass = false) override;
		virtual void	parse_attributes() override;
		virtual void	draw(uint_ptr hdc, int x, int y, const position* clip) override;
		virtual void	get_content_size(size& sz, int max_width) override;

		/* Atomic box: the shape children never take part in layout/paint. */
		virtual void	draw_children(uint_ptr hdc, int x, int y, const position* clip, draw_flag flag, int zindex) override;
	};
}
