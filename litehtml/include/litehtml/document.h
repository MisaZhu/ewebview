#pragma once
#include "style.h"
#include "types.h"
#include "context.h"
#include "animation.h"
#include "gumbo/gumbo.h"
#include <stdint.h>

namespace litehtml
{
	struct css_text
	{
		typedef std::vector<css_text>	vector;

		tstring	text;
		tstring	baseurl;
		tstring	media;
		
		css_text()
		{
		}

		css_text(const tchar_t* txt, const tchar_t* url, const tchar_t* media_str)
		{
			text	= txt ? txt : _t("");
			baseurl	= url ? url : _t("");
			media	= media_str ? media_str : _t("");
		}

		css_text(const css_text& val)
		{
			text	= val.text;
			baseurl	= val.baseurl;
			media	= val.media;
		}
	};

	struct stop_tags_t
	{
		const litehtml::tchar_t*	tags;
		const litehtml::tchar_t*	stop_parent;
	};

	struct ommited_end_tags_t
	{
		const litehtml::tchar_t*	tag;
		const litehtml::tchar_t*	followed_tags;
	};

	class html_tag;

	/* Defined in html_tag.cpp: dumps and resets the per-update apply-phase
	 * timing counters (candidate collect / match / add_style / recursion). */
	void dump_apply_phase_profile();

	class document
	{
	public:
		typedef document*	ptr;
		typedef const document*	const_ptr;
	private:
		element::ptr					m_root;
		document_container*					m_container;
		/* Scroll position noted by the embedder before a hit test, so
		 * position:sticky boxes can be tested where they are pinned (the hit
		 * walk itself only sees local/document coordinates). */
		int								m_hit_scroll_x = 0;
		int								m_hit_scroll_y = 0;
		/* Scroll captured at the top of draw(); the paint walk reads it so
		 * position:sticky pinning is depth-independent. */
		int								m_draw_scroll_x = 0;
		int								m_draw_scroll_y = 0;
		/* Frame-space row of the viewport origin (vertical overscan). The
		 * embedder may draw into a buffer taller than the viewport with the
		 * viewport starting this many rows down, so draw() receives
		 * y = vp_origin - scroll. Fixed-position boxes paint at the client
		 * rect (viewport-relative), so they must be shifted down by this
		 * offset to land at their viewport rows in the buffer, and the true
		 * scroll derives as vp_origin - y. 0 = no overscan (buffer ==
		 * viewport, the long-standing layout). */
		int								m_draw_vp_origin_y = 0;
		fonts_map							m_fonts;
		css_text::vector					m_css;
		litehtml::css						m_styles;
		/* True once create_document's one-shot m_css sweep has parsed every
		 * queued sheet into m_styles. m_css is never consumed again afterwards,
		 * so add_stylesheet must parse a later sheet (a <style> JS injects at
		 * runtime) straight into m_styles instead of queuing it to be dropped. */
		bool								m_css_swept = false;
		litehtml::web_color					m_def_color;
		litehtml::context*					m_context;
		litehtml::size						m_size;
		position::vector					m_fixed_boxes;
		media_query_list::vector			m_media_lists;
		/* Set when a post-creation style update re-resolves computed styles:
		 * table grids (html_tag::m_grid) were built in init() against the old
		 * display values, so they must be rebuilt before the next layout or
		 * cells whose display flipped (e.g. to none) keep occupying columns. */
		bool								m_tables_dirty;
		element::ptr						m_over_element;
		elements_vector						m_tabular_elements;
		/* Set when add_tabular registers an element that was not already in
		 * m_tabular_elements. create_document runs fix_tables_layout() once at
		 * the end of the initial parse; elements registered later (fast-mode
		 * builds register nothing until the chunked reparse, and JS restyles
		 * can flip display to table) would otherwise never get their anonymous
		 * boxes, so document::render consumes this flag and re-runs the fixup
		 * before layout. */
		bool								m_tabular_dirty = false;
		media_features						m_media;
		tstring                             m_lang;
		tstring                             m_culture;
		bool								m_last_font_valid;
		tstring								m_last_font_name;
		tstring								m_last_font_weight;
		tstring								m_last_font_style;
		tstring								m_last_font_decoration;
		int									m_last_font_size;
		uint_ptr							m_last_font;
		font_metrics						m_last_font_metrics;
		/* Time-sliced master-style update state: a full refresh+apply+parse
		 * pass on a CSS-heavy page costs seconds, so it is split into chunks
		 * bounded by a wall-clock deadline handed in by the UI thread. Each
		 * chunk walks the tree and stamps visited elements with m_step_epoch;
		 * stamped elements are skipped until the epoch changes, which makes a
		 * paused walk resumable without redoing work. */
		unsigned int						m_step_epoch;
		int									m_step_phase;
		uint64_t							m_step_deadline;
		uint32_t							m_step_visits;
		uint32_t							m_step_stamped;
		uint32_t							m_step_apply_ms;
		uint32_t							m_step_parse_ms;
		uint64_t							m_step_start;
		bool								m_step_exhausted;
		/* CSS animation subsystem (Phase 2). @keyframes rules keyed by their
		 * lowercase name, populated by css::parse_atrule when a stylesheet
		 * carrying @keyframes is added. The active timeline holds every
		 * running transition/animation instance; tick_animations() advances
		 * them and writes interpolated values back onto their target
		 * elements. m_animations_disabled latches EWEB_DISABLE_ANIMATION at
		 * construction so a single env var can turn the whole subsystem off
		 * for regression bisecting. */
		std::map<tstring, keyframes_rule>	m_keyframes;
		std::vector<active_anim>			m_anim_timeline;
		bool								m_animations_disabled;
		uint64_t							m_anim_last_tick_ms;
		/* Phase 3.1: elements whose geometry changed this tick because a
		 * layout-affecting length property was interpolated. The engine drains
		 * this after tick_animations() and re-runs parse_styles + render on
		 * them (animation-driven relayout). Elements whose subtree exceeds the
		 * relayout node budget are skipped (degraded to a discrete switch). */
		std::vector<html_tag*>				m_anim_relayout;
		/* Sub-pixel-precise computed font-size (in px) of the root element,
		 * refreshed whenever the root's font is (re)initialised. m_font_size is
		 * an int, so a viewport-scaled root like "font-size:0.67px" (common in
		 * mobile-first rem layouts) would truncate to 0 and make every rem
		 * length fall back to the 16px default; this keeps the fraction for the
		 * rem unit resolver. 0 means "not set yet". */
		double								m_root_font_size_px = 0.0;
	public:
		void							set_root_font_size(double px) { m_root_font_size_px = px; }
		double							get_root_font_size() const { return m_root_font_size_px; }

