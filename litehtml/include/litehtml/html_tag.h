#pragma once

#include "element.h"
#include "style.h"
#include "background.h"
#include "css_margins.h"
#include "borders.h"
#include "css_selector.h"
#include "stylesheet.h"
#include "box.h"
#include "table.h"
#include "animation.h"

namespace litehtml
{
	struct line_context
	{
		int calculatedTop;
		int top;
		int left;
		int right;

		int width()
		{
			return right - left;
		}
		void fix_top()
		{
			calculatedTop = top;
		}
	};

	class html_tag : public element
	{
		friend class elements_iterator;
		friend class el_table;
		friend class table_grid;
		friend class block_box;
		friend class line_box;
	public:
		typedef std::shared_ptr<litehtml::html_tag>	ptr;
	protected:
		box::vector				m_boxes;
		string_vector			m_class_values;
		tstring					m_tag;
		litehtml::style			m_style;
		/* Animation-driven property overrides (Phase 2). Written by
		 * document::tick_animations, read by parse_styles AFTER the cascade
		 * so an in-flight transition/animation wins over authored values.
		 * Kept as raw CSS strings for symmetry with style::m_properties; the
		 * set of properties that can be overridden is gated by
		 * property_is_interpolable() in animation.cpp. */
		string_map				m_anim_overrides;
		/* Parsed transition and animation declarations (Phase 2). Populated
		 * by parse_styles from m_style on every cascade; compared against the
		 * previous set to detect new transitions (value changes) and new
		 * animations (animation-name appears). */
		std::vector<anim_declaration>	m_transitions;
		std::vector<anim_declaration>	m_animations;
		/* Last computed transform string (override or CSS), kept so the
		 * transition trigger in parse_styles can detect a change and fire a
		 * transform transition. Empty means identity/none. */
		tstring							m_transform_str;
		/* Resolved --custom-properties visible to this element (inherited
		 * from the parent chain plus own declarations), used to expand
		 * var() references in raw property values at parse_styles time. */
		string_map				m_custom_props;
		string_map				m_attrs;
		mutable string_map		m_style_property_cache;
		vertical_align			m_vertical_align;
		text_align				m_text_align;
		text_transform			m_text_transform;
		style_display			m_display;
		list_style_type			m_list_style_type;
		list_style_position		m_list_style_position;
		white_space				m_white_space;
		element_float			m_float;
		element_clear			m_clear;
		floated_box::vector		m_floats_left;
		floated_box::vector		m_floats_right;
		elements_vector			m_positioned;
		background				m_bg;
		element_position		m_el_position;
		int						m_line_height;
		bool					m_lh_predefined;
		string_vector			m_pseudo_classes;
		used_selector::vector	m_used_styles;		
		
		uint_ptr				m_font;
		int						m_font_size;
		font_metrics			m_font_metrics;

		css_margins				m_css_margins;
		css_margins				m_css_padding;
		css_borders				m_css_borders;
		/* When >0, overrides the width percentage padding/margin/borders resolve
		 * against in calc_outlines (flex items: spec says their containing block
		 * is the flex container content box, not their resolved main size). */
		int						m_pct_cb_width = 0;
		/* Parsed CSS `transform` function list, paint-time only: the box's
		 * borders are drawn through the composed matrix (see draw_background).
		 * Empty list = identity. Not inherited.
		 * Type codes: 0=rotate 1=translate 2=translateX 3=translateY
		 *             4=scale 5=scaleX 6=scaleY 7=skew 8=skewX 9=skewY
		 *             10=matrix(a,b,c,d,e,f) */
		struct transform_fn
		{
			int			type;
			float		deg;		// rotate/skew angle in degrees
			css_length	x, y;		// translate lengths (percentages resolve at paint)
			float		sx, sy;		// scale factors
			float		mat[6];		// matrix(a,b,c,d,e,f) components
			transform_fn() : type(0), deg(0), sx(1), sy(1) { mat[0]=1;mat[1]=0;mat[2]=0;mat[3]=1;mat[4]=0;mat[5]=0; }
		};
		std::vector<transform_fn>	m_transform;
		css_length				m_css_width;
		css_length				m_css_height;
		css_length				m_css_min_width;
		css_length				m_css_min_height;
		css_length				m_css_max_width;
		css_length				m_css_max_height;
		css_offsets				m_css_offsets;
		css_length				m_css_text_indent;

