#include "html.h"
#include "css_selector.h"
#include "document.h"

namespace
{
	/* CSS escapes inside selector text: a backslash followed by up to six hex
	 * digits (plus one optional whitespace), or by any single character,
	 * stands for that character. Tailwind-style sheets are full of them
	 * (.h-\[826rem\], .m\:h-auto, .\!p-0); without decoding, the parsed
	 * class/id tokens keep the backslashes and never equal the plain tokens
	 * of the class attribute, so every arbitrary-value utility rule misses. */
	unsigned long css_hex_val( litehtml::tchar_t c )
	{
		if(c >= _t('0') && c <= _t('9')) return (unsigned long) (c - _t('0'));
		if(c >= _t('a') && c <= _t('f')) return (unsigned long) (c - _t('a') + 10);
		if(c >= _t('A') && c <= _t('F')) return (unsigned long) (c - _t('A') + 10);
		return 16;
	}

	bool css_is_hex( litehtml::tchar_t c )
	{
		return css_hex_val(c) < 16;
	}

	void append_utf8( litehtml::tstring& out, unsigned long cp )
	{
		if(cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
		if(cp < 0x80)
		{
			out += (litehtml::tchar_t) cp;
		} else if(cp < 0x800)
		{
			out += (litehtml::tchar_t) (0xC0 | (cp >> 6));
			out += (litehtml::tchar_t) (0x80 | (cp & 0x3F));
		} else if(cp < 0x10000)
		{
			out += (litehtml::tchar_t) (0xE0 | (cp >> 12));
			out += (litehtml::tchar_t) (0x80 | ((cp >> 6) & 0x3F));
			out += (litehtml::tchar_t) (0x80 | (cp & 0x3F));
		} else
		{
			out += (litehtml::tchar_t) (0xF0 | (cp >> 18));
			out += (litehtml::tchar_t) (0x80 | ((cp >> 12) & 0x3F));
			out += (litehtml::tchar_t) (0x80 | ((cp >> 6) & 0x3F));
			out += (litehtml::tchar_t) (0x80 | (cp & 0x3F));
		}
	}

	litehtml::tstring css_unescape( const litehtml::tstring& in )
	{
		if(in.find(_t('\\')) == litehtml::tstring::npos)
		{
			return in;
		}
		litehtml::tstring out;
		for(size_t i = 0; i < in.length(); )
		{
			litehtml::tchar_t c = in[i];
			if(c != _t('\\'))
			{
				out += c;
				i++;
				continue;
			}
			if(i + 1 >= in.length())
			{
				break;
			}
			litehtml::tchar_t n = in[i + 1];
			if(css_is_hex(n))
			{
				unsigned long cp = 0;
				size_t j = i + 1;
				int cnt = 0;
				while(j < in.length() && cnt < 6 && css_is_hex(in[j]))
				{
					cp = cp * 16 + css_hex_val(in[j]);
					j++;
					cnt++;
				}
				if(j < in.length() && (in[j] == _t(' ') || in[j] == _t('\t') || in[j] == _t('\n') || in[j] == _t('\r')))
				{
					j++;
				}
				append_utf8(out, cp);
				i = j;
			} else if(n == _t('\n'))
			{
				i += 2;
			} else
			{
				out += n;
				i += 2;
			}
		}
		return out;
	}

	/* Position of the first unescaped character of `chars` at or after from. */
	litehtml::tstring::size_type find_first_unescaped( const litehtml::tstring& txt, const litehtml::tchar_t* chars, litehtml::tstring::size_type from )
	{
		for(size_t i = from; i < txt.length(); i++)
		{
			if(txt[i] == _t('\\'))
			{
				i++;
				continue;
			}
			for(const litehtml::tchar_t* c = chars; *c; c++)
			{
				if(txt[i] == *c)
				{
					return i;
				}
			}
		}
		return litehtml::tstring::npos;
	}

	/* Split a class selector value on unescaped whitespace and decode each
	 * token, so ".foo\ bar" stays one class named "foo bar". */
	void split_class_unescaped( const litehtml::tstring& val, litehtml::string_vector& out )
	{
		litehtml::tstring cur;
		for(size_t i = 0; i < val.length(); i++)
		{
			if(val[i] == _t('\\') && i + 1 < val.length())
			{
				cur += val[i];
				cur += val[i + 1];
				i++;
				continue;
			}
			if(val[i] == _t(' '))
			{
				if(!cur.empty())
				{
					out.push_back(css_unescape(cur));
					cur.clear();
				}
				continue;
			}
			cur += val[i];
		}
		if(!cur.empty())
		{
			out.push_back(css_unescape(cur));
		}
	}

	/* Split selector text into compound and combinator tokens while skipping
	 * escaped characters and bracketed groups, so compounds such as
	 * ".a-\[x\]" survive intact. Mirrors split_string(..., " \t>+~", "([")
	 * for texts without escapes. */
	void tokenize_selector( const litehtml::tstring& text, litehtml::string_vector& tokens )
	{
		litehtml::tstring cur;
		for(size_t i = 0; i < text.length(); )
		{
			litehtml::tchar_t c = text[i];
			if(c == _t('\\') && i + 1 < text.length())
			{
				cur += c;
				cur += text[i + 1];
				i += 2;
				continue;
			}
			if(c == _t('(') || c == _t('['))
			{
				litehtml::tchar_t open = c;
				litehtml::tchar_t close = (c == _t('(')) ? _t(')') : _t(']');
				int depth = 0;
				size_t j = i;
				for(; j < text.length(); )
				{
					if(text[j] == _t('\\') && j + 1 < text.length())
					{
						j += 2;
						continue;
					}
					if(text[j] == open)
					{
						depth++;
					} else if(text[j] == close)
					{
						depth--;
						if(!depth)
						{
							j++;
							break;
						}
					}
					j++;
				}
				if(j > text.length())
				{
					j = text.length();
				}
				cur += text.substr(i, j - i);
				i = j;
				continue;
			}
			if(c == _t(' ') || c == _t('\t') || c == _t('>') || c == _t('+') || c == _t('~'))
			{
				if(!cur.empty())
				{
					tokens.push_back(cur);
					cur.clear();
				}
				tokens.push_back(litehtml::tstring(1, c));
				i++;
				continue;
			}
			cur += c;
			i++;
		}
		if(!cur.empty())
		{
			tokens.push_back(cur);
		}
	}
}

void litehtml::css_element_selector::parse( const tstring& txt )
{
	tstring::size_type el_end = find_first_unescaped(txt, _t(".#[:"), 0);
	m_tag = css_unescape(txt.substr(0, el_end));
	litehtml::lcase(m_tag);
	while(el_end != tstring::npos)
	{
		if(txt[el_end] == _t('.'))
		{
			css_attribute_selector attribute;

			tstring::size_type pos = find_first_unescaped(txt, _t(".#[:"), el_end + 1);
			attribute.val		= txt.substr(el_end + 1, pos - el_end - 1);
			split_class_unescaped( attribute.val, attribute.class_val );
			attribute.condition	= select_equal;
			attribute.attribute	= _t("class");
			m_attrs.push_back(attribute);
			el_end = pos;
		} else if(txt[el_end] == _t(':'))
		{
			css_attribute_selector attribute;

			if(txt[el_end + 1] == _t(':'))
			{
				tstring::size_type pos = find_first_unescaped(txt, _t(".#[:"), el_end + 2);
				attribute.val		= css_unescape(txt.substr(el_end + 2, pos - el_end - 2));
				attribute.condition	= select_pseudo_element;
				litehtml::lcase(attribute.val);
				attribute.attribute	= _t("pseudo-el");
				m_attrs.push_back(attribute);
				el_end = pos;
			} else
			{
				tstring::size_type pos = find_first_unescaped(txt, _t(".#:[("), el_end + 1);
				if(pos != tstring::npos && txt.at(pos) == _t('('))
				{
					pos = find_close_bracket(txt, pos);
					if(pos != tstring::npos)
					{
						pos++;
					} else
					{
						int iii = 0;
						iii++;
					}
				}
				if(pos != tstring::npos)
				{
					attribute.val		= css_unescape(txt.substr(el_end + 1, pos - el_end - 1));
				} else
				{
					attribute.val		= css_unescape(txt.substr(el_end + 1));
				}
				litehtml::lcase(attribute.val);
				if(attribute.val == _t("after") || attribute.val == _t("before"))
				{
					attribute.condition	= select_pseudo_element;
				} else
				{
					attribute.condition	= select_pseudo_class;
				}
				attribute.attribute	= _t("pseudo");
				m_attrs.push_back(attribute);
				el_end = pos;
			}
		} else if(txt[el_end] == _t('#'))
		{
			css_attribute_selector attribute;

			tstring::size_type pos = find_first_unescaped(txt, _t(".#[:"), el_end + 1);
			attribute.val		= css_unescape(txt.substr(el_end + 1, pos - el_end - 1));
			attribute.condition	= select_equal;
			attribute.attribute	= _t("id");
			m_attrs.push_back(attribute);
			el_end = pos;
		} else if(txt[el_end] == _t('['))
		{
			css_attribute_selector attribute;

			tstring::size_type pos = find_first_unescaped(txt, _t("]~=|$*^"), el_end + 1);
			tstring attr = css_unescape(txt.substr(el_end + 1, pos - el_end - 1));
			trim(attr);
			litehtml::lcase(attr);
			if(pos != tstring::npos)
			{
				if(txt[pos] == _t(']'))
				{
					attribute.condition = select_exists;
				} else if(txt[pos] == _t('='))
				{
					attribute.condition = select_equal;
					pos++;
				} else if(txt.substr(pos, 2) == _t("~="))
				{
					attribute.condition = select_contain_str;
					pos += 2;
				} else if(txt.substr(pos, 2) == _t("|="))
				{
					attribute.condition = select_start_str;
					pos += 2;
				} else if(txt.substr(pos, 2) == _t("^="))
				{
					attribute.condition = select_start_str;
					pos += 2;
				} else if(txt.substr(pos, 2) == _t("$="))
				{
					attribute.condition = select_end_str;
					pos += 2;
				} else if(txt.substr(pos, 2) == _t("*="))
				{
					attribute.condition = select_contain_str;
					pos += 2;
				} else
				{
					attribute.condition = select_exists;
					pos += 1;
				}
				pos = txt.find_first_not_of(_t(" \t"), pos);
				if(pos != tstring::npos)
				{
					if(txt[pos] == _t('"'))
					{
						tstring::size_type pos2 = pos + 1;
						while(pos2 < txt.length() && txt[pos2] != _t('"'))
						{
							if(txt[pos2] == _t('\\'))
							{
								pos2++;
							}
							pos2++;
						}
						bool closed = pos2 < txt.length();
						attribute.val = css_unescape(closed ? txt.substr(pos + 1, pos2 - pos - 1) : txt.substr(pos + 1));
						pos = closed ? pos2 + 1 : tstring::npos;
					} else if(txt[pos] == _t(']'))
					{
						pos ++;
					} else
					{
						tstring::size_type pos2 = find_first_unescaped(txt, _t("]"), pos + 1);
						attribute.val = css_unescape(txt.substr(pos, pos2 == tstring::npos ? pos2 : (pos2 - pos)));
						trim(attribute.val);
						pos = pos2 == tstring::npos ? pos2 : (pos2 + 1);
					}
				}
			} else
			{
				attribute.condition = select_exists;
			}
			attribute.attribute	= attr;
			m_attrs.push_back(attribute);
			el_end = pos;
		} else
		{
			el_end++;
		}
		el_end = find_first_unescaped(txt, _t(".#[:"), el_end);
	}
}


bool litehtml::css_selector::parse( const tstring& text )
{
	if(text.empty())
	{
		return false;
	}
	string_vector tokens;
	if(text.find(_t('\\')) == tstring::npos)
	{
		split_string(text, tokens, _t(""), _t(" \t>+~"), _t("(["));
	} else
	{
		tokenize_selector(text, tokens);
	}

	if(tokens.empty())
	{
		return false;
	}

	tstring left;
	tstring right = tokens.back();
	tchar_t combinator = 0;

	tokens.pop_back();
	while(!tokens.empty() && (tokens.back() == _t(" ") || tokens.back() == _t("\t") || tokens.back() == _t("+") || tokens.back() == _t("~") || tokens.back() == _t(">")))
	{
		if(combinator == _t(' ') || combinator == 0)
		{
			combinator = tokens.back()[0];
		}
		tokens.pop_back();
	}

	for(string_vector::const_iterator i = tokens.begin(); i != tokens.end(); i++)
	{
		left += *i;
	}

	trim(left);
	trim(right);

	if(right.empty())
	{
		return false;
	}

	m_right.parse(right);

	switch(combinator)
	{
	case _t('>'):
		m_combinator	= combinator_child;
		break;
	case _t('+'):
		m_combinator	= combinator_adjacent_sibling;
		break;
	case _t('~'):
		m_combinator	= combinator_general_sibling;
		break;
	default:
		m_combinator	= combinator_descendant;
		break;
	}

	m_left = 0;

	if(!left.empty())
	{
		m_left = new css_selector(media_query_list::ptr(0));
		if(!m_left->parse(left))
		{
			return false;
		}
	}

	return true;
}

void litehtml::css_selector::calc_specificity()
{
	if(!m_right.m_tag.empty() && m_right.m_tag != _t("*"))
	{
		m_specificity.d = 1;
	}
	for(css_attribute_selector::vector::iterator i = m_right.m_attrs.begin(); i != m_right.m_attrs.end(); i++)
	{
		if(i->attribute == _t("id"))
		{
			m_specificity.b++;
		} else
		{
			if(i->attribute == _t("class"))
			{
				m_specificity.c += (int) i->class_val.size();
			} else
			{
				m_specificity.c++;
			}
		}	
	}
	if(m_left)
	{
		m_left->calc_specificity();
		m_specificity += m_left->m_specificity;
	}
}

void litehtml::css_selector::add_media_to_doc( document* doc ) const
{
	if(m_media_query && doc)
	{
		doc->add_media_list(m_media_query);
	}
}