		document(litehtml::document_container* objContainer, litehtml::context* ctx);
		virtual ~document();

		litehtml::document_container*	container()	{ return m_container; }
		uint_ptr						get_font(const tchar_t* name, int size, const tchar_t* weight, const tchar_t* style, const tchar_t* decoration, font_metrics* fm);
		int								render(int max_width, render_type rt = render_all);
		void							draw(uint_ptr hdc, int x, int y, const position* clip);
		web_color						get_def_color()	{ return m_def_color; }
		int								cvt_units(const tchar_t* str, int fontSize, bool* is_percent = 0) const;
		int								cvt_units(css_length& val, int fontSize, int size = 0) const;
		int								width() const;
		int								height() const;
		void							add_stylesheet(const tchar_t* str, const tchar_t* baseurl, const tchar_t* media);
		/* Parse a sheet straight into m_styles NOW, for sheets that appear or
		 * change after the creation-time m_css sweep (a <style> whose text JS
		 * rewrites). add_stylesheet only queues m_css, which is consumed once at
		 * create_document, so late queues would never be parsed. */
		void							add_stylesheet_now(const tchar_t* str, const tchar_t* baseurl, const tchar_t* media);
		bool							on_mouse_over(int x, int y, int client_x, int client_y, position::vector& redraw_boxes);
		bool							on_lbutton_down(int x, int y, int client_x, int client_y, position::vector& redraw_boxes);
		bool							on_lbutton_up(int x, int y, int client_x, int client_y, position::vector& redraw_boxes);
		bool							on_mouse_leave(position::vector& redraw_boxes);
		litehtml::element::ptr			create_element(const tchar_t* tag_name, const string_map& attributes);
		/* Match the master sheet, attribute styles and the document sheets against
		 * a subtree that was built after document creation (createElement + appendChild).
		 * Nodes styled by an earlier pass (at creation, innerHTML, or a previous
		 * call) are re-cascaded from a clean slate via
		 * html_tag::reapply_style_cascade, because they may have been styled while
		 * detached, where ancestor-dependent selectors could not match. A work
		 * budget bounds each call. */
		bool style_detached_subtree(element* el);
		element::ptr					root();
		void							set_hit_scroll(int x, int y) { m_hit_scroll_x = x; m_hit_scroll_y = y; }
		int								get_hit_scroll_x() const { return m_hit_scroll_x; }
		int								get_hit_scroll_y() const { return m_hit_scroll_y; }
		int								get_draw_scroll_y() const { return m_draw_scroll_y; }
		void							set_draw_vp_origin_y(int v) { m_draw_vp_origin_y = v; }
		int								get_draw_vp_origin_y() const { return m_draw_vp_origin_y; }
		void							get_fixed_boxes(position::vector& fixed_boxes);
		void							add_fixed_box(const position& pos);
		void							add_media_list(media_query_list::ptr list);
		/* Register and evaluate the @media lists of the context (master)
		 * stylesheets. Those sheets are parsed without a document (external
		 * CSS goes through context::load_master_stylesheet), so nothing else
		 * ever calls add_media_list for them and their selectors would stay
		 * permanently inactive (is_media_valid()==false). */
		void							register_master_media_lists();
		bool							media_changed();
		bool							lang_changed();
		bool                            match_lang(const tstring & lang);
		void							add_tabular(const element::ptr& el);
		void							update_master_styles();
		/* Runs one time-bounded chunk of the master-style update; returns true
		 * when the whole update (refresh+apply+parse) has completed. Callers
		 * drive it from their event loop so a slow page never blocks input. */
		bool							update_master_styles_step(uint64_t deadline_ms);
		bool							style_step_active() const { return m_step_phase != 0; }
		int								style_step_phase() const { return m_step_phase; }
		unsigned int					style_step_epoch() const { return m_step_epoch; }
		/* The document's own sheets (<style> tags + linked sheets parsed into
		 * m_styles). The chunked apply walk applies master CSS and these in one
		 * pass per element, so a resumed chunk keeps a single consistent stamp
		 * epoch instead of re-walking the tree once per sheet. */
		const litehtml::css&			doc_styles() const { return m_styles; }
		/* True when any applied selector tests the named attribute; the DOM
		 * bridge uses it to restyle on data- and aria- mutations (EWebJs.cc). */
		bool							uses_attribute_selector(const tchar_t* name) const;
		/* The context (master) sheet set. Async containers (ewebview) feed every
		 * fetched <link> sheet to context::load_master_stylesheet, so doc_styles
		 * alone can be empty; late-cascade sites (el_svg shape children) must
		 * match against both. */
		const litehtml::css&			master_styles() const
		{
			static const litehtml::css no_master;
			return m_context ? m_context->master_css() : no_master;
		}
		/* Progress counters for the chunked walk: stamped is cumulative across
		 * chunks (so it must keep climbing, otherwise the walk is stuck redoing
		 * the same elements), visits is per chunk. */
		uint32_t						style_step_stamped() const { return m_step_stamped; }
		uint32_t						style_step_visits() const { return m_step_visits; }
		bool							style_step_exhausted();
		void							style_step_stamp(element* el);
		/* Master css changed while a step was in flight: restart from scratch. */
		void							abort_style_step() { m_step_phase = 0; }
		bool							is_fast_mode() const { return m_context && m_context->is_fast_mode(); }

