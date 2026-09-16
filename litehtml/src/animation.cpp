#include "html.h"
#include "animation.h"
#include <math.h>
#include <stdlib.h>

/* Implementation of the Phase 2 animation module. See animation.h for the
 * scope statement (what animates, what parses but snaps, what is deferred). */

namespace litehtml
{

/* ---- timing function evaluation ----------------------------------------- */

/* Preset cubic-bezier control points from the CSS Easing Functions spec. */
static const float kPresetBezier[][4] = {
	/* linear      */ {0.0f, 0.0f, 1.0f, 1.0f},
	/* ease        */ {0.25f, 0.1f, 0.25f, 1.0f},
	/* ease-in     */ {0.42f, 0.0f, 1.0f, 1.0f},
	/* ease-out    */ {0.0f, 0.0f, 0.58f, 1.0f},
	/* ease-in-out */ {0.42f, 0.0f, 0.58f, 1.0f},
};

/* Solve the cubic bezier x(t) = X for t using bisection. Newton-Raphson is
 * faster but can diverge on stiff curves; bisection with 20 iterations gives
 * ~1e-6 precision on the [0,1] domain which is far more than enough for a
 * 60Hz tick (a 1e-6 error at 200ms duration is 0.2 microseconds). */
static float bezier_solve_t(float x1, float x2, float X)
{
	if(X <= 0.0f) return 0.0f;
	if(X >= 1.0f) return 1.0f;
	float lo = 0.0f, hi = 1.0f;
	for(int i = 0; i < 20; i++)
	{
		float t = 0.5f * (lo + hi);
		/* x(t) = 3(1-t)^2 t x1 + 3(1-t) t^2 x2 + t^3 */
		float u = 1.0f - t;
		float xt = 3.0f * u * u * t * x1 + 3.0f * u * t * t * x2 + t * t * t;
		if(xt < X) lo = t; else hi = t;
	}
	return 0.5f * (lo + hi);
}

static float bezier_eval_y(float y1, float y2, float t)
{
	float u = 1.0f - t;
	return 3.0f * u * u * t * y1 + 3.0f * u * t * t * y2 + t * t * t;
}

float anim_timing_fn::eval(float t) const
{
	if(t <= 0.0f) return 0.0f;
	if(t >= 1.0f) return 1.0f;
	if(is_steps)
	{
		int n = steps_count > 0 ? steps_count : 1;
		/* steps(n, end): the value jumps at t = 1/n, 2/n, ..., 1.
		 * steps(n, start): the value jumps at t = 0, 1/n, ..., (n-1)/n. */
		int idx;
		if(steps_jump_start) idx = (int) floorf(t * (float) n + 1e-6f);
		else                 idx = (int) floorf(t * (float) n);
		if(idx < 0) idx = 0;
		if(idx > n) idx = n;
		return (float) idx / (float) n;
	}
	if(preset == anim_timing_linear && !is_cubic_bezier) return t;
	if(preset == anim_timing_step_start) return 1.0f;
	if(preset == anim_timing_step_end) return 0.0f;

	/* Cubic-bezier path: use parsed control points when present, otherwise
	 * the preset's. Phase 3 will widen this to properly support
	 * cubic-bezier(x1,y1,x2,y2) end-to-end; for now the parser accepts the
	 * syntax and stores the control points, and we evaluate them here so
	 * `cubic-bezier(0.4, 0, 0.2, 1)` already animates correctly. */
	const float* cp;
	float custom[4];
	if(is_cubic_bezier)
	{
		custom[0] = cb[0]; custom[1] = cb[1]; custom[2] = cb[2]; custom[3] = cb[3];
		cp = custom;
	}
	else
	{
		int idx = (int) preset;
		if(idx < 0 || idx > 4) idx = 0;
		cp = kPresetBezier[idx];
	}
	float tt = bezier_solve_t(cp[0], cp[2], t);
	return bezier_eval_y(cp[1], cp[3], tt);
}

/* ---- timing function parsing -------------------------------------------- */

static bool starts_with_ci(const tstring& s, const char* prefix)
{
	size_t n = strlen(prefix);
	if(s.length() < n) return false;
	for(size_t i = 0; i < n; i++)
	{
		char a = (char) s[i];
		char b = prefix[i];
		if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if(a != b) return false;
	}
	return true;
}

bool anim_timing_fn::parse(const tstring& s_in, anim_timing_fn& out)
{
	tstring s = s_in;
	trim(s);
	lcase(s);
	if(s.empty()) return false;

	out = anim_timing_fn();	/* reset to defaults */

	if(s == _t("linear"))       { out.preset = anim_timing_linear;      return true; }
	if(s == _t("ease"))         { out.preset = anim_timing_ease;        return true; }
	if(s == _t("ease-in"))      { out.preset = anim_timing_ease_in;     return true; }
	if(s == _t("ease-out"))     { out.preset = anim_timing_ease_out;    return true; }
	if(s == _t("ease-in-out"))  { out.preset = anim_timing_ease_in_out; return true; }
	if(s == _t("step-start"))   { out.preset = anim_timing_step_start;  return true; }
	if(s == _t("step-end"))     { out.preset = anim_timing_step_end;    return true; }

	if(starts_with_ci(s, "cubic-bezier("))
	{
		tstring::size_type open = s.find(_t('('));
		tstring::size_type close = find_close_bracket(s, open);
		if(open == tstring::npos || close == tstring::npos || close <= open + 1) return false;
		tstring inner = s.substr(open + 1, close - open - 1);
		string_vector parts;
		split_string(inner, parts, _t(","), _t(""), _t(""));
		if(parts.size() != 4) return false;
		float v[4];
		for(int i = 0; i < 4; i++)
		{
			tstring p = parts[i];
			trim(p);
			v[i] = (float) t_strtod(p.c_str(), nullptr);
		}
		out.is_cubic_bezier = true;
		out.cb[0] = v[0]; out.cb[1] = v[1]; out.cb[2] = v[2]; out.cb[3] = v[3];
		return true;
	}

	if(starts_with_ci(s, "steps("))
	{
		tstring::size_type open = s.find(_t('('));
		tstring::size_type close = find_close_bracket(s, open);
		if(open == tstring::npos || close == tstring::npos || close <= open + 1) return false;
		tstring inner = s.substr(open + 1, close - open - 1);
		string_vector parts;
		split_string(inner, parts, _t(","), _t(""), _t(""));
		if(parts.empty()) return false;
		tstring n_str = parts[0];
		trim(n_str);
		int n = (int) t_strtol(n_str.c_str(), nullptr, 10);
		if(n < 1) n = 1;
		out.is_steps = true;
		out.steps_count = n;
		out.steps_jump_start = false;
		if(parts.size() >= 2)
		{
			tstring pos = parts[1];
			trim(pos);
			lcase(pos);
			if(pos == _t("start") || pos == _t("jump-start")) out.steps_jump_start = true;
		}
		return true;
	}

	/* Unknown token: leave as default (ease) so the caller still gets a
	 * usable timing function instead of a hard failure. */
	return false;
}

/* ---- time parsing -------------------------------------------------------- */

int parse_time_ms(const tstring& s_in)
{
	tstring s = s_in;
	trim(s);
	lcase(s);
	if(s.empty()) return 0;
	/* The value may be "200ms", "1.5s", "0", or ".5s". strtod stops at the
	 * unit suffix, which is exactly what we want. */
	double v = t_strtod(s.c_str(), nullptr);
	if(s.find(_t("ms")) != tstring::npos) return (int) (v + (v >= 0 ? 0.5 : -0.5));
	if(s.find(_t('s')) != tstring::npos)  return (int) (v * 1000.0 + (v >= 0 ? 0.5 : -0.5));
	/* Bare number is treated as seconds per CSS spec (unitless 0 is fine). */
	return (int) (v * 1000.0 + (v >= 0 ? 0.5 : -0.5));
}

/* ---- comma-list splitting that respects parens and quotes ---------------- */

static void split_top_level_commas(const tstring& in, std::vector<tstring>& out)
{
	tstring cur;
	int depth = 0;
	bool in_str = false;
	tchar_t str_ch = 0;
	for(size_t i = 0; i < in.length(); i++)
	{
		tchar_t c = in[i];
		if(in_str)
		{
			cur += c;
			if(c == _t('\\') && i + 1 < in.length()) { cur += in[++i]; continue; }
			if(c == str_ch) in_str = false;
			continue;
		}
		if(c == _t('"') || c == _t('\'')) { in_str = true; str_ch = c; cur += c; continue; }
		if(c == _t('(') || c == _t('[') || c == _t('{')) depth++;
		else if(c == _t(')') || c == _t(']') || c == _t('}')) depth--;
		if(c == _t(',') && depth == 0)
		{
			out.push_back(cur);
			cur.clear();
			continue;
		}
		cur += c;
	}
	out.push_back(cur);
}

/* Split a space-separated token list, keeping parenthesised groups intact
 * (so "cubic-bezier(0.4, 0, 0.2, 1)" stays one token). */
static void split_top_level_spaces(const tstring& in, string_vector& out)
{
	tstring cur;
	int depth = 0;
	for(size_t i = 0; i < in.length(); i++)
	{
		tchar_t c = in[i];
		if(c == _t('(') || c == _t('[') || c == _t('{')) depth++;
		else if(c == _t(')') || c == _t(']') || c == _t('}')) depth--;
		if((c == _t(' ') || c == _t('\t') || c == _t('\n') || c == _t('\r')) && depth == 0)
		{
			if(!cur.empty()) { out.push_back(cur); cur.clear(); }
			continue;
		}
		cur += c;
	}
	if(!cur.empty()) out.push_back(cur);
}

/* Look up a property by name in the props_map, returning "" when absent. */
static tstring prop_get(const props_map& m, const char* name)
{
	props_map::const_iterator it = m.find(tstring(name));
	if(it == m.end()) return tstring();
	return it->second.m_value;
}

/* ---- transition parsing -------------------------------------------------- */

/* Classify a token in a transition shorthand item. Order matters:
 * time tokens contain 's' or 'ms', so check them before the generic
 * identifier path. */
enum trans_tok_kind { TT_UNKNOWN, TT_TIME, TT_TIMING, TT_PROPERTY };

static trans_tok_kind classify_transition_token(const tstring& tok, anim_declaration& d)
{
	tstring t = tok;
	lcase(t);
	/* timing functions first (they can contain parens and commas which
	 * would confuse a naive property-name check) */
	if(t == _t("linear") || t == _t("ease") || t == _t("ease-in") ||
	   t == _t("ease-out") || t == _t("ease-in-out") ||
	   t == _t("step-start") || t == _t("step-end") ||
	   starts_with_ci(t, "cubic-bezier(") || starts_with_ci(t, "steps("))
	{
		anim_timing_fn fn;
		if(anim_timing_fn::parse(t, fn)) { d.timing = fn; return TT_TIMING; }
	}
	/* time: has a digit and ends with s or ms */
	bool has_digit = false;
	for(size_t i = 0; i < t.length(); i++) if(t[i] >= _t('0') && t[i] <= _t('9')) { has_digit = true; break; }
	if(has_digit && (t.find(_t("ms")) != tstring::npos ||
					 t.find_last_of(_t('s')) == t.length() - 1))
	{
		return TT_TIME;
	}
	/* property name (identifier or "all") */
	if(!t.empty()) return TT_PROPERTY;
	return TT_UNKNOWN;
}

void parse_transition_declarations(const props_map& props, std::vector<anim_declaration>& out)
{
	out.clear();

	/* Longhand-first assembly: gather the four longhand lists (property,
	 * duration, timing-function, delay), each comma-separated. If none of
	 * the longhands are present, fall back to the shorthand. When both are
	 * present, longhands override the corresponding shorthand components
	 * (matching browser behaviour after the shorthand has been expanded). */
	tstring sh = prop_get(props, "transition");
	tstring lp = prop_get(props, "transition-property");
	tstring ld = prop_get(props, "transition-duration");
	tstring lt = prop_get(props, "transition-timing-function");
	tstring ly = prop_get(props, "transition-delay");

	std::vector<tstring> items;
	if(!sh.empty()) split_top_level_commas(sh, items);

	/* Parse the shorthand items into declarations first. */
	for(size_t i = 0; i < items.size(); i++)
	{
		tstring item = items[i];
		trim(item);
		if(item.empty()) continue;
		anim_declaration d;
		d.is_animation = false;
		string_vector toks;
		split_top_level_spaces(item, toks);
		int time_slot = 0;	/* first time = duration, second = delay */
		for(size_t k = 0; k < toks.size(); k++)
		{
			trans_tok_kind kind = classify_transition_token(toks[k], d);
			if(kind == TT_TIME)
			{
				int ms = parse_time_ms(toks[k]);
				if(time_slot == 0) { d.duration_ms = ms; time_slot = 1; }
				else               { d.delay_ms = ms; }
			} else if(kind == TT_PROPERTY)
			{
				if(d.property.empty())
				{
					d.property = toks[k];
					lcase(d.property);
				}
			}
			/* TT_TIMING already consumed by classify_transition_token */
		}
		if(d.property.empty()) d.property = _t("all");
		out.push_back(d);
	}

	/* Apply longhands on top. */
	auto split_list = [](const tstring& s, std::vector<tstring>& dst) {
		if(s.empty()) return;
		split_top_level_commas(s, dst);
		for(size_t i = 0; i < dst.size(); i++) trim(dst[i]);
	};
	std::vector<tstring> l_lp, l_ld, l_lt, l_ly;
	split_list(lp, l_lp);
	split_list(ld, l_ld);
	split_list(lt, l_lt);
	split_list(ly, l_ly);

	size_t n = l_lp.size();
	if(l_ld.size() > n) n = l_ld.size();
	if(l_lt.size() > n) n = l_lt.size();
	if(l_ly.size() > n) n = l_ly.size();
	if(n == 0) return;

	if(out.size() < n) out.resize(n);
	for(size_t i = 0; i < n; i++)
	{
		anim_declaration& d = out[i];
		d.is_animation = false;
		if(i < l_lp.size() && !l_lp[i].empty()) { d.property = l_lp[i]; lcase(d.property); }
		if(i < l_ld.size() && !l_ld[i].empty()) d.duration_ms = parse_time_ms(l_ld[i]);
		if(i < l_lt.size() && !l_lt[i].empty())
		{
			anim_timing_fn fn;
			if(anim_timing_fn::parse(l_lt[i], fn)) d.timing = fn;
		}
		if(i < l_ly.size() && !l_ly[i].empty()) d.delay_ms = parse_time_ms(l_ly[i]);
		if(d.property.empty()) d.property = _t("all");
	}

	/* Drop entries with zero duration AND no delay — nothing to animate. */
	std::vector<anim_declaration> kept;
	kept.reserve(out.size());
	for(size_t i = 0; i < out.size(); i++)
	{
		if(out[i].duration_ms > 0 || out[i].delay_ms > 0) kept.push_back(out[i]);
	}
	out.swap(kept);
}

/* ---- animation parsing --------------------------------------------------- */

/* Classify a token in an animation shorthand item. Time / iteration-count /
 * direction / fill-mode / play-state / timing-function all have distinct
 * keyword sets; the remaining bare identifier is the @keyframes name. */
void parse_animation_declarations(const props_map& props, std::vector<anim_declaration>& out)
{
	out.clear();

	tstring sh = prop_get(props, "animation");
	tstring ln = prop_get(props, "animation-name");
	tstring ld = prop_get(props, "animation-duration");
	tstring lt = prop_get(props, "animation-timing-function");
	tstring ly = prop_get(props, "animation-delay");
	tstring li = prop_get(props, "animation-iteration-count");
	tstring lr = prop_get(props, "animation-direction");
	tstring lf = prop_get(props, "animation-fill-mode");
	tstring lp = prop_get(props, "animation-play-state");

	std::vector<tstring> items;
	if(!sh.empty()) split_top_level_commas(sh, items);

	for(size_t i = 0; i < items.size(); i++)
	{
		tstring item = items[i];
		trim(item);
		if(item.empty()) continue;
		anim_declaration d;
		d.is_animation = true;
		string_vector toks;
		split_top_level_spaces(item, toks);
		int time_slot = 0;
		for(size_t k = 0; k < toks.size(); k++)
		{
			tstring t = toks[k];
			tstring tl = t;
			lcase(tl);

			/* timing function */
			anim_timing_fn fn;
			if(anim_timing_fn::parse(tl, fn)) { d.timing = fn; continue; }

			/* time */
			bool has_digit = false;
			for(size_t q = 0; q < tl.length(); q++) if(tl[q] >= _t('0') && tl[q] <= _t('9')) { has_digit = true; break; }
			if(has_digit && (tl.find(_t("ms")) != tstring::npos || tl.find_last_of(_t('s')) == tl.length() - 1))
			{
				int ms = parse_time_ms(tl);
				if(time_slot == 0) { d.duration_ms = ms; time_slot = 1; }
				else               { d.delay_ms = ms; }
				continue;
			}

			/* iteration count: "infinite" or a bare number */
			if(tl == _t("infinite")) { d.iteration_count = -1.0f; continue; }
			if(has_digit && tl.find_first_of(_t("sSmM")) == tstring::npos)
			{
				/* Numeric with no time unit → iteration count (only if we
				 * haven't already claimed a slot for it). */
				if(d.iteration_count == 1.0f)
				{
					d.iteration_count = (float) t_strtod(tl.c_str(), nullptr);
					continue;
				}
			}

			/* direction */
			if(tl == _t("normal"))            { d.direction = anim_dir_normal;            continue; }
			if(tl == _t("reverse"))           { d.direction = anim_dir_reverse;           continue; }
			if(tl == _t("alternate"))         { d.direction = anim_dir_alternate;         continue; }
			if(tl == _t("alternate-reverse")) { d.direction = anim_dir_alternate_reverse; continue; }

			/* fill mode */
			if(tl == _t("none"))      { d.fill_mode = anim_fill_none;     continue; }
			if(tl == _t("forwards"))  { d.fill_mode = anim_fill_forwards; continue; }
			if(tl == _t("backwards")) { d.fill_mode = anim_fill_backwards; continue; }
			if(tl == _t("both"))      { d.fill_mode = anim_fill_both;     continue; }

			/* play state */
			if(tl == _t("running")) { d.play_state = anim_play_running; continue; }
			if(tl == _t("paused"))  { d.play_state = anim_play_paused;  continue; }

			/* Otherwise: the @keyframes name. Only claim the first bare
			 * identifier so shorthand like `animation: fade 1s ease` puts
			 * "fade" into name and ignores the rest. */
			if(d.name.empty()) { d.name = t; continue; }
		}
		out.push_back(d);
	}

	/* Longhands override. */
	auto split_list = [](const tstring& s, std::vector<tstring>& dst) {
		if(s.empty()) return;
		split_top_level_commas(s, dst);
		for(size_t i = 0; i < dst.size(); i++) trim(dst[i]);
	};
	std::vector<tstring> l_ln, l_ld, l_lt, l_ly, l_li, l_lr, l_lf, l_lp;
	split_list(ln, l_ln);
	split_list(ld, l_ld);
	split_list(lt, l_lt);
	split_list(ly, l_ly);
	split_list(li, l_li);
	split_list(lr, l_lr);
	split_list(lf, l_lf);
	split_list(lp, l_lp);

	size_t n = l_ln.size();
	if(l_ld.size() > n) n = l_ld.size();
	if(l_lt.size() > n) n = l_lt.size();
	if(l_ly.size() > n) n = l_ly.size();
	if(l_li.size() > n) n = l_li.size();
	if(l_lr.size() > n) n = l_lr.size();
	if(l_lf.size() > n) n = l_lf.size();
	if(l_lp.size() > n) n = l_lp.size();
	if(n == 0) return;

	if(out.size() < n) out.resize(n);
	for(size_t i = 0; i < n; i++)
	{
		anim_declaration& d = out[i];
		d.is_animation = true;
		if(i < l_ln.size() && !l_ln[i].empty()) d.name = l_ln[i];
		if(i < l_ld.size() && !l_ld[i].empty()) d.duration_ms = parse_time_ms(l_ld[i]);
		if(i < l_lt.size() && !l_lt[i].empty())
		{
			anim_timing_fn fn;
			if(anim_timing_fn::parse(l_lt[i], fn)) d.timing = fn;
		}
		if(i < l_ly.size() && !l_ly[i].empty()) d.delay_ms = parse_time_ms(l_ly[i]);
		if(i < l_li.size() && !l_li[i].empty())
		{
			tstring s = l_li[i];
			lcase(s);
			if(s == _t("infinite")) d.iteration_count = -1.0f;
			else d.iteration_count = (float) t_strtod(s.c_str(), nullptr);
		}
		if(i < l_lr.size() && !l_lr[i].empty())
		{
			tstring s = l_lr[i]; lcase(s);
			if(s == _t("normal"))            d.direction = anim_dir_normal;
			else if(s == _t("reverse"))      d.direction = anim_dir_reverse;
			else if(s == _t("alternate"))    d.direction = anim_dir_alternate;
			else if(s == _t("alternate-reverse")) d.direction = anim_dir_alternate_reverse;
		}
		if(i < l_lf.size() && !l_lf[i].empty())
		{
			tstring s = l_lf[i]; lcase(s);
			if(s == _t("none"))           d.fill_mode = anim_fill_none;
			else if(s == _t("forwards"))  d.fill_mode = anim_fill_forwards;
			else if(s == _t("backwards")) d.fill_mode = anim_fill_backwards;
			else if(s == _t("both"))      d.fill_mode = anim_fill_both;
		}
		if(i < l_lp.size() && !l_lp[i].empty())
		{
			tstring s = l_lp[i]; lcase(s);
			if(s == _t("running")) d.play_state = anim_play_running;
			else if(s == _t("paused")) d.play_state = anim_play_paused;
		}
	}

	/* Drop entries with no name or zero duration — nothing to animate. */
	std::vector<anim_declaration> kept;
	kept.reserve(out.size());
	for(size_t i = 0; i < out.size(); i++)
	{
		if(out[i].name.empty() || out[i].name == _t("none")) continue;
		if(out[i].duration_ms <= 0 && out[i].iteration_count != -1.0f) continue;
		kept.push_back(out[i]);
	}
	out.swap(kept);
}

/* ---- @keyframes parsing -------------------------------------------------- */

/* Parse a percentage or "from"/"to" selector into a normalised offset. */
static bool parse_keyframe_offset(const tstring& tok, float& out_off)
{
	tstring t = tok;
	trim(t);
	lcase(t);
	if(t.empty()) return false;
	if(t == _t("from")) { out_off = 0.0f; return true; }
	if(t == _t("to"))   { out_off = 1.0f; return true; }
	if(t[t.length() - 1] == _t('%'))
	{
		tstring num = t.substr(0, t.length() - 1);
		trim(num);
		out_off = (float) t_strtod(num.c_str(), nullptr) / 100.0f;
		if(out_off < 0.0f) out_off = 0.0f;
		if(out_off > 1.0f) out_off = 1.0f;
		return true;
	}
	return false;
}

bool parse_keyframes_body(const tstring& body, keyframes_rule& out)
{
	out.stops.clear();
	/* Body is a sequence of `<selector-list> { <decls> }` blocks. Scan by
	 * matching braces so nested at-rules inside keyframes (rare but legal)
	 * don't confuse the parser. */
	size_t i = 0;
	while(i < body.length())
	{
		size_t brace = body.find(_t('{'), i);
		if(brace == tstring::npos) break;
		tstring sel_text = body.substr(i, brace - i);
		size_t close = find_close_bracket(body, brace, _t('{'), _t('}'));
		if(close == tstring::npos) break;
		tstring decl_text = body.substr(brace + 1, close - brace - 1);

		/* Split the selector list on commas; each entry becomes its own stop
		 * with the same declarations (matches CSS spec: "from, 25% { ... }"
		 * is equivalent to two stops with identical props). */
		std::vector<tstring> sels;
		split_top_level_commas(sel_text, sels);

		/* Parse the declarations into a props_map. Reuse style::add which
		 * already handles "prop: value !important;" splitting correctly. */
		props_map parsed;
		{
			style tmp;
			tmp.add(decl_text.c_str(), nullptr);
			const props_map& m = tmp.properties();
			for(props_map::const_iterator it = m.begin(); it != m.end(); ++it)
			{
				parsed[it->first] = it->second;
			}
		}

		for(size_t k = 0; k < sels.size(); k++)
		{
			float off = 0.0f;
			if(!parse_keyframe_offset(sels[k], off)) continue;
			keyframe_stop st;
			st.offset = off;
			st.props = parsed;
			out.stops.push_back(st);
		}

		i = close + 1;
	}

	/* Sort stops by offset for binary-search sampling. */
	for(size_t a = 1; a < out.stops.size(); a++)
	{
		keyframe_stop key = out.stops[a];
		size_t b = a;
		while(b > 0 && out.stops[b - 1].offset > key.offset)
		{
			out.stops[b] = out.stops[b - 1];
			b--;
		}
		out.stops[b] = key;
	}
	return !out.stops.empty();
}

/* ---- property interpolation --------------------------------------------- */

/* Phase 3.1: the layout-affecting length properties. Interpolating any of
 * these changes geometry, so the engine must relayout after the tick writes
 * the value. border-radius is paint-only and is excluded from relayout. */
static bool is_length_prop(const tstring& n)
{
	return n == _t("width") || n == _t("height") ||
		n == _t("min-width") || n == _t("max-width") ||
		n == _t("min-height") || n == _t("max-height") ||
		n == _t("top") || n == _t("left") || n == _t("right") || n == _t("bottom") ||
		n == _t("margin-top") || n == _t("margin-right") ||
		n == _t("margin-bottom") || n == _t("margin-left") ||
		n == _t("padding-top") || n == _t("padding-right") ||
		n == _t("padding-bottom") || n == _t("padding-left") ||
		n == _t("border-radius") || n == _t("border-width") ||
		n == _t("font-size") || n == _t("letter-spacing") || n == _t("line-height");
}

bool property_needs_relayout(const tstring& name)
{
	tstring n = name;
	lcase(n);
	if(n == _t("border-radius")) return false;  /* paint-only */
	return is_length_prop(n);
}

bool property_is_interpolable(const tstring& name)
{
	/* Phase 2 supports opacity; Phase 3 adds transform (precise list lerp) and
	 * the layout-affecting length properties (Phase 3.1). Every entry must be
	 * handled by interpolate_property() and by the writeback path in
	 * document::tick_animations(). */
	tstring n = name;
	lcase(n);
	return n == _t("opacity") || n == _t("transform") || is_length_prop(n);
}

/* Parse a raw opacity value ("0.5", "1", ".25") into a float. */
static bool parse_opacity(const tstring& s_in, float& out_v)
{
	tstring s = s_in;
	trim(s);
	if(s.empty()) return false;
	/* Reject non-numeric keywords ("initial", "inherit") so callers can
	 * decide to snap rather than lerp toward zero. */
	for(size_t i = 0; i < s.length(); i++)
	{
		tchar_t c = s[i];
		if(!(c == _t('.') || c == _t('+') || c == _t('-') || (c >= _t('0') && c <= _t('9'))))
		{
			return false;
		}
	}
	out_v = (float) t_strtod(s.c_str(), nullptr);
	if(out_v < 0.0f) out_v = 0.0f;
	if(out_v > 1.0f) out_v = 1.0f;
	return true;
}

/* ---- Transform string to matrix (Phase 3.2) ------------------------------ */

/* Parse a CSS transform string and compose it into a 3x3 affine matrix
 * stored as m[6] = {a,b,c,d,e,f} where the matrix is:
 *   | a c e |
 *   | b d f |
 *   | 0 0 1 |
 * Percentage translate values are resolved against a nominal 100x100 box;
 * the real box is applied at paint time by html_tag::compute_transform_matrix,
 * so the interpolation only needs to be monotonic in t.
 * Returns false when the string cannot be parsed (unknown function, etc.). */
static bool transform_string_to_matrix(const tstring& s_in, float m[6])
{
	/* Identity */
	m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 1; m[4] = 0; m[5] = 0;

	tstring s = s_in;
	trim(s);
	if(s.empty() || s == _t("none")) return true;  /* identity */

	const tchar_t* p = s.c_str();
	float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

	while(p && *p)
	{
		while(*p == ' ' || *p == '\t') p++;
		if(!*p) break;

		int type = -1;
		if(!t_strncmp(p, _t("rotate("), 7))          { type = 0; p += 7; }
		else if(!t_strncmp(p, _t("translateX("), 11)) { type = 2; p += 11; }
		else if(!t_strncmp(p, _t("translateY("), 11)) { type = 3; p += 11; }
		else if(!t_strncmp(p, _t("translate3d("), 12)){ type = 1; p += 12; }
		else if(!t_strncmp(p, _t("translate("), 10))  { type = 1; p += 10; }
		else if(!t_strncmp(p, _t("scaleX("), 7))      { type = 5; p += 7; }
		else if(!t_strncmp(p, _t("scaleY("), 7))      { type = 6; p += 7; }
		else if(!t_strncmp(p, _t("scale("), 6))       { type = 4; p += 6; }
		else if(!t_strncmp(p, _t("skewX("), 6))       { type = 8; p += 6; }
		else if(!t_strncmp(p, _t("skewY("), 6))       { type = 9; p += 6; }
		else if(!t_strncmp(p, _t("skew("), 5))        { type = 7; p += 5; }
		else if(!t_strncmp(p, _t("matrix("), 7))      { type = 10; p += 7; }
		else return false;  /* unknown function */

		tstring args;
		while(*p && *p != ')') args += *p++;
		if(*p == ')') p++;

		float fa = 1, fb = 0, fc = 0, fd = 1, fe = 0, ff = 0;

		if(type == 0)  /* rotate(deg) */
		{
			char* end = 0;
			float deg = (float) strtod(args.c_str(), &end);
			if(end && !t_strncmp(end, _t("rad"), 3))       deg *= 57.2957795f;
			else if(end && !t_strncmp(end, _t("turn"), 4)) deg *= 360.0f;
			else if(end && !t_strncmp(end, _t("grad"), 4)) deg *= 0.9f;
			float th = deg * 3.14159265358979f / 180.0f;
			fa = cosf(th); fb = sinf(th); fc = -sinf(th); fd = cosf(th);
		}
		else if(type == 1 || type == 2 || type == 3)  /* translate* */
		{
			string_vector toks;
			split_string(args, toks, _t(", "));
			float dx = 0, dy = 0;
			if(toks.size() >= 1)
			{
				tstring tx = toks[0]; trim(tx);
				if(tx.find(_t('%')) != tstring::npos)
					dx = (float) t_strtod(tx.c_str(), nullptr) * 100.0f / 100.0f;  /* nominal 100px box */
				else
					dx = (float) t_strtod(tx.c_str(), nullptr);
			}
			if(toks.size() >= 2 && (type == 1))
			{
				tstring ty = toks[1]; trim(ty);
				if(ty.find(_t('%')) != tstring::npos)
					dy = (float) t_strtod(ty.c_str(), nullptr) * 100.0f / 100.0f;
				else
					dy = (float) t_strtod(ty.c_str(), nullptr);
			}
			if(type == 3) { dy = dx; dx = 0; }  /* translateY uses first arg as y */
			fe = dx; ff = dy;
		}
		else if(type == 4 || type == 5 || type == 6)  /* scale* */
		{
			string_vector toks;
			split_string(args, toks, _t(", "));
			float sx = 1, sy = 1;
			if(toks.size() >= 1) sx = (float) t_strtod(toks[0].c_str(), nullptr);
			if(toks.size() >= 2) sy = (float) t_strtod(toks[1].c_str(), nullptr);
			else sy = sx;
			if(type == 5) { fa = sx; fd = 1; }
			else if(type == 6) { fa = 1; fd = sx; }
			else { fa = sx; fd = sy; }
		}
		else if(type == 7 || type == 8 || type == 9)  /* skew* */
		{
			string_vector toks;
			split_string(args, toks, _t(", "));
			float ax = 0, ay = 0;
			if(toks.size() >= 1)
			{
				char* end = 0;
				ax = (float) strtod(toks[0].c_str(), &end);
				if(end && !t_strncmp(end, _t("rad"), 3)) ax *= 57.2957795f;
			}
			if(toks.size() >= 2 && type == 7)
			{
				char* end = 0;
				ay = (float) strtod(toks[1].c_str(), &end);
				if(end && !t_strncmp(end, _t("rad"), 3)) ay *= 57.2957795f;
			}
			if(type == 8) { fc = tanf(ax * 3.14159265358979f / 180.0f); }
			else if(type == 9) { fb = tanf(ax * 3.14159265358979f / 180.0f); }
			else { fc = tanf(ax * 3.14159265358979f / 180.0f); fb = tanf(ay * 3.14159265358979f / 180.0f); }
		}
		else if(type == 10)  /* matrix(a,b,c,d,e,f) */
		{
			string_vector toks;
			split_string(args, toks, _t(", "));
			if(toks.size() >= 6)
			{
				fa = (float) t_strtod(toks[0].c_str(), nullptr);
				fb = (float) t_strtod(toks[1].c_str(), nullptr);
				fc = (float) t_strtod(toks[2].c_str(), nullptr);
				fd = (float) t_strtod(toks[3].c_str(), nullptr);
				fe = (float) t_strtod(toks[4].c_str(), nullptr);
				ff = (float) t_strtod(toks[5].c_str(), nullptr);
			}
		}

		/* Compose: result = result * fn */
		float na = a * fa + c * fb;
		float nb = b * fa + d * fb;
		float nc = a * fc + c * fd;
		float nd = b * fc + d * fd;
		float ne = a * fe + c * ff + e;
		float nf = b * fe + d * ff + f;
		a = na; b = nb; c = nc; d = nd; e = ne; f = nf;
	}

	m[0] = a; m[1] = b; m[2] = c; m[3] = d; m[4] = e; m[5] = f;
	return true;
}

/* ---- Precise transform interpolation (Phase 3.2) ------------------------- */

/* Matrix-component lerp alone cannot represent a rotation whose endpoints
 * compose to the SAME matrix (rotate(0deg) and rotate(360deg) are both the
 * identity), so a spinner would freeze. CSS interpolates transform lists
 * function-by-function instead; this parses one function into its parameters
 * so matching lists can be lerped precisely and re-emitted. */
struct xfn
{
	int     type;        /* same codes as transform_string_to_matrix */
	float   a, b;        /* angle(deg) / length / scale factor */
	tstring au, bu;      /* unit suffix for a and b ("deg", "px", "%", ...) */
	int     nargs;
	bool    is_matrix;
	xfn() : type(-1), a(0), b(0), nargs(0), is_matrix(false) {}
};

/* Split "12px" / "0.5" / "45deg" into a numeric value and its unit suffix. */
static void split_num_unit(const tstring& tok, float& v, tstring& unit)
{
	tstring s = tok;
	trim(s);
	size_t i = 0;
	if(i < s.length() && (s[i] == _t('+') || s[i] == _t('-'))) i++;
	while(i < s.length() && ((s[i] >= _t('0') && s[i] <= _t('9')) || s[i] == _t('.'))) i++;
	tstring num = s.substr(0, i);
	unit = s.substr(i);
	trim(unit);
	v = num.empty() ? 0.0f : (float) t_strtod(num.c_str(), nullptr);
}

/* Parse an angle token and normalise it to degrees. */
static float parse_angle_deg(const tstring& tok)
{
	float v; tstring u;
	split_num_unit(tok, v, u);
	if(u == _t("rad"))  return v * 57.2957795f;
	if(u == _t("turn")) return v * 360.0f;
	if(u == _t("grad")) return v * 0.9f;
	return v;  /* deg or unitless */
}

/* Parse one transform function at *pp, advancing past the closing ')'. */
static bool parse_one_xfn(const tchar_t** pp, xfn& f)
{
	const tchar_t* p = *pp;
	while(*p == ' ' || *p == '\t') p++;
	if(!*p) return false;
	int type = -1;
	if(!t_strncmp(p, _t("rotate("), 7))          { type = 0; p += 7; }
	else if(!t_strncmp(p, _t("translateX("), 11)) { type = 2; p += 11; }
	else if(!t_strncmp(p, _t("translateY("), 11)) { type = 3; p += 11; }
	else if(!t_strncmp(p, _t("translate3d("), 12)){ type = 1; p += 12; }
	else if(!t_strncmp(p, _t("translate("), 10))  { type = 1; p += 10; }
	else if(!t_strncmp(p, _t("scaleX("), 7))      { type = 5; p += 7; }
	else if(!t_strncmp(p, _t("scaleY("), 7))      { type = 6; p += 7; }
	else if(!t_strncmp(p, _t("scale("), 6))       { type = 4; p += 6; }
	else if(!t_strncmp(p, _t("skewX("), 6))       { type = 8; p += 6; }
	else if(!t_strncmp(p, _t("skewY("), 6))       { type = 9; p += 6; }
	else if(!t_strncmp(p, _t("skew("), 5))        { type = 7; p += 5; }
	else if(!t_strncmp(p, _t("matrix("), 7))      { type = 10; p += 7; f.is_matrix = true; }
	else return false;

	tstring args;
	while(*p && *p != ')') args += *p++;
	if(*p == ')') p++;
	*pp = p;

	string_vector toks;
	split_string(args, toks, _t(", "));
	f.type = type;
	f.nargs = (int)toks.size();

	if(type == 0 || type == 7 || type == 8 || type == 9)  /* angle-valued */
	{
		if(toks.size() >= 1) { f.a = parse_angle_deg(toks[0]); f.au = _t("deg"); }
		if(toks.size() >= 2 && type == 7) { f.b = parse_angle_deg(toks[1]); f.bu = _t("deg"); }
	}
	else if(type == 4 || type == 5 || type == 6)  /* scale */
	{
		if(toks.size() >= 1) f.a = (float) t_strtod(toks[0].c_str(), nullptr);
		if(toks.size() >= 2) f.b = (float) t_strtod(toks[1].c_str(), nullptr);
		else f.b = f.a;  /* scale(s) == scale(s,s) */
	}
	else if(type == 10)  /* matrix: caller falls back to component lerp */
	{
		f.a = f.b = 0.0f;
	}
	else  /* translate / translateX / translateY / translate3d */
	{
		if(toks.size() >= 1) split_num_unit(toks[0], f.a, f.au);
		if(toks.size() >= 2 && (type == 1)) split_num_unit(toks[1], f.b, f.bu);
		else { f.b = 0.0f; f.bu = f.au; }
	}
	return true;
}

/* Parse a whole transform list. Returns false on unknown syntax or matrix()
 * (both force the component-lerp fallback). An empty/"none" string yields an
 * empty list, which represents the identity. */
static bool parse_xfn_list(const tstring& s_in, std::vector<xfn>& out)
{
	out.clear();
	tstring s = s_in;
	trim(s);
	if(s.empty() || s == _t("none")) return true;
	const tchar_t* p = s.c_str();
	while(p && *p)
	{
		while(*p == ' ' || *p == '\t') p++;
		if(!*p) break;
		xfn f;
		if(!parse_one_xfn(&p, f)) return false;
		if(f.is_matrix) return false;
		out.push_back(f);
	}
	return true;
}

/* The identity function matching `type`, used to pad a shorter (or empty)
 * list so none<->rotate(360deg) style transitions still sweep precisely. */
static xfn identity_xfn(int type, const tstring& au, const tstring& bu)
{
	xfn f;
	f.type = type;
	f.a = 0.0f; f.b = 0.0f;
	f.au = au;  f.bu = bu;
	if(type == 4)      { f.a = 1.0f; f.b = 1.0f; }  /* scale */
	else if(type == 5 || type == 6) { f.a = 1.0f; }  /* scaleX / scaleY */
	return f;
}

/* Interpolate two transform lists function-by-function. Returns true and
 * writes `out` when the lists are compatible (same sequence after identity
 * padding, matching units); false lets the caller fall back to matrix lerp. */
static bool interpolate_transform_precise(const tstring& from, const tstring& to, float t, tstring& out)
{
	std::vector<xfn> A, B;
	if(!parse_xfn_list(from, A)) return false;
	if(!parse_xfn_list(to, B)) return false;

	if(A.empty() && B.empty()) { out = _t("none"); return true; }
	if(A.size() != B.size())
	{
		if(A.empty())
		{
			for(size_t i = 0; i < B.size(); i++) A.push_back(identity_xfn(B[i].type, B[i].au, B[i].bu));
		}
		else if(B.empty())
		{
			for(size_t i = 0; i < A.size(); i++) B.push_back(identity_xfn(A[i].type, A[i].au, A[i].bu));
		}
		else
		{
			return false;  /* different function counts: component lerp */
		}
	}

	tstring result;
	char buf[80];
	for(size_t i = 0; i < A.size(); i++)
	{
		const xfn& a = A[i];
		const xfn& b = B[i];
		if(a.type != b.type) return false;
		if(i > 0) result += _t(" ");
		switch(a.type)
		{
		case 0:  /* rotate */
			snprintf(buf, sizeof(buf), "rotate(%.4fdeg)", a.a + (b.a - a.a) * t);
			result += buf;
			break;
		case 8:  /* skewX */
			snprintf(buf, sizeof(buf), "skewX(%.4fdeg)", a.a + (b.a - a.a) * t);
			result += buf;
			break;
		case 9:  /* skewY */
			snprintf(buf, sizeof(buf), "skewY(%.4fdeg)", a.a + (b.a - a.a) * t);
			result += buf;
			break;
		case 7:  /* skew(ax,ay) */
			snprintf(buf, sizeof(buf), "skew(%.4fdeg,%.4fdeg)",
				a.a + (b.a - a.a) * t, a.b + (b.b - a.b) * t);
			result += buf;
			break;
		case 4:  /* scale */
			snprintf(buf, sizeof(buf), "scale(%.5f,%.5f)",
				a.a + (b.a - a.a) * t, a.b + (b.b - a.b) * t);
			result += buf;
			break;
		case 5:  /* scaleX */
			snprintf(buf, sizeof(buf), "scaleX(%.5f)", a.a + (b.a - a.a) * t);
			result += buf;
			break;
		case 6:  /* scaleY */
			snprintf(buf, sizeof(buf), "scaleY(%.5f)", a.a + (b.a - a.a) * t);
			result += buf;
			break;
		case 2:  /* translateX */
		case 3:  /* translateY */
		{
			if(a.au != b.au && !(a.a == 0.0f && b.a == 0.0f)) return false;
			float v = a.a + (b.a - a.a) * t;
			tstring u = a.au.empty() ? b.au : a.au;
			snprintf(buf, sizeof(buf), "%s(%.4f%s)",
				a.type == 2 ? "translateX" : "translateY", v, u.c_str());
			result += buf;
			break;
		}
		case 1:  /* translate(x,y) */
		{
			bool ux_ok = (a.au == b.au) || (a.a == 0.0f && b.a == 0.0f);
			bool uy_ok = (a.bu == b.bu) || (a.b == 0.0f && b.b == 0.0f);
			if(!ux_ok || !uy_ok) return false;
			float vx = a.a + (b.a - a.a) * t;
			float vy = a.b + (b.b - a.b) * t;
			tstring ux = a.au.empty() ? b.au : a.au;
			tstring uy = a.bu.empty() ? b.bu : a.bu;
			snprintf(buf, sizeof(buf), "translate(%.4f%s,%.4f%s)", vx, ux.c_str(), vy, uy.c_str());
			result += buf;
			break;
		}
		default:
			return false;
		}
	}
	out = result;
	return true;
}

/* Interpolate a single CSS length ("12px", "50%", "1.5em"). Same-unit values
 * lerp; mismatched units (px vs %) or non-numeric keywords (auto/none/inherit)
 * return false so the caller snaps to the end state (discrete switch). */
static bool interpolate_length(const tstring& from_in, const tstring& to_in, float t, tstring& out)
{
	tstring from = from_in, to = to_in;
	trim(from);
	trim(to);
	if(from.empty() || to.empty()) return false;
	/* Reject values that do not lead with a number (after sign/space). */
	bool lead_ok = true;
	for(int which = 0; which < 2 && lead_ok; which++)
	{
		const tstring& s = (which == 0) ? from : to;
		bool seen = false;
		for(size_t i = 0; i < s.length(); i++)
		{
			tchar_t c = s[i];
			if(c == _t('.') || (c >= _t('0') && c <= _t('9'))) { seen = true; break; }
			if(c == _t('+') || c == _t('-') || c == _t(' ')) continue;
			break;
		}
		if(!seen) lead_ok = false;
	}
	if(!lead_ok) return false;
	float av, bv;
	tstring au, bu;
	split_num_unit(from, av, au);
	split_num_unit(to, bv, bu);
	if(au != bu && !(av == 0.0f && bv == 0.0f)) return false;  /* discrete switch */
	float v = av + (bv - av) * t;
	tstring u = au.empty() ? bu : au;
	char buf[64];
	snprintf(buf, sizeof(buf), "%.4f%s", v, u.c_str());
	out = buf;
	return true;
}

bool interpolate_property(const tstring& name, const tstring& from, const tstring& to, float t, tstring& out)
{
	tstring n = name;
	lcase(n);
	if(n == _t("opacity"))
	{
		float a = 1.0f, b = 1.0f;
		if(!parse_opacity(from, a)) return false;
		if(!parse_opacity(to, b)) return false;
		float v = a + (b - a) * t;
		if(v < 0.0f) v = 0.0f;
		if(v > 1.0f) v = 1.0f;
		char buf[32];
		snprintf(buf, sizeof(buf), "%.4f", v);
		out = buf;
		return true;
	}
	if(n == _t("transform"))
	{
		/* Precise path first: interpolate matching transform lists
		 * function-by-function. This is essential for rotation, where both
		 * endpoints (e.g. 0deg and 360deg) compose to the same matrix and a
		 * component lerp would freeze the spinner. */
		tstring precise;
		if(interpolate_transform_precise(from, to, t, precise))
		{
			out = precise;
			return true;
		}
		/* Fallback: matrix-component lerp for mixed/multiple functions,
		 * matrix() endpoints, or mismatched units. Percentage translate values
		 * resolve against a nominal 100x100 box (the real box is applied at
		 * paint time), so the interpolation only needs to be monotonic in t. */
		float m1[6], m2[6];
		if(!transform_string_to_matrix(from, m1)) return false;
		if(!transform_string_to_matrix(to, m2)) return false;
		float mr[6];
		for(int i = 0; i < 6; i++)
			mr[i] = m1[i] + (m2[i] - m1[i]) * t;
		char buf[128];
		snprintf(buf, sizeof(buf), "matrix(%.6f,%.6f,%.6f,%.6f,%.6f,%.6f)",
			mr[0], mr[1], mr[2], mr[3], mr[4], mr[5]);
		out = buf;
		return true;
	}
	/* Phase 3.1: layout-affecting length properties lerp when the units match
	 * and snap (discrete) when they do not. */
	if(is_length_prop(n))
	{
		return interpolate_length(from, to, t, out);
	}
	return false;
}

void sample_keyframes(const keyframes_rule& rule, float p, props_map& out)
{
	out.clear();
	if(rule.stops.empty()) return;
	if(p < 0.0f) p = 0.0f;
	if(p > 1.0f) p = 1.0f;

	/* Find the surrounding stops. */
	const keyframe_stop* lo = nullptr;
	const keyframe_stop* hi = nullptr;
	for(size_t i = 0; i < rule.stops.size(); i++)
	{
		if(rule.stops[i].offset <= p) lo = &rule.stops[i];
		if(rule.stops[i].offset >= p && !hi) hi = &rule.stops[i];
	}
	if(!lo) lo = &rule.stops.front();
	if(!hi) hi = &rule.stops.back();

	/* Local progress between the two stops. */
	float span = hi->offset - lo->offset;
	float local_t = (span <= 0.0f) ? 1.0f : (p - lo->offset) / span;
	if(local_t < 0.0f) local_t = 0.0f;
	if(local_t > 1.0f) local_t = 1.0f;

	/* Union of property names across both stops; interpolate the ones we
	 * support, snap the rest to the hi-stop value (so at least the end
	 * state is honoured even if the property doesn't animate). */
	std::vector<tstring> names;
	for(props_map::const_iterator it = lo->props.begin(); it != lo->props.end(); ++it)
	{
		names.push_back(it->first);
	}
	for(props_map::const_iterator it = hi->props.begin(); it != hi->props.end(); ++it)
	{
		if(std::find(names.begin(), names.end(), it->first) == names.end())
		{
			names.push_back(it->first);
		}
	}
	for(size_t i = 0; i < names.size(); i++)
	{
		const tstring& nm = names[i];
		props_map::const_iterator a = lo->props.find(nm);
		props_map::const_iterator b = hi->props.find(nm);
		tstring av = (a != lo->props.end()) ? a->second.m_value : tstring();
		tstring bv = (b != hi->props.end()) ? b->second.m_value : tstring();
		property_value pv;
		pv.m_important = false;
		if(property_is_interpolable(nm) && !av.empty() && !bv.empty())
		{
			tstring interp;
			if(interpolate_property(nm, av, bv, local_t, interp)) pv.m_value = interp;
			else pv.m_value = bv;
		}
		else
		{
			/* Snap to whichever stop is closer; at local_t >= 0.5 use hi. */
			pv.m_value = (local_t < 0.5f) ? av : bv;
			if(pv.m_value.empty()) pv.m_value = bv.empty() ? av : bv;
		}
		out[nm] = pv;
	}
}

}	/* namespace litehtml */
