#pragma once
#include "html_tag.h"

namespace litehtml
{
	class el_before_after_base : public html_tag
	{
	public:
		el_before_after_base(litehtml::document* doc, bool before);
		virtual ~el_before_after_base();
	
		virtual void add_style(const litehtml::style& st, const litehtml::selector_specificity& spec = litehtml::inline_style_specificity) override;
		virtual void apply_stylesheet(const litehtml::css& stylesheet) override;
		virtual style_display get_display() const override;
	private:
		void	add_text(const tstring& txt);
		void	add_function(const tstring& fnc, const tstring& params);
		tchar_t convert_escape(const tchar_t* txt);
		/* a pseudo-element whose computed content stays none/normal (or
		 * absent) generates no box at all, per CSS; without this the
		 * universal `*, *::before, *::after` reset inserts phantom
		 * grid/flex items */
		bool	m_box_content;
		tstring	m_content;
	};

	class el_before : public el_before_after_base
	{
	public:
		el_before(litehtml::document* doc) : el_before_after_base(doc, true)
		{

		}
	};

	class el_after : public el_before_after_base
	{
	public:
		el_after(litehtml::document* doc) : el_before_after_base(doc, false)
		{

		}
	};
}