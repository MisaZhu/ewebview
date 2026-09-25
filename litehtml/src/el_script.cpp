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
	/* REPLACE, not accumulate: js_set_element_text() drops the old children and
	 * appends a single fresh el_text per textContent assignment, so a re-assigned
	 * body must not concatenate onto the previous one. */
	tstring t;
	el->get_text(t);
	m_text = t;
	return true;
}

void litehtml::el_script::get_text( tstring& text )
{
	text += m_text;
}

const litehtml::tchar_t* litehtml::el_script::get_tagName() const
{
	return _t("script");
}

/* el_script derives from element (not html_tag), whose select_all is a no-op,
 * so a <script> node was invisible to getElementsByTagName("script") and
 * document.scripts. Match the selector's right-hand tag against our fixed tag
 * name plus exact/exists attribute tests (enough for script[src] style
 * lookups); pseudo classes are ignored, and scripts have no element children
 * to recurse into (appendChild captures text). */
void litehtml::el_script::select_all(const css_selector& selector, elements_vector& res)
{
	const css_element_selector& right = selector.m_right;
	if(!right.m_tag.empty() && right.m_tag != _t("*") && right.m_tag != get_tagName())
	{
		return;
	}
	for(const auto& attr_sel : right.m_attrs)
	{
		const tchar_t* own = get_attr(attr_sel.attribute.c_str());
		if(attr_sel.condition == select_exists)
		{
			if(own == nullptr) return;
		}
		else if(attr_sel.condition == select_equal)
		{
			if(own == nullptr || t_strcasecmp(own, attr_sel.val.c_str()) != 0) return;
		}
		else if(attr_sel.condition == select_contain_str)
		{
			/* `[attr~=v]` / `[attr*=v]`: css_selector folds both to a substring
			 * test. github's catalyst `@target` accessor looks its JSON island up
			 * with `[data-target~="react-app.embeddedData"]`; refusing substring
			 * conditions made every data island invisible to querySelectorAll and
			 * the element's connectedCallback aborted with "No embedded data". */
			if(own == nullptr || t_strstr(own, attr_sel.val.c_str()) == nullptr) return;
		}
		else if(attr_sel.condition == select_start_str)
		{
			if(own == nullptr || t_strncmp(own, attr_sel.val.c_str(), attr_sel.val.length()) != 0) return;
		}
		else if(attr_sel.condition == select_end_str)
		{
			if(own == nullptr)
				return;
			size_t own_len = t_strlen(own);
			size_t val_len = attr_sel.val.length();
			if(val_len > own_len ||
			   t_strncmp(own + (own_len - val_len), attr_sel.val.c_str(), val_len) != 0)
				return;
		}
		else
		{
			return; /* unsupported condition: do not claim a match */
		}
	}
	res.push_back(this);
}

litehtml::element::ptr litehtml::el_script::select_one(const css_selector& selector)
{
	elements_vector res;
	select_all(selector, res);
	return res.empty() ? nullptr : res.front();
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
