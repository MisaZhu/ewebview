#pragma once

#include "html_tag.h"

namespace litehtml
{
	/* Minimal inline <svg> support: a replaced element whose intrinsic size
	 * comes from the width/height attributes (viewBox as fallback) and whose
	 * content is tessellated once at parse time into flattened polylines.
	 * Supported shapes: <path> (M/L/H/V/C/S/Q/T/A/Z, absolute + relative) and
	 * <rect>; <g> is traversed. Fill is non-zero-winding scanline, painted by
	 * the container through draw_svg (the port has no polygon primitive).
	 * Enough for icon-font SVGs such as apple.com's globalnav labels. */
	class el_svg : public html_tag
	{
		struct svg_subpath
		{
			std::vector<float>	pts;	/* flattened x,y pairs (user space) */
		};

		std::vector<svg_subpath>	m_subpaths;
		bool	m_shapes_ready;					/* tessellated on first paint */
		float	m_vb_x, m_vb_y, m_vb_w, m_vb_h;	/* viewBox */
		int		m_intrinsic_w, m_intrinsic_h;		/* width/height attrs */

		void	collect_shape(const element::ptr& el);
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
