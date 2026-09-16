#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include "os_types.h"
#include "types.h"
#include "style.h"

/* CSS animation support (Phase 2 of the animation plan).
 *
 * Scope of this initial cut — deliberately narrow so the whole pipeline runs
 * end to end before we widen it:
 *
 *   - @keyframes parsing (percent / from / to selectors, per-stop property
 *     maps) stored on the document.
 *   - transition / transition-* and animation / animation-* property parsing
 *     into anim_declaration lists on each html_tag.
 *   - A per-document timeline of active_anim entries driven by tick(now_ms).
 *   - Interpolation on exactly ONE property in this phase: opacity. Any other
 *     property declared in transition/animation is captured but silently
 *     jumps to its end state (no half-animated garbage on screen).
 *   - Timing function support is limited to linear and the four CSS presets
 *     (ease, ease-in, ease-out, ease-in-out). cubic-bezier() and steps() are
 *     parsed and stored but fall back to linear until Phase 3.
 *   - iteration-count / direction / fill-mode / delay / play-state are all
 *     parsed and stored; only the ones that affect the opacity path are
 *     honoured right now (delay, duration, iteration-count including
 *     infinite, direction, fill-mode, play-state).
 *
 * Deliberately OUT of scope in this cut (Phase 3 follow-up):
 *   - Interpolation of color / background-color / length properties /
 *     transform. Declarations for those properties are parsed but do not
 *     animate; they snap to their end state on tick 0.
 *   - cubic-bezier(x1,y1,x2,y2) evaluation, steps(n, <pos>) evaluation.
 *   - Element.animate() / getAnimations() / AnimationEvent JS APIs.
 *
 * The whole subsystem is gated by an EWEB_DISABLE_ANIMATION=1 environment
 * variable checked at document construction, so a regression can be bisected
 * by rerunning with animations forced off. */

namespace litehtml
{
	class html_tag;
	class document;

	/* Preset timing functions from the CSS spec. Cubic-bezier control points
	 * for the presets are the same ones browsers use. */
	enum anim_timing_preset
	{
		anim_timing_linear,
		anim_timing_ease,
		anim_timing_ease_in,
		anim_timing_ease_out,
		anim_timing_ease_in_out,
		anim_timing_step_start,
		anim_timing_step_end,
	};

	struct anim_timing_fn
	{
		anim_timing_preset	preset;
		bool				is_cubic_bezier;	/* parsed but only linear-evaluated in Phase 2 */
		float				cb[4];				/* x1,y1,x2,y2 */
		bool				is_steps;
		int					steps_count;
		bool				steps_jump_start;	/* steps(n, start) vs steps(n, end) */

		anim_timing_fn()
			: preset(anim_timing_ease)
			, is_cubic_bezier(false)
			, is_steps(false)
			, steps_count(1)
			, steps_jump_start(false)
		{
			cb[0] = 0.25f; cb[1] = 0.1f; cb[2] = 0.25f; cb[3] = 1.0f;
		}

		/* Evaluate the easing curve at normalised progress t in [0,1]. */
		float eval(float t) const;

		/* Parse a timing-function value string. Returns true on success;
		 * unknown values fall back to `ease`. */
		static bool parse(const tstring& s, anim_timing_fn& out);
	};

	enum anim_direction
	{
		anim_dir_normal,
		anim_dir_reverse,
		anim_dir_alternate,
		anim_dir_alternate_reverse,
	};

	enum anim_fill_mode
	{
		anim_fill_none,
		anim_fill_forwards,
		anim_fill_backwards,
		anim_fill_both,
	};

	enum anim_play_state
	{
		anim_play_running,
		anim_play_paused,
	};

	/* A parsed transition or animation declaration sitting on an element. */
	struct anim_declaration
	{
		tstring				property;		/* transition: property name or "all"; animation: unused */
		tstring				name;			/* animation: @keyframes name; transition: unused */
		int					duration_ms;
		int					delay_ms;
		anim_timing_fn		timing;
		float				iteration_count;	/* -1 = infinite */
		anim_direction		direction;
		anim_fill_mode		fill_mode;
		anim_play_state		play_state;
		bool				is_animation;		/* true = @keyframes animation, false = transition */

