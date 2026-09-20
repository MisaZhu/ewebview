#pragma once
#include "types.h"

namespace litehtml
{
	class css_length
	{
		union
		{
			float	m_value;
			int		m_predef;
		};
		css_units	m_units;
		bool		m_is_predefined;
		/* Fixed px addend from a calc(P% +/- Npx) expression: percentage stays in
		 * m_value/m_units and this pixel offset is added after the percentage is
		 * resolved (0 for non-calc lengths). Pure-pixel calc folds entirely into
		 * m_value. A single non-px addend term (em/rem/pt/...) is held unresolved
		 * in m_calc_add_val/m_calc_add_units until document::cvt_units folds it
		 * into m_calc_px with the font/root sizes in hand. */
		float		m_calc_px;
		float		m_calc_add_val;
		css_units	m_calc_add_units;
	public:
		css_length();
		css_length(const css_length& val);

		css_length&	operator=(const css_length& val);
		css_length&	operator=(float val);
		bool		is_predefined() const;
		void		predef(int val);
		int			predef() const;
		void		set_value(float val, css_units units);
		float		val() const;
		css_units	units() const;
		int			calc_percent(int width) const;
		/* calc() pixel addend accessors (see m_calc_px / m_calc_add_*). */
		float		calc_px() const { return m_calc_px; }
		bool		has_calc_add() const { return m_calc_add_units != css_units_none; }
		float		calc_add_val() const { return m_calc_add_val; }
		css_units	calc_add_units() const { return m_calc_add_units; }
		void		fold_calc_px(float px) { m_calc_px += px; m_calc_add_val = 0; m_calc_add_units = css_units_none; }
		void		fromString(const tchar_t* str, const tchar_t* predefs = _t(""), int defValue = 0);
		void		fromString(const tstring& str, const tstring& predefs = _t(""), int defValue = 0);
	};

	// css_length inlines

	inline css_length::css_length()
	{
		m_value			= 0;
		m_predef		= 0;
		m_units			= css_units_none;
		m_is_predefined	= false;
		m_calc_px		= 0;
		m_calc_add_val	= 0;
		m_calc_add_units = css_units_none;
	}

	inline css_length::css_length(const css_length& val)
	{
		if(val.is_predefined())
		{
			m_predef	= val.m_predef;
		} else
		{
			m_value		= val.m_value;
		}
		m_units			= val.m_units;
		m_is_predefined	= val.m_is_predefined;
		m_calc_px		= val.m_calc_px;
		m_calc_add_val	= val.m_calc_add_val;
		m_calc_add_units = val.m_calc_add_units;
	}

	inline css_length&	css_length::operator=(const css_length& val)
	{
		if(val.is_predefined())
		{
			m_predef	= val.m_predef;
		} else
		{
			m_value		= val.m_value;
		}
		m_units			= val.m_units;
		m_is_predefined	= val.m_is_predefined;
		m_calc_px		= val.m_calc_px;
		m_calc_add_val	= val.m_calc_add_val;
		m_calc_add_units = val.m_calc_add_units;
		return *this;
	}

	inline css_length&	css_length::operator=(float val)
	{
		m_value = val;
		m_units = css_units_px;
		m_is_predefined = false;
		m_calc_px = 0;
		m_calc_add_val = 0;
		m_calc_add_units = css_units_none;
		return *this;
	}

	inline bool css_length::is_predefined() const
	{ 
		return m_is_predefined;					
	}

	inline void css_length::predef(int val)		
	{ 
		m_predef		= val; 
		m_is_predefined = true;	
	}

	inline int css_length::predef() const
	{ 
		if(m_is_predefined)
		{
			return m_predef; 
		}
		return 0;
	}

	inline void css_length::set_value(float val, css_units units)		
	{ 
		m_value			= val; 
		m_is_predefined = false;	
		m_units			= units;
		m_calc_px		= 0;
		m_calc_add_val	= 0;
		m_calc_add_units = css_units_none;
	}

	inline float css_length::val() const
	{
		if(!m_is_predefined)
		{
			return m_value;
		}
		return 0;
	}

	inline css_units css_length::units() const
	{
		return m_units;
	}

	inline int css_length::calc_percent(int width) const
	{
		if(!is_predefined())
		{
			if(units() == css_units_percentage)
			{
				return (int) ((double) width * (double) m_value / 100.0 + (double) m_calc_px);
			} else
			{
				return (int) val();
			}
		}
		return 0;
	}
}
