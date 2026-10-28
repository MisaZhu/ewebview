#pragma once
#include "html_tag.h"

namespace litehtml
{
	class el_style : public element
	{
		elements_vector		m_children;
		/* Text already contributed to the document master sheet, so a runtime
		 * mutation (React re-writing style.textContent) re-registers exactly
		 * once per distinct payload instead of appending duplicates. */
		tstring				m_reg_text;
		bool				m_registered = false;
		void				register_if_changed();
	public:
		el_style(litehtml::document* doc);
		virtual ~el_style();

		virtual void			parse_attributes() override;
		virtual bool			appendChild(const ptr &el) override;
		virtual const tchar_t*	get_tagName() const override;
	};
}
