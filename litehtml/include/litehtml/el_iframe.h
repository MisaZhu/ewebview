#pragma once

#include "el_image.h"

namespace litehtml
{
	/* <iframe> as a replaced element: the container fetches the frame URL as
	 * an HTML document and paints a static render of it into the frame box
	 * (no nested scripting lifecycle). Sizing rides the el_image machinery:
	 * CSS width/height win, the intrinsic fallback is the HTML default
	 * 300x150 instead of an image's natural size. Fallback markup inside the
	 * tag never spills into the page (replaced-element draw). */
	class el_iframe : public el_image
	{
	public:
		el_iframe(litehtml::document* doc);
		virtual ~el_iframe(void);

		virtual void	parse_styles(bool is_reparse = false) override;
		/* Dynamic src assignment (Angular ng-src) must re-fetch the frame. */
		virtual void	set_attr(const tchar_t* name, const tchar_t* val) override;
		virtual void	get_content_size(size& sz, int max_width) override;
		virtual void	draw(uint_ptr hdc, int x, int y, const position* clip) override;
	};
}