#pragma once
#include "stylesheet.h"

namespace litehtml
{
	class context
	{
		litehtml::css	m_master_css;
		bool			m_fast_mode;
	public:
		context() : m_fast_mode(false) {}
		void			load_master_stylesheet(const tchar_t* str);
		litehtml::css&	master_css()
		{
			return m_master_css;
		}
		void			set_fast_mode(bool fast)
		{
			m_fast_mode = fast;
		}
		bool			is_fast_mode() const
		{
			return m_fast_mode;
		}
	};
}