		/* ---- CSS animation subsystem (Phase 2) ---- */
		/* Register a parsed @keyframes rule. Later registrations with the
		 * same name replace earlier ones (CSS cascade order). */
		void							add_keyframes(const tstring& name, const keyframes_rule& rule);
		/* Look up a @keyframes rule by (case-insensitive) name; returns
		 * nullptr when the name is unknown. */
		const keyframes_rule*			find_keyframes(const tstring& name) const;
		/* Enqueue a new active animation on the timeline. Silently drops the
		 * request when animations are disabled, when the timeline is at its
		 * hard cap, or when the element already has a running animation on
		 * the same (property, keyframes_name) pair (in which case the
		 * existing entry is replaced). */
		void							start_animation(const active_anim& a);
		/* Advance every active animation to `now_ms` and write interpolated
		 * values onto the target elements. Returns true when any animation
		 * is still running after the tick (so the caller can request another
		 * frame). Safe to call with an empty timeline. */
		bool							tick_animations(uint64_t now_ms);
		/* Phase 3.1: move the set of elements that need an animation-driven
		 * relayout into `out` and clear the internal list. The engine re-runs
		 * parse_styles + render on each. Returns true when non-empty. */
		bool							drain_anim_relayout(std::vector<html_tag*>& out);
		bool							has_anim_relayout() const { return !m_anim_relayout.empty(); }
		/* Phase 3.1: record that `el` needs a relayout because a layout-affecting
		 * length property was just interpolated. No-op for paint-only props and
		 * for elements whose subtree exceeds the relayout node budget. */
		void							note_anim_relayout(html_tag* el, const tstring& prop);
		/* Drop every active animation. Called on navigation and when a
		 * target element is removed from the tree. */
		void							clear_animations();
		/* Drop only the animations targeting `el`. Called from html_tag's
		 * teardown path so a deleted element cannot leave a dangling
		 * pointer in the timeline. */
		void							clear_animations_for(html_tag* el);
		bool							animations_disabled() const { return m_animations_disabled; }