		anim_declaration()
			: duration_ms(0)
			, delay_ms(0)
			, iteration_count(1.0f)
			, direction(anim_dir_normal)
			, fill_mode(anim_fill_none)
			, play_state(anim_play_running)
			, is_animation(false)
		{
		}
	};

	/* A single keyframe stop inside an @keyframes rule. */
	struct keyframe_stop
	{
		float		offset;		/* 0.0 .. 1.0 */
		props_map	props;		/* raw property values as authored */
	};

	struct keyframes_rule
	{
		std::vector<keyframe_stop>	stops;	/* sorted by offset after parse */
	};

	/* A running animation instance. Owned by document::m_anim_timeline. */
	struct active_anim
	{
		html_tag*			element;
		bool				is_transition;
		/* For transitions: the property being animated and the from/to values
		 * captured when the transition started. For keyframe animations: the
		 * keyframes name (property is derived per-tick from the surrounding
		 * stops). */
		tstring				property;
		tstring				from_str;
		tstring				to_str;
		tstring				keyframes_name;

		uint64_t			start_ms;		/* wall-clock tick base */
		int					duration_ms;
		int					delay_ms;
		anim_timing_fn		timing;
		float				iteration_count;
		anim_direction		direction;
		anim_fill_mode		fill_mode;
		anim_play_state		play_state;
		uint64_t			paused_at_ms;	/* valid when play_state == paused */
		uint64_t			pause_accum_ms;	/* total time spent paused */
		bool				finished;

		active_anim()
			: element(nullptr)
			, is_transition(false)
			, start_ms(0)
			, duration_ms(0)
			, delay_ms(0)
			, iteration_count(1.0f)
			, direction(anim_dir_normal)
			, fill_mode(anim_fill_none)
			, play_state(anim_play_running)
			, paused_at_ms(0)
			, pause_accum_ms(0)
			, finished(false)
		{
		}
	};

	/* ---- Parsing helpers (implemented in animation.cpp) ---- */

	/* Parse a comma-separated transition list. Handles both the shorthand
	 * (`transition: opacity 200ms ease 50ms, color 1s`) and the longhand
	 * properties (`transition-property`, `transition-duration`, ...).
	 * Longhands win over the shorthand when both are present, matching the
	 * CSS cascade rules for shorthands. */
	void parse_transition_declarations(const props_map& props, std::vector<anim_declaration>& out);

	/* Parse a comma-separated animation list. Same shorthand/longhand rule. */
	void parse_animation_declarations(const props_map& props, std::vector<anim_declaration>& out);

	/* Parse a duration token like "200ms", "1.5s", "0". Returns milliseconds. */
	int parse_time_ms(const tstring& s);

	/* Parse the body of `@keyframes NAME { ... }` into a rule. */
	bool parse_keyframes_body(const tstring& body, keyframes_rule& out);

	/* ---- Interpolation (Phase 2: opacity only) ---- */

	/* Returns true when the given property is interpolable in this phase. */
	bool property_is_interpolable(const tstring& name);

	/* Phase 3.1: true when interpolating this property changes geometry and so
	 * requires an animation-driven relayout after the tick writes the value.
	 * Paint-only animated properties (opacity, transform, border-radius)
	 * return false. */
	bool property_needs_relayout(const tstring& name);

	/* Interpolate raw string values for a property at progress t in [0,1].
	 * Writes the resulting string into `out`. Returns false when the values
	 * cannot be interpolated (unknown property, mismatched units, etc.), in
	 * which case callers should snap to `to`. */
	bool interpolate_property(const tstring& name, const tstring& from, const tstring& to, float t, tstring& out);

	/* Sample a keyframes rule at progress p in [0,1] and write the resolved
	 * property values (interpolated between the surrounding stops) into out.
	 * Only properties listed in property_is_interpolable() are emitted; other
	 * properties present in the stops are silently dropped in this phase. */
	void sample_keyframes(const keyframes_rule& rule, float p, props_map& out);
}
