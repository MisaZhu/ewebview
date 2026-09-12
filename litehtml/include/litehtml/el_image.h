#pragma once

#include "html_tag.h"

namespace litehtml
{

	class el_image : public html_tag
	{
		tstring	m_src;
		tstring	m_srcset;

		/* HTML5 responsive images: pick a concrete URL from the img srcset
		 * or, when src is absent, from the <source> children of a <picture>
		 * parent, before the base class loads m_src. */
		void	resolve_effective_src();
	public:
		el_image(litehtml::document* doc);
		virtual ~el_image(void);

		virtual int		line_height() const override;
		virtual bool	is_replaced() const override;
		virtual int		render(int x, int y, int max_width, bool second_pass = false) override;
		virtual void	parse_attributes() override;
		virtual void	parse_styles(bool is_reparse = false) override;
		virtual void	draw(uint_ptr hdc, int x, int y, const position* clip) override;
		virtual void	get_content_size(size& sz, int max_width) override;
	};
}
