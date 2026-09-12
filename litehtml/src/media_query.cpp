#include "html.h"
#include "media_query.h"
#include "document.h"
#include <math.h>

/* Numeric value of a media range-syntax operand: a <length> in px
 * ("1012px", "87.5rem", ".02px"), a <resolution> in dpi ("192dpi", "2x",
 * "2dppx"), a bare number, or a simple additive calc() over those
 * ("calc(48rem - .02px)" is what real-world sheets emit for open ranges). */
static bool media_range_value(const litehtml::tstring& str, litehtml::document* doc, double& out)
{
	litehtml::tstring s = str;
	litehtml::trim(s);
	if(s.empty())
	{
		return false;
	}
	if(s.length() > 6 && s.compare(0, 5, _t("calc(")) == 0 && s[s.length() - 1] == _t(')'))
	{
		litehtml::tstring inner = s.substr(5, s.length() - 6);
		/* find the last top-level additive operator; CSS requires spaces
		 * around + and - inside calc(), which also keeps negative signs and
		 * unit suffixes from being taken for operators */
		int depth = 0;
		for(litehtml::tstring::size_type i = inner.length() - 1; i > 0; i--)
		{
			litehtml::tchar_t c = inner[i];
			if(c == _t(')'))
			{
				depth++;
			} else if(c == _t('('))
			{
				depth--;
			} else if(depth == 0 && (c == _t('+') || c == _t('-')) &&
				i + 1 < inner.length() &&
				isspace((unsigned char) inner[i - 1]) && isspace((unsigned char) inner[i + 1]))
			{
				double a = 0, b = 0;
				if(!media_range_value(inner.substr(0, i), doc, a) ||
				   !media_range_value(inner.substr(i + 1), doc, b))
				{
					return false;
				}
				out = (c == _t('+')) ? a + b : a - b;
				return true;
			}
		}
		return media_range_value(inner, doc, out);
	}
	/* dppx / x resolutions are not css_length units */
	if(s.length() > 4 && s.compare(s.length() - 4, 4, _t("dppx")) == 0)
	{
		out = t_strtod(s.substr(0, s.length() - 4).c_str(), 0) * 96.0;
		return true;
	}
	if(s.length() > 1 && s[s.length() - 1] == _t('x') && s[s.length() - 2] != _t('p'))
	{
		out = t_strtod(s.substr(0, s.length() - 1).c_str(), 0) * 96.0;
		return true;
	}
	litehtml::css_length length;
	length.fromString(s);
	if(length.is_predefined())
	{
		return false;
	}
	switch(length.units())
	{
	case litehtml::css_units_dpi:
		out = length.val();
		return true;
	case litehtml::css_units_dpcm:
		out = length.val() * 2.54;
		return true;
	case litehtml::css_units_none:
	case litehtml::css_units_px:
		out = length.val();
		return true;
	case litehtml::css_units_em:
	case litehtml::css_units_rem:
		/* media queries resolve font-relative units against the initial
		 * font size, never the styled root element */
		out = length.val() * (doc ? doc->container()->get_default_font_size() : 16);
		return true;
	default:
		if(doc)
		{
			doc->cvt_units(length, doc->container()->get_default_font_size());
			out = length.val();
			return true;
		}
		return false;
	}
}

/* comparison operators of the media range syntax */
enum media_range_op
{
	media_range_op_lt,
	media_range_op_le,
	media_range_op_gt,
	media_range_op_ge,
	media_range_op_eq
};

static int media_range_op_mirror(int op)
{
	switch(op)
	{
	case media_range_op_lt: return media_range_op_gt;
	case media_range_op_le: return media_range_op_ge;
	case media_range_op_gt: return media_range_op_lt;
	case media_range_op_ge: return media_range_op_le;
	}
	return op;
}

/* Emit "feature op value" as an equivalent min-XXX / max-XXX expression;
 * those variants sit right after the base feature in media_feature_strings
 * so check() needs no new cases. Fractional and strict bounds fold into the
 * integer val the same way the viewport is measured (whole px). */
