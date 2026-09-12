#pragma once
#include "html_tag.h"

namespace litehtml
{
	class el_link : public html_tag
	{
	public:
		el_link(litehtml::document* doc);
		virtual ~el_link();

	protected:
		virtual void	parse_attributes() override;
		/* import_css()/link() must run once per node: refresh_styles() re-runs
		 * parse_attributes() to restore attribute styles, and re-loading the
		 * stylesheet on every restyle would queue duplicate fetches. */
		bool	m_attrs_processed = false;
	};
}
