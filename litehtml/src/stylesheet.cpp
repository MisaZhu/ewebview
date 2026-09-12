#include "html.h"
#include "stylesheet.h"
#include <algorithm>
#include "document.h"
#include <ewoksys/klog.h>

/*
 * xBrowser render/network diagnostics. Silenced by default so the console is
 * not flooded during normal browsing; build with -DXBROWSER_DEBUG=1 to enable.
 * The "if (0)" form keeps argument expressions referenced so perf-timing
 * locals do not trip -Wunused when logging is off.
 */
#ifndef XBROWSER_DEBUG
#define XBROWSER_DEBUG 0
#endif
#if !XBROWSER_DEBUG
/* Gate klog() itself (every klog() here is an [xBrowser] trace). The macro's
 * self-reference is not re-expanded, so the real klog() stays under "if (0)". */
#define klog(...) do { if (0) klog(__VA_ARGS__); } while (0)
#endif

namespace litehtml
{

static const tchar_t* skip_whitespace(const tchar_t* p)
{
	while(*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
	return p;
}

static const tchar_t* skip_comment(const tchar_t* p)
{
	if(p[0] == '/' && p[1] == '*')
	{
		p += 2;
		while(*p && !(p[0] == '*' && p[1] == '/')) p++;
		if(*p) p += 2;
	}
	return p;
}

static const tchar_t* skip_comments_and_ws(const tchar_t* p)
{
	while(true)
	{
		const tchar_t* next = skip_whitespace(p);
		next = skip_comment(next);
		if(next == p) break;
		p = next;
	}
	return p;
}

static const tchar_t* find_char(const tchar_t* p, tchar_t c)
{
	while(*p && *p != c) p++;
	return p;
}

static const tchar_t* find_close_bracket_ptr(const tchar_t* p, tchar_t open_b, tchar_t close_b)
{
	if(*p != open_b) return p;
	int cnt = 1;
	p++;
	while(*p && cnt)
	{
		if(*p == open_b) cnt++;
		else if(*p == close_b) cnt--;
		p++;
	}
	return p;
}

static tstring ptr_to_string(const tchar_t* start, const tchar_t* end)
{
	return tstring(start, end - start);
}

void css::parse_stylesheet(const tchar_t* str, const tchar_t* baseurl, document* doc, const media_query_list::ptr& media)
{
	const tchar_t* p = str;

	while(*p)
	{
		p = skip_comments_and_ws(p);
		if(!*p) break;

		while(*p == '@')
		{
			const tchar_t* sPos = p;
			const tchar_t* brace = find_char(p, '{');
			const tchar_t* semi = find_char(p, ';');
			if(semi < brace)
			{
				/* Statement at-rule (@charset, @namespace, @layer a,b; and
				 * @import url(...);): ends at ';' before any block. Scanning
				 * for '{' here would swallow the following style rule. */
				parse_atrule(ptr_to_string(sPos, semi), baseurl, doc, media);
				p = semi + 1;
			}
			else
			{
				p = brace;
				if(*p == '{')
				{
					p = find_close_bracket_ptr(p, '{', '}');
				}
				/* p may sit on the terminating '\0' when the at-rule closes the
				 * text; the block is still complete and must be parsed (testing
				 * *p here dropped the last at-rule of every minified sheet). */
				if(p > sPos)
				{
					parse_atrule(ptr_to_string(sPos, p), baseurl, doc, media);
				}
			}
			p = skip_comments_and_ws(p);
		}

		if(!*p) break;

		const tchar_t* style_start = find_char(p, '{');
		/* Balanced close: native CSS nesting puts inner '{...}' blocks inside
		 * the declaration text; stopping at the first '}' would desync the
		 * scan and drop the rest of the sheet. Nested rules land in the
		 * declaration text where style::add ignores them as malformed. */
		const tchar_t* style_end = *style_start ? find_close_bracket_ptr(style_start, '{', '}') : style_start;
		/* style_end points one past the closing '}' and may be the
		 * terminating '\0' when the rule ends the text - every @media body
		 * and every minified sheet ends that way, so *style_end must not
		 * gate the parse (it used to drop the final rule of each of them). */
		if(*style_start && style_end > style_start + 1)
		{
			style::ptr st = new style();
			st->add(style_start + 1, style_end - style_start - 2, baseurl);

			parse_selectors(ptr_to_string(p, style_start), st, media);

			if(media && doc)
			{
				doc->add_media_list(media);
			}

			p = style_end;
		} else
		{
			break;
		}

		p = skip_comments_and_ws(p);
	}
}

void css::parse_css_url( const tstring& str, tstring& url )
{
	url = _t("");
	size_t pos1 = str.find(_t('('));
	size_t pos2 = str.find(_t(')'));
	if(pos1 == tstring::npos || pos2 == tstring::npos || pos2 < pos1)
	{
		return;
	}
	/* Only url(...) names a fetchable image. Any other function --
	 * linear-gradient(), radial-gradient(), image-set() -- is paint syntax,
	 * not a reference: feeding its argument list to the loader used to send
	 * the download thread off resolving "45deg, #ececec 25%, ..." as a host
	 * name (a 3-second DNS stall per gradient, plus log spam). */
	{
		tstring func = str.substr(0, pos1);
		trim(func);
		for(size_t i = 0; i < func.length(); i++)
		{
			if(func[i] >= _t('A') && func[i] <= _t('Z'))
			{
				func[i] = (tchar_t)(func[i] - _t('A') + _t('a'));
			}
		}
		if(!func.empty() && func != _t("url"))
		{
			return;
		}
	}
	url = str.substr(pos1 + 1, pos2 - pos1 - 1);
	if(url.length())
	{
		if(url[0] == _t('\'') || url[0] == _t('"'))
		{
			url.erase(0, 1);
		}
	}
	if(url.length())
	{
		if(url[url.length() - 1] == _t('\'') || url[url.length() - 1] == _t('"'))
		{
			url.erase(url.length() - 1, 1);
		}
	}
}

bool css::parse_selectors( const tstring& txt, const litehtml::style::ptr& styles, const media_query_list::ptr& media )
{
	tstring selector = txt;
	trim(selector);
	string_vector tokens;
	split_string(selector, tokens, _t(","));

	bool added_something = false;

	for(string_vector::iterator tok = tokens.begin(); tok != tokens.end(); tok++)
	{
		css_selector::ptr selector = new css_selector(media);
		selector->m_style = styles;
		trim(*tok);
		if(selector->parse(*tok))
		{
			selector->calc_specificity();
			add_selector(selector);
			added_something = true;
		}
	}

	return added_something;
}

void css::sort_selectors()
{
	m_index.built = false;
	/* Stable: equal specificity must keep source order so that the later
	 * rule wins the cascade (std::sort let same-specificity rules land in
	 * arbitrary order, e.g. a display:flex rule beating a later display:grid
	 * rule on the same selector). */
	std::stable_sort(m_selectors.begin(), m_selectors.end(),
		 [](const css_selector::ptr& v1, const css_selector::ptr& v2)
		 {
			 return (*v1) < (*v2);
		 }
	);
}

static tstring index_key_lower(const tstring& src)
{
	tstring out = src;
	for(size_t i = 0; i < out.length(); i++)
	{
		if(out[i] >= 'A' && out[i] <= 'Z')
		{
			out[i] = (tchar_t)(out[i] - 'A' + 'a');
		}
	}
	return out;
}

css::selector_index& css::get_selector_index() const
{
	if(m_index.built)
	{
		return m_index;
	}
	m_index.buckets.clear();
	m_index.universal.clear();
	m_index.stamps.assign(m_selectors.size(), 0);
	m_index.epoch = 0;
	for(size_t i = 0; i < m_selectors.size(); i++)
	{
		const css_element_selector& right = m_selectors[i]->m_right;
		int kind = 0;
		tstring key;
		for(css_attribute_selector::vector::const_iterator it = right.m_attrs.begin(); it != right.m_attrs.end(); ++it)
		{
			if(it->attribute == _t("class") && it->condition == select_equal && !it->class_val.empty())
			{
				kind = 1;
				key = index_key_lower(it->class_val.front());
				break;
			}
		}
		if(!kind)
		{
			for(css_attribute_selector::vector::const_iterator it = right.m_attrs.begin(); it != right.m_attrs.end(); ++it)
			{
				if(it->attribute == _t("id") && it->condition == select_equal && !it->val.empty())
				{
					kind = 2;
					key = index_key_lower(it->val);
					break;
				}
			}
		}
		if(!kind && !right.m_tag.empty() && right.m_tag != _t("*"))
		{
			kind = 3;
			key = index_key_lower(right.m_tag);
		}
		if(kind)
		{
			selector_index::key_t key2(kind, key);
			// buckets are unsorted while building: linear scan for the key
			std::vector<int>* bucket = 0;
			for(size_t b = 0; b < m_index.buckets.size(); b++)
			{
				if(m_index.buckets[b].first.first == kind && m_index.buckets[b].first.second == key)
				{
					bucket = &m_index.buckets[b].second;
					break;
				}
			}
			if(!bucket)
			{
				m_index.buckets.push_back(selector_index::bucket_t(key2, std::vector<int>()));
				bucket = &m_index.buckets.back().second;
			}
			bucket->push_back((int)i);
		}
		else
		{
			m_index.universal.push_back((int)i);
		}
	}
	std::sort(m_index.buckets.begin(), m_index.buckets.end(),
		 [](const selector_index::bucket_t& a, const selector_index::bucket_t& b)
		 {
			if(a.first.first != b.first.first)
			{
				return a.first.first < b.first.first;
			}
			return a.first.second < b.first.second;
		 }
	);
	m_index.built = true;
	klog("[xBrowser] selector index: buckets=%u universal=%u selectors=%u\n",
		(unsigned)m_index.buckets.size(), (unsigned)m_index.universal.size(), (unsigned)m_selectors.size());
	return m_index;
}

/* True when the engine lays out CSS grid containers; @supports evaluation
 * uses it to decide whether '(display: grid)' feature queries hold. */
static bool supports_display_grid()
{
	return true;
}

void css::parse_atrule(const tstring& text, const tchar_t* baseurl, document* doc, const media_query_list::ptr& media)
{
	if(text.substr(0, 7) == _t("@import"))
	{
		int sPos = 7;
		tstring iStr;
		iStr = text.substr(sPos);
		if(iStr[iStr.length() - 1] == _t(';'))
		{
			iStr.erase(iStr.length() - 1);
		}
		trim(iStr);
		string_vector tokens;
		split_string(iStr, tokens, _t(" "), _t(""), _t("(\""));
		if(!tokens.empty())
		{
			tstring url;
			parse_css_url(tokens.front(), url);
			if(url.empty())
			{
				url = tokens.front();
			}
			tokens.erase(tokens.begin());
			if(doc)
			{
				document_container* doc_cont = doc->container();
				if(doc_cont)
				{
					tstring css_text;
					tstring css_baseurl;
					if(baseurl)
					{
						css_baseurl = baseurl;
					}
					doc_cont->import_css(css_text, url, css_baseurl);
					if(!css_text.empty())
					{
						media_query_list::ptr new_media = media;
						if(!tokens.empty())
						{
							tstring media_str;
							for(string_vector::iterator iter = tokens.begin(); iter != tokens.end(); iter++)
							{
								if(iter != tokens.begin())
								{
									media_str += _t(" ");
								}
								media_str += (*iter);
							}
							new_media = media_query_list::create_from_string(media_str, doc);
							if(!new_media)
							{
								new_media = media;
							}
						}
						parse_stylesheet(css_text.c_str(), css_baseurl.c_str(), doc, new_media);
					}
				}
			}
		}
	} else if(text.substr(0, 6) == _t("@media"))
	{
		tstring::size_type b1 = text.find_first_of(_t('{'));
		tstring::size_type b2 = text.find_last_of(_t('}'));
		if(b1 != tstring::npos)
		{
			tstring media_type = text.substr(6, b1 - 6);
			trim(media_type);
			media_query_list::ptr new_media = media_query_list::create_from_string(media_type, doc);

			tstring media_style;
			if(b2 != tstring::npos)
			{
				media_style = text.substr(b1 + 1, b2 - b1 - 1);
			} else
			{
				media_style = text.substr(b1 + 1);
			}

			parse_stylesheet(media_style.c_str(), baseurl, doc, new_media);
		}
	} else if(text.substr(0, 6) == _t("@layer"))
	{
		/* Cascade layers: layer ordering is not implemented, but dropping
		 * the body (previous behaviour) lost every rule on sites that wrap
		 * their stylesheet in @layer. Parse the body as ordinary rules at
		 * the current media; source order approximates layer order. */
		tstring::size_type b1 = text.find_first_of(_t('{'));
		tstring::size_type b2 = text.find_last_of(_t('}'));
		if(b1 != tstring::npos)
		{
			tstring layer_style = (b2 != tstring::npos) ? text.substr(b1 + 1, b2 - b1 - 1) : text.substr(b1 + 1);
			parse_stylesheet(layer_style.c_str(), baseurl, doc, media);
		}
	} else if(text.substr(0, 9) == _t("@supports"))
	{
		/* Feature queries: sites (e.g. w3.org) gate their grid rules behind
		 * '@supports (display: grid)' with a flex fallback outside. Dropping
		 * the whole block left such pages stuck on the fallback layout, so
		 * evaluate the condition against what this engine really renders and
		 * parse the body when it holds. */
		tstring::size_type b1 = text.find_first_of(_t('{'));
		if(b1 != tstring::npos)
		{
			tstring cond = text.substr(9, b1 - 9);
			trim(cond);
			/* Body braces must be matched, not last-of: @supports nested in
			 * @media (or vice versa) puts another '}' after this block. */
			tstring::size_type b2 = find_close_bracket(text, b1, _t('{'), _t('}'));
			tstring body = (b2 != tstring::npos && b2 > b1 + 1) ? text.substr(b1 + 1, b2 - b1 - 1) : tstring();
			if(supports_condition_holds(cond) && !body.empty())
			{
				parse_stylesheet(body.c_str(), baseurl, doc, media);
			}
		}
	}
}

bool css::supports_condition_holds(const tstring& cond)
{
	/* Strip wrapping parentheses: '((display: grid))' == '(display: grid)'. */
	tstring c = cond;
	trim(c);
	while(!c.empty() && c[0] == _t('('))
	{
		tstring::size_type close = find_close_bracket(c, 0);
		if(close != c.length() - 1)
		{
			break;	/* '(a) and (b)' -- not a single wrapped group */
		}
		c = c.substr(1, c.length() - 2);
		trim(c);
	}

	if(c.empty())
	{
		return false;
	}

	/* 'not (...)' inverts the wrapped condition. */
	if(c.substr(0, 4) == _t("not "))
	{
		tstring inner = c.substr(4);
		trim(inner);
		return !supports_condition_holds(inner);
	}

	/* Conjunctive/disjunctive forms: evaluate the first declaration only.
		 * Compound conditions are rare in the wild; a single-feature answer
		 * keeps the common 'and'-chained queries close to correct. */
	tstring::size_type kw = c.find(_t(" and "));
	if(kw == tstring::npos) kw = c.find(_t(" or "));
	if(kw != tstring::npos)
	{
		return supports_condition_holds(c.substr(0, kw));
	}

	/* 'selector(...)' syntax: no selector support query is honoured. */
	if(c.substr(0, 9) == _t("selector("))
	{
		return false;
	}

	/* Declaration test '(property: value)'. */
	tstring::size_type colon = c.find(_t(':'));
	if(colon == tstring::npos)
	{
		return false;
	}
	tstring prop = c.substr(0, colon);
	tstring val = c.substr(colon + 1);
	trim(prop);
	trim(val);
	if(t_strcasecmp(prop.c_str(), _t("display")) == 0)
	{
		if(t_strcasecmp(val.c_str(), _t("grid")) == 0 ||
			t_strcasecmp(val.c_str(), _t("inline-grid")) == 0)
		{
			return supports_display_grid();
		}
		if(t_strcasecmp(val.c_str(), _t("flex")) == 0 ||
			t_strcasecmp(val.c_str(), _t("inline-flex")) == 0)
		{
			return true;
		}
		return false;
	}
	/* Unknown property: conservatively false so the fallback rules outside
	 * the query stay in charge. */
	return false;
}

} // namespace litehtml