static bool media_range_emit(int base_feature, int op, double v, litehtml::media_query_expression::vector& out)
{
	switch(base_feature)
	{
	case litehtml::media_feature_width:
	case litehtml::media_feature_height:
	case litehtml::media_feature_device_width:
	case litehtml::media_feature_device_height:
	case litehtml::media_feature_color:
	case litehtml::media_feature_color_index:
	case litehtml::media_feature_monochrome:
	case litehtml::media_feature_resolution:
		break;
	default:
		return false;
	}
	litehtml::media_query_expression expr;
	expr.check_as_bool = false;
	switch(op)
	{
	case media_range_op_lt:
		expr.feature = (litehtml::media_feature) (base_feature + 2);	/* max-* */
		expr.val = (int) ceil(v) - 1;
		break;
	case media_range_op_le:
		expr.feature = (litehtml::media_feature) (base_feature + 2);	/* max-* */
		expr.val = (int) floor(v);
		break;
	case media_range_op_gt:
		expr.feature = (litehtml::media_feature) (base_feature + 1);	/* min-* */
		expr.val = (int) floor(v) + 1;
		break;
	case media_range_op_ge:
		expr.feature = (litehtml::media_feature) (base_feature + 1);	/* min-* */
		expr.val = (int) ceil(v);
		break;
	case media_range_op_eq:
		expr.feature = (litehtml::media_feature) base_feature;
		expr.val = (int) (v + 0.5);
		break;
	default:
		return false;
	}
	out.push_back(expr);
	return true;
}

/* Media Queries Level 4 range syntax: "width>=1012px", "width<=1011.98px",
 * "1012px<=width", "48rem<=width<=87.5rem". Returns false when the
 * expression cannot be understood, which must make the whole query unknown
 * (never-matching), not vacuously true. */
static bool parse_media_range(const litehtml::tstring& s, litehtml::document* doc,
	litehtml::media_query_expression::vector& out)
{
	litehtml::string_vector parts;
	std::vector<int> ops;
	int depth = 0;
	litehtml::tstring::size_type start = 0;
	for(litehtml::tstring::size_type i = 0; i < s.length(); i++)
	{
		litehtml::tchar_t c = s[i];
		if(c == _t('('))
		{
			depth++;
		} else if(c == _t(')'))
		{
			depth--;
		} else if(depth == 0 && (c == _t('<') || c == _t('>') || c == _t('=')))
		{
			parts.push_back(s.substr(start, i - start));
			if(c == _t('=') )
			{
				ops.push_back(media_range_op_eq);
			} else if(i + 1 < s.length() && s[i + 1] == _t('='))
			{
				ops.push_back(c == _t('<') ? media_range_op_le : media_range_op_ge);
				i++;
			} else
			{
				ops.push_back(c == _t('<') ? media_range_op_lt : media_range_op_gt);
			}
			start = i + 1;
		}
	}
	parts.push_back(s.substr(start));
	for(litehtml::string_vector::iterator p = parts.begin(); p != parts.end(); p++)
	{
		litehtml::trim(*p);
	}

	if(parts.size() == 2 && ops.size() == 1)
	{
		int feat = litehtml::value_index(parts[0], media_feature_strings, litehtml::media_feature_none);
		double v = 0;
		if(feat != litehtml::media_feature_none)
		{
			return media_range_value(parts[1], doc, v) && media_range_emit(feat, ops[0], v, out);
		}
		feat = litehtml::value_index(parts[1], media_feature_strings, litehtml::media_feature_none);
		if(feat != litehtml::media_feature_none)
		{
			return media_range_value(parts[0], doc, v) &&
				media_range_emit(feat, media_range_op_mirror(ops[0]), v, out);
		}
		return false;
	}
	if(parts.size() == 3 && ops.size() == 2)
	{
		/* "v1 op feature op v2": the feature sits in the middle */
		int feat = litehtml::value_index(parts[1], media_feature_strings, litehtml::media_feature_none);
		double v1 = 0, v2 = 0;
		if(feat == litehtml::media_feature_none ||
		   !media_range_value(parts[0], doc, v1) || !media_range_value(parts[2], doc, v2))
		{
			return false;
		}
		return media_range_emit(feat, media_range_op_mirror(ops[0]), v1, out) &&
			media_range_emit(feat, ops[1], v2, out);
	}
	return false;
}