		static litehtml::document::ptr createFromString(const tchar_t* str, litehtml::document_container* objPainter, litehtml::context* ctx, litehtml::css* user_styles = 0);
		static litehtml::document::ptr createFromUTF8(const char* str, litehtml::document_container* objPainter, litehtml::context* ctx, litehtml::css* user_styles = 0);
		/* Parse an HTML fragment (innerHTML semantics) into detached elements
		 * owned by THIS document, appended to `out`. gumbo wraps the fragment in
		 * a body; the same create_node path a full parse uses builds the nodes,
		 * so attributes/images behave identically. The caller parents and styles
		 * them (style_detached_subtree + parse_styles), which runs parse_attributes
		 * so an <img> resolves its src and queues a fetch. */
		void create_fragment(const tchar_t* html, elements_vector& out);
	
	private:
		litehtml::uint_ptr	add_font(const tchar_t* name, int size, const tchar_t* weight, const tchar_t* style, const tchar_t* decoration, font_metrics* fm);

		void create_node(GumboNode* node, elements_vector& elements, int depth = 0);
		bool update_media_lists(const media_features& features);
		void fix_tables_layout();
		void fix_table_children(element::ptr& el_ptr, style_display disp, const tchar_t* disp_str);
		void fix_table_parent(element::ptr& el_ptr, style_display disp, const tchar_t* disp_str);
	};

	inline element::ptr document::root()
	{
		return m_root;
	}
	inline void document::add_tabular(const element::ptr& el)
	{
		/* parse_styles re-runs on every restyle/reparse epoch: registering the
		 * same element again would grow m_tabular_elements without bound (the
		 * deferred fixup walks the whole list). Only a genuinely new entry
		 * marks the list dirty. */
		if (std::find(m_tabular_elements.begin(), m_tabular_elements.end(), el)
				== m_tabular_elements.end())
		{
			m_tabular_elements.push_back(el);
			m_tabular_dirty = true;
		}
	}
	inline bool document::match_lang(const tstring & lang)
	{
		/* Initialise lazily: :lang() selectors are evaluated during the first
		 * style pass, before embedders normally have a reason to call
		 * lang_changed().  CSS language ranges match both an exact tag and a
		 * hyphen-separated subtag ("en" matches "en-US"). */
		if(m_lang.empty() && m_container)
		{
			tstring culture;
			m_container->get_language(m_lang, culture);
			m_culture = culture.empty() ? tstring() : m_lang + _t('-') + culture;
		}
		auto matches = [&lang](const tstring& value)
		{
			return value.length() >= lang.length() &&
				!t_strncasecmp(value.c_str(), lang.c_str(), lang.length()) &&
				(value.length() == lang.length() || value[lang.length()] == _t('-'));
		};
		return !lang.empty() && (matches(m_lang) || matches(m_culture));
	}

	/* Split a character-data run into line-breakable chunks: browsers may
	 * break between any two CJK characters, so an ideographic run becomes
	 * one chunk per character, while ASCII/Latin runs stay whole (they only
	 * break at spaces, which the caller keeps in separate el_space nodes).
	 * Two kinsoku rules are honoured: closing punctuation (。，」 etc.) never
	 * starts a chunk, opening punctuation (「（【 etc.) never ends one.
	 * Pure-ASCII input yields the input unchanged. Used by the parser's text
	 * flush and by html_tag::appendChild/insertBefore so script-created text
	 * nodes (textContent) wrap exactly like parsed ones. */
	void split_cjk_text(const tstring& in, std::vector<tstring>& out);
}
