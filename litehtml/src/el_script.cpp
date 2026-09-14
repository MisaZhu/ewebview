#include "html.h"
#include "el_script.h"
#include "document.h"


/* ASCII-only case fold, matching html_tag.cpp's file-local ascii_tolower_char
 * (which is not exported). Attribute keys are stored lower-cased so get_attr /
 * remove_attr round-trip regardless of the caller's casing, exactly like
 * html_tag. */
static litehtml::tchar_t script_tolower_char(litehtml::tchar_t ch)
{
	return (ch >= 'A' && ch <= 'Z') ? (ch - 'A' + 'a') : ch;
}


litehtml::el_script::el_script(litehtml::document* doc) : litehtml::element(doc)
{

}

litehtml::el_script::~el_script()
{

}

void litehtml::el_script::parse_attributes()
{
	//TODO: pass script text to document container
}

bool litehtml::el_script::appendChild(const ptr &el)
{
	el->get_text(m_text);
	return true;
}

const litehtml::tchar_t* litehtml::el_script::get_tagName() const
{
	return _t("script");
}

void litehtml::el_script::set_attr(const tchar_t* name, const tchar_t* val)
{
	if(name && val)
	{
		tstring key = name;
		for(size_t i = 0; i < key.length(); i++)
		{
			key[i] = script_tolower_char(key[i]);
		}
		m_script_attrs[key] = val;
	}
}

const litehtml::tchar_t* litehtml::el_script::get_attr(const tchar_t* name, const tchar_t* def /*= 0*/)
{
	if(!name)
	{
		return def;
	}
	tstring key = name;
	for(size_t i = 0; i < key.length(); i++)
	{
		key[i] = script_tolower_char(key[i]);
	}
	string_map::const_iterator attr = m_script_attrs.find(key);
	if(attr != m_script_attrs.end())
	{
		return attr->second.c_str();
	}
	return def;
}

void litehtml::el_script::remove_attr(const tchar_t* name)
{
	if(!name)
	{
		return;
	}
	tstring key = name;
	for(size_t i = 0; i < key.length(); i++)
	{
		key[i] = script_tolower_char(key[i]);
	}
	m_script_attrs.erase(key);
}