litehtml::media_query::media_query()
{
	m_media_type	= media_type_all;
	m_not			= false;
	m_unknown		= false;
}

litehtml::media_query::media_query( const media_query& val )
{
	m_not			= val.m_not;
	m_expressions	= val.m_expressions;
	m_media_type	= val.m_media_type;
	m_unknown		= val.m_unknown;
}

litehtml::media_query::ptr litehtml::media_query::create_from_string(const tstring& str, document* doc)
{
	media_query::ptr query = new media_query();

	string_vector tokens;
	split_string(str, tokens, _t(" \t\r\n"), _t(""), _t("("));

	for(string_vector::iterator tok = tokens.begin(); tok != tokens.end(); tok++)
	{
		if((*tok) == _t("not"))
		{
			query->m_not = true;
		} else if(tok->at(0) == _t('('))
		{
			tok->erase(0, 1);
			if(tok->at(tok->length() - 1) == _t(')'))
			{
				tok->erase(tok->length() - 1, 1);
			}
			if(tok->find_first_of(_t("<>=")) != tstring::npos)
			{
				/* Level 4 range syntax ((width>=1012px)); a colon never
				 * appears together with a comparison operator. */
				if(!parse_media_range(*tok, doc, query->m_expressions))
				{
					query->m_unknown = true;
				}
				continue;
			}
			media_query_expression expr;
			string_vector expr_tokens;
			split_string((*tok), expr_tokens, _t(":"));
			if(!expr_tokens.empty())
			{
				trim(expr_tokens[0]);
				expr.feature = (media_feature) value_index(expr_tokens[0], media_feature_strings, media_feature_none);
				if(expr.feature != media_feature_none)
				{
					if(expr_tokens.size() == 1)
					{
						expr.check_as_bool = true;
					} else
					{
						trim(expr_tokens[1]);
						expr.check_as_bool = false;
						if(expr.feature == media_feature_orientation)
						{
							expr.val = value_index(expr_tokens[1], media_orientation_strings, media_orientation_landscape);
						} else
						if(expr.feature == media_feature_prefers_color_scheme)
						{
							/* This browser only ships a light theme. */
							expr.val = value_index(expr_tokens[1], _t("dark;light"), 1);
						} else
						if(expr.feature == media_feature_prefers_contrast)
						{
							expr.val = value_index(expr_tokens[1], _t("no-preference;more;less;custom"), 0);
						} else
						if(expr.feature == media_feature_prefers_reduced_motion)
						{
							expr.val = value_index(expr_tokens[1], _t("no-preference;reduce"), 0);
						} else
						if(expr.feature == media_feature_forced_colors)
						{
							expr.val = value_index(expr_tokens[1], _t("none;active"), 0);
						} else
						if(expr.feature == media_feature_pointer || expr.feature == media_feature_any_pointer)
						{
							/* Mouse-driven desktop: fine pointer. */
							expr.val = value_index(expr_tokens[1], _t("none;coarse;fine"), 2);
						} else
						if(expr.feature == media_feature_hover || expr.feature == media_feature_any_hover)
						{
							expr.val = value_index(expr_tokens[1], _t("none;hover"), 1);
						} else
						if(expr.feature == media_feature_display_mode)
						{
							expr.val = value_index(expr_tokens[1], _t("browser;fullscreen;standalone;minimal-ui"), 0);
						} else
						{
							tstring::size_type slash_pos = expr_tokens[1].find(_t('/'));
							if( slash_pos != tstring::npos )
							{
								tstring val1 = expr_tokens[1].substr(0, slash_pos);
								tstring val2 = expr_tokens[1].substr(slash_pos + 1);
								trim(val1);
								trim(val2);
								expr.val = t_atoi(val1.c_str());
								expr.val2 = t_atoi(val2.c_str());
							} else
							{
								css_length length;
								length.fromString(expr_tokens[1]);
								if(length.units() == css_units_dpcm)
								{
									expr.val = (int) (length.val() * 2.54);
								} else if(length.units() == css_units_dpi)
								{
									expr.val = (int) (length.val() * 2.54);
								} else
								{
									if(doc)
									{
										doc->cvt_units(length, doc->container()->get_default_font_size());
									}
									expr.val = (int) length.val();
								}
							}
						}
					}
					query->m_expressions.push_back(expr);
				} else
				{
					/* Feature name not in media_feature_strings: the expression
					 * can never be evaluated, so the whole query must never
					 * match (dropping it here would make the query vacuously
					 * true and apply e.g. dark-scheme styles unconditionally). */
					query->m_unknown = true;
				}
			}
		} else
		{
			query->m_media_type = (media_type) value_index((*tok), media_type_strings, media_type_all);

		}
	}

	return query;
}