		overflow				m_overflow;
		visibility				m_visibility;
		/* Own CSS 'opacity' (0..1, default 1) and the cumulative product with
		 * every ancestor's opacity, resolved top-down in parse_styles. The
		 * cumulative value drives drawing: ~0 skips the subtree entirely, an
		 * intermediate value scales background/border alpha. */
		float					m_opacity;
		float					m_opacity_cum;
		int						m_z_index;
		/* True when 'z-index' is the keyword auto (or absent). Only an explicit
		 * integer z-index on a positioned box / flex-grid item turns it into a
		 * stacking context; z-index:auto leaves it transparent so its z-indexed
		 * descendants bubble to the nearest real stacking context. */
		bool						m_z_index_auto;
		box_sizing				m_box_sizing;

		int_int_cache			m_cahe_line_left;
		int_int_cache			m_cahe_line_right;

		// data for table rendering
		std::unique_ptr<table_grid>	m_grid;
		css_length				m_css_border_spacing_x;
		css_length				m_css_border_spacing_y;
		int						m_border_spacing_x;
		int						m_border_spacing_y;
		border_collapse			m_border_collapse;
		void					init_font(const tchar_t* own_font_size, const tchar_t* own_name, const tchar_t* own_weight, const tchar_t* own_style, const tchar_t* own_decoration);
		void					resolve_custom_properties();
		void					expand_css_functions();

		virtual void			select_all(const css_selector& selector, elements_vector& res);

	public:
		html_tag(litehtml::document* doc);
		virtual ~html_tag();

		/* render functions */

		virtual int					render(int x, int y, int max_width, bool second_pass = false) override;

		virtual int					render_inline(const element::ptr &container, int max_width) override;
		virtual int					place_element(const element::ptr &el, int max_width) override;
		virtual bool				fetch_positioned() override;
		virtual void				render_positioned(render_type rt = render_all) override;

		int							new_box(const element::ptr &el, int max_width, line_context& line_ctx);

		int							get_cleared_top(const element::ptr &el, int line_top) const;
		int							finish_last_box(bool end_of_render = false);

		virtual bool				appendChild(const element::ptr &el) override;
		virtual bool				removeChild(const element::ptr &el) override;
		virtual bool				insertBefore(const element::ptr &el, const element::ptr &ref) override;
		virtual element::ptr		clone_node(bool deep) override;
		virtual void				clearRecursive() override;
		virtual const tchar_t*		get_tagName() const override;
		virtual bool				is_html_tag() const override;
		virtual void				set_tagName(const tchar_t* tag) override;
		virtual void				set_data(const tchar_t* data) override;
		virtual element_float		get_float() const override;
		virtual vertical_align		get_vertical_align() const override;
		virtual css_length			get_css_left() const override;
		virtual css_length			get_css_right() const override;
		virtual css_length			get_css_top() const override;
		virtual css_length			get_css_bottom() const override;
		virtual css_length			get_css_width() const override;
		virtual css_offsets			get_css_offsets() const override;
		virtual void				set_css_width(css_length& w) override;
		virtual css_length			get_css_height() const override;
		virtual element_clear		get_clear() const override;
		virtual size_t				get_children_count() const override;
		virtual element::ptr		get_child(int idx) const override;
		virtual const string_map*	get_custom_props() const override { return &m_custom_props; }
		virtual element_position	get_element_position(css_offsets* offsets = 0) const override;
		virtual overflow			get_overflow() const override;
		box_sizing				get_box_sizing() const { return m_box_sizing; }
		/* Automatic margins resolve against free space at pack time; they must
		 * contribute 0 to intrinsic (min/max-content) sizes, so callers computing
		 * those need to tell an auto margin apart from a resolved length. */
		bool					margin_left_is_auto() const { return m_css_margins.left.is_predefined(); }
		bool					margin_right_is_auto() const { return m_css_margins.right.is_predefined(); }

		virtual void				set_attr(const tchar_t* name, const tchar_t* val) override;
		virtual const tchar_t*		get_attr(const tchar_t* name, const tchar_t* def = 0) override;
		virtual const string_map*	eweb_attrs() override { return &m_attrs; }
		virtual void				remove_attr(const tchar_t* name) override;
		virtual void				apply_stylesheet(const litehtml::css& stylesheet) override;
		/* Own-element stylesheet matching without the children walk; used by
		 * apply_stylesheet to make the chunked style update resumable. */
		void						apply_stylesheet_own(const litehtml::css& stylesheet);
		/* Full own-element re-cascade (master sheet, presentation attributes,
		 * document sheet) from a clean slate: clears m_style and the used-style
		 * list, drops pseudo elements so the new pass recreates them, then
		 * re-matches. Used by document::style_detached_subtree for nodes that
		 * were styled while detached - ancestor-dependent selectors could not
		 * match then, so re-attachment must re-run the cascade. */
		void						reapply_style_cascade(const litehtml::css& master, const litehtml::css& doc_css);
		virtual void				refresh_styles() override;

