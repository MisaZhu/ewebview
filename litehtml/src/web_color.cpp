#include "html.h"
#include "web_color.h"
#include <string.h>
#include <stdint.h>
#include <math.h>

namespace litehtml {
void profile_color_parse(uint64_t start_ms);

namespace {

static int hex_digit_value(litehtml::tchar_t ch)
{
	if(ch >= '0' && ch <= '9') return ch - '0';
	if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

static int clamp255(int v)
{
	if(v < 0) return 0;
	if(v > 255) return 255;
	return v;
}

/* rgb() component: plain 0..255 integer or percentage of the range. */
static int color_component(const tstring& tok)
{
	if(!tok.empty() && tok[tok.length() - 1] == _t('%'))
	{
		return clamp255((int)(t_atoi(tok.c_str()) * 255 / 100));
	}
	return clamp255(t_atoi(tok.c_str()));
}

/* Alpha channel: fraction (0..1) or percentage. */
static int color_alpha(const tstring& tok)
{
	tstring t = tok;
	trim(t);
	if(t.empty())
	{
		return 255;
	}
	double v;
	if(t[t.length() - 1] == _t('%'))
	{
		v = t_strtod(t.c_str(), 0) * 2.55;
	} else
	{
		v = t_strtod(t.c_str(), 0) * 255.0;
	}
	return clamp255((int)(v + 0.5));
}

/* Hue angle to degrees: bare number/deg plus the other CSS units. */
static double hue_degrees(const tstring& tok)
{
	double v = t_strtod(tok.c_str(), 0);
	if(tok.find(_t("turn")) != tstring::npos)
	{
		v *= 360.0;
	} else if(tok.find(_t("rad")) != tstring::npos)
	{
		v *= 57.29577951308232;
	} else if(tok.find(_t("grad")) != tstring::npos)
	{
		v *= 0.9;
	}
	return v;
}

static double hue2rgb(double p, double q, double t)
{
	if(t < 0.0) t += 1.0;
	if(t > 1.0) t -= 1.0;
	if(t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
	if(t < 1.0 / 2.0) return q;
	if(t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
	return p;
}

static void hsl_to_rgb(double h, double s, double l, int& r, int& g, int& b)
{
	h = fmod(h, 360.0);
	if(h < 0.0) h += 360.0;
	if(s < 0.0) s = 0.0;
	if(s > 1.0) s = 1.0;
	if(l < 0.0) l = 0.0;
	if(l > 1.0) l = 1.0;
	h /= 360.0;
	if(s == 0.0)
	{
		r = g = b = (int)(l * 255.0 + 0.5);
		return;
	}
	double q = (l < 0.5) ? l * (1.0 + s) : l + s - l * s;
	double p = 2.0 * l - q;
	r = (int)(hue2rgb(p, q, h + 1.0 / 3.0) * 255.0 + 0.5);
	g = (int)(hue2rgb(p, q, h) * 255.0 + 0.5);
	b = (int)(hue2rgb(p, q, h - 1.0 / 3.0) * 255.0 + 0.5);
}

}
} // namespace litehtml

litehtml::def_color litehtml::g_def_colors[] = 
{
	{_t("transparent"),_t("rgba(0, 0, 0, 0)")},
	{_t("AliceBlue"),_t("#F0F8FF")},
	{_t("AntiqueWhite"),_t("#FAEBD7")},
	{_t("Aqua"),_t("#00FFFF")},
	{_t("Aquamarine"),_t("#7FFFD4")},
	{_t("Azure"),_t("#F0FFFF")},
	{_t("Beige"),_t("#F5F5DC")},
	{_t("Bisque"),_t("#FFE4C4")},
	{_t("Black"),_t("#000000")},
	{_t("BlanchedAlmond"),_t("#FFEBCD")},
	{_t("Blue"),_t("#0000FF")},
	{_t("BlueViolet"),_t("#8A2BE2")},
	{_t("Brown"),_t("#A52A2A")},
	{_t("BurlyWood"),_t("#DEB887")},
	{_t("CadetBlue"),_t("#5F9EA0")},
	{_t("Chartreuse"),_t("#7FFF00")},
	{_t("Chocolate"),_t("#D2691E")},
	{_t("Coral"),_t("#FF7F50")},
	{_t("CornflowerBlue"),_t("#6495ED")},
	{_t("Cornsilk"),_t("#FFF8DC")},
	{_t("Crimson"),_t("#DC143C")},
	{_t("Cyan"),_t("#00FFFF")},
	{_t("DarkBlue"),_t("#00008B")},
	{_t("DarkCyan"),_t("#008B8B")},
	{_t("DarkGoldenRod"),_t("#B8860B")},
	{_t("DarkGray"),_t("#A9A9A9")},
	{_t("DarkGrey"),_t("#A9A9A9")},
	{_t("DarkGreen"),_t("#006400")},
	{_t("DarkKhaki"),_t("#BDB76B")},
	{_t("DarkMagenta"),_t("#8B008B")},
	{_t("DarkOliveGreen"),_t("#556B2F")},
	{_t("Darkorange"),_t("#FF8C00")},
	{_t("DarkOrchid"),_t("#9932CC")},
	{_t("DarkRed"),_t("#8B0000")},
	{_t("DarkSalmon"),_t("#E9967A")},
	{_t("DarkSeaGreen"),_t("#8FBC8F")},
	{_t("DarkSlateBlue"),_t("#483D8B")},
	{_t("DarkSlateGray"),_t("#2F4F4F")},
	{_t("DarkSlateGrey"),_t("#2F4F4F")},
	{_t("DarkTurquoise"),_t("#00CED1")},
	{_t("DarkViolet"),_t("#9400D3")},
	{_t("DeepPink"),_t("#FF1493")},
	{_t("DeepSkyBlue"),_t("#00BFFF")},
	{_t("DimGray"),_t("#696969")},
	{_t("DimGrey"),_t("#696969")},
	{_t("DodgerBlue"),_t("#1E90FF")},
	{_t("FireBrick"),_t("#B22222")},
	{_t("FloralWhite"),_t("#FFFAF0")},
	{_t("ForestGreen"),_t("#228B22")},
	{_t("Fuchsia"),_t("#FF00FF")},
	{_t("Gainsboro"),_t("#DCDCDC")},
	{_t("GhostWhite"),_t("#F8F8FF")},
	{_t("Gold"),_t("#FFD700")},
	{_t("GoldenRod"),_t("#DAA520")},
	{_t("Gray"),_t("#808080")},
	{_t("Grey"),_t("#808080")},
	{_t("Green"),_t("#008000")},
	{_t("GreenYellow"),_t("#ADFF2F")},
	{_t("HoneyDew"),_t("#F0FFF0")},
	{_t("HotPink"),_t("#FF69B4")},
	{_t("Ivory"),_t("#FFFFF0")},
	{_t("Khaki"),_t("#F0E68C")},
	{_t("Lavender"),_t("#E6E6FA")},
	{_t("LavenderBlush"),_t("#FFF0F5")},
	{_t("LawnGreen"),_t("#7CFC00")},
	{_t("LemonChiffon"),_t("#FFFACD")},
	{_t("LightBlue"),_t("#ADD8E6")},
	{_t("LightCoral"),_t("#F08080")},
	{_t("LightCyan"),_t("#E0FFFF")},
	{_t("LightGoldenRodYellow"),_t("#FAFAD2")},
	{_t("LightGray"),_t("#D3D3D3")},
	{_t("LightGrey"),_t("#D3D3D3")},
	{_t("LightGreen"),_t("#90EE90")},
	{_t("LightPink"),_t("#FFB6C1")},
	{_t("LightSalmon"),_t("#FFA07A")},
	{_t("LightSeaGreen"),_t("#20B2AA")},
	{_t("LightSkyBlue"),_t("#87CEFA")},
	{_t("LightSlateGray"),_t("#778899")},
	{_t("LightSlateGrey"),_t("#778899")},
	{_t("LightSteelBlue"),_t("#B0C4DE")},
	{_t("LightYellow"),_t("#FFFFE0")},
	{_t("Lime"),_t("#00FF00")},
	{_t("LimeGreen"),_t("#32CD32")},
	{_t("Linen"),_t("#FAF0E6")},
	{_t("Magenta"),_t("#FF00FF")},
	{_t("Maroon"),_t("#800000")},
	{_t("MediumAquaMarine"),_t("#66CDAA")},
	{_t("MediumBlue"),_t("#0000CD")},
	{_t("MediumOrchid"),_t("#BA55D3")},
	{_t("MediumPurple"),_t("#9370D8")},
	{_t("MediumSeaGreen"),_t("#3CB371")},
	{_t("MediumSlateBlue"),_t("#7B68EE")},
	{_t("MediumSpringGreen"),_t("#00FA9A")},
	{_t("MediumTurquoise"),_t("#48D1CC")},
	{_t("MediumVioletRed"),_t("#C71585")},
	{_t("MidnightBlue"),_t("#191970")},
	{_t("MintCream"),_t("#F5FFFA")},
	{_t("MistyRose"),_t("#FFE4E1")},
	{_t("Moccasin"),_t("#FFE4B5")},
	{_t("NavajoWhite"),_t("#FFDEAD")},
	{_t("Navy"),_t("#000080")},
	{_t("OldLace"),_t("#FDF5E6")},
	{_t("Olive"),_t("#808000")},
	{_t("OliveDrab"),_t("#6B8E23")},
	{_t("Orange"),_t("#FFA500")},
	{_t("OrangeRed"),_t("#FF4500")},
	{_t("Orchid"),_t("#DA70D6")},
	{_t("PaleGoldenRod"),_t("#EEE8AA")},
	{_t("PaleGreen"),_t("#98FB98")},
	{_t("PaleTurquoise"),_t("#AFEEEE")},
	{_t("PaleVioletRed"),_t("#D87093")},
	{_t("PapayaWhip"),_t("#FFEFD5")},
	{_t("PeachPuff"),_t("#FFDAB9")},
	{_t("Peru"),_t("#CD853F")},
	{_t("Pink"),_t("#FFC0CB")},
	{_t("Plum"),_t("#DDA0DD")},
	{_t("PowderBlue"),_t("#B0E0E6")},
	{_t("Purple"),_t("#800080")},
	{_t("Red"),_t("#FF0000")},
	{_t("RosyBrown"),_t("#BC8F8F")},
	{_t("RoyalBlue"),_t("#4169E1")},
	{_t("SaddleBrown"),_t("#8B4513")},
	{_t("Salmon"),_t("#FA8072")},
	{_t("SandyBrown"),_t("#F4A460")},
	{_t("SeaGreen"),_t("#2E8B57")},
	{_t("SeaShell"),_t("#FFF5EE")},
	{_t("Sienna"),_t("#A0522D")},
	{_t("Silver"),_t("#C0C0C0")},
	{_t("SkyBlue"),_t("#87CEEB")},
	{_t("SlateBlue"),_t("#6A5ACD")},
	{_t("SlateGray"),_t("#708090")},
	{_t("SlateGrey"),_t("#708090")},
	{_t("Snow"),_t("#FFFAFA")},
	{_t("SpringGreen"),_t("#00FF7F")},
	{_t("SteelBlue"),_t("#4682B4")},
	{_t("Tan"),_t("#D2B48C")},
	{_t("Teal"),_t("#008080")},
	{_t("Thistle"),_t("#D8BFD8")},
	{_t("Tomato"),_t("#FF6347")},
	{_t("Turquoise"),_t("#40E0D0")},
	{_t("Violet"),_t("#EE82EE")},
	{_t("Wheat"),_t("#F5DEB3")},
	{_t("White"),_t("#FFFFFF")},
	{_t("WhiteSmoke"),_t("#F5F5F5")},
	{_t("Yellow"),_t("#FFFF00")},
	{_t("YellowGreen"),_t("#9ACD32")},
	{0,0}
};


litehtml::web_color litehtml::web_color::from_string(const tchar_t* str, litehtml::document_container* callback)
{
	uint64_t start_ms = sys_tic_ms(0);
	if(!str || !str[0])
	{
		litehtml::profile_color_parse(start_ms);
		return web_color(0, 0, 0, 0);
	}
	if(str[0] == _t('#'))
	{
		web_color clr;
		size_t hex_len = t_strlen(str + 1);
		if(hex_len == 3)
		{
			int r = hex_digit_value(str[1]);
			int g = hex_digit_value(str[2]);
			int b = hex_digit_value(str[3]);
			if(r >= 0 && g >= 0 && b >= 0)
			{
				clr.red = (byte)((r << 4) | r);
				clr.green = (byte)((g << 4) | g);
				clr.blue = (byte)((b << 4) | b);
				litehtml::profile_color_parse(start_ms);
				return clr;
			}
		}
		else if(hex_len == 4)
		{
			/* #RGBA */
			int r = hex_digit_value(str[1]);
			int g = hex_digit_value(str[2]);
			int b = hex_digit_value(str[3]);
			int a = hex_digit_value(str[4]);
			if(r >= 0 && g >= 0 && b >= 0 && a >= 0)
			{
				clr.red = (byte)((r << 4) | r);
				clr.green = (byte)((g << 4) | g);
				clr.blue = (byte)((b << 4) | b);
				clr.alpha = (byte)((a << 4) | a);
				litehtml::profile_color_parse(start_ms);
				return clr;
			}
		}
		else if(hex_len == 6)
		{
			int r1 = hex_digit_value(str[1]);
			int r2 = hex_digit_value(str[2]);
			int g1 = hex_digit_value(str[3]);
			int g2 = hex_digit_value(str[4]);
			int b1 = hex_digit_value(str[5]);
			int b2 = hex_digit_value(str[6]);
			if(r1 >= 0 && r2 >= 0 && g1 >= 0 && g2 >= 0 && b1 >= 0 && b2 >= 0)
			{
				clr.red = (byte)((r1 << 4) | r2);
				clr.green = (byte)((g1 << 4) | g2);
				clr.blue = (byte)((b1 << 4) | b2);
				litehtml::profile_color_parse(start_ms);
				return clr;
			}
		}
		else if(hex_len == 8)
		{
			/* #RRGGBBAA, the CSS Color 4 order used all over modern design
			 * systems; parsing it as opaque black used to turn every such
			 * background into a black box. */
			int r1 = hex_digit_value(str[1]);
			int r2 = hex_digit_value(str[2]);
			int g1 = hex_digit_value(str[3]);
			int g2 = hex_digit_value(str[4]);
			int b1 = hex_digit_value(str[5]);
			int b2 = hex_digit_value(str[6]);
			int a1 = hex_digit_value(str[7]);
			int a2 = hex_digit_value(str[8]);
			if(r1 >= 0 && r2 >= 0 && g1 >= 0 && g2 >= 0 && b1 >= 0 && b2 >= 0 && a1 >= 0 && a2 >= 0)
			{
				clr.red = (byte)((r1 << 4) | r2);
				clr.green = (byte)((g1 << 4) | g2);
				clr.blue = (byte)((b1 << 4) | b2);
				clr.alpha = (byte)((a1 << 4) | a2);
				litehtml::profile_color_parse(start_ms);
				return clr;
			}
		}
		litehtml::profile_color_parse(start_ms);
		return web_color(0, 0, 0, 0);
	} else if(!t_strncmp(str, _t("rgb"), 3))
	{
		tstring s = str;

		tstring::size_type pos = s.find_first_of(_t("("));
		if(pos != tstring::npos)
		{
			s.erase(s.begin(), s.begin() + pos + 1);
		}
		pos = s.find_last_of(_t(")"));
		if(pos != tstring::npos)
		{
			s.erase(s.begin() + pos, s.end());
		}

		/* Modern space-separated syntax carries the alpha after a slash:
		 * "rgb(31 35 40 / 50%)". Split it off before tokenising. */
		tstring alpha_tok;
		pos = s.find(_t('/'));
		if(pos != tstring::npos)
		{
			alpha_tok = s.substr(pos + 1);
			s.erase(pos);
		}

		std::vector<tstring> tokens;
		split_string(s, tokens, _t(", \t"));

		web_color clr;

		if(tokens.size() >= 1)	clr.red		= (byte) color_component(tokens[0]);
		if(tokens.size() >= 2)	clr.green	= (byte) color_component(tokens[1]);
		if(tokens.size() >= 3)	clr.blue	= (byte) color_component(tokens[2]);
		if(!alpha_tok.empty())	clr.alpha	= (byte) color_alpha(alpha_tok);
		else if(tokens.size() >= 4)	clr.alpha	= (byte) color_alpha(tokens[3]);

		litehtml::profile_color_parse(start_ms);
		return clr;
	} else if(!t_strncmp(str, _t("hsl"), 3))
	{
		tstring s = str;

		tstring::size_type pos = s.find_first_of(_t("("));
		if(pos != tstring::npos)
		{
			s.erase(s.begin(), s.begin() + pos + 1);
		}
		pos = s.find_last_of(_t(")"));
		if(pos != tstring::npos)
		{
			s.erase(s.begin() + pos, s.end());
		}

		tstring alpha_tok;
		pos = s.find(_t('/'));
		if(pos != tstring::npos)
		{
			alpha_tok = s.substr(pos + 1);
			s.erase(pos);
		}

		std::vector<tstring> tokens;
		split_string(s, tokens, _t(", \t"));
		if(tokens.size() >= 3)
		{
			int r = 0, g = 0, b = 0;
			hsl_to_rgb(hue_degrees(tokens[0]), t_strtod(tokens[1].c_str(), 0) / 100.0,
				t_strtod(tokens[2].c_str(), 0) / 100.0, r, g, b);
			web_color clr((byte) r, (byte) g, (byte) b);
			if(!alpha_tok.empty())	clr.alpha = (byte) color_alpha(alpha_tok);
			else if(tokens.size() >= 4)	clr.alpha = (byte) color_alpha(tokens[3]);
			litehtml::profile_color_parse(start_ms);
			return clr;
		}
		litehtml::profile_color_parse(start_ms);
		return web_color(0, 0, 0, 0);
	} else
	{
		tstring rgb = resolve_name(str, callback);
		if(!rgb.empty())
		{
			litehtml::web_color clr = from_string(rgb.c_str(), callback);
			litehtml::profile_color_parse(start_ms);
			return clr;
		}
	}
	/* Unrecognised syntax (color-mix(), light-dark(), leftover var()...):
	 * behave like an invalid colour, i.e. fully transparent, instead of
	 * painting an opaque black rectangle over the page. */
	litehtml::profile_color_parse(start_ms);
	return web_color(0, 0, 0, 0);
}

litehtml::tstring litehtml::web_color::resolve_name(const tchar_t* name, litehtml::document_container* callback)
{
	for(int i=0; g_def_colors[i].name; i++)
	{
		if(!t_strcasecmp(name, g_def_colors[i].name))
		{
            return std::move(litehtml::tstring(g_def_colors[i].rgb));
		}
	}
    if (callback)
    {
        litehtml::tstring clr = callback->resolve_color(name);
        return std::move(clr);
    }
    return std::move(litehtml::tstring());
}

bool litehtml::web_color::is_color(const tchar_t* str)
{
	if(!t_strncasecmp(str, _t("rgb"), 3) || str[0] == _t('#'))
	{
		return true;
	}
    if (!t_isdigit(str[0]) && str[0] != _t('.'))
	{
		return true;
	}
	return false;
}
