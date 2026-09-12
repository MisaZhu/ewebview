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