		virtual bool				is_white_space() const override;
		virtual bool				is_body() const override;
		virtual bool				is_break() const override;
		virtual int					get_base_line() override;
		virtual bool				on_mouse_over() override;
		virtual bool				on_mouse_leave() override;
		virtual bool				on_lbutton_down() override;
		virtual bool				on_lbutton_up() override;
		virtual void				on_click() override;
		virtual bool				find_styles_changes(position::vector& redraw_boxes, int x, int y) override;
		virtual const tchar_t*		get_cursor() override;
		virtual void				init_font() override;
		virtual bool				set_pseudo_class(const tchar_t* pclass, bool add) override;
		/* Read-only view of the runtime pseudo-class list. Used by the
		 * :focus-within matcher to walk ancestors looking for a live "focus"
		 * entry without needing friend access to m_pseudo_classes. */
		const string_vector&			pseudo_classes() const { return m_pseudo_classes; }
		virtual bool				set_class(const tchar_t* pclass, bool add) override;
		virtual bool				is_replaced() const override;
		virtual int					line_height() const override;
		virtual bool				is_line_height_normal() const override;
		virtual text_align			get_text_align() const override;
		virtual text_transform		get_text_transform() const override;
		virtual white_space			get_white_space() const override;
		virtual style_display		get_display() const override;
	void				set_display(style_display d) override { m_display = d; }
		virtual visibility			get_visibility() const override;
		virtual float				get_opacity_cum() const override { return m_opacity_cum; }
		virtual void				parse_styles(bool is_reparse = false) override;
		/* ---- CSS animation overrides (Phase 2) ----
		 * When a transition or @keyframes animation is running on this
		 * element, document::tick_animations writes the interpolated value
		 * into m_anim_overrides (keyed by lowercase property name, value is
		 * the raw CSS string form). parse_styles consults the override map
		 * AFTER the normal cascade so animated values win over authored
		 * styles without the animation having to rewrite the stylesheet.
		 * Cleared when the animation ends without fill-mode:forwards, or
		 * when the element is detached. */
		void						set_anim_override(const tchar_t* prop, const tchar_t* value);
		void						clear_anim_override(const tchar_t* prop);
		void						clear_anim_overrides();
		bool						has_anim_overrides() const { return !m_anim_overrides.empty(); }
		const tchar_t*				anim_override(const tchar_t* prop) const;
		/* Apply an animated opacity value directly (bypasses parse_styles).
		 * Sets m_opacity, recomputes m_opacity_cum, and propagates the new
		 * cumulative value down the subtree so descendants stay correct.
		 * Called by document::tick_animations every frame. */
		void						set_animated_opacity(float op);
		/* Apply an animated transform string directly (bypasses parse_styles).
		 * Stores the override and re-parses it into m_transform so the next
		 * draw_background composes the new paint matrix. Called by
		 * document::tick_animations every frame for transform animations. */
		void						set_animated_transform(const tchar_t* val);
		/* Phase 3.1 relayout guard: true when the subtree rooted at this element
		 * has <= limit nodes. The walk is bounded by `limit` so the check never
		 * costs more than the per-frame relayout budget it protects. */
		bool						subtree_within_budget(int limit) const;
		virtual void				draw(uint_ptr hdc, int x, int y, const position* clip) override;
		virtual void				draw_background(uint_ptr hdc, int x, int y, const position* clip) override;
		/* CSS transform subset (rotate/translate*): parse into m_transform and
		 * compose the device-space paint matrix for `box` (origin = box center,
		 * translate percentages against the box size). False = identity. */
		void						parse_transform_list(const tchar_t* val);
		bool						compute_transform_matrix(const position& box, float m[6]) const;

		virtual const tchar_t*		get_style_property_own(const tchar_t* name) const override;
		virtual const tchar_t*		get_style_property(const tchar_t* name, bool inherited, const tchar_t* def = 0) override;
		virtual uint_ptr			get_font(font_metrics* fm = 0) override;
		virtual int					get_font_size() const override;

		elements_vector&			children();
		virtual void				calc_outlines(int parent_width) override;
		virtual void				calc_auto_margins(int parent_width) override;

