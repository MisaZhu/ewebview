#include "html.h"
#include "el_before_after.h"
#include "el_text.h"
#include "el_space.h"
#include "el_image.h"

litehtml::el_before_after_base::el_before_after_base(litehtml::document* doc, bool before) : html_tag(doc)
{
	m_box_content = false;
	if(before)
	{
		set_tagName(_t("::before"));
	} else
	{
		set_tagName(_t("::after"));
	}
}

litehtml::el_before_after_base::~el_before_after_base()
{

}

void litehtml::el_before_after_base::add_style(const litehtml::style& st, const litehtml::selector_specificity& spec)
{
	html_tag::add_style(st, spec);

	tstring content = get_style_property(_t("content"), false, _t(""));
	if(content == m_content)
	{
		return;
	}

	/* A pseudo-element accumulates declarations from many rules and sheets.
	 * Rebuild generated children only when the winning content value changes;
	 * reparsing it after every unrelated rule duplicates attr()/string text. */
	m_content = content;
	m_children.clear();
	m_box_content = false;
	if(content.empty())
	{
		return;
	}

	int idx = value_index(content.c_str(), content_property_string);
	/* content keywords none/normal suppress the box. Empty quoted strings are
	 * valid generated content: they create a box without a text child. */
	m_box_content = idx < 0 || idx > 1;
	if(idx >= 0)
	{
		return;
	}

	tstring fnc;
	tstring::size_type i = 0;
	/* css-content-3 alternative text: `content: <content-list> / "<alt>"`. The
	 * string after the slash is exposed to assistive technology only and is
	 * never rendered (MDN's external-link icon hides "(external)" this way).
	 * Drop it before tokenizing: a top-level '/' cannot occur inside a content
	 * list item because quoted strings and url()/attr() parentheses are scanned
	 * past, so the first slash outside them is the alt separator. */
	{
		bool in_quote = false;
		tchar_t quote = 0;
		int paren = 0;
		for(tstring::size_type p = 0; p < content.length(); p++)
		{
			tchar_t c = content.at(p);
			if(in_quote)
			{
				if(c == _t('\\'))
				{
					p++;
				} else if(c == quote)
				{
					in_quote = false;
				}
			} else if(c == _t('"') || c == _t('\''))
			{
				in_quote = true;
				quote = c;
			} else if(c == _t('('))
			{
				paren++;
			} else if(c == _t(')'))
			{
				paren--;
			} else if(c == _t('/') && paren == 0)
			{
				content.erase(p);
				break;
			}
		}
	}
	while(i < content.length() && i != tstring::npos)
	{
		if(content.at(i) == _t('"'))
		{
			fnc.clear();
			i++;
			tstring::size_type pos = content.find(_t('"'), i);
			tstring txt;
			if(pos == tstring::npos)
			{
				txt = content.substr(i);
				i = tstring::npos;
			} else
			{
				txt = content.substr(i, pos - i);
				i = pos + 1;
			}
			add_text(txt);
		} else if(content.at(i) == _t('('))
		{
			i++;
			litehtml::trim(fnc);
			litehtml::lcase(fnc);
			tstring::size_type pos = content.find(_t(')'), i);
			tstring params;
			if(pos == tstring::npos)
			{
				params = content.substr(i);
				i = tstring::npos;
			} else
			{
				params = content.substr(i, pos - i);
				i = pos + 1;
			}
			add_function(fnc, params);
			fnc.clear();
		} else
		{
			fnc += content.at(i);
			i++;
		}
	}
}

