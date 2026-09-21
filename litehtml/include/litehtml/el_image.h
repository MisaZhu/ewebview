#pragma once

#include "html_tag.h"

namespace litehtml
{

	class el_image : public html_tag
	{
	protected:
		tstring	m_src;
		tstring	m_srcset;

		/* Viewport snapshot from the last resolve_effective_src: <picture>
		 * source and srcset density selection depend on media features, and
		 * the first style pass can run before the window has its size. */
		int		m_resolved_mw;
		int		m_resolved_mh;

		/* width/height ATTRIBUTES are presentation hints in the CSS cascade:
		 * they lose to EVERY author rule, so `.avatar img{width:100%}` must
		 * beat width="24". Injecting them into m_style made them inline style
		 * (which wins everything), so the w3.org avatar img kept its 24px
		 * box inside the 32px round clip and the ellipse mask only nicked
		 * its corners - a grey square in a blue ring instead of a disc.
		 * Keep them out of the cascade and apply them as the intrinsic
		 * size, which is what they mean when no author rule sizes the box. */
		int		m_attr_width;
		int		m_attr_height;
		void	attr_size(litehtml::size& sz) const;

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

	/* <video> with a poster: the engine has no media decoder, so the element
	 * degrades to its poster frame - exactly what a browser shows while a
	 * preload=none video has not started. Without a poster the element stays
	 * out of flow (the UA sheet hides it); the fallback markup inside must
	 * never spill into the page, which the replaced-element draw guarantees. */
	class el_video : public el_image
	{
	public:
		el_video(litehtml::document* doc);
		virtual ~el_video(void);

		virtual void	parse_attributes() override;
		virtual void	parse_styles(bool is_reparse = false) override;
	};
}