bool litehtml::media_query::check( const media_features& features ) const
{
	bool res = false;
	if(m_unknown)
	{
		/* Unrecognised feature: never matches, before the not-inversion. */
		return false;
	}
	if(m_media_type == media_type_all || m_media_type == features.type)
	{
		res = true;
		for(media_query_expression::vector::const_iterator expr = m_expressions.begin(); expr != m_expressions.end() && res; expr++)
		{
			if(!expr->check(features))
			{
				res = false;
			}
		}
	}

	if(m_not)
	{
		res = !res;
	}

	return res;
}

//////////////////////////////////////////////////////////////////////////

litehtml::media_query_list::ptr litehtml::media_query_list::create_from_string(const tstring& str, document* doc)
{
	media_query_list::ptr list = new media_query_list();

	string_vector tokens;
	split_string(str, tokens, _t(","));

	for(string_vector::iterator tok = tokens.begin(); tok != tokens.end(); tok++)
	{
		trim(*tok);
		lcase(*tok);

		litehtml::media_query::ptr query = media_query::create_from_string(*tok, doc);
		if(query)
		{
			list->m_queries.push_back(query);
		}
	}
	if(list->m_queries.empty())
	{
		list = 0;
	}

	return list;
}

bool litehtml::media_query_list::apply_media_features( const media_features& features )
{
	bool apply = false;
	
	for(media_query::vector::iterator iter = m_queries.begin(); iter != m_queries.end() && !apply; iter++)
	{
		if((*iter)->check(features))
		{
			apply = true;
		}
	}

	bool ret = (apply != m_is_used);
	m_is_used = apply;
	return ret;
}