		virtual int					select(const css_selector& selector, bool apply_pseudo = true) override;
		virtual int					select(const css_element_selector& selector, bool apply_pseudo = true) override;

		virtual elements_vector		select_all(const tstring& selector) override;
		virtual elements_vector		select_all(const css_selector& selector) override;

		virtual element::ptr		select_one(const tstring& selector) override;
		virtual element::ptr		select_one(const css_selector& selector) override;

		virtual element::ptr		find_ancestor(const css_selector& selector, bool apply_pseudo = true, bool* is_pseudo = 0) override;
		virtual element::ptr		find_adjacent_sibling(const element::ptr& el, const css_selector& selector, bool apply_pseudo = true, bool* is_pseudo = 0) override;
		virtual element::ptr		find_sibling(const element::ptr& el, const css_selector& selector, bool apply_pseudo = true, bool* is_pseudo = 0) override;
		virtual void				get_text(tstring& text) override;
		virtual void				parse_attributes() override;

		virtual bool				is_first_child_inline(const element::ptr& el) const override;
		virtual bool				is_last_child_inline(const element::ptr& el) override;
		virtual bool				have_inline_child() const override;
		virtual void				get_content_size(size& sz, int max_width) override;
		virtual void				init() override;
		virtual void				get_inline_boxes(position::vector& boxes) override;
		virtual bool				is_floats_holder() const override;
		virtual int					get_floats_height(element_float el_float = float_none) const override;
		virtual int					get_left_floats_height() const override;
		virtual int					get_right_floats_height() const override;
		virtual int					get_line_left(int y) override;
		virtual int					get_line_right(int y, int def_right) override;
		virtual void				get_line_left_right(int y, int def_right, int& ln_left, int& ln_right) override;
		virtual void				add_float(const element::ptr &el, int x, int y) override;
		virtual void				update_floats(int dy, const element::ptr &parent) override;
		virtual void				add_positioned(const element::ptr &el) override;
		virtual bool				is_stacking_participant() const override;
		virtual bool				is_stacking_context() const override;
		virtual int					find_next_line_top(int top, int width, int def_right) override;
		virtual void				apply_vertical_align() override;
		virtual void				draw_children(uint_ptr hdc, int x, int y, const position* clip, draw_flag flag, int zindex) override;
		virtual int					get_zindex() const override;
		virtual void				draw_stacking_context(uint_ptr hdc, int x, int y, const position* clip, bool with_positioned) override;
		virtual void				calc_document_size(litehtml::size& sz, int x = 0, int y = 0) override;
		virtual void				get_redraw_box(litehtml::position& pos, int x = 0, int y = 0) override;
		virtual void				add_style(const litehtml::style& st) override;
		virtual element::ptr		get_element_by_point(int x, int y, int client_x, int client_y) override;
		virtual element::ptr		get_child_by_point(int x, int y, int client_x, int client_y, draw_flag flag, int zindex) override;

		virtual bool				is_nth_child(const element::ptr& el, int num, int off, bool of_type) const override;
		virtual bool				is_nth_last_child(const element::ptr& el, int num, int off, bool of_type) const override;
		virtual bool				is_only_child(const element::ptr& el, bool of_type) const override;
		virtual const background*	get_background(bool own_only = false) override;

	protected:
		void						draw_children_box(uint_ptr hdc, int x, int y, const position* clip, draw_flag flag, int zindex);
		void						draw_children_table(uint_ptr hdc, int x, int y, const position* clip, draw_flag flag, int zindex);
		/* Recompute m_opacity_cum from the parent's cum and own m_opacity,
		 * then recurse into children. Called by set_animated_opacity. */
		void						propagate_opacity_cum();
		int							render_box(int x, int y, int max_width, bool second_pass = false);
		int											render_flex(int x, int y, int max_width, bool second_pass = false);
		int											render_grid(int x, int y, int max_width, bool second_pass = false);
		int							render_table(int x, int y, int max_width, bool second_pass = false);
		int							fix_line_width(int max_width, element_float flt);
		void						parse_background();
		void						clear_style_property_cache() const;
		void						init_background_paint( position pos, background_paint &bg_paint, const background* bg );
		void						draw_list_marker( uint_ptr hdc, const position &pos );
		void						parse_nth_child_params( tstring param, int &num, int &off );
		void						remove_before_after();
		litehtml::element::ptr		get_element_before();
		litehtml::element::ptr		get_element_after();
	};

	/************************************************************************/
	/*                        Inline Functions                              */
	/************************************************************************/

	inline elements_vector& litehtml::html_tag::children()
	{
		return m_children;
	}
}
