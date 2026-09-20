#include "html.h"
#include "css_length.h"

void litehtml::css_length::fromString( const tchar_t* str, const tchar_t* predefs, int defValue )
{
	if(!str)
	{
		m_is_predefined = true;
		m_predef = defValue;
		return;
	}

	if(!t_strncmp(str, _t("calc"), 4))
	{
		/* Targeted calc() support for the common CSS forms calc(P% +/- Npx),
		 * calc(Npx +/- P%) and single px/percentage terms (GitHub positions
		 * its active-tab underline with bottom:calc(50% - 24px)). The grammar
		 * handled is term (('+'|'-') term)* where each term is a number with an
		 * optional %/px unit; a percentage term stays in m_value/m_units and
		 * pixel terms accumulate in m_calc_px (a pure-pixel expression folds
		 * everything into m_value as px). Anything richer (nested calc/var,
		 * multiplication, other units) falls back to predefined 0. */
		const tchar_t* p = str + 4;
		while(*p == _t(' ')) p++;
		if(*p == _t('('))
		{
			p++;
			double pct = 0, px = 0;
			float add_val = 0;
			css_units add_units = css_units_none;
			bool have_pct = false, ok = true, any = false;
			int sign = 1;
			bool expect_term = true;
			while(*p && *p != _t(')'))
			{
				if(*p == _t(' ')) { p++; continue; }
				if(!expect_term)
				{
					if(*p == _t('+')) { sign = 1; p++; expect_term = true; continue; }
					if(*p == _t('-')) { sign = -1; p++; expect_term = true; continue; }
					ok = false; break;
				}
				/* parse a signed number */
				const tchar_t* num = p;
				if(*p == _t('+') || *p == _t('-')) p++;
				bool digits = false;
				while(t_isdigit(*p) || *p == _t('.')) { if(t_isdigit(*p)) digits = true; p++; }
				if(!digits) { ok = false; break; }
				double v = (double) t_strtod(num, 0) * sign;
				/* unit: read the alpha/% run and classify */
				const tchar_t* us = p;
				if(*p == _t('%')) p++;
				else while((*p >= _t('a') && *p <= _t('z')) || (*p >= _t('A') && *p <= _t('Z'))) p++;
				size_t ul = (size_t)(p - us);
				if(ul == 1 && us[0] == _t('%'))
				{
					pct += v; have_pct = true;
				}
				else if(ul == 0 || (ul == 2 && us[0] == _t('p') && us[1] == _t('x')))
				{
					px += v;  /* unitless or px */
				}
				else
				{
					/* font/viewport-relative addend (em/rem/pt/vh/...): keep a
					 * single such term unresolved for cvt_units to fold later. */
					tchar_t ubuf[8];
					if(ul >= sizeof(ubuf)) { ok = false; break; }
					for(size_t k = 0; k < ul; k++) ubuf[k] = us[k];
					ubuf[ul] = 0;
					css_units u = (css_units) value_index(ubuf, css_units_strings, css_units_none);
					if(u == css_units_none || add_units != css_units_none) { ok = false; break; }
					add_val = (float) v;
					add_units = u;
				}
				any = true;
				sign = 1;
				expect_term = false;
			}
			if(ok && any)
			{
				m_is_predefined = false;
				m_calc_add_val = add_val;
				m_calc_add_units = add_units;
				if(have_pct)
				{
					m_value = (float) pct;
					m_units = css_units_percentage;
					m_calc_px = (float) px;
				}
				else
				{
					m_value = (float) px;
					m_units = css_units_px;
					m_calc_px = 0;
				}
				return;
			}
		}
		m_is_predefined = true;
		m_predef = 0;
		return;
	}

	int predef = value_index(str, predefs ? predefs : _t(""), -1);
	if(predef >= 0)
	{
		m_is_predefined = true;
		m_predef = predef;
		return;
	}

	m_is_predefined = false;
	const tchar_t* unit = str;
	while(*unit)
	{
		if(!(t_isdigit(*unit) || *unit == _t('.') || *unit == _t('+') || *unit == _t('-')))
		{
			break;
		}
		unit++;
	}

	if(unit != str)
	{
		m_value = (float) t_strtod(str, 0);
		m_units = (css_units) value_index(unit, css_units_strings, css_units_none);
	}
	else
	{
		m_is_predefined = true;
		m_predef = defValue;
	}
}

void litehtml::css_length::fromString( const tstring& str, const tstring& predefs, int defValue )
{
	fromString(str.c_str(), predefs.c_str(), defValue);
}