bool litehtml::media_query_expression::check( const media_features& features ) const
{
	switch(feature)
	{
	case media_feature_width:
		if(check_as_bool)
		{
			return (features.width != 0);
		} else if(features.width == val)
		{
			return true;
		}
		break;
	case media_feature_min_width:
		if(features.width >= val)
		{
			return true;
		}
		break;
	case media_feature_max_width:
		if(features.width <= val)
		{
			return true;
		}
		break;
	case media_feature_height:
		if(check_as_bool)
		{
			return (features.height != 0);
		} else if(features.height == val)
		{
			return true;
		}
		break;
	case media_feature_min_height:
		if(features.height >= val)
		{
			return true;
		}
		break;
	case media_feature_max_height:
		if(features.height <= val)
		{
			return true;
		}
		break;

	case media_feature_device_width:
		if(check_as_bool)
		{
			return (features.device_width != 0);
		} else if(features.device_width == val)
		{
			return true;
		}
		break;
	case media_feature_min_device_width:
		if(features.device_width >= val)
		{
			return true;
		}
		break;
	case media_feature_max_device_width:
		if(features.device_width <= val)
		{
			return true;
		}
		break;
	case media_feature_device_height:
		if(check_as_bool)
		{
			return (features.device_height != 0);
		} else if(features.device_height == val)
		{
			return true;
		}
		break;
	case media_feature_min_device_height:
		if(features.device_height <= val)
		{
			return true;
		}
		break;
	case media_feature_max_device_height:
		if(features.device_height <= val)
		{
			return true;
		}
		break;

	case media_feature_orientation:
		if(features.height >= features.width)
		{
			if(val == media_orientation_portrait)
			{
				return true;
			}
		} else
		{
			if(val == media_orientation_landscape)
			{
				return true;
			}
		}
		break;
	case media_feature_aspect_ratio:
		if(features.height && val2)
		{
			int ratio_this = round_d( (double) val / (double) val2 * 100 );
			int ratio_feat = round_d( (double) features.width / (double) features.height * 100.0 );
			if(ratio_this == ratio_feat)
			{
				return true;
			}
		}
		break;
	case media_feature_min_aspect_ratio:
		if(features.height && val2)
		{
			int ratio_this = round_d( (double) val / (double) val2 * 100 );
			int ratio_feat = round_d( (double) features.width / (double) features.height * 100.0 );
			if(ratio_feat >= ratio_this)
			{
				return true;
			}
		}
		break;
	case media_feature_max_aspect_ratio:
		if(features.height && val2)
		{
			int ratio_this = round_d( (double) val / (double) val2 * 100 );
			int ratio_feat = round_d( (double) features.width / (double) features.height * 100.0 );
			if(ratio_feat <= ratio_this)
			{
				return true;
			}
		}
		break;

	case media_feature_device_aspect_ratio:
		if(features.device_height && val2)
		{
			int ratio_this = round_d( (double) val / (double) val2 * 100 );
			int ratio_feat = round_d( (double) features.device_width / (double) features.device_height * 100.0 );
			if(ratio_feat == ratio_this)
			{
				return true;
			}
		}
		break;
	case media_feature_min_device_aspect_ratio:
		if(features.device_height && val2)
		{
			int ratio_this = round_d( (double) val / (double) val2 * 100 );
			int ratio_feat = round_d( (double) features.device_width / (double) features.device_height * 100.0 );
			if(ratio_feat >= ratio_this)
			{
				return true;
			}
		}
		break;
	case media_feature_max_device_aspect_ratio:
		if(features.device_height && val2)
		{
			int ratio_this = round_d( (double) val / (double) val2 * 100 );
			int ratio_feat = round_d( (double) features.device_width / (double) features.device_height * 100.0 );
			if(ratio_feat <= ratio_this)
			{
				return true;
			}
		}
		break;

	case media_feature_color:
		if(check_as_bool)
		{
			return (features.color != 0);
		} else if(features.color == val)
		{
			return true;
		}
		break;
	case media_feature_min_color:
		if(features.color >= val)
		{
			return true;
		}
		break;
	case media_feature_max_color:
		if(features.color <= val)
		{
			return true;
		}
		break;

	case media_feature_color_index:
		if(check_as_bool)
		{
			return (features.color_index != 0);
		} else if(features.color_index == val)
		{
			return true;
		}
		break;
	case media_feature_min_color_index:
		if(features.color_index >= val)
		{
			return true;
		}
		break;
	case media_feature_max_color_index:
		if(features.color_index <= val)
		{
			return true;
		}
		break;

	case media_feature_monochrome:
		if(check_as_bool)
		{
			return (features.monochrome != 0);
		} else if(features.monochrome == val)
		{
			return true;
		}
		break;
	case media_feature_min_monochrome:
		if(features.monochrome >= val)
		{
			return true;
		}
		break;
	case media_feature_max_monochrome:
		if(features.monochrome <= val)
		{
			return true;
		}
		break;

	case media_feature_resolution:
		if(features.resolution == val)
		{
			return true;
		}
		break;
	case media_feature_min_resolution:
		if(features.resolution >= val)
		{
			return true;
		}
		break;
	case media_feature_max_resolution:
		if(features.resolution <= val)
		{
			return true;
		}
		break;

	/* Environment features: constant capabilities of this browser. */
	case media_feature_prefers_color_scheme:
		if(check_as_bool)
		{
			return true;
		}
		return val == 1;	/* light */
	case media_feature_prefers_contrast:
	case media_feature_prefers_reduced_motion:
	case media_feature_forced_colors:
		if(check_as_bool)
		{
			/* boolean context is false when the value is the "none" /
			 * "no-preference" one, so "(prefers-reduced-motion)" must not
			 * apply reduce-styles on this no-preference browser */
			return false;
		}
		return val == 0;	/* no-preference / none */
	case media_feature_display_mode:
		if(check_as_bool)
		{
			return true;
		}
		return val == 0;	/* browser */
	case media_feature_pointer:
	case media_feature_any_pointer:
		if(check_as_bool)
		{
			return true;
		}
		return val == 2;	/* fine */
	case media_feature_hover:
	case media_feature_any_hover:
		if(check_as_bool)
		{
			return true;
		}
		return val == 1;	/* hover */
	default:
		return false;
	}

	return false;
}