void litehtml::el_before_after_base::add_text( const tstring& txt )
{
	tstring word;
	tstring esc;
	for(tstring::size_type i = 0; i < txt.length(); i++)
	{
		if( (txt.at(i) == _t(' ')) || (txt.at(i) == _t('\t')) || (txt.at(i) == _t('\\') && !esc.empty()) )
		{
			if(esc.empty())
			{
				if(!word.empty())
				{
					element::ptr el = new el_text(word.c_str(), get_document());
					appendChild(el);
					word.clear();
				}

				element::ptr el = new el_space(txt.substr(i, 1).c_str(), get_document());
				appendChild(el);
			} else
			{
				word += convert_escape(esc.c_str() + 1);
				esc.clear();
				if(txt.at(i) == _t('\\'))
				{
					esc += txt.at(i);
				}
			}
		} else
		{
			if(!esc.empty() || txt.at(i) == _t('\\'))
			{
				esc += txt.at(i);
			} else
			{
				word += txt.at(i);
			}
		}
	}

	if(!esc.empty())
	{
		word += convert_escape(esc.c_str() + 1);
	}
	if(!word.empty())
	{
		element::ptr el = new el_text(word.c_str(), get_document());
		appendChild(el);
		word.clear();
	}
}

void litehtml::el_before_after_base::add_function( const tstring& fnc, const tstring& params )
{
	int idx = value_index(fnc.c_str(), _t("attr;counter;url"));
	switch(idx)
	{
	// attr
	case 0:
		{
			tstring p_name = params;
			trim(p_name);
			lcase(p_name);
			element::ptr el_parent = parent();
			if (el_parent)
			{
				const tchar_t* attr_value = el_parent->get_attr(p_name.c_str());
				if (attr_value)
				{
					add_text(attr_value);
				}
			}
		}
		break;
	// counter
	case 1:
		break;
	// url
	case 2:
		{
			tstring p_url = params;
			trim(p_url);
			if(!p_url.empty())
			{
				if(p_url.at(0) == _t('\'') || p_url.at(0) == _t('\"'))
				{
					p_url.erase(0, 1);
				}
			}
			if(!p_url.empty())
			{
				if(p_url.at(p_url.length() - 1) == _t('\'') || p_url.at(p_url.length() - 1) == _t('\"'))
				{
					p_url.erase(p_url.length() - 1, 1);
				}
			}
			if(!p_url.empty())
			{
				element::ptr el = new el_image(get_document());
				el->set_attr(_t("src"), p_url.c_str());
				el->set_attr(_t("style"), _t("display:inline-block"));
				el->set_tagName(_t("img"));
				appendChild(el);
				el->parse_attributes();
			}
		}
		break;
	}
}

litehtml::tstring litehtml::el_before_after_base::convert_escape( const tchar_t* txt )
{
	tchar_t* sss = 0;
	unsigned long cp = (unsigned long) t_strtol(txt, &sss, 16);
	/* CSS Syntax 3 §4.3.6: a hex escape denotes a Unicode code point; 0,
	 * surrogates and >10FFFF become U+FFFD. Encode as UTF-8 - returning a
	 * single tchar_t truncated 0x200B to a 0x0B control byte, which the font
	 * painted as a .notdef hollow box (MDN's a.external::after invisible
	 * anchor `content:"\200b"` showed up as a tofu square). */
	if(cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
	tstring out;
	if(cp < 0x80)
	{
		out += (tchar_t)cp;
	}
	else if(cp < 0x800)
	{
		out += (tchar_t)(0xC0 | (cp >> 6));
		out += (tchar_t)(0x80 | (cp & 0x3F));
	}
	else if(cp < 0x10000)
	{
		out += (tchar_t)(0xE0 | (cp >> 12));
		out += (tchar_t)(0x80 | ((cp >> 6) & 0x3F));
		out += (tchar_t)(0x80 | (cp & 0x3F));
	}
	else
	{
		out += (tchar_t)(0xF0 | (cp >> 18));
		out += (tchar_t)(0x80 | ((cp >> 12) & 0x3F));
		out += (tchar_t)(0x80 | ((cp >> 6) & 0x3F));
		out += (tchar_t)(0x80 | (cp & 0x3F));
	}
	return out;
}

void litehtml::el_before_after_base::apply_stylesheet( const litehtml::css& stylesheet )
{

}

litehtml::style_display litehtml::el_before_after_base::get_display() const
{
	if(!m_box_content)
	{
		return display_none;
	}
	return html_tag::get_display();
}
