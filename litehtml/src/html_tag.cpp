#include "html.h"
#include "html_tag.h"
#include "document.h"
#include "el_text.h"
#include "iterators.h"
#include "stylesheet.h"
#include "table.h"
#include <algorithm>
#include <locale>
#include <new>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "el_before_after.h"

/* Opacity support ---------------------------------------------------------
 * litehtml historically ignored CSS 'opacity' entirely, so Material-style
 * state layers (::before/::after painted with opacity:0 until hovered) drew
 * as solid coloured pills/boxes over buttons and inputs. Cumulative opacity
 * is resolved in parse_styles (html_tag::m_opacity_cum); these helpers apply
 * it at draw time: a subtree whose cumulative opacity rounds to zero paints
 * nothing, an intermediate value scales the alpha of backgrounds/borders. */
static const float OPACITY_EPS = 0.004f;

static inline bool opacity_hidden(float cum)
{
	return cum <= OPACITY_EPS;
}

static inline int opacity_scale_alpha(int a, float cum)
{
	if(cum >= 1.0f)
	{
		return a;
	}
	int v = (int)((float)a * cum + 0.5f);
	if(v < 0) v = 0;
	if(v > 255) v = 255;
	return v;
}

/*
 * O(1), non-dereferencing heap-membership test provided by the EwokOS libc
 * (libgloss/compat.c). Used to validate m_children slots before litehtml
 * dereferences them - see child_slot_sane() below.
 */
extern "C" int ewok_ptr_in_heap(const void* p);

#ifdef LITEHTML_LIFETIME_DEBUG
/* Defined next to selector_key_lower below; declared here because the
 * html_tag ctor/dtor register into the lifetime set. */
void litehtml_lifetime_add(const void* p);
void litehtml_lifetime_remove(const void* p);
bool litehtml_tag_is_live(const void* p);
size_t litehtml_lifetime_count();
#endif

namespace litehtml {
void profile_apply_stylesheet(uint32_t selector_count, uint64_t start_ms);
void profile_select(uint64_t start_ms);
void profile_select_element(uint64_t start_ms);
void profile_get_style_property(bool cache_hit, uint32_t parent_steps, uint64_t start_ms);
void profile_init_font(bool inherit_fast, uint64_t start_ms);
}

namespace {

static inline bool font_metrics_ptr_writable(litehtml::font_metrics* fm)
{
	if(!fm)
	{
		return false;
	}
	uintptr_t addr = (uintptr_t)fm;
	return addr >= 0x180000;
}

static inline void css_length_set_zero(litehtml::css_length& value)
{
	value.set_value(0, litehtml::css_units_px);
}

static inline void css_length_set_predef0(litehtml::css_length& value)
{
	value.predef(0);
}

static inline litehtml::tchar_t ascii_tolower_char(litehtml::tchar_t ch)
{
	return (ch >= 'A' && ch <= 'Z') ? (ch - 'A' + 'a') : ch;
}

struct own_style_refs
{
	litehtml::tstring font_size;
	litehtml::tstring font_family;
	litehtml::tstring font_weight;
	litehtml::tstring font_style;
	litehtml::tstring text_decoration;
	litehtml::tstring position;
	litehtml::tstring overflow;
	litehtml::tstring display;
	litehtml::tstring box_sizing;
	litehtml::tstring text_align;
	litehtml::tstring text_transform;
	litehtml::tstring white_space;
	litehtml::tstring visibility;
	litehtml::tstring z_index;
	litehtml::tstring vertical_align;
	litehtml::tstring float_value;
	litehtml::tstring clear_value;
};

static inline const litehtml::tchar_t* own_style_ref_ptr(const litehtml::tstring& value)
{
	return value.empty() ? nullptr : value.c_str();
}

static own_style_refs collect_own_style_refs(const litehtml::style& style)
{
	own_style_refs refs;
	if(style.empty())
	{
		return refs;
	}

	const litehtml::props_map& props = style.properties();
	for(litehtml::props_map::const_iterator it = props.begin(); it != props.end(); ++it)
	{
		const litehtml::tchar_t* name = it->first.c_str();
		const litehtml::tstring& value = it->second.m_value;
		switch(name[0])
		{
		case 'b':
			if(!t_strcmp(name, _t("box-sizing"))) refs.box_sizing = value;
			break;
		case 'c':
			if(!t_strcmp(name, _t("clear"))) refs.clear_value = value;
			break;
		case 'd':
			if(!t_strcmp(name, _t("display"))) refs.display = value;
			break;
		case 'f':
			if(!t_strcmp(name, _t("font-size"))) refs.font_size = value;
			else if(!t_strcmp(name, _t("font-family"))) refs.font_family = value;
			else if(!t_strcmp(name, _t("font-weight"))) refs.font_weight = value;
			else if(!t_strcmp(name, _t("font-style"))) refs.font_style = value;
			else if(!t_strcmp(name, _t("float"))) refs.float_value = value;
			break;
		case 'o':
			if(!t_strcmp(name, _t("overflow"))) refs.overflow = value;
			break;
		case 'p':
			if(!t_strcmp(name, _t("position"))) refs.position = value;
			break;
		case 't':
			if(!t_strcmp(name, _t("text-align"))) refs.text_align = value;
			else if(!t_strcmp(name, _t("text-transform"))) refs.text_transform = value;
			else if(!t_strcmp(name, _t("text-decoration"))) refs.text_decoration = value;
			break;
		case 'v':
			if(!t_strcmp(name, _t("visibility"))) refs.visibility = value;
			else if(!t_strcmp(name, _t("vertical-align"))) refs.vertical_align = value;
			break;
		case 'w':
			if(!t_strcmp(name, _t("white-space"))) refs.white_space = value;
			break;
		case 'z':
			if(!t_strcmp(name, _t("z-index"))) refs.z_index = value;
			break;
		}
	}

	return refs;
}

static bool selector_has_before_after(const litehtml::css_selector::ptr& sel)
{
	for(litehtml::css_attribute_selector::vector::const_iterator it = sel->m_right.m_attrs.begin(); it != sel->m_right.m_attrs.end(); ++it)
	{
		if(it->condition == litehtml::select_pseudo_element && (it->val == _t("before") || it->val == _t("after")))
		{
			return true;
		}
	}
	return false;
}

static bool used_styles_have_before_after(const litehtml::used_selector::vector& used_styles)
{
	for(litehtml::used_selector::vector::const_iterator it = used_styles.begin(); it != used_styles.end(); ++it)
	{
		if(selector_has_before_after(it->m_selector))
		{
			return true;
		}
	}
	return false;
}

struct parse_style_profile_t
{
	uint32_t nodes;
	uint32_t inline_style_ms;
	uint32_t init_font_ms;
	uint32_t basic_ms;
	uint32_t flow_ms;
	uint32_t size_ms;
	uint32_t box_ms;
	uint32_t line_list_ms;
	uint32_t background_ms;
	uint32_t child_ms;
};

static bool g_parse_style_profile_active = false;
static parse_style_profile_t g_parse_style_profile = {};
static const bool g_parse_style_profile_supported = true;

static uint32_t g_apply_calls = 0;
static uint32_t g_apply_cand_ms = 0;
static uint32_t g_apply_match_ms = 0;
static uint32_t g_apply_add_ms = 0;
static uint32_t g_apply_recur_ms = 0;

static inline bool parse_style_profile_enabled()
{
	return g_parse_style_profile_active;
}

static inline void parse_style_profile_add(uint32_t& slot, uint64_t start_ms)
{
	if(g_parse_style_profile_active)
	{
		slot += (uint32_t)(sys_tic_ms(0) - start_ms);
	}
}

struct select_scope_t
{
	uint64_t start_ms;
	select_scope_t(): start_ms(sys_tic_ms(0)) {}
	~select_scope_t() { litehtml::profile_select(start_ms); }
};

struct select_element_scope_t
{
	uint64_t start_ms;
	select_element_scope_t(): start_ms(sys_tic_ms(0)) {}
	~select_element_scope_t() { litehtml::profile_select_element(start_ms); }
};

}

namespace litehtml {

void reset_parse_style_profile()
{
	g_parse_style_profile_active = g_parse_style_profile_supported;
	memset(&g_parse_style_profile, 0, sizeof(g_parse_style_profile));
}

void dump_parse_style_profile()
{
	if(!g_parse_style_profile_active)
	{
		return;
	}
	g_parse_style_profile_active = false;
}

}

litehtml::html_tag::html_tag(litehtml::document* doc) : litehtml::element(doc)
{
#ifdef LITEHTML_LIFETIME_DEBUG
	litehtml_lifetime_add(this);
#endif
	m_box_sizing			= box_sizing_content_box;
	m_z_index				= 0;
	m_z_index_auto			= true;
	m_isolate				= false;
	m_has_css_clip			= false;
	m_overflow				= overflow_visible;
	m_box					= 0;
	m_text_align			= text_align_left;
	m_text_transform		= text_transform_none;
	m_el_position			= element_position_static;
	m_display				= display_inline;
	m_vertical_align		= va_baseline;
	m_list_style_type		= list_style_type_none;
	m_list_style_position	= list_style_position_outside;
	m_float					= float_none;
	m_clear					= clear_none;
	m_font					= 0;
	m_font_size				= 0;
	m_white_space			= white_space_normal;
	m_text_wrap_balance		= false;
	m_balanced_line_width	= 0;
	m_lh_predefined			= false;
	m_lh_factor				= 0.0f;
	m_line_height			= 0;
	m_visibility			= visibility_visible;
	m_opacity				= 1.0f;
	m_opacity_cum			= 1.0f;
	m_border_spacing_x		= 0;
	m_border_spacing_y		= 0;
	m_border_collapse		= border_collapse_separate;
}

litehtml::html_tag::~html_tag()
{
#ifdef LITEHTML_LIFETIME_DEBUG
	litehtml_lifetime_remove(this);
#endif
	/* Remove any running animations targeting this element so the document
	 * timeline never holds a dangling pointer after the element is freed. */
	{
		document* doc = get_document();
		if(doc) doc->clear_animations_for(this);
	}
	// Clear parent reference for all children before deleting them
	// to prevent any issues with dangling parent pointers
	for(auto& child : m_children)
	{
		if(child)
		{
			child->parent(nullptr);
		}
	}
	// Delete all children to prevent memory leaks
	for(auto& child : m_children)
	{
		if(child)
		{
			delete child;
		}
	}
	m_children.clear();
}

/* A script-inserted text node (document.createTextNode, textContent, the
 * string form of append()) is one unbreakable run: the line packer only
 * breaks between nodes, so a Chinese sentence would never wrap. Split it
 * into kinsoku-aware per-character chunks (split_cjk_text); the inserted
 * node keeps the first chunk so caller handles stay valid, the rest go in
 * as siblings at the same position. Non-text nodes pass through. */
static void cjk_split_insert(litehtml::document* doc, const litehtml::element::ptr& el,
                             std::vector<litehtml::element::ptr>& nodes)
{
	nodes.clear();
	if(!el)
	{
		return;
	}
	if(el->get_display() != litehtml::display_inline_text || el->is_white_space())
	{
		nodes.push_back(el);
		return;
	}
	litehtml::tstring txt;
	el->get_text(txt);
	std::vector<litehtml::tstring> chunks;
	litehtml::split_cjk_text(txt, chunks);
	if(chunks.size() <= 1)
	{
		nodes.push_back(el);
		return;
	}
	static_cast<litehtml::el_text*>(el)->set_text(chunks[0]);
	nodes.push_back(el);
	for(size_t i = 1; i < chunks.size(); i++)
	{
		void* mem = malloc(sizeof(litehtml::el_text));
		if(!mem)
		{
			break;
		}
		litehtml::element::ptr t = new (mem) litehtml::el_text(chunks[i].c_str(), doc);
		nodes.push_back(t);
	}
}

static void parent_and_style_text(litehtml::element* parent, litehtml::element::ptr t)
{
	t->parent(parent);
	if(t->get_display() == litehtml::display_inline_text)
	{
		t->parse_styles(false);
	}
}

/* display:contents keeps its DOM node and relationships, but contributes no
 * layout box. Recursively expose its children to the surrounding formatting
 * context without mutating the DOM tree (hydrators depend on childNodes). */
static void collect_box_children(litehtml::element::ptr parent, litehtml::elements_vector& out)
{
	if(!parent) return;
	for(size_t i = 0; i < parent->get_children_count(); i++)
	{
		litehtml::element::ptr child = parent->get_child((int)i);
		if(!child) continue;
		if(child->get_display() == litehtml::display_contents)
			collect_box_children(child, out);
		else
			out.push_back(child);
	}
}

bool litehtml::html_tag::appendChild(const element::ptr &el)
{
	if(el)
	{
		/* DOM appendChild moves an attached node to the new final position. Keep
		 * the object alive while unlinking it; removeChild never deletes. */
		element::ptr old_parent = el->parent();
		if(old_parent)
		{
			old_parent->removeChild(el);
		}
		std::vector<element::ptr> nodes;
		cjk_split_insert(m_doc, el, nodes);
		for(size_t i = 0; i < nodes.size(); i++)
		{
			parent_and_style_text(this, nodes[i]);
			m_children.push_back(nodes[i]);
		}
		return true;
	}
	return false;
}

litehtml::element::ptr litehtml::html_tag::clone_node(bool deep)
{
	/* Recreate the same tag with the same attributes through the document factory
	 * (which picks the right element subclass), then deep-copy the children. The
	 * clone is unstyled and detached; appendChild + style_detached_subtree on the
	 * engine side match the styles once it is spliced into the live tree. */
	element::ptr dst = m_doc->create_element(m_tag.c_str(), m_attrs);
	if(dst && deep)
	{
		for(auto& c : m_children)
		{
			if(!c) continue;
			element::ptr cc = c->clone_node(true);
			if(cc) dst->appendChild(cc);
		}
	}
	return dst;
}

bool litehtml::html_tag::removeChild(const element::ptr &el)
{
	if(el && el->parent() == this)
	{
		el->parent(nullptr);
		m_children.erase(std::remove(m_children.begin(), m_children.end(), el), m_children.end());
		return true;
	}
	return false;
}

bool litehtml::html_tag::insertBefore(const element::ptr &el, const element::ptr &ref)
{
	if(!el)
	{
		return false;
	}
	/* DOM insertBefore moves an already-attached node instead of cloning it, so
	 * detach first. removeChild only unlinks (it never deletes), which keeps
	 * `el` alive across the move. Do this before locating `ref`: when `ref` is
	 * `el`'s next sibling, removing `el` shifts its index. */
	element::ptr old_parent = el->parent();
	if(old_parent)
	{
		old_parent->removeChild(el);
	}
	std::vector<element::ptr> nodes;
	cjk_split_insert(m_doc, el, nodes);
	if(ref && ref->parent() == this)
	{
		for(size_t i = 0; i < m_children.size(); i++)
		{
			if(m_children[i] == ref)
			{
				for(size_t k = 0; k < nodes.size(); k++)
				{
					parent_and_style_text(this, nodes[k]);
					m_children.insert(m_children.begin() + i + k, nodes[k]);
				}
				return true;
			}
		}
	}
	/* No ref, or a ref that is not our child: append, exactly like the DOM. */
	for(size_t k = 0; k < nodes.size(); k++)
	{
		parent_and_style_text(this, nodes[k]);
		m_children.push_back(nodes[k]);
	}
	return true;
}

void litehtml::html_tag::clearRecursive()
{
	for(auto& el : m_children)
	{
		el->clearRecursive();
		el->parent(nullptr);
	}
	m_children.clear();
}


const litehtml::tchar_t* litehtml::html_tag::get_tagName() const
{
	return m_tag.c_str();
}

bool litehtml::html_tag::is_html_tag() const
{
	return true;
}

void litehtml::html_tag::set_attr( const tchar_t* name, const tchar_t* val )
{
	if(name && val)
	{
		clear_style_property_cache();
		tstring s_val = name;
		for(size_t i = 0; i < s_val.length(); i++)
		{
			s_val[i] = ascii_tolower_char(s_val[i]);
		}
		m_attrs[s_val] = val;

		if( t_strcasecmp( name, _t("class") ) == 0 )
		{
			m_class_values.resize( 0 );
			split_string( val, m_class_values, _t(" ") );
		}
	}
}

const litehtml::tchar_t* litehtml::html_tag::get_attr( const tchar_t* name, const tchar_t* def )
{
	/* set_attr() stores every key lower-cased and remove_attr() erases by the
	 * lower-cased key, so the lookup has to be folded too. Without it the two
	 * halves of a mixed-case round trip disagree:
	 *     setAttribute("readOnly", "x"); getAttribute("readOnly")  -> null
	 * which also breaks the DOM's rule that getAttribute is ASCII
	 * case-insensitive on an HTML element, so getAttribute("DATA-X") missed an
	 * attribute the parser had stored as "data-x". m_attrs is written only by
	 * set_attr(), so no stored key can have an upper-case letter in it. */
	if(!name)
	{
		return def;
	}
	/* Fast path: nearly every caller passes an already-lower-case literal
	 * ("href", "rel", "colspan", ...) and so does the CSS parser, and this runs
	 * per element per selector during matching. Only pay for the copy when the
	 * name actually carries an upper-case letter. */
	const tchar_t* key = name;
	tstring s_name;
	for(const tchar_t* p = name; *p; ++p)
	{
		if(ascii_tolower_char(*p) != *p)
		{
			s_name = name;
			for(size_t i = 0; i < s_name.length(); i++)
			{
				s_name[i] = ascii_tolower_char(s_name[i]);
			}
			key = s_name.c_str();
			break;
		}
	}
	string_map::const_iterator attr = m_attrs.find(key);
	if(attr != m_attrs.end())
	{
		return attr->second.c_str();
	}
	return def;
}

void litehtml::html_tag::remove_attr( const tchar_t* name )
{
	if(!name)
	{
		return;
	}
	/* Mirror set_attr's key normalisation: attribute names are stored
	 * lower-cased, so erasing the raw name would miss every HTML attribute
	 * parsed from source with upper-case letters in it. */
	tstring s_val = name;
	for(size_t i = 0; i < s_val.length(); i++)
	{
		s_val[i] = ascii_tolower_char(s_val[i]);
	}
	if(m_attrs.erase(s_val) == 0)
	{
		return;
	}
	clear_style_property_cache();
	/* Dropping class= must also drop the parsed class list, otherwise the
	 * element keeps matching .foo selectors it no longer carries. */
	if( t_strcasecmp( name, _t("class") ) == 0 )
	{
		m_class_values.resize( 0 );
	}
}

litehtml::elements_vector litehtml::html_tag::select_all( const tstring& selector )
{
	css_selector sel(media_query_list::ptr(0));
	sel.parse(selector);
	
	return select_all(sel);
}

litehtml::elements_vector litehtml::html_tag::select_all( const css_selector& selector )
{
	litehtml::elements_vector res;
	select_all(selector, res);
	return res;
}

void litehtml::html_tag::select_all(const css_selector& selector, elements_vector& res)
{
	if(select(selector))
	{
		res.push_back(this);
	}
	
	for(auto& el : m_children)
	{
		el->select_all(selector, res);
	}
}


litehtml::element::ptr litehtml::html_tag::select_one( const tstring& selector )
{
	css_selector sel(media_query_list::ptr(0));
	sel.parse(selector);

	return select_one(sel);
}

litehtml::element::ptr litehtml::html_tag::select_one( const css_selector& selector )
{
	if(select(selector))
	{
		return this;
	}

	for(auto& el : m_children)
	{
		element::ptr res = el->select_one(selector);
		if(res)
		{
			return res;
		}
	}
	return 0;
}

static litehtml::tstring selector_key_lower(const litehtml::tstring& src)
{
	litehtml::tstring out = src;
	for(size_t i = 0; i < out.length(); i++)
	{
		if(out[i] >= 'A' && out[i] <= 'Z')
		{
			out[i] = (litehtml::tchar_t)(out[i] - 'A' + 'a');
		}
	}
	return out;
}

#ifdef LITEHTML_LIFETIME_DEBUG
/* Lifetime registry of every html_tag ever constructed and not yet destroyed.
 * The style walk consults it to catch dangling element pointers (raw-pointer
 * litehtml has no refcount, so a JS mutation that leaves a freed node in the
 * tree turns into a wild read of reused heap). Debug builds only.
 * Deliberately a raw malloc'd array: an STL container here would reference
 * operator new/delete, the linker would extract new_delete.o from
 * libewokstl.a, and that collides with libcxx.a's cxx.o at xBrowser link. */
#include <stdlib.h>
static const void** g_live_tags = nullptr;
static size_t g_live_tags_n = 0, g_live_tags_cap = 0;

void litehtml_lifetime_add(const void* p)
{
	if(g_live_tags_n == g_live_tags_cap)
	{
		size_t ncap = g_live_tags_cap ? g_live_tags_cap * 2 : 256;
		const void** n = (const void**)realloc(g_live_tags, ncap * sizeof(*n));
		if(!n) return;
		g_live_tags = n;
		g_live_tags_cap = ncap;
	}
	g_live_tags[g_live_tags_n++] = p;
}

void litehtml_lifetime_remove(const void* p)
{
	for(size_t i = 0; i < g_live_tags_n; i++)
	{
		if(g_live_tags[i] == p)
		{
			g_live_tags[i] = g_live_tags[--g_live_tags_n];
			return;
		}
	}
}

bool litehtml_tag_is_live(const void* p)
{
	for(size_t i = 0; i < g_live_tags_n; i++)
	{
		if(g_live_tags[i] == p) return true;
	}
	return false;
}

size_t litehtml_lifetime_count()
{
	return g_live_tags_n;
}
#endif

/* Dumps and clears the apply_stylesheet phase counters; called by
 * document::update_master_styles_step when a chunked update completes. */
void litehtml::dump_apply_phase_profile()
{
	g_apply_calls = 0;
	g_apply_cand_ms = 0;
	g_apply_match_ms = 0;
	g_apply_add_ms = 0;
	g_apply_recur_ms = 0;
}

/* Per-element stylesheet matching (apply_stylesheet_own) plus the children
 * walk and the chunked-update gating live in the wrapper below. */
void litehtml::html_tag::apply_stylesheet( const litehtml::css& stylesheet )
{
#ifdef LITEHTML_LIFETIME_DEBUG
	if(!litehtml_tag_is_live(this))
	{
		/* Freed html_tag still linked in the tree: prune it. */
		return;
	}
#endif
	document* doc = get_document();
	bool stepping = (doc && doc->style_step_phase() == 1);
	if(stepping)
	{
		if(m_step_stamp == doc->style_step_epoch())
		{
			/* This element's own matching already ran in the current chunked
			 * apply epoch. Its subtree may still hold unvisited nodes, so the
			 * children walk below must run even though self-work is skipped --
			 * unless the whole subtree already finished, in which case pruning
			 * here keeps a resumed chunk O(path length) instead of O(tree). */
			if(m_step_done)
			{
				return;
			}
		}
		else if(doc->style_step_exhausted())
		{
			/* Time slice used up before this element was reached: leave it
			 * unstamped so the resumed walk redoes it. */
			return;
		}
		else
		{
			apply_stylesheet_own(stylesheet);
			/* Chunked apply runs master CSS and the document sheets in a single
			 * walk: applying both here keeps one stamp epoch per update, so a
			 * resumed chunk prunes by stamp instead of redoing the master pass
			 * from the root every round. */
			const litehtml::css& doc_styles = doc->doc_styles();
			if(&doc_styles != &stylesheet)
			{
				apply_stylesheet_own(doc_styles);
			}
			doc->style_step_stamp(this);
		}
	}
	else
	{
		apply_stylesheet_own(stylesheet);
	}

	for(auto& el : m_children)
	{
		if(stepping && doc->style_step_exhausted())
		{
			break;
		}
		if(el->get_display() != display_inline_text)
		{
			el->apply_stylesheet(stylesheet);
		}
	}
	/* The whole subtree is covered unless the slice ran out mid-walk. */
	if(stepping && !doc->style_step_exhausted())
	{
		m_step_done = true;
	}
}

/* The pseudo-element part name of a selector (lowercase, no "::" prefix), or
 * an empty string when it carries none. Both style-dispatch sites use it to
 * route widget-part rules to element::add_widget_part_style. */
static litehtml::tstring selector_pseudo_element_val( const litehtml::css_selector::ptr& sel )
{
	if(!sel)
	{
		return litehtml::tstring();
	}
	for(const auto& attr : sel->m_right.m_attrs)
	{
		if(attr.condition == litehtml::select_pseudo_element)
		{
			return attr.val;
		}
	}
	return litehtml::tstring();
}

void litehtml::html_tag::apply_stylesheet_own( const litehtml::css& stylesheet )
{
#ifdef LITEHTML_LIFETIME_DEBUG
	/* Backstop for every caller (apply_stylesheet and the detached-subtree
	 * walk): never match styles against a freed html_tag. Reading m_class_values
	 * off a block the mario VM has recycled aborts in selector_key_lower. The
	 * registry lookup is non-dereferencing, so it is safe here. */
	if(!litehtml_tag_is_live(this))
	{
		return;
	}
#endif
	m_sheets_applied = true;
	if((const void*)&stylesheet == nullptr)
	{
		return;
	}
	uint64_t apply_start = sys_tic_ms(0);

	const tstring k_class_attr = _t("class");
	const tstring k_id_attr = _t("id");
	auto right_selector_maybe_matches = [&](const litehtml::css_selector::ptr& sel) -> bool
	{
		const css_element_selector& right = sel->m_right;
		if(!right.m_tag.empty() && right.m_tag != _t("*") && right.m_tag != m_tag)
		{
			return false;
		}

		const tchar_t* own_id = nullptr;
		bool own_id_loaded = false;

		for(const auto& attr_sel : right.m_attrs)
		{
			const bool is_class_attr = (attr_sel.attribute == k_class_attr);
			const bool is_id_attr = (attr_sel.attribute == k_id_attr);
			switch(attr_sel.condition)
			{
			case select_exists:
				if(is_class_attr)
				{
					if(m_class_values.empty())
					{
						return false;
					}
				}
				else if(is_id_attr)
				{
					if(!own_id_loaded)
					{
						own_id = get_attr(_t("id"));
						own_id_loaded = true;
					}
					if(!own_id)
					{
						return false;
					}
				}
				break;
			case select_equal:
				if(is_class_attr)
				{
					bool found = true;
					for(const auto& cls : attr_sel.class_val)
					{
						bool matched = false;
						for(const auto& own_cls : m_class_values)
						{
							if(!t_strcasecmp(cls.c_str(), own_cls.c_str()))
							{
								matched = true;
								break;
							}
						}
						if(!matched)
						{
							found = false;
							break;
						}
					}
					if(!found)
					{
						return false;
					}
				}
				else if(is_id_attr)
				{
					if(!own_id_loaded)
					{
						own_id = get_attr(_t("id"));
						own_id_loaded = true;
					}
					if(!own_id || t_strcasecmp(attr_sel.val.c_str(), own_id))
					{
						return false;
					}
				}
				break;
			case select_contain_str:
				if(is_id_attr)
				{
					if(!own_id_loaded)
					{
						own_id = get_attr(_t("id"));
						own_id_loaded = true;
					}
					if(!own_id || !t_strstr(own_id, attr_sel.val.c_str()))
					{
						return false;
					}
				}
				break;
			case select_start_str:
				if(is_id_attr)
				{
					if(!own_id_loaded)
					{
						own_id = get_attr(_t("id"));
						own_id_loaded = true;
					}
					if(!own_id || t_strncmp(own_id, attr_sel.val.c_str(), attr_sel.val.length()))
					{
						return false;
					}
				}
				break;
			case select_end_str:
				if(is_id_attr)
				{
					if(!own_id_loaded)
					{
						own_id = get_attr(_t("id"));
						own_id_loaded = true;
					}
					if(!own_id)
					{
						return false;
					}
					size_t attr_len = t_strlen(own_id);
					size_t val_len = attr_sel.val.length();
					if(attr_len < val_len || t_strcasecmp(own_id + attr_len - val_len, attr_sel.val.c_str()))
					{
						return false;
					}
				}
				break;
			case select_pseudo_element:
			case select_pseudo_class:
				break;
			default:
				break;
			}
		}
		return true;
	};

	css::selector_index& sel_idx = stylesheet.get_selector_index();
	std::vector<int> candidates;
	uint64_t cand_start = sys_tic_ms(0);
	{
		uint32_t epoch = sel_idx.begin_visit();
		auto push_bucket = [&](const css::selector_index::key_t& key)
		{
			std::vector<int>* bucket = css::find_selector_bucket(sel_idx, key);
			if(!bucket)
			{
				return;
			}
			for(size_t j = 0; j < bucket->size(); j++)
			{
				int si = (*bucket)[j];
				if(sel_idx.stamps[si] != epoch)
				{
					sel_idx.stamps[si] = epoch;
					candidates.push_back(si);
				}
			}
		};
		for(size_t u = 0; u < sel_idx.universal.size(); u++)
		{
			int si = sel_idx.universal[u];
			if(sel_idx.stamps[si] != epoch)
			{
				sel_idx.stamps[si] = epoch;
				candidates.push_back(si);
			}
		}
		css::selector_index::key_t key;
		key.first = 3;
		key.second = selector_key_lower(m_tag);
		push_bucket(key);
		const tchar_t* own_id_attr = get_attr(k_id_attr.c_str());
		if(own_id_attr)
		{
			key.first = 2;
			key.second = selector_key_lower(tstring(own_id_attr));
			push_bucket(key);
		}

		for(size_t c = 0; c < m_class_values.size(); c++)
		{
			key.first = 1;
			key.second = selector_key_lower(m_class_values[c]);
			push_bucket(key);
		}
		std::sort(candidates.begin(), candidates.end());
	}
	g_apply_cand_ms += sys_tic_ms(0) - cand_start;
	g_apply_calls++;

	for(size_t ci = 0; ci < candidates.size(); ci++)
	{
		uint64_t match_start = sys_tic_ms(0);
		const litehtml::css_selector::ptr& sel = stylesheet.selectors()[candidates[ci]];
		if(!sel->is_media_valid())
		{
			continue;
		}
		if(!right_selector_maybe_matches(sel))
		{
			continue;
		}
		int apply = select(*sel, false);

		if(apply == select_no_match)
		{
			g_apply_match_ms += sys_tic_ms(0) - match_start;
			continue;
		}
		if(apply & select_match_pseudo_class)
		{
			apply = select(*sel, true);
			if(apply == select_no_match)
			{
				g_apply_match_ms += sys_tic_ms(0) - match_start;
				continue;
			}
		}
		g_apply_match_ms += sys_tic_ms(0) - match_start;
		uint64_t add_start = sys_tic_ms(0);

		if(apply != select_no_match)
		{
			used_selector* us = nullptr;
			auto ensure_used_style = [&]() -> used_selector&
			{
				if(!us)
				{
					m_used_styles.emplace_back(sel, false);
					us = &m_used_styles.back();
				}
				return *us;
			};

			{
				if(apply & select_match_with_after)
				{
					ensure_used_style();
					element::ptr el = get_element_after();
					if(el)
					{
						el->add_style(*sel->m_style, sel->m_specificity);
					}
				} else if(apply & select_match_with_before)
				{
					ensure_used_style();
					element::ptr el = get_element_before();
					if(el)
					{
						el->add_style(*sel->m_style, sel->m_specificity);
					}
				} else if(apply & select_match_with_widget)
				{
					/* Form-widget part rule: the selector targets a painted
					 * sub-part of a replaced control, not a tree node. */
					tstring part = selector_pseudo_element_val(sel);
					if(!part.empty())
					{
						ensure_used_style();
						add_widget_part_style(part, *sel->m_style);
					}
				} else
				{
					add_style(*sel->m_style, sel->m_specificity);
					ensure_used_style().m_used = true;
				}
			}
		}
		g_apply_add_ms += (uint32_t)(sys_tic_ms(0) - add_start);
	}

	/* The children walk and the per-root phase dump live in the
	 * apply_stylesheet wrapper; recursion timing is folded into it. */
	litehtml::profile_apply_stylesheet((uint32_t)stylesheet.selectors().size(), apply_start);
}

void litehtml::html_tag::reapply_style_cascade( const litehtml::css& master, const litehtml::css& doc_css )
{
#ifdef LITEHTML_LIFETIME_DEBUG
	/* Same backstop as apply_stylesheet_own: never match against a freed tag. */
	if(!litehtml_tag_is_live(this))
	{
		return;
	}
#endif
	clear_style_property_cache();
	/* A full cascade rebuild must discard generated boxes once, before the
	 * master and author sheets are accumulated. Removing them inside each
	 * stylesheet pass loses ::before/::after rules from every earlier sheet. */
	remove_before_after();
	/* Clean slate: the old cascade was computed with a different (or no)
	 * ancestor chain, so both the merged property bag and the matched-selector
	 * list are stale. apply_stylesheet_own rebuilds both; the inline style=
	 * attribute is not lost - parse_styles re-adds it after every cascade. */
	m_style.clear();
	m_used_styles.clear();
	/* Cascade order mirrors document creation: master sheet, attribute-derived
	 * properties, then document sheets, so author rules keep beating
	 * presentation attributes. */
	apply_stylesheet_own(master);
	parse_attributes();
	apply_stylesheet_own(doc_css);
}

void litehtml::html_tag::get_content_size( size& sz, int max_width )
{
	sz.height	= 0;
	if(m_display == display_block)
	{
		sz.width	= max_width;
	} else
	{
		sz.width	= 0;
	}
}

bool litehtml::html_tag::push_css_clip(uint_ptr hdc, int x, int y)
{
	if(!m_has_css_clip ||
	   (m_el_position != element_position_absolute && m_el_position != element_position_fixed))
		return false;

	position box = m_pos;
	box.x += x;
	box.y += y;
	box += m_padding;
	box += m_borders;
	int top = m_css_clip.top.is_predefined() ? 0 : m_css_clip.top.calc_percent(box.height);
	int right = m_css_clip.right.is_predefined() ? box.width : m_css_clip.right.calc_percent(box.width);
	int bottom = m_css_clip.bottom.is_predefined() ? box.height : m_css_clip.bottom.calc_percent(box.height);
	int left = m_css_clip.left.is_predefined() ? 0 : m_css_clip.left.calc_percent(box.width);
	position r(box.x + left, box.y + top,
		(right > left) ? right - left : 0,
		(bottom > top) ? bottom - top : 0);
	border_radiuses radius;
	get_document()->container()->set_clip(r, radius, true, true);
	(void)hdc;
	return true;
}

void litehtml::html_tag::draw( uint_ptr hdc, int x, int y, const position* clip )
{
	/* display:contents has no principal box; descendants are painted by the
	 * normal recursive child walk. */
	if(m_display == display_contents) return;
	/* opacity:0 (own or inherited from an ancestor) paints nothing. Layout is
	 * untouched - this only suppresses drawing of the element's own box. */
	if(opacity_hidden(m_opacity_cum))
	{
		return;
	}

	position pos = m_pos;
	pos.x	+= x;
	pos.y	+= y;
	bool css_clipped = push_css_clip(hdc, x, y);

	draw_background(hdc, x, y, clip);

	if(m_display == display_list_item && m_list_style_type != list_style_type_none)
	{
		if(m_overflow > overflow_visible)
		{
			position border_box = pos;
			border_box += m_padding;
			border_box += m_borders;

			border_radiuses bdr_radius = m_css_borders.radius.calc_percents(border_box.width, border_box.height);

			bdr_radius -= m_borders;
			bdr_radius -= m_padding;

			get_document()->container()->set_clip(pos, bdr_radius, true, true);
		}

		draw_list_marker(hdc, pos);

		if(m_overflow > overflow_visible)
		{
			get_document()->container()->del_clip();
		}
	}
	if(css_clipped)
		get_document()->container()->del_clip();
}

litehtml::uint_ptr litehtml::html_tag::get_font(font_metrics* fm)
{
	if(font_metrics_ptr_writable(fm))
	{
		*fm = m_font_metrics;
	}
	return m_font;
}

const litehtml::tchar_t* litehtml::html_tag::get_style_property( const tchar_t* name, bool inherited, const tchar_t* def /*= 0*/ )
{
	uint64_t start_ms = sys_tic_ms(0);
	if(!name)
	{
		litehtml::profile_get_style_property(false, 0, start_ms);
		return def;
	}

	tstring cache_key = inherited ? _t("i:") : _t("o:");
	cache_key += name;
	string_map::const_iterator cached = m_style_property_cache.find(cache_key);
	if(cached != m_style_property_cache.end())
	{
		litehtml::profile_get_style_property(true, 0, start_ms);
		return cached->second.c_str();
	}

	const tchar_t* ret = get_style_property_own(name);
	if(ret && t_strcasecmp(ret, _t("inherit")))
	{
		m_style_property_cache[cache_key] = ret;
		litehtml::profile_get_style_property(false, 0, start_ms);
		return ret;
	}

	element::ptr el_parent = parent();
	uint32_t parent_steps = 0;
	if(el_parent && (ret || inherited))
	{
		while(el_parent)
		{
			parent_steps++;
			ret = el_parent->get_style_property_own(name);
			if(ret)
			{
				if(t_strcasecmp(ret, _t("inherit")))
				{
					m_style_property_cache[cache_key] = ret;
					litehtml::profile_get_style_property(false, parent_steps, start_ms);
					return ret;
				}
			}
			else if(!inherited)
			{
				break;
			}
			el_parent = el_parent->parent();
		}
	}

	litehtml::profile_get_style_property(false, parent_steps, start_ms);
	return def;
}

const litehtml::tchar_t* litehtml::html_tag::get_style_property_own(const tchar_t* name) const
{
	/* Animation overrides (transition/@keyframes, Phase 3.1) win over the
	 * cascade so an animation-driven relayout picks up the interpolated value
	 * without rewriting the stylesheet. The map is empty except while an
	 * animation runs, so non-animated pages are byte-for-byte unaffected. */
	if(name && !m_anim_overrides.empty())
	{
		const tchar_t* ov = anim_override(name);
		if(ov) return ov;
	}
	return m_style.get_property(name);
}

/*---------------------------------------------------------------------------------
 Modern CSS value support: custom properties (var()) and the clamp()/min()/max()
 math functions. litehtml::style keeps property values as raw strings until
 parse_styles() consumes them, so both are expanded here, per element, right
 before consumption: custom properties resolve through the parent chain first,
 then every var()/clamp()/min()/max() occurrence in a value is rewritten to a
 plain px literal (or the declaration is dropped, as the spec requires for
 unresolvable var() references).
---------------------------------------------------------------------------------*/

namespace litehtml {

static bool expand_var_refs(const tstring& in, const string_map& vars, tstring& out, int depth)
{
	if(depth > 8)
	{
		return false;
	}
	out.clear();
	size_t i = 0;
	while(i < in.length())
	{
		size_t f = in.find(_t("var("), i);
		if(f == tstring::npos)
		{
			out += in.substr(i);
			break;
		}
		out += in.substr(i, f - i);
		int paren = 1;
		size_t j = f + 4;
		size_t comma = tstring::npos;
		while(j < in.length() && paren > 0)
		{
			tchar_t c = in[j];
			if(c == _t('('))
			{
				paren++;
			} else if(c == _t(')'))
			{
				paren--;
				if(paren == 0) break;
			} else if(c == _t(',') && paren == 1 && comma == tstring::npos)
			{
				comma = j;
			}
			j++;
		}
		if(j >= in.length())
		{
			return false;	// unbalanced parens
		}
		tstring name = in.substr(f + 4, (comma == tstring::npos ? j : comma) - (f + 4));
		trim(name);
		tstring sub;
		string_map::const_iterator it = vars.find(name);
		if(it != vars.end())
		{
			sub = it->second;
		} else if(comma != tstring::npos)
		{
			/* Fallbacks keep their whitespace in the spec, but property values
			 * in m_custom_props are stored trimmed, so trim here too to keep
			 * both sources interchangeable ("var(--x, 88px)"). */
			sub = in.substr(comma + 1, j - comma - 1);
			trim(sub);
		} else
		{
			return false;	// undefined custom property, no fallback
		}
		if(sub.find(_t("var(")) != tstring::npos)
		{
			tstring subexp;
			if(!expand_var_refs(sub, vars, subexp, depth + 1))
			{
				return false;
			}
			sub = subexp;
		}
		out += sub;
		i = j + 1;
	}
	return true;
}

/* Position of the innermost clamp(/min(/max( at or after 'from', or npos.
 * The greatest match position wins, so nested functions are evaluated
 * inside-out by the caller loop. */
static size_t find_math_func(const tstring& s, size_t from, tstring& fname)
{
	static const tchar_t* names[3] = { _t("clamp("), _t("min("), _t("max(") };
	size_t best = tstring::npos;
	for(int k = 0; k < 3; k++)
	{
		size_t p = s.find(names[k], from);
		while(p != tstring::npos && p > 0)
		{
			tchar_t prev = s[p - 1];
			if((prev >= _t('a') && prev <= _t('z')) || (prev >= _t('A') && prev <= _t('Z')) || prev == _t('-'))
			{
				p = s.find(names[k], p + 1);
				continue;
			}
			break;
		}
		if(p != tstring::npos && (best == tstring::npos || p > best))
		{
			best = p;
			fname = names[k];
		}
	}
	return best;
}

/* Evaluate a single length token (no operators, no nested function) to px.
 * Percent and predefined keywords are rejected: they need a containing block
 * this value-expansion pass does not have. */
static bool eval_one_length(const tstring& tok, document* doc, int font_size, int& out_px)
{
	tstring a = tok;
	trim(a);
	if(a.empty() || a.find(_t('(')) != tstring::npos)
	{
		return false;
	}
	css_length len;
	len.fromString(a.c_str());
	if(len.is_predefined() || len.units() == css_units_percentage)
	{
		return false;
	}
	out_px = doc->cvt_units(len, font_size);
	return true;
}

/* CSS2 clip applies a rectangular paint clip to positioned elements. Modern
 * sites still pair it with clip-path for screen-reader-only text. Keep `auto`
 * as a predefined edge so paint can resolve it against the final border box. */
static bool parse_css_clip_rect(const tchar_t* raw, css_offsets& out)
{
	if(!raw) return false;
	tstring val = raw;
	trim(val);
	if(val.length() < 6 || t_strncasecmp(val.c_str(), _t("rect("), 5) || val[val.length() - 1] != _t(')'))
		return false;
	tstring args = val.substr(5, val.length() - 6);
	string_vector toks;
	split_string(args, toks, _t(", \t\r\n"));
	if(toks.size() != 4) return false;
	css_length* edges[4] = { &out.top, &out.right, &out.bottom, &out.left };
	for(size_t i = 0; i < 4; i++)
	{
		trim(toks[i]);
		if(!t_strcasecmp(toks[i].c_str(), _t("auto")))
			edges[i]->predef(0);
		else
			edges[i]->fromString(toks[i].c_str());
	}
	return true;
}

/* Evaluate an additive length expression: terms separated by top-level '+'/'-'
 * (e.g. "2.51rem + 6.198vw", the standard fluid-type preferred value inside
 * clamp()). Every term must be a resolvable length; a percent or nested
 * function makes the whole expression unresolvable so the caller falls back. */
static bool eval_length_expr(const tstring& expr, document* doc, int font_size, int& out_px)
{
	tstring s = expr;
	trim(s);
	if(s.empty() || s.find(_t('(')) != tstring::npos)
	{
		return false;
	}
	int total = 0;
	int sign = 1;
	bool any = false;
	tstring term;
	for(size_t k = 0; k < s.length(); k++)
	{
		tchar_t c = s[k];
		if((c == _t('+') || c == _t('-')) && k > 0)
		{
			int px = 0;
			if(!term.empty())
			{
				if(!eval_one_length(term, doc, font_size, px))
				{
					return false;
				}
				total += sign * px;
				any = true;
			}
			term.clear();
			sign = (c == _t('-')) ? -1 : 1;
		} else
		{
			term += c;
		}
	}
	if(!term.empty())
	{
		int px = 0;
		if(!eval_one_length(term, doc, font_size, px))
		{
			return false;
		}
		total += sign * px;
		any = true;
	}
	if(!any)
	{
		return false;
	}
	out_px = total;
	return true;
}

/* One multiplicative term of a calc(): factors joined by top-level '*'/'/'.
 * Exactly one factor may be a length (length*length is meaningless); the rest
 * must be plain numbers, which covers the idioms real sheets emit such as
 * "calc(var(--x) * 2)" and "calc(2 * var(--x))". */
static bool eval_term_px(const tstring& in, document* doc, int font_size, int& out_px)
{
	tstring s = in;
	trim(s);
	if(s.empty() || s.find(_t('(')) != tstring::npos)
	{
		return false;
	}
	/* split into (op, factor) pairs */
	std::vector<tchar_t> ops;
	std::vector<tstring> facs;
	tstring cur;
	for(size_t k = 0; k <= s.length(); k++)
	{
		tchar_t c = (k < s.length()) ? s[k] : 0;
		if(c == _t('*') || c == _t('/') || c == 0)
		{
			tstring f = cur;
			trim(f);
			if(f.empty()) return false;
			facs.push_back(f);
			ops.push_back(c == 0 ? _t('*') : c);
			cur.clear();
			if(c == 0) break;
			continue;
		}
		cur += c;
	}
	double acc = 0;
	bool have = false;
	bool saw_len = false;
	for(size_t k = 0; k < facs.size(); k++)
	{
		int px = 0;
		if(eval_one_length(facs[k], doc, font_size, px))
		{
			if(saw_len) return false;		/* length * length */
			saw_len = true;
			if(!have) { acc = px; have = true; }
			else if(ops[k] == _t('*')) acc *= px;
			else return false;			/* dividing by a length is not a length */
		}
		else
		{
			/* plain number */
			const tchar_t* st = facs[k].c_str();
			tchar_t* end = 0;
			double n = strtod((const char*)st, (char**)&end);
			if(!end || *end != 0) return false;
			if(!have) { acc = n; have = true; }
			else if(ops[k] == _t('*')) acc *= n;
			else acc /= n;
		}
	}
	if(!have || !saw_len) return false;
	out_px = (int)(acc + (acc >= 0 ? 0.5 : -0.5));
	return true;
}

/* A calc() body with no unit anywhere is a <number> expression (grid-column
 * spans, z-index, opacity, flex-grow...), not a length: "calc(6 - 2 + 1)".
 * Evaluates + - * / with the usual precedence; parens are never present here
 * because nested calc() has already been folded innermost-first. */
static bool eval_number_expr(const tstring& body, double& out)
{
	for(size_t k = 0; k < body.length(); k++)
	{
		tchar_t c = body[k];
		if(c == _t('%') || c == _t('(') || (c >= _t('a') && c <= _t('z')) || (c >= _t('A') && c <= _t('Z')))
		{
			return false;
		}
	}
	double total = 0;
	int sign = 1;
	bool any = false;
	tstring term;
	for(size_t k = 0; k <= body.length(); k++)
	{
		tchar_t c = (k < body.length()) ? body[k] : 0;
		bool is_op = (c == _t('+') || c == _t('-'));
		if(is_op)
		{
			/* a sign directly after another operator or at the start belongs to
			 * the number ("2 * -1"); a sign after an operand is an operator */
			tstring t = term; trim(t);
			if(t.empty() || t[t.length() - 1] == _t('*') || t[t.length() - 1] == _t('/'))
			{
				is_op = false;
			}
		}
		if(is_op || c == 0)
		{
			tstring t = term; trim(t);
			if(t.empty())
			{
				if(c == 0) break;
				return false;
			}
			/* multiplicative term */
			double acc = 0;
			bool have = false;
			tchar_t op = _t('*');
			tstring fac;
			for(size_t m = 0; m <= t.length(); m++)
			{
				tchar_t d = (m < t.length()) ? t[m] : 0;
				if(d == _t('*') || d == _t('/') || d == 0)
				{
					tstring f = fac; trim(f);
					if(f.empty()) return false;
					tchar_t* end = 0;
					double n = strtod((const char*) f.c_str(), (char**) &end);
					if(!end || *end != 0) return false;
					if(!have) { acc = n; have = true; }
					else if(op == _t('*')) acc *= n;
					else { if(n == 0) return false; acc /= n; }
					fac.clear();
					op = d;
					if(d == 0) break;
					continue;
				}
				fac += d;
			}
			if(!have) return false;
			total += sign * acc;
			any = true;
			term.clear();
			if(c == 0) break;
			sign = (c == _t('-')) ? -1 : 1;
		}
		else
		{
			term += c;
		}
	}
	if(!any) return false;
	out = total;
	return true;
}

/* Rewrite every resolvable bare calc() in 'val' to a px literal. apple.com
 * sizes its buttons with "padding-inline:calc(var(--pad) - var(--border))";
 * after var() expansion that is a plain additive calc() which nothing else
 * evaluated, so the length parsed to zero and the pill lost its padding.
 * Unlike clamp()/min()/max(), an unresolvable calc() is LEFT UNTOUCHED rather
 * than dropping the whole declaration: percent-bearing calc() (e.g.
 * "calc(50% - 8px)") is legitimate and must reach the layout code as-is. */
static void eval_calc_funcs(tstring& val, document* doc, int font_size)
{
	size_t from = 0;
	for(int guard = 0; guard < 16; guard++)
	{
		tstring fname;
		/* innermost-first: reuse the clamp/min/max scanner by looking for a
		 * calc( at or after 'from' with the greatest position. */
		size_t f = tstring::npos;
		{
			size_t p = val.find(_t("calc("), from);
			while(p != tstring::npos && p > 0)
			{
				tchar_t prev = val[p - 1];
				if((prev >= _t('a') && prev <= _t('z')) || (prev >= _t('A') && prev <= _t('Z')) || prev == _t('-'))
				{
					p = val.find(_t("calc("), p + 1);
					continue;
				}
				break;
			}
			f = p;
		}
		if(f == tstring::npos)
		{
			return;
		}
		(void)fname;
		size_t args_start = f + 5;
		int paren = 1;
		size_t j = args_start;
		while(j < val.length() && paren > 0)
		{
			if(val[j] == _t('(')) paren++;
			else if(val[j] == _t(')'))
			{
				paren--;
				if(paren == 0) break;
			}
			j++;
		}
		if(j >= val.length())
		{
			return;	/* unbalanced */
		}
		tstring body = val.substr(args_start, j - args_start);
		int px = 0;
		bool ok = false;
		double num = 0;
		if(body.find(_t(',')) == tstring::npos && eval_number_expr(body, num))
		{
			/* workspace.google.com: grid-column-end:span calc(var(--end) -
			 * var(--start) + 1). Folding that to "span 5px" made the placement
			 * parser reject the span, so the hero copy collapsed into one track. */
			char buf[64];
			if(num == (double)(long long) num)
			{
				snprintf(buf, sizeof(buf), "%lld", (long long) num);
			}
			else
			{
				snprintf(buf, sizeof(buf), "%.4f", num);
				size_t L = strlen(buf);
				while(L > 0 && buf[L - 1] == '0') buf[--L] = 0;
				if(L > 0 && buf[L - 1] == '.') buf[--L] = 0;
			}
			tstring repl = buf;
			val.replace(f, (j - f) + 1, repl);
			from = f + repl.length();
			continue;
		}
		if(body.find(_t(',')) == tstring::npos)
		{
			/* additive over top-level '+'/'-' of multiplicative terms */
			int total = 0;
			int sign = 1;
			bool any = false;
			tstring term;
			bool fail = false;
			for(size_t k = 0; k <= body.length(); k++)
			{
				tchar_t c = (k < body.length()) ? body[k] : 0;
				if((c == _t('+') || c == _t('-')) && k > 0)
				{
					int t = 0;
					if(!term.empty())
					{
						if(!eval_term_px(term, doc, font_size, t)) { fail = true; break; }
						total += sign * t;
						any = true;
					}
					term.clear();
					sign = (c == _t('-')) ? -1 : 1;
				}
				else if(c == 0)
				{
					if(!term.empty())
					{
						int t = 0;
						if(!eval_term_px(term, doc, font_size, t)) { fail = true; }
						else { total += sign * t; any = true; }
					}
					break;
				}
				else term += c;
			}
			if(!fail && any)
			{
				px = total;
				ok = true;
			}
		}
		if(ok)
		{
			tstring repl = std::to_string(px) + _t("px");
			val.replace(f, (j - f) + 1, repl);
			from = f + repl.length();
		}
		else
		{
			from = j + 1;	/* leave this calc() alone, scan past it */
		}
	}
}

/* Rewrite every clamp()/min()/max() in 'val' to a px literal. clamp(MIN,PREF,
 * MAX) applies real clamping max(MIN, min(PREF,MAX)); each argument may be an
 * additive length expression (Xrem + Yvw). Percent/calc()/nested-func arguments
 * that cannot be resolved here are skipped. Returns false when nothing
 * evaluates, so the caller can drop the declaration. */
static bool eval_math_funcs(tstring& val, document* doc, int font_size)
{
	for(int guard = 0; guard < 16; guard++)
	{
		tstring fname;
		size_t f = find_math_func(val, 0, fname);
		if(f == tstring::npos)
		{
			return true;
		}
		size_t args_start = f + fname.length();
		int paren = 1;
		size_t j = args_start;
		size_t start = args_start;
		std::vector<tstring> args;
		while(j < val.length() && paren > 0)
		{
			tchar_t c = val[j];
			if(c == _t('('))
			{
				paren++;
			} else if(c == _t(')'))
			{
				paren--;
				if(paren == 0)
				{
					args.push_back(val.substr(start, j - start));
					break;
				}
			} else if(c == _t(',') && paren == 1)
			{
				args.push_back(val.substr(start, j - start));
				start = j + 1;
			}
			j++;
		}
		if(j >= val.length())
		{
			return false;	// unbalanced parens
		}
		int picked = 0;
		bool have = false;
		if(fname[0] == _t('c'))
		{
			/* clamp(MIN, PREFERRED, MAX) = max(MIN, min(PREFERRED, MAX)). */
			int lo = 0, pref = 0, hi = 0;
			bool hlo = false, hpref = false, hhi = false;
			if(args.size() >= 1) hlo   = eval_length_expr(args[0], doc, font_size, lo);
			if(args.size() >= 2) hpref = eval_length_expr(args[1], doc, font_size, pref);
			if(args.size() >= 3) hhi   = eval_length_expr(args[2], doc, font_size, hi);
			if(hpref)
			{
				picked = pref;
				if(hhi && picked > hi) picked = hi;
				if(hlo && picked < lo) picked = lo;
				have = true;
			}
			else if(hhi) { picked = hi; have = true; }	/* preferred unresolvable: assume wide viewport */
			else if(hlo) { picked = lo; have = true; }
		} else
		{
			bool want_min = (fname[1] == _t('i'));
			for(size_t k = 0; k < args.size(); k++)
			{
				int px = 0;
				if(!eval_length_expr(args[k], doc, font_size, px))
				{
					continue;
				}
				if(!have || (want_min ? px < picked : px > picked))
				{
					picked = px;
					have = true;
				}
			}
		}
		if(!have)
		{
			return false;
		}
		tstring repl = std::to_string(picked) + _t("px");
		val.replace(f, (j - f) + 1, repl);
	}
	return true;
}

void litehtml::html_tag::resolve_custom_properties()
{
	m_custom_props.clear();
	element::ptr p = parent();
	if(p)
	{
		/* Virtual accessor: works without RTTI and returns null for
		 * non-html_tag parents (text nodes etc.). */
		const string_map* pm = p->get_custom_props();
		if(pm)
		{
			m_custom_props = *pm;
		}
	}
	for(props_map::const_iterator it = m_style.properties().begin(); it != m_style.properties().end(); ++it)
	{
		if(it->first.length() > 2 && it->first[0] == _t('-') && it->first[1] == _t('-'))
		{
			tstring v;
			if(!expand_var_refs(it->second.m_value, m_custom_props, v, 0))
			{
				v = it->second.m_value;
			}
			m_custom_props[it->first] = v;
		}
	}
}

/* env() substitutions for the safe-area insets desktop ports have none of:
 * every env() collapses to its fallback (or 0px), so idioms like
 * padding-left:max(12px, env(safe-area-inset-left) - 12px) still evaluate
 * instead of dropping the declaration (apple.com's section gutters). */
static void eval_env_funcs(tstring& val)
{
	size_t from = 0;
	for(int guard = 0; guard < 16; guard++)
	{
		size_t i = val.find(_t("env("), from);
		if(i == tstring::npos) break;
		if(i && (val[i - 1] == _t('-') || val[i - 1] == _t('_') ||
			   (val[i - 1] >= _t('a') && val[i - 1] <= _t('z')) ||
			   (val[i - 1] >= _t('A') && val[i - 1] <= _t('Z')) ||
			   (val[i - 1] >= _t('0') && val[i - 1] <= _t('9'))))
		{
			from = i + 4;
			continue;
		}
		int depth = 0;
		size_t j = i + 3;
		for(; j < val.size(); j++)
		{
			if(val[j] == _t('(')) depth++;
			else if(val[j] == _t(')'))
			{
				if(--depth == 0) break;
			}
		}
		if(j >= val.size()) break;
		tstring repl = _t("0px");
		size_t comma = val.find(_t(','), i + 4);
		if(comma != tstring::npos && comma < j)
		{
			tstring fb = val.substr(comma + 1, j - comma - 1);
			trim(fb);
			if(!fb.empty()) repl = fb;
		}
		val.replace(i, j - i + 1, repl);
		from = i + repl.size();
	}
}

void litehtml::html_tag::expand_css_functions()
{
	bool has_func = false;
	for(props_map::const_iterator it = m_style.properties().begin(); it != m_style.properties().end(); ++it)
	{
		const tstring& v = it->second.m_value;
		if(v.find(_t("var(")) != tstring::npos || v.find(_t("clamp(")) != tstring::npos ||
		   v.find(_t("min(")) != tstring::npos || v.find(_t("max(")) != tstring::npos ||
		   v.find(_t("calc(")) != tstring::npos || v.find(_t("env(")) != tstring::npos)
		{
			has_func = true;
			break;
		}
	}
	if(!has_func)
	{
		return;
	}
	document* doc = get_document();
	props_map old = m_style.properties();
	m_style.clear();
	/* Two passes: deferred shorthands (`font`/`flex` kept raw by
	 * style::add_property because they held a var()) expand first, so the
	 * longhands that survived the cascade re-apply on top of them in pass 1. */
	for(int pass = 0; pass < 2; pass++)
	for(props_map::const_iterator it = old.begin(); it != old.end(); ++it)
	{
		const tstring& name = it->first;
		const tstring& raw = it->second.m_value;
		bool shorthand = (name == _t("font") || name == _t("flex"));
		if(shorthand != (pass == 0)) continue;
		if(name.length() > 2 && name[0] == _t('-') && name[1] == _t('-'))
		{
			/* keep the raw definition: it is re-resolved per element */
			m_style.add_property(name.c_str(), raw.c_str(), NULL, it->second.m_important,
				it->second.m_specificity);
			continue;
		}
		tstring v = raw;
		if(v.find(_t("var(")) != tstring::npos)
		{
			tstring exp;
			if(!expand_var_refs(v, m_custom_props, exp, 0))
			{
				continue;	// invalid at computed-value time: drop it
			}
			v = exp;
		}
		if(v.find(_t("env(")) != tstring::npos)
		{
			eval_env_funcs(v);
		}
		if(v.find(_t("clamp(")) != tstring::npos || v.find(_t("min(")) != tstring::npos || v.find(_t("max(")) != tstring::npos ||
		   v.find(_t("calc(")) != tstring::npos)
		{
			/* calc() first so nested calc() inside clamp() arguments collapses
			 * to a literal the clamp() evaluator can consume. */
			if(doc && v.find(_t("calc(")) != tstring::npos)
			{
				eval_calc_funcs(v, doc, m_font_size);
			}
			if(v.find(_t("clamp(")) != tstring::npos || v.find(_t("min(")) != tstring::npos || v.find(_t("max(")) != tstring::npos)
			{
				if(doc && !eval_math_funcs(v, doc, m_font_size))
				{
					continue;
				}
			}
		}
		m_style.add_property(name.c_str(), v.c_str(), NULL, it->second.m_important,
			it->second.m_specificity);
	}
}

} // namespace litehtml

void litehtml::html_tag::parse_styles(bool is_reparse)
{
	document* step_doc = get_document();
	bool step_parse = (!is_reparse && step_doc && step_doc->style_step_phase() == 2);
	if(step_parse)
	{
		if(m_step_stamp == step_doc->style_step_epoch())
		{
			/* Own work already completed in this chunked-parse epoch. Falling
			 * through to it anyway (the previous behaviour) made every resumed
			 * chunk re-run the whole stamped prefix from the root down to the
			 * frontier, so a ~1k-node tree needed ~1k chunks and never finished
			 * -- the visible doc stayed style-pending and never rendered. Only
			 * the paused subtree still needs walking, and if this subtree
			 * already finished it is pruned outright, which makes a resume cost
			 * O(path length) instead of O(tree). */
			if(m_step_done)
			{
				return;
			}
			for(auto& el : m_children)
			{
				if(step_doc->style_step_exhausted())
					break;
				el->parse_styles();
			}
			if(!step_doc->style_step_exhausted())
			{
				m_step_done = true;
			}
			return;
		}
		else if(step_doc->style_step_exhausted())
		{
			/* Slice exhausted before this element: leave it for the next chunk.
			 * Cache stays warm; the resume pass clears it when work is done. */
			return;
		}
	}
	clear_style_property_cache();
	/* Snapshot the pre-cascade opacity so the transition trigger at the end
	 * of parse_styles can detect a change. m_opacity holds the value from
	 * the previous parse_styles call (or the constructor default on first
	 * call). */
	float old_opacity_for_transition = m_opacity;
	/* Snapshot the pre-cascade transform string for the same reason: the
	 * trigger block at the end compares it against the freshly computed
	 * m_transform_str to fire a transform transition. */
	tstring old_transform_for_transition = m_transform_str;
	bool profile_enabled = parse_style_profile_enabled();
	uint64_t part_start = 0;
	if(profile_enabled)
	{
		g_parse_style_profile.nodes++;
		part_start = sys_tic_ms(0);
	}
	const tchar_t* style = get_attr(_t("style"));

	if(style)
	{
		m_style.add(style, NULL);
	}
	if(profile_enabled)
	{
		parse_style_profile_add(g_parse_style_profile.inline_style_ms, part_start);
		part_start = sys_tic_ms(0);
	}

	/* Resolve --custom-properties (inherited + own) and rewrite var()/
	 * clamp()/min()/max() in the raw values before anything below consumes
	 * them. No-op on trees that use none of these. */
	resolve_custom_properties();
	expand_css_functions();

	own_style_refs own_refs = collect_own_style_refs(m_style);
	init_font(own_style_ref_ptr(own_refs.font_size), own_style_ref_ptr(own_refs.font_family),
		own_style_ref_ptr(own_refs.font_weight), own_style_ref_ptr(own_refs.font_style),
		own_style_ref_ptr(own_refs.text_decoration));
	if(profile_enabled)
	{
		parse_style_profile_add(g_parse_style_profile.init_font_ms, part_start);
		part_start = sys_tic_ms(0);
	}
	document* doc = get_document();
	element::ptr el_parent = parent();
	m_has_css_clip = parse_css_clip_rect(get_style_property_own(_t("clip")), m_css_clip);
	if(m_has_css_clip)
	{
		doc->cvt_units(m_css_clip.top, m_font_size);
		doc->cvt_units(m_css_clip.right, m_font_size);
		doc->cvt_units(m_css_clip.bottom, m_font_size);
		doc->cvt_units(m_css_clip.left, m_font_size);
	}
	const tchar_t* own_position = own_style_ref_ptr(own_refs.position);
	const tchar_t* own_overflow = own_style_ref_ptr(own_refs.overflow);
	const tchar_t* own_display = own_style_ref_ptr(own_refs.display);
	const tchar_t* own_box_sizing = own_style_ref_ptr(own_refs.box_sizing);
	const tchar_t* own_text_align = own_style_ref_ptr(own_refs.text_align);
	const tchar_t* own_text_transform = own_style_ref_ptr(own_refs.text_transform);

	m_el_position	= (element_position)	value_index((own_position && t_strcasecmp(own_position, _t("inherit"))) ? own_position : _t("static"),			element_position_strings,	element_position_fixed);
	/* overflow:clip is hidden without any scrollability: for painting purposes
	 * the engine clips identically, and apple.com relies on it to keep
	 * full-bleed tile artwork inside its tile. */
	if(own_overflow && !t_strcasecmp(own_overflow, _t("clip")))
	{
		own_overflow = _t("hidden");
	}
	m_overflow		= (overflow)			value_index((own_overflow && t_strcasecmp(own_overflow, _t("inherit"))) ? own_overflow : _t("visible"),		overflow_strings,			overflow_visible);
	/* css-overflow-3 §3.3: the root's overflow - or <body>'s when the root is
	 * visible - propagates to the viewport and the element's own used value
	 * becomes visible. The shell scrolls the viewport itself, so the propagated
	 * value is simply dropped here; keeping it on <body> would clip positioned
	 * children to the body box (workspace.google.com: body{overflow-y:scroll}). */
	if(m_overflow > overflow_visible)
	{
		if(!el_parent)
		{
			m_overflow = overflow_visible;
		}
		else if(is_body() && el_parent->get_overflow() == overflow_visible)
		{
			m_overflow = overflow_visible;
		}
	}
	if(own_display && t_strcasecmp(own_display, _t("inherit")))
	{
		m_display = (style_display) value_index(own_display, style_display_strings, display_block);
	}
	else if(own_display && el_parent)
	{
		/* display:inherit resolves against the parent's computed value; the
		 * style walk is top-down, so the parent's display is already final
		 * (this is how apple.com chains display:contents down a wrapper). */
		m_display = el_parent->get_display();
	}
	else
	{
		/* No declared display: replaced elements keep their UA default of
		 * inline-block (the el_image/el_svg constructors seed it), plain
		 * elements are inline. Forcing inline here made restyles of
		 * detached-built subtrees (innerHTML/cloneNode, e.g. the w3.org
		 * member logos) collapse <img> to display:inline, which place_element
		 * routes to render_inline - a path that never lays a replaced box
		 * out, so the images stayed zero-sized. */
		m_display = is_replaced() ? display_inline_block : display_inline;
	}
	m_box_sizing	= (box_sizing)			value_index((own_box_sizing && t_strcasecmp(own_box_sizing, _t("inherit"))) ? own_box_sizing : _t("content-box"),	box_sizing_strings,			box_sizing_content_box);

	/* UA behaviour for <details>: a closed widget renders only its <summary>,
	 * the remaining children stay out of flow until the open attribute shows
	 * up (Chrome hides them inside the shadow DOM). Without this the collapsed
	 * menus of modern sites paint as giant black overlays. */
	if(m_display != display_none && m_tag != _t("summary") && el_parent &&
		!t_strcasecmp(el_parent->get_tagName(), _t("details")) && !el_parent->get_attr(_t("open")))
	{
		m_display = display_none;
	}

	/* UA behaviour for <noscript>: this engine always runs scripts, so the
	 * fallback content must stay out of the box tree (apple.com ships whole
	 * unpositioned fallback <img> sets inside noscript that otherwise paint
	 * over the following sections). */
	if(m_display != display_none && m_tag == _t("noscript"))
	{
		m_display = display_none;
	}

	if(own_text_align && t_strcasecmp(own_text_align, _t("inherit")))
	{
		m_text_align = (text_align) value_index(own_text_align, text_align_strings, text_align_left);
	}
	else if(el_parent)
	{
		m_text_align = el_parent->get_text_align();
	}
	else
	{
		m_text_align = text_align_left;
	}

	if(own_text_transform && t_strcasecmp(own_text_transform, _t("inherit")))
	{
		m_text_transform = (text_transform) value_index(own_text_transform, text_transform_strings, text_transform_none);
	}
	else if(el_parent)
	{
		m_text_transform = el_parent->get_text_transform();
	}
	else
	{
		m_text_transform = text_transform_none;
	}

	/* transform is not inherited: an absent own declaration is the identity.
	 * An animation override (m_anim_overrides["transform"]) wins over the CSS
	 * value so tick_animations can drive transform without a re-cascade. */
	m_transform.clear();
	const tchar_t* own_transform = anim_override(_t("transform"));
	if(!own_transform)
		own_transform = get_style_property_own(_t("transform"));
	if(own_transform)
	{
		parse_transform_list(own_transform);
	}
	else
	{
		m_transform_str.clear();
	}

	const tchar_t* own_white_space = own_style_ref_ptr(own_refs.white_space);
	if(own_white_space && t_strcasecmp(own_white_space, _t("inherit")))
	{
		m_white_space = (white_space) value_index(own_white_space, white_space_strings, white_space_normal);
	}
	else if(el_parent)
	{
		m_white_space = el_parent->get_white_space();
	}
	else
	{
		m_white_space = white_space_normal;
	}

	/* text-wrap is inherited. `balance` keeps the normal number of lines but
	 * chooses a narrower wrapping threshold so their lengths are more even. */
	{
		const tchar_t* text_wrap = get_style_property(_t("text-wrap"), true, _t("wrap"));
		m_text_wrap_balance = text_wrap && !t_strcasecmp(text_wrap, _t("balance"));
	}

	/* text-overflow:ellipsis is defined for single-line boxes only. Without
	 * this, a clipped list row (fixed height + overflow:hidden) wraps to a
	 * second line that the clip then slices mid-glyph - the "torn row"
	 * artifact on dense news lists. Browsers never produce that second line
	 * because ellipsis implies nowrap; mirror that here so the box lays out
	 * one line and the clip cuts cleanly at the right edge instead. */
	if(m_white_space == white_space_normal)
	{
		const tchar_t* tovl = get_style_property(_t("text-overflow"), false, _t("clip"));
		const tchar_t* ovf  = get_style_property(_t("overflow"), false, _t("visible"));
		if(tovl && ovf &&
		   !t_strcmp(tovl, _t("ellipsis")) &&
		   t_strcmp(ovf, _t("visible")))
		{
			m_white_space = white_space_nowrap;
		}
	}

	const tchar_t* own_visibility = own_style_ref_ptr(own_refs.visibility);
	if(own_visibility && t_strcasecmp(own_visibility, _t("inherit")))
	{
		m_visibility = (visibility) value_index(own_visibility, visibility_strings, visibility_visible);
	}
	else if(el_parent)
	{
		m_visibility = el_parent->get_visibility();
	}
	else
	{
		m_visibility = visibility_visible;
	}

	/* CSS 'opacity' (non-inherited, 0..1). The visible transparency is the
	 * product of this element's opacity and every ancestor's, so accumulate
	 * top-down: the style walk visits parents first (see the display:inherit
	 * path above), therefore the parent's cumulative value is already final.
	 * opacity:0 keeps layout but must paint nothing - handled at draw time.
	 * An animation override (m_anim_overrides["opacity"]) wins over the CSS
	 * value so tick_animations can drive the property without a re-cascade. */
	{
		const tchar_t* own_opacity = anim_override(_t("opacity"));
		if(!own_opacity)
			own_opacity = get_style_property(_t("opacity"), false, _t("1"));
		float op = own_opacity ? (float)atof(own_opacity) : 1.0f;
		if(op < 0.0f) op = 0.0f;
		if(op > 1.0f) op = 1.0f;
		/* Script-reveal guard: SSR pages (Google sign-in ships a trailing
		 * `body{opacity:0}`) hide the document until their hydration JS flips it
		 * back. A JS-limited engine can never run that script, so honouring the
		 * guard would leave the page permanently blank; treat a fully
		 * transparent document root as "revealed". Scoped to html/body only -
		 * opacity:0 on real content (Material state layers) still hides. */
		if(op <= 0.0f && (m_tag == _t("html") || m_tag == _t("body")))
		{
			op = 1.0f;
		}
		m_opacity = op;
		float parent_cum = el_parent ? el_parent->get_opacity_cum() : 1.0f;
		m_opacity_cum = parent_cum * op;
	}
	if(profile_enabled)
	{
		parse_style_profile_add(g_parse_style_profile.basic_ms, part_start);
		part_start = sys_tic_ms(0);
	}

	/* z-index applies to positioned boxes and, per the flexbox/grid specs, to
	 * flex/grid items even when position:static. Parse it unconditionally and
	 * record whether the value was the keyword 'auto' (or absent) so the
	 * stacking-context / stacking-participant tests can tell z-index:auto apart
	 * from an explicit z-index:0. */
	{
		const tchar_t* val = own_style_ref_ptr(own_refs.z_index);
		if(val && t_strcasecmp(val, _t("auto")))
		{
			m_z_index = t_atoi(val);
			m_z_index_auto = false;
		} else
		{
			m_z_index = 0;
			m_z_index_auto = true;
		}
	}
	{
		const tchar_t* iso = get_style_property_own(_t("isolation"));
		m_isolate = iso && !t_strcasecmp(iso, _t("isolate"));
	}

	const tchar_t* own_vertical_align = own_style_ref_ptr(own_refs.vertical_align);
	if(own_vertical_align && t_strcasecmp(own_vertical_align, _t("inherit")))
	{
		m_vertical_align = (vertical_align) value_index(own_vertical_align, vertical_align_strings, va_baseline);
	}
	else if(el_parent)
	{
		m_vertical_align = el_parent->get_vertical_align();
	}
	else
	{
		m_vertical_align = va_baseline;
	}

	const tchar_t* own_float = own_style_ref_ptr(own_refs.float_value);
	const tchar_t* own_clear = own_style_ref_ptr(own_refs.clear_value);
	const tchar_t* fl = (own_float && t_strcasecmp(own_float, _t("inherit"))) ? own_float : _t("none");
	m_float = (element_float) value_index(fl, element_float_strings, float_none);

	m_clear = (element_clear) value_index((own_clear && t_strcasecmp(own_clear, _t("inherit"))) ? own_clear : _t("none"), element_clear_strings, clear_none);
	if(profile_enabled)
	{
		parse_style_profile_add(g_parse_style_profile.flow_ms, part_start);
	}

	if(m_style.empty())
	{
		m_css_text_indent.fromString(_t("0"), _t("0"));
		css_length_set_predef0(m_css_width);
		css_length_set_predef0(m_css_height);
		css_length_set_zero(m_css_min_width);
		css_length_set_zero(m_css_min_height);
		css_length_set_predef0(m_css_max_width);
		css_length_set_predef0(m_css_max_height);
		css_length_set_predef0(m_css_offsets.left);
		css_length_set_predef0(m_css_offsets.right);
		css_length_set_predef0(m_css_offsets.top);
		css_length_set_predef0(m_css_offsets.bottom);
		css_length_set_zero(m_css_margins.left);
		css_length_set_zero(m_css_margins.right);
		css_length_set_zero(m_css_margins.top);
		css_length_set_zero(m_css_margins.bottom);
		css_length_set_zero(m_css_padding.left);
		css_length_set_zero(m_css_padding.right);
		css_length_set_zero(m_css_padding.top);
		css_length_set_zero(m_css_padding.bottom);
		css_length_set_predef0(m_css_borders.left.width);
		css_length_set_predef0(m_css_borders.right.width);
		css_length_set_predef0(m_css_borders.top.width);
		css_length_set_predef0(m_css_borders.bottom.width);
		m_css_borders.left.style = border_style_none;
		m_css_borders.right.style = border_style_none;
		m_css_borders.top.style = border_style_none;
		m_css_borders.bottom.style = border_style_none;
		m_css_borders.left.color = web_color(0, 0, 0, 0);
		m_css_borders.right.color = web_color(0, 0, 0, 0);
		m_css_borders.top.color = web_color(0, 0, 0, 0);
		m_css_borders.bottom.color = web_color(0, 0, 0, 0);
		css_length_set_zero(m_css_borders.radius.top_left_x);
		css_length_set_zero(m_css_borders.radius.top_left_y);
		css_length_set_zero(m_css_borders.radius.top_right_x);
		css_length_set_zero(m_css_borders.radius.top_right_y);
		css_length_set_zero(m_css_borders.radius.bottom_right_x);
		css_length_set_zero(m_css_borders.radius.bottom_right_y);
		css_length_set_zero(m_css_borders.radius.bottom_left_x);
		css_length_set_zero(m_css_borders.radius.bottom_left_y);
		m_margins.left = m_margins.right = m_margins.top = m_margins.bottom = 0;
		m_padding.left = m_padding.right = m_padding.top = m_padding.bottom = 0;
		m_borders.left = m_borders.right = m_borders.top = m_borders.bottom = 0;
		m_bg.m_color = web_color(0, 0, 0, 0);
		m_bg.m_image.clear();
		m_bg.m_baseurl.clear();
		m_bg.m_attachment = background_attachment_scroll;
		m_bg.m_repeat = background_repeat_repeat;
		m_bg.m_clip = background_box_border;
		m_bg.m_origin = background_box_padding;
		m_bg.m_position.x.set_value(0, css_units_percentage);
		m_bg.m_position.y.set_value(0, css_units_percentage);
		m_bg.m_position.width.predef(background_size_auto);
		m_bg.m_position.height.predef(background_size_auto);
		if(el_parent)
		{
			if(el_parent->is_line_height_normal())
			{
				m_line_height = m_font_metrics.height;
				m_lh_predefined = true;
				m_lh_factor = 0.0f;
			} else if(el_parent->line_height_factor() > 0.0f)
			{
				m_lh_factor = el_parent->line_height_factor();
				m_line_height = (int) (m_lh_factor * m_font_size);
				m_lh_predefined = false;
			} else
			{
				m_line_height = el_parent->line_height();
				m_lh_predefined = false;
				m_lh_factor = 0.0f;
			}
		}
		else
		{
			m_line_height = m_font_metrics.height;
			m_lh_predefined = true;
			m_lh_factor = 0.0f;
		}
		m_list_style_type = list_style_type_none;
		m_list_style_position = list_style_position_outside;
		if(!is_reparse)
		{
			if(step_parse)
			{
				step_doc->style_step_stamp(this);
			}
			if(profile_enabled)
				part_start = sys_tic_ms(0);
			for(auto& el : m_children)
			{
				if(step_parse && step_doc->style_step_exhausted())
					break;
				el->parse_styles();
			}
			if(step_parse && !step_doc->style_step_exhausted())
			{
				m_step_done = true; /* subtree fully covered in this epoch */
			}
			if(profile_enabled)
			{
				parse_style_profile_add(g_parse_style_profile.child_ms, part_start);
			}
		}
		return;
	}

	if(doc->is_fast_mode() && !is_reparse)
	{
		css_length_set_zero(m_css_text_indent);
		css_length_set_predef0(m_css_width);
		css_length_set_predef0(m_css_height);
		css_length_set_zero(m_css_min_width);
		css_length_set_zero(m_css_min_height);
		css_length_set_predef0(m_css_max_width);
		css_length_set_predef0(m_css_max_height);
		css_length_set_predef0(m_css_offsets.left);
		css_length_set_predef0(m_css_offsets.right);
		css_length_set_predef0(m_css_offsets.top);
		css_length_set_predef0(m_css_offsets.bottom);
		css_length_set_zero(m_css_margins.left);
		css_length_set_zero(m_css_margins.right);
		css_length_set_zero(m_css_margins.top);
		css_length_set_zero(m_css_margins.bottom);
		css_length_set_zero(m_css_padding.left);
		css_length_set_zero(m_css_padding.right);
		css_length_set_zero(m_css_padding.top);
		css_length_set_zero(m_css_padding.bottom);
		css_length_set_predef0(m_css_borders.left.width);
		css_length_set_predef0(m_css_borders.right.width);
		css_length_set_predef0(m_css_borders.top.width);
		css_length_set_predef0(m_css_borders.bottom.width);
		m_css_borders.left.style = border_style_none;
		m_css_borders.right.style = border_style_none;
		m_css_borders.top.style = border_style_none;
		m_css_borders.bottom.style = border_style_none;
		m_css_borders.left.color = web_color(0, 0, 0, 0);
		m_css_borders.right.color = web_color(0, 0, 0, 0);
		m_css_borders.top.color = web_color(0, 0, 0, 0);
		m_css_borders.bottom.color = web_color(0, 0, 0, 0);
		css_length_set_zero(m_css_borders.radius.top_left_x);
		css_length_set_zero(m_css_borders.radius.top_left_y);
		css_length_set_zero(m_css_borders.radius.top_right_x);
		css_length_set_zero(m_css_borders.radius.top_right_y);
		css_length_set_zero(m_css_borders.radius.bottom_right_x);
		css_length_set_zero(m_css_borders.radius.bottom_right_y);
		css_length_set_zero(m_css_borders.radius.bottom_left_x);
		css_length_set_zero(m_css_borders.radius.bottom_left_y);
		m_margins.left = m_margins.right = m_margins.top = m_margins.bottom = 0;
		m_padding.left = m_padding.right = m_padding.top = m_padding.bottom = 0;
		m_borders.left = m_borders.right = m_borders.top = m_borders.bottom = 0;
		m_bg.m_color = web_color(0, 0, 0, 0);
		m_bg.m_image.clear();
		m_bg.m_baseurl.clear();
		m_bg.m_attachment = background_attachment_scroll;
		m_bg.m_repeat = background_repeat_repeat;
		m_bg.m_clip = background_box_border;
		m_bg.m_origin = background_box_padding;
		m_bg.m_position.x.set_value(0, css_units_percentage);
		m_bg.m_position.y.set_value(0, css_units_percentage);
		m_bg.m_position.width.predef(background_size_auto);
		m_bg.m_position.height.predef(background_size_auto);
		m_line_height = m_font_metrics.height;
		m_lh_predefined = true;
		m_lh_factor = 0.0f;
		m_list_style_type = list_style_type_none;
		m_list_style_position = list_style_position_outside;

		if(!is_reparse)
		{
			if(step_parse)
			{
				step_doc->style_step_stamp(this);
			}
			if(profile_enabled)
				part_start = sys_tic_ms(0);
			for(auto& el : m_children)
			{
				if(step_parse && step_doc->style_step_exhausted())
					break;
				el->parse_styles();
			}
			if(step_parse && !step_doc->style_step_exhausted())
			{
				m_step_done = true; /* subtree fully covered in this epoch */
			}
			if(profile_enabled)
			{
				parse_style_profile_add(g_parse_style_profile.child_ms, part_start);
			}
		}
		return;
	}

	if (m_float != float_none)
	{
		// reset display in to block for floating elements
		if (m_display != display_none)
		{
			m_display = display_block;
		}
	}
	else if (doc && (m_display == display_table ||
		m_display == display_table_caption ||
		m_display == display_table_cell ||
		m_display == display_table_column ||
		m_display == display_table_column_group ||
		m_display == display_table_footer_group ||
		m_display == display_table_header_group ||
		m_display == display_table_row ||
		m_display == display_table_row_group))
	{
		doc->add_tabular(this);
	}
	// fix inline boxes with absolute/fixed positions
	else if (m_display != display_none && is_inline_box())
	{
		if (m_el_position == element_position_absolute || m_el_position == element_position_fixed)
		{
			m_display = display_block;
		}
		/* Flex/grid items are block-level: the computed display of an in-flow
		 * child of a flex/grid container is blockified (CSS Display 3 2.4). The
		 * style walk is top-down so the parent's display is already final here.
		 * This must live inside the is_inline_box() branch - every blockifiable
		 * display is an inline box, so a separate else-if below never runs.
		 * Without it an undeclared-display ::after caret inside a flex button
		 * stays display:inline - the inline draw path applies no transform, so
		 * the border-trick chevron paints as an unrotated corner (w3.org nav). */
		else if (el_parent)
		{
		style_display pd = el_parent->get_display();
		if (pd == display_flex || pd == display_inline_flex ||
			pd == display_grid || pd == display_inline_grid)
		{
			switch (m_display)
			{
			case display_inline:		m_display = display_block;	break;
			case display_inline_block:	m_display = display_block;	break;
			case display_inline_table:	m_display = display_table;	break;
			case display_inline_flex:	m_display = display_flex;	break;
			case display_inline_grid:	m_display = display_grid;	break;
			default:											break;
			}
		}
		}
	}

	if(m_display == display_contents)
	{
		/* The wrapper has no principal box. Its descendants are laid out by the
		 * surrounding formatting context via collect_box_children(). */
		m_pos.clear();
	}

	if(profile_enabled)
		part_start = sys_tic_ms(0);
	m_css_text_indent.fromString(	get_style_property(_t("text-indent"),	true,	_t("0")),	_t("0"));

	const tchar_t* own_width = get_style_property_own(_t("width"));
	if(!own_width)
	{
		/* CSS Logical: inline-size is the writing-mode-aware width. w3.org sizes
		 * its nav caret purely with inline-size/block-size, so without this the
		 * border-trick chevron collapses to a 0x0 dot. */
		own_width = get_style_property_own(_t("inline-size"));
	}
	if(own_width) {
		/* The intrinsic-size keywords stay "predefined" (every auto-width
		 * check keeps working) but with a non-zero predef so render_box can
		 * shrink-to-fit the block like a float: apple.com's tab pill is a
		 * plain <div> with width:fit-content and stretched to 1260px. */
		m_css_width.fromString(own_width, _t("auto;fit-content;max-content;min-content"));
	} else {
		css_length_set_predef0(m_css_width);
	}

	const tchar_t* own_height = get_style_property_own(_t("height"));
	if(!own_height)
	{
		own_height = get_style_property_own(_t("block-size"));
	}
	if(own_height) {
		m_css_height.fromString(own_height, _t("auto"));
	} else {
		css_length_set_predef0(m_css_height);
	}

	/* <canvas> is a replaced element: when CSS supplies no width/height its
	 * width/height ATTRIBUTES are the intrinsic size (as for <img>). Without
	 * this the box collapses to 0x0, so flex centering offsets it by half its
	 * width and its background/border never paint. A reparse resets these to
	 * predef(0), which marks them predefined but keeps the stale m_units, so
	 * test is_predefined() as well as units() or the reset would win. */
	if(m_tag == _t("canvas"))
	{
		if(m_css_width.is_predefined() || m_css_width.units() == css_units_none)
		{
			const tchar_t* aw = get_attr(_t("width"));
			if(aw && atoi(aw) > 0) m_css_width.set_value((float)atoi(aw), css_units_px);
		}
		if(m_css_height.is_predefined() || m_css_height.units() == css_units_none)
		{
			const tchar_t* ah = get_attr(_t("height"));
			if(ah && atoi(ah) > 0) m_css_height.set_value((float)atoi(ah), css_units_px);
		}
	}

	doc->cvt_units(m_css_width, m_font_size);
	doc->cvt_units(m_css_height, m_font_size);

	const tchar_t* own_min_width = get_style_property_own(_t("min-width"));
	if(own_min_width) {
		m_css_min_width.fromString(own_min_width);
	} else {
		css_length_set_zero(m_css_min_width);
	}

	const tchar_t* own_min_height = get_style_property_own(_t("min-height"));
	if(own_min_height) {
		m_css_min_height.fromString(own_min_height);
	} else {
		css_length_set_zero(m_css_min_height);
	}

	const tchar_t* own_max_width = get_style_property_own(_t("max-width"));
	if(own_max_width) {
		m_css_max_width.fromString(own_max_width, _t("none"));
	} else {
		css_length_set_predef0(m_css_max_width);
	}

	const tchar_t* own_max_height = get_style_property_own(_t("max-height"));
	if(own_max_height) {
		m_css_max_height.fromString(own_max_height, _t("none"));
	} else {
		css_length_set_predef0(m_css_max_height);
	}
	
	doc->cvt_units(m_css_min_width, m_font_size);
	doc->cvt_units(m_css_min_height, m_font_size);

	const tchar_t* own_left = get_style_property_own(_t("left"));
	if(own_left) {
		m_css_offsets.left.fromString(own_left, _t("auto"));
	} else {
		css_length_set_predef0(m_css_offsets.left);
	}

	const tchar_t* own_right = get_style_property_own(_t("right"));
	if(own_right) {
		m_css_offsets.right.fromString(own_right, _t("auto"));
	} else {
		css_length_set_predef0(m_css_offsets.right);
	}

	const tchar_t* own_top = get_style_property_own(_t("top"));
	if(own_top) {
		m_css_offsets.top.fromString(own_top, _t("auto"));
	} else {
		css_length_set_predef0(m_css_offsets.top);
	}

	const tchar_t* own_bottom = get_style_property_own(_t("bottom"));
	if(own_bottom) {
		m_css_offsets.bottom.fromString(own_bottom, _t("auto"));
	} else {
		css_length_set_predef0(m_css_offsets.bottom);
	}

	doc->cvt_units(m_css_offsets.left, m_font_size);
	doc->cvt_units(m_css_offsets.right, m_font_size);
	doc->cvt_units(m_css_offsets.top,		m_font_size);
	doc->cvt_units(m_css_offsets.bottom,	m_font_size);
	if(profile_enabled)
	{
		parse_style_profile_add(g_parse_style_profile.size_ms, part_start);
		part_start = sys_tic_ms(0);
	}

	const tchar_t* own_margin_left = get_style_property_own(_t("margin-left"));
	if(own_margin_left) {
		m_css_margins.left.fromString(own_margin_left, _t("auto"));
	} else {
		css_length_set_zero(m_css_margins.left);
	}

	const tchar_t* own_margin_right = get_style_property_own(_t("margin-right"));
	if(own_margin_right) {
		m_css_margins.right.fromString(own_margin_right, _t("auto"));
	} else {
		css_length_set_zero(m_css_margins.right);
	}

	const tchar_t* own_margin_top = get_style_property_own(_t("margin-top"));
	if(own_margin_top) {
		m_css_margins.top.fromString(own_margin_top, _t("auto"));
	} else {
		css_length_set_zero(m_css_margins.top);
	}

	const tchar_t* own_margin_bottom = get_style_property_own(_t("margin-bottom"));
	if(own_margin_bottom) {
		m_css_margins.bottom.fromString(own_margin_bottom, _t("auto"));
	} else {
		css_length_set_zero(m_css_margins.bottom);
	}

	const tchar_t* own_padding_left = get_style_property_own(_t("padding-left"));
	if(own_padding_left) {
		m_css_padding.left.fromString(own_padding_left, _t(""));
	} else {
		css_length_set_zero(m_css_padding.left);
	}

	const tchar_t* own_padding_right = get_style_property_own(_t("padding-right"));
	if(own_padding_right) {
		m_css_padding.right.fromString(own_padding_right, _t(""));
	} else {
		css_length_set_zero(m_css_padding.right);
	}

	const tchar_t* own_padding_top = get_style_property_own(_t("padding-top"));
	if(own_padding_top) {
		m_css_padding.top.fromString(own_padding_top, _t(""));
	} else {
		css_length_set_zero(m_css_padding.top);
	}

	const tchar_t* own_padding_bottom = get_style_property_own(_t("padding-bottom"));
	if(own_padding_bottom) {
		m_css_padding.bottom.fromString(own_padding_bottom, _t(""));
	} else {
		css_length_set_zero(m_css_padding.bottom);
	}

	const tchar_t* own_border_left_width = get_style_property_own(_t("border-left-width"));
	if(own_border_left_width) {
		m_css_borders.left.width.fromString(own_border_left_width, border_width_strings);
	} else {
		css_length_set_predef0(m_css_borders.left.width);
	}

	const tchar_t* own_border_right_width = get_style_property_own(_t("border-right-width"));
	if(own_border_right_width) {
		m_css_borders.right.width.fromString(own_border_right_width, border_width_strings);
	} else {
		css_length_set_predef0(m_css_borders.right.width);
	}

	const tchar_t* own_border_top_width = get_style_property_own(_t("border-top-width"));
	if(own_border_top_width) {
		m_css_borders.top.width.fromString(own_border_top_width, border_width_strings);
	} else {
		css_length_set_predef0(m_css_borders.top.width);
	}

	const tchar_t* own_border_bottom_width = get_style_property_own(_t("border-bottom-width"));
	if(own_border_bottom_width) {
		m_css_borders.bottom.width.fromString(own_border_bottom_width, border_width_strings);
	} else {
		css_length_set_predef0(m_css_borders.bottom.width);
	}

	const tchar_t* own_border_left_color = get_style_property_own(_t("border-left-color"));
	m_css_borders.left.color = own_border_left_color ? web_color::from_string(own_border_left_color, doc->container()) : web_color();
	const tchar_t* own_border_left_style = get_style_property_own(_t("border-left-style"));
	m_css_borders.left.style = own_border_left_style ? (border_style) value_index(own_border_left_style, border_style_strings, border_style_none) : border_style_none;

	const tchar_t* own_border_right_color = get_style_property_own(_t("border-right-color"));
    m_css_borders.right.color = own_border_right_color ? web_color::from_string(own_border_right_color, doc->container()) : web_color();
	const tchar_t* own_border_right_style = get_style_property_own(_t("border-right-style"));
	m_css_borders.right.style = own_border_right_style ? (border_style) value_index(own_border_right_style, border_style_strings, border_style_none) : border_style_none;

	const tchar_t* own_border_top_color = get_style_property_own(_t("border-top-color"));
    m_css_borders.top.color = own_border_top_color ? web_color::from_string(own_border_top_color, doc->container()) : web_color();
	const tchar_t* own_border_top_style = get_style_property_own(_t("border-top-style"));
	m_css_borders.top.style = own_border_top_style ? (border_style) value_index(own_border_top_style, border_style_strings, border_style_none) : border_style_none;

	const tchar_t* own_border_bottom_color = get_style_property_own(_t("border-bottom-color"));
    m_css_borders.bottom.color = own_border_bottom_color ? web_color::from_string(own_border_bottom_color, doc->container()) : web_color();
	const tchar_t* own_border_bottom_style = get_style_property_own(_t("border-bottom-style"));
	m_css_borders.bottom.style = own_border_bottom_style ? (border_style) value_index(own_border_bottom_style, border_style_strings, border_style_none) : border_style_none;

	const tchar_t* own_radius_top_left_x = get_style_property_own(_t("border-top-left-radius-x"));
	if(own_radius_top_left_x) {
		m_css_borders.radius.top_left_x.fromString(own_radius_top_left_x);
	} else {
		css_length_set_zero(m_css_borders.radius.top_left_x);
	}
	const tchar_t* own_radius_top_left_y = get_style_property_own(_t("border-top-left-radius-y"));
	if(own_radius_top_left_y) {
		m_css_borders.radius.top_left_y.fromString(own_radius_top_left_y);
	} else {
		css_length_set_zero(m_css_borders.radius.top_left_y);
	}

	const tchar_t* own_radius_top_right_x = get_style_property_own(_t("border-top-right-radius-x"));
	if(own_radius_top_right_x) {
		m_css_borders.radius.top_right_x.fromString(own_radius_top_right_x);
	} else {
		css_length_set_zero(m_css_borders.radius.top_right_x);
	}
	const tchar_t* own_radius_top_right_y = get_style_property_own(_t("border-top-right-radius-y"));
	if(own_radius_top_right_y) {
		m_css_borders.radius.top_right_y.fromString(own_radius_top_right_y);
	} else {
		css_length_set_zero(m_css_borders.radius.top_right_y);
	}

	const tchar_t* own_radius_bottom_right_x = get_style_property_own(_t("border-bottom-right-radius-x"));
	if(own_radius_bottom_right_x) {
		m_css_borders.radius.bottom_right_x.fromString(own_radius_bottom_right_x);
	} else {
		css_length_set_zero(m_css_borders.radius.bottom_right_x);
	}
	const tchar_t* own_radius_bottom_right_y = get_style_property_own(_t("border-bottom-right-radius-y"));
	if(own_radius_bottom_right_y) {
		m_css_borders.radius.bottom_right_y.fromString(own_radius_bottom_right_y);
	} else {
		css_length_set_zero(m_css_borders.radius.bottom_right_y);
	}

	const tchar_t* own_radius_bottom_left_x = get_style_property_own(_t("border-bottom-left-radius-x"));
	if(own_radius_bottom_left_x) {
		m_css_borders.radius.bottom_left_x.fromString(own_radius_bottom_left_x);
	} else {
		css_length_set_zero(m_css_borders.radius.bottom_left_x);
	}
	const tchar_t* own_radius_bottom_left_y = get_style_property_own(_t("border-bottom-left-radius-y"));
	if(own_radius_bottom_left_y) {
		m_css_borders.radius.bottom_left_y.fromString(own_radius_bottom_left_y);
	} else {
		css_length_set_zero(m_css_borders.radius.bottom_left_y);
	}

	doc->cvt_units(m_css_borders.radius.bottom_left_x,			m_font_size);
	doc->cvt_units(m_css_borders.radius.bottom_left_y,			m_font_size);
	doc->cvt_units(m_css_borders.radius.bottom_right_x,			m_font_size);
	doc->cvt_units(m_css_borders.radius.bottom_right_y,			m_font_size);
	doc->cvt_units(m_css_borders.radius.top_left_x,				m_font_size);
	doc->cvt_units(m_css_borders.radius.top_left_y,				m_font_size);
	doc->cvt_units(m_css_borders.radius.top_right_x,				m_font_size);
	doc->cvt_units(m_css_borders.radius.top_right_y,				m_font_size);

	doc->cvt_units(m_css_text_indent,								m_font_size);

	m_margins.left		= doc->cvt_units(m_css_margins.left,		m_font_size);
	m_margins.right		= doc->cvt_units(m_css_margins.right,		m_font_size);
	m_margins.top		= doc->cvt_units(m_css_margins.top,		m_font_size);
	m_margins.bottom	= doc->cvt_units(m_css_margins.bottom,	m_font_size);

	m_padding.left		= doc->cvt_units(m_css_padding.left,		m_font_size);
	m_padding.right		= doc->cvt_units(m_css_padding.right,		m_font_size);
	m_padding.top		= doc->cvt_units(m_css_padding.top,		m_font_size);
	m_padding.bottom	= doc->cvt_units(m_css_padding.bottom,	m_font_size);

	m_borders.left		= doc->cvt_units(m_css_borders.left.width,	m_font_size);
	m_borders.right		= doc->cvt_units(m_css_borders.right.width,	m_font_size);
	m_borders.top		= doc->cvt_units(m_css_borders.top.width,		m_font_size);
	m_borders.bottom	= doc->cvt_units(m_css_borders.bottom.width,	m_font_size);
	if(profile_enabled)
	{
		parse_style_profile_add(g_parse_style_profile.box_ms, part_start);
		part_start = sys_tic_ms(0);
	}

	const tchar_t* own_line_height = get_style_property_own(_t("line-height"));
	m_lh_factor = 0.0f;
	if(own_line_height)
	{
		css_length line_height;
		line_height.fromString(own_line_height, _t("normal"));
		if(line_height.is_predefined())
		{
			m_line_height = m_font_metrics.height;
			m_lh_predefined = true;
		}
		else if(line_height.units() == css_units_none)
		{
			/* Unitless number: the computed value IS the number, so it inherits
			 * as a factor and every descendant re-multiplies by its own font
			 * size. Inheriting the pixel product instead gave a 3rem heading
			 * under html{line-height:1.5} a 24px line box and overlapping lines
			 * (workspace.google.com). */
			m_lh_factor = line_height.val();
			m_line_height = (int) (line_height.val() * m_font_size);
			m_lh_predefined = false;
		}
		else
		{
			m_line_height =  doc->cvt_units(line_height,	m_font_size, m_font_size);
			m_lh_predefined = false;
		}
	}
	else if(el_parent)
	{
		/* 'normal' is not an inherited pixel value: every element resolves it
		 * from its OWN font (CSS 2.1 10.8.1). Inheriting the parent's computed
		 * height made rokid.com's html{font-size:1px} rem trick collapse every
		 * normal line box in the page to a 1px line. */
		if(el_parent->is_line_height_normal())
		{
			m_line_height = m_font_metrics.height;
			m_lh_predefined = true;
		} else if(el_parent->line_height_factor() > 0.0f)
		{
			m_lh_factor = el_parent->line_height_factor();
			m_line_height = (int) (m_lh_factor * m_font_size);
			m_lh_predefined = false;
		} else
		{
			m_line_height = el_parent->line_height();
			m_lh_predefined = false;
		}
	}
	else
	{
		m_line_height = m_font_metrics.height;
		m_lh_predefined = true;
	}


	if(m_display == display_list_item)
	{
		const tchar_t* list_type = get_style_property(_t("list-style-type"), true, _t("disc"));
		m_list_style_type = (list_style_type) value_index(list_type, list_style_type_strings, list_style_type_disc);

		const tchar_t* list_pos = get_style_property(_t("list-style-position"), true, _t("outside"));
		m_list_style_position = (list_style_position) value_index(list_pos, list_style_position_strings, list_style_position_outside);

		const tchar_t* list_image = get_style_property(_t("list-style-image"), true, 0);
		if(list_image && list_image[0])
		{
			tstring url;
			css::parse_css_url(list_image, url);

			const tchar_t* list_image_baseurl = get_style_property(_t("list-style-image-baseurl"), true, 0);
			doc->container()->load_image(url.c_str(), list_image_baseurl, true);
		}

	}
	if(profile_enabled)
	{
		parse_style_profile_add(g_parse_style_profile.line_list_ms, part_start);
		part_start = sys_tic_ms(0);
	}

	parse_background();
	if(profile_enabled)
	{
		parse_style_profile_add(g_parse_style_profile.background_ms, part_start);
	}

	/* ---- CSS animation trigger (Phase 2) ----
	 * Parse transition/animation declarations from the freshly cascaded
	 * m_style, detect changes, and enqueue active_anim entries on the
	 * document timeline. Only opacity is interpolable in this phase; other
	 * properties declared in transition/animation are captured but snap to
	 * their end state (no half-animated garbage). */
	{
		std::vector<anim_declaration> new_transitions;
		std::vector<anim_declaration> new_animations;
		parse_transition_declarations(m_style.properties(), new_transitions);
		parse_animation_declarations(m_style.properties(), new_animations);

		document* adoc = get_document();
		if(adoc && !adoc->animations_disabled())
		{
			uint64_t now = sys_tic_ms(0);

			/* Transition trigger: fire when the computed opacity changed and
			 * a transition declaration covers it. */
			if(fabsf(m_opacity - old_opacity_for_transition) > 0.0001f)
			{
				for(size_t ti = 0; ti < new_transitions.size(); ti++)
				{
					const anim_declaration& tr = new_transitions[ti];
					bool applies = (tr.property == _t("all") || tr.property == _t("opacity"));
					if(!applies || tr.duration_ms <= 0) continue;
					active_anim a;
					a.element      = this;
					a.is_transition = true;
					a.property     = _t("opacity");
					char fbuf[32], tbuf[32];
					snprintf(fbuf, sizeof(fbuf), "%.4f", old_opacity_for_transition);
					snprintf(tbuf, sizeof(tbuf), "%.4f", m_opacity);
					a.from_str     = fbuf;
					a.to_str       = tbuf;
					a.start_ms     = now;
					a.duration_ms  = tr.duration_ms;
					a.delay_ms     = tr.delay_ms;
					a.timing       = tr.timing;
					a.iteration_count = 1.0f;
					a.direction    = anim_dir_normal;
					a.fill_mode    = tr.fill_mode;
					a.play_state   = tr.play_state;
					adoc->start_animation(a);
					break;  /* one transition per property */
				}
			}

			/* Transform transition trigger: fire when the computed transform
			 * string changed and a transition declaration covers it. Empty and
			 * "none" both mean identity, so a class flip between them is not a
			 * change. from/to are passed as raw strings; interpolate_property
			 * resolves each to a matrix and lerps the components. */
			{
				tstring old_xf = old_transform_for_transition;
				tstring new_xf = m_transform_str;
				trim(old_xf);
				trim(new_xf);
				bool old_none = old_xf.empty() || old_xf == _t("none");
				bool new_none = new_xf.empty() || new_xf == _t("none");
				bool xf_changed = (old_none != new_none) ||
					(!old_none && !new_none && old_xf != new_xf);
				if(xf_changed)
				{
					for(size_t ti = 0; ti < new_transitions.size(); ti++)
					{
						const anim_declaration& tr = new_transitions[ti];
						bool applies = (tr.property == _t("all") || tr.property == _t("transform"));
						if(!applies || tr.duration_ms <= 0) continue;
						active_anim a;
						a.element      = this;
						a.is_transition = true;
						a.property     = _t("transform");
						a.from_str     = old_none ? _t("none") : old_xf;
						a.to_str       = new_none ? _t("none") : new_xf;
						a.start_ms     = now;
						a.duration_ms  = tr.duration_ms;
						a.delay_ms     = tr.delay_ms;
						a.timing       = tr.timing;
						a.iteration_count = 1.0f;
						a.direction    = anim_dir_normal;
						a.fill_mode    = tr.fill_mode;
						a.play_state   = tr.play_state;
						adoc->start_animation(a);
						break;  /* one transition per property */
					}
				}
			}

			/* Animation trigger: start (or restart) each @keyframes animation
			 * whose declaration set changed since the last parse_styles call.
			 * Comparing name+duration is enough to detect a meaningful change
			 * without restarting on every re-cascade that doesn't touch the
			 * animation shorthand. */
			bool anims_changed = (m_animations.size() != new_animations.size());
			if(!anims_changed)
			{
				for(size_t i = 0; i < m_animations.size(); i++)
				{
					if(m_animations[i].name != new_animations[i].name ||
					   m_animations[i].duration_ms != new_animations[i].duration_ms ||
					   m_animations[i].iteration_count != new_animations[i].iteration_count)
					{
						anims_changed = true;
						break;
					}
				}
			}
			if(anims_changed)
			{
				for(size_t ai = 0; ai < new_animations.size(); ai++)
				{
					const anim_declaration& an = new_animations[ai];
					if(an.name.empty() || an.name == _t("none")) continue;
					if(an.duration_ms <= 0) continue;
					active_anim a;
					a.element        = this;
					a.is_transition  = false;
					a.keyframes_name = an.name;
					a.start_ms       = now;
					a.duration_ms    = an.duration_ms;
					a.delay_ms       = an.delay_ms;
					a.timing         = an.timing;
					a.iteration_count = an.iteration_count;
					a.direction      = an.direction;
					a.fill_mode      = an.fill_mode;
					a.play_state     = an.play_state;
					adoc->start_animation(a);
				}
			}
		}

		m_transitions = std::move(new_transitions);
		m_animations  = std::move(new_animations);
	}

	/* A restyle (is_reparse) must reach the whole subtree: jsRestyleSubtree
	 * re-cascades every descendant's m_style, but only this recursive walk
	 * re-resolves the computed values (display and friends). Without it a
	 * class toggle on an ancestor (apple.com flips html.no-js to html.js in
	 * a head script) leaves descendants painting stale resolved values. */
	if(step_parse)
	{
		step_doc->style_step_stamp(this);
	}
	if(profile_enabled)
		part_start = sys_tic_ms(0);
	for(auto& el : m_children)
	{
		if(step_parse && step_doc->style_step_exhausted())
			break;
		el->parse_styles(is_reparse);
	}
	if(step_parse && !step_doc->style_step_exhausted())
	{
		m_step_done = true; /* subtree fully covered in this epoch */
	}
	if(profile_enabled)
	{
		parse_style_profile_add(g_parse_style_profile.child_ms, part_start);
	}
}

int litehtml::html_tag::render( int x, int y, int max_width, bool second_pass )
{
	if (m_display == display_table || m_display == display_inline_table)
	{
		return render_table(x, y, max_width, second_pass);
	}
	else if (m_display == display_flex || m_display == display_inline_flex)
	{
		return render_flex(x, y, max_width, second_pass);
	}
	else if (m_display == display_grid || m_display == display_inline_grid)
	{
		return render_grid(x, y, max_width, second_pass);
	}
	else
	{
		return render_box(x, y, max_width, second_pass);
	}
}

bool litehtml::html_tag::is_white_space() const
{
	return false;
}

int litehtml::html_tag::get_font_size() const
{
	return m_font_size;
}

int litehtml::html_tag::get_base_line()
{
	if(is_replaced())
	{
		return 0;
	}
	int bl = 0;
	if(!m_boxes.empty())
	{
		bl = m_boxes.back()->baseline() + content_margins_bottom();
	}
	return bl;
}

void litehtml::html_tag::init()
{
	if (m_display == display_table || m_display == display_inline_table)
	{
		if (m_grid)
		{
			m_grid->clear();
		}
		else
		{
			m_grid = std::unique_ptr<table_grid>(new table_grid());
		}

		go_inside_table 		table_selector;
		table_rows_selector		row_selector;
		table_cells_selector	cell_selector;

		elements_iterator row_iter(this, &table_selector, &row_selector);

		element::ptr row = row_iter.next(false);
		while (row)
		{
			m_grid->begin_row(row);

			elements_iterator cell_iter(row, &table_selector, &cell_selector);
			element::ptr cell = cell_iter.next();
			while (cell)
			{
				m_grid->add_cell(cell);

				cell = cell_iter.next(false);
			}
			row = row_iter.next(false);
		}

		m_grid->finish();
	}

	for (auto& el : m_children)
	{
		el->init();
	}
}

int litehtml::html_tag::select(const css_selector& selector, bool apply_pseudo)
{
	select_scope_t scope;
	int right_res = select(selector.m_right, apply_pseudo);
	if(right_res == select_no_match)
	{
		return select_no_match;
	}
	element::ptr el_parent = parent();
	if(selector.m_left)
	{
		if (!el_parent)
		{
			return select_no_match;
		}
		switch(selector.m_combinator)
		{
		case combinator_descendant:
			{
				bool is_pseudo = false;
				element::ptr res = find_ancestor(*selector.m_left, apply_pseudo, &is_pseudo);
				if(!res)
				{
					return select_no_match;
				} else
				{
					if(is_pseudo)
					{
						right_res |= select_match_pseudo_class;
					}
				}
			}
			break;
		case combinator_child:
			{
				int res = el_parent->select(*selector.m_left, apply_pseudo);
				if(res == select_no_match)
				{
					return select_no_match;
				} else
				{
					if(right_res != select_match_pseudo_class)
					{
						right_res |= res;
					}
				}
			}
			break;
		case combinator_adjacent_sibling:
			{
				bool is_pseudo = false;
				element::ptr res = el_parent->find_adjacent_sibling(this, *selector.m_left, apply_pseudo, &is_pseudo);
				if(!res)
				{
					return select_no_match;
				} else
				{
					if(is_pseudo)
					{
						right_res |= select_match_pseudo_class;
					}
				}
			}
			break;
		case combinator_general_sibling:
			{
				bool is_pseudo = false;
				element::ptr res =  el_parent->find_sibling(this, *selector.m_left, apply_pseudo, &is_pseudo);
				if(!res)
				{
					return select_no_match;
				} else
				{
					if(is_pseudo)
					{
						right_res |= select_match_pseudo_class;
					}
				}
			}
			break;
		default:
			right_res = select_no_match;
		}
	}
	return right_res;
}

/* Split a selector list on top-level commas: commas inside parentheses or
 * brackets belong to a nested pseudo-class argument, not to the list. */
static void split_selector_list(const litehtml::tstring& txt, std::vector<litehtml::tstring>& out)
{
	int depth = 0;
	litehtml::tstring cur;
	for(litehtml::tstring::const_iterator it = txt.begin(); it != txt.end(); ++it)
	{
		litehtml::tchar_t ch = *it;
		if(ch == '(' || ch == '[') depth++;
		else if(ch == ')' || ch == ']') depth--;
		if(ch == ',' && depth == 0)
		{
			out.push_back(cur);
			cur.clear();
			continue;
		}
		cur += ch;
	}
	out.push_back(cur);
}

/* Match an element against a selector list (the argument of :is/:where/:not).
 * Returns true when any member of the list matches the element. */
static bool match_selector_list(litehtml::html_tag* el, const litehtml::tstring& param, bool apply_pseudo)
{
	std::vector<litehtml::tstring> parts;
	split_selector_list(param, parts);
	for(size_t i = 0; i < parts.size(); i++)
	{
		litehtml::tstring part = parts[i];
		litehtml::trim(part);
		if(part.empty()) continue;
		litehtml::css_selector sel(nullptr);
		if(!sel.parse(part)) continue;
		if(el->select(sel, apply_pseudo) != litehtml::select_no_match)
		{
			return true;
		}
	}
	return false;
}

/* :has() subset. The argument is a relative selector list anchored at `el`.
 * We support the two forms pages actually ship:
 *   :has(> SEL)  - a direct child matching SEL
 *   :has(SEL)    - any descendant matching SEL (implicit descendant combinator)
 * Each list member is matched with the normal css_selector engine, so compound
 * arguments like :has(:nth-child(2)) or :has(a.button) work. apple.com drives
 * its CTA grid with '.tile-ctas:has(:nth-child(2))'. */
static bool has_matching_descendant(litehtml::html_tag* el, const litehtml::tstring& param, bool apply_pseudo)
{
	litehtml::tstring arg = param;
	litehtml::trim(arg);
	bool child_only = false;
	if(arg.size() && arg[0] == _t('>'))
	{
		child_only = true;
		arg = arg.substr(1);
		litehtml::trim(arg);
	}
	/* Strip other leading combinators we don't model (+, ~): fall back to a
	 * descendant search rather than dropping the whole rule. */
	while(arg.size() && (arg[0] == _t('+') || arg[0] == _t('~')))
	{
		arg = arg.substr(1);
		litehtml::trim(arg);
	}
	if(arg.empty()) return false;

	/* Iterative DFS over descendants (no recursion: live trees can be deep). */
	std::vector<litehtml::element::ptr> stack;
	for(size_t i = 0; i < el->get_children_count(); i++)
	{
		litehtml::element::ptr c = el->get_child((int)i);
		if(c) stack.push_back(c);
	}
	int guard = 0;
	while(!stack.empty() && guard++ < 4096)
	{
		litehtml::element::ptr cur = stack.back();
		stack.pop_back();
		if(cur->is_html_tag())
		{
			litehtml::html_tag* t = static_cast<litehtml::html_tag*>(cur);
			if(match_selector_list(t, arg, apply_pseudo)) return true;
			if(!child_only)
			{
				for(size_t i = 0; i < cur->get_children_count(); i++)
				{
					litehtml::element::ptr c = cur->get_child((int)i);
					if(c) stack.push_back(c);
				}
			}
		}
		else if(!child_only)
		{
			for(size_t i = 0; i < cur->get_children_count(); i++)
			{
				litehtml::element::ptr c = cur->get_child((int)i);
				if(c) stack.push_back(c);
			}
		}
	}
	return false;
}

/* --- Attribute / state pseudo-class helpers (Phase 1.2) -------------------- */

/* True when the element is a form control the CSS pseudo-classes :enabled,
 * :disabled, :required, :optional, :read-only, :read-write, :default apply
 * to. Kept in one place so the individual matchers agree on the tag set. */
static bool is_form_control(litehtml::html_tag* el)
{
	const litehtml::tchar_t* tag = el->get_tagName();
	if(!tag) return false;
	return !t_strcasecmp(tag, _t("input")) ||
		   !t_strcasecmp(tag, _t("textarea")) ||
		   !t_strcasecmp(tag, _t("select")) ||
		   !t_strcasecmp(tag, _t("button"));
}

/* :any-link / :link — element that is a hyperlink source. */
static bool is_link_element(litehtml::html_tag* el)
{
	const litehtml::tchar_t* tag = el->get_tagName();
	if(!tag) return false;
	const litehtml::tchar_t* href = el->get_attr(_t("href"), nullptr);
	if(!href || !href[0]) return false;
	return !t_strcasecmp(tag, _t("a")) ||
		   !t_strcasecmp(tag, _t("area")) ||
		   !t_strcasecmp(tag, _t("link"));
}

/* :checked — checkbox/radio input with the checked attribute, or an <option>
 * with the selected attribute. We deliberately do not track the live checked
 * state mutated by user clicks; the DOM attribute is what CSS reads for
 * cascade-time selection, and dynamic re-check is out of scope here. */
static bool is_checked_element(litehtml::html_tag* el)
{
	const litehtml::tchar_t* tag = el->get_tagName();
	if(!tag) return false;
	if(!t_strcasecmp(tag, _t("input")))
	{
		const litehtml::tchar_t* type = el->get_attr(_t("type"), nullptr);
		if(!type) return false;
		if(t_strcasecmp(type, _t("checkbox")) && t_strcasecmp(type, _t("radio")))
		{
			return false;
		}
		return el->get_attr(_t("checked"), nullptr) != nullptr;
	}
	if(!t_strcasecmp(tag, _t("option")))
	{
		return el->get_attr(_t("selected"), nullptr) != nullptr;
	}
	return false;
}

/* :dir(ltr|rtl) — nearest ancestor (or self) with an explicit dir attribute
 * decides; if none carries one, the document direction defaults to ltr per
 * HTML. We do not implement the unicode-bidi heuristic. */
static bool matches_dir(litehtml::html_tag* el, const litehtml::tstring& want)
{
	litehtml::tstring want_lc = want;
	litehtml::trim(want_lc);
	litehtml::lcase(want_lc);
	if(want_lc != _t("ltr") && want_lc != _t("rtl"))
	{
		return false;
	}
	litehtml::element* cur = el;
	int guard = 0;
	while(cur && guard++ < 128)
	{
		const litehtml::tchar_t* d = cur->get_attr(_t("dir"), nullptr);
		if(d && d[0])
		{
			litehtml::tstring dv = d;
			litehtml::lcase(dv);
			return dv == want_lc;
		}
		cur = cur->parent();
	}
	return want_lc == _t("ltr");
}

/* :focus-within — self or any ancestor currently carries the runtime
 * "focus" pseudo-class. The engine's setFocus() is responsible for adding
 * and removing "focus" via set_pseudo_class(). */
static bool has_focus_within(litehtml::html_tag* el)
{
	litehtml::element* cur = el;
	int guard = 0;
	while(cur && guard++ < 128)
	{
		if(cur->is_html_tag())
		{
			litehtml::html_tag* t = static_cast<litehtml::html_tag*>(cur);
			const litehtml::string_vector& pcs = t->pseudo_classes();
			if(std::find(pcs.begin(), pcs.end(), litehtml::tstring(_t("focus"))) != pcs.end())
			{
				return true;
			}
		}
		cur = cur->parent();
	}
	return false;
}

int litehtml::html_tag::select(const css_element_selector& selector, bool apply_pseudo)
{
	select_element_scope_t scope;
	if(!selector.m_tag.empty() && selector.m_tag != _t("*"))
	{
		if(selector.m_tag != m_tag)
		{
			return select_no_match;
		}
	}

	int res = select_match;
	element::ptr el_parent = parent();

	for(css_attribute_selector::vector::const_iterator i = selector.m_attrs.begin(); i != selector.m_attrs.end(); i++)
	{
		const tchar_t* attr_value = get_attr(i->attribute.c_str());
		switch(i->condition)
		{
		case select_exists:
			if(!attr_value)
			{
				return select_no_match;
			}
			break;
		case select_equal:
			if(!attr_value)
			{
				return select_no_match;
			} else 
			{
				if(i->attribute == _t("class"))
				{
					const string_vector & tokens1 = m_class_values;
					const string_vector & tokens2 = i->class_val;
					bool found = true;
					for(string_vector::const_iterator str1 = tokens2.begin(); str1 != tokens2.end() && found; str1++)
					{
						bool f = false;
						for(string_vector::const_iterator str2 = tokens1.begin(); str2 != tokens1.end() && !f; str2++)
						{
							if( !t_strcasecmp(str1->c_str(), str2->c_str()) )
							{
								f = true;
							}
						}
						if(!f)
						{
							found = false;
						}
					}
					if(!found)
					{
						return select_no_match;
					}
				} else
				{
					if( t_strcasecmp(i->val.c_str(), attr_value) )
					{
						return select_no_match;
					}
				}
			}
			break;
		case select_contain_str:
			if(!attr_value)
			{
				return select_no_match;
			} else if(!t_strstr(attr_value, i->val.c_str()))
			{
				return select_no_match;
			}
			break;
		case select_start_str:
			if(!attr_value)
			{
				return select_no_match;
			} else if(t_strncmp(attr_value, i->val.c_str(), i->val.length()))
			{
				return select_no_match;
			}
			break;
		case select_end_str:
			if(!attr_value)
			{
				return select_no_match;
			} else if(t_strncmp(attr_value, i->val.c_str(), i->val.length()))
			{
				const tchar_t* s = attr_value + t_strlen(attr_value) - i->val.length() - 1;
				if(s < attr_value)
				{
					return select_no_match;
				}
				if(i->val != s)
				{
					return select_no_match;
				}
			}
			break;
		case select_pseudo_element:
			if(i->val == _t("after"))
			{
				res |= select_match_with_after;
			} else if(i->val == _t("before"))
			{
				res |= select_match_with_before;
			} else if(i->val == _t("-webkit-slider-thumb") ||
					  i->val == _t("-webkit-slider-runnable-track") ||
					  i->val == _t("-moz-range-thumb") ||
					  i->val == _t("-moz-range-track") ||
					  i->val == _t("-moz-range-progress"))
			{
				/* Form-widget part pseudo-element: the rule paints a sub-part
				 * of a replaced control (range slider thumb/track). Match it and
				 * let the dispatch sites hand the block to the element; unknown
				 * pseudo-elements still refuse to match. */
				res |= select_match_with_widget;
			} else
			{
				return select_no_match;
			}
			break;
		case select_pseudo_class:
			if(apply_pseudo)
			{
				tstring selector_param;
				tstring	selector_name;

				tstring::size_type begin	= i->val.find_first_of(_t('('));
				tstring::size_type end		= (begin == tstring::npos) ? tstring::npos : find_close_bracket(i->val, begin);
				if(begin != tstring::npos && end != tstring::npos)
				{
					selector_param = i->val.substr(begin + 1, end - begin - 1);
				}
				if(begin != tstring::npos)
				{
					selector_name = i->val.substr(0, begin);
					litehtml::trim(selector_name);
				} else
				{
					selector_name = i->val;
				}

				int selector = value_index(selector_name.c_str(), pseudo_class_strings);

				/* Structural pseudo-classes need a parent; :root is the
				 * opposite - it matches exactly the parentless element. :has()
				 * inspects descendants, not the parent, so it is exempt too.
				 * Attribute/state pseudo-classes (:focus-within, :focus-visible,
				 * :dir(), :checked, :disabled, :enabled, :required, :optional,
				 * :read-only, :read-write, :empty, :any-link, :default) match on
				 * self or ancestors, so they must not be rejected when the
				 * element happens to be a root. */
				auto parentless_ok = [](int s) {
					return s == pseudo_class_root ||
						   s == pseudo_class_has ||
						   s == pseudo_class_focus_within ||
						   s == pseudo_class_focus_visible ||
						   s == pseudo_class_dir ||
						   s == pseudo_class_checked ||
						   s == pseudo_class_disabled ||
						   s == pseudo_class_enabled ||
						   s == pseudo_class_required ||
						   s == pseudo_class_optional ||
						   s == pseudo_class_read_only ||
						   s == pseudo_class_read_write ||
						   s == pseudo_class_empty ||
						   s == pseudo_class_any_link ||
						   s == pseudo_class_default;
				};
				if(!el_parent && !parentless_ok(selector))
				{
					return select_no_match;
				}

				switch(selector)
				{
				case pseudo_class_only_child:
					if (!el_parent->is_only_child(this, false))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_only_of_type:
					if (!el_parent->is_only_child(this, true))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_first_child:
					if (!el_parent->is_nth_child(this, 0, 1, false))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_first_of_type:
					if (!el_parent->is_nth_child(this, 0, 1, true))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_last_child:
					if (!el_parent->is_nth_last_child(this, 0, 1, false))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_last_of_type:
					if (!el_parent->is_nth_last_child(this, 0, 1, true))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_nth_child:
				case pseudo_class_nth_of_type:
				case pseudo_class_nth_last_child:
				case pseudo_class_nth_last_of_type:
					{
						if(selector_param.empty()) return select_no_match;

						int num = 0;
						int off = 0;

						parse_nth_child_params(selector_param, num, off);
						if(!num && !off) return select_no_match;
						switch(selector)
						{
						case pseudo_class_nth_child:
							if (!el_parent->is_nth_child(this, num, off, false))
							{
								return select_no_match;
							}
							break;
						case pseudo_class_nth_of_type:
							if (!el_parent->is_nth_child(this, num, off, true))
							{
								return select_no_match;
							}
							break;
						case pseudo_class_nth_last_child:
							if (!el_parent->is_nth_last_child(this, num, off, false))
							{
								return select_no_match;
							}
							break;
						case pseudo_class_nth_last_of_type:
							if (!el_parent->is_nth_last_child(this, num, off, true))
							{
								return select_no_match;
							}
							break;
						}

					}
					break;
				case pseudo_class_not:
					{
						/* :not() takes a selector list: the element matches
						 * the pseudo-class only when no member matches. */
						if(match_selector_list(this, selector_param, apply_pseudo))
						{
							return select_no_match;
						}
					}
					break;
				case pseudo_class_where:
				case pseudo_class_is:
					{
						/* :where() and :is() are forgiving selector lists:
						 * they match when any member matches (the two differ
						 * only in specificity contribution). */
						if(!match_selector_list(this, selector_param, apply_pseudo))
						{
							return select_no_match;
						}
					}
					break;
				case pseudo_class_has:
					{
						/* :has() matches when a relative selector in its list
						 * matches some descendant (or child, for '> SEL'). */
						if(!has_matching_descendant(this, selector_param, apply_pseudo))
						{
							return select_no_match;
						}
					}
					break;
				case pseudo_class_focus_within:
					/* :focus-within matches when self or any ancestor carries
					 * the runtime "focus" pseudo-class. */
					if(!has_focus_within(this))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_focus_visible:
					/* :focus-visible approximation: we do not track keyboard
					 * vs pointer focus origin, so treat it as an alias for
					 * :focus. Pages that hide focus rings for pointer input
					 * will still show them; better than dropping the rule. */
					if(std::find(m_pseudo_classes.begin(), m_pseudo_classes.end(), tstring(_t("focus"))) == m_pseudo_classes.end())
					{
						return select_no_match;
					}
					break;
				case pseudo_class_dir:
					if(!matches_dir(this, selector_param))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_checked:
					if(!is_checked_element(this))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_disabled:
					/* Form controls with the disabled attribute; button/optgroup/
					 * option/fieldset also honour it. */
					if(!is_form_control(this) &&
					   t_strcasecmp(m_tag.c_str(), _t("optgroup")) &&
					   t_strcasecmp(m_tag.c_str(), _t("option")) &&
					   t_strcasecmp(m_tag.c_str(), _t("fieldset")))
					{
						return select_no_match;
					}
					if(!get_attr(_t("disabled"), nullptr))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_enabled:
					/* Form controls without the disabled attribute. Browsers
					 * also enable elements focusable by default; we stick to
					 * the attribute rule that pages actually rely on. */
					if(!is_form_control(this) &&
					   t_strcasecmp(m_tag.c_str(), _t("optgroup")) &&
					   t_strcasecmp(m_tag.c_str(), _t("option")) &&
					   t_strcasecmp(m_tag.c_str(), _t("fieldset")))
					{
						return select_no_match;
					}
					if(get_attr(_t("disabled"), nullptr))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_required:
					if(!is_form_control(this))
					{
						return select_no_match;
					}
					if(!get_attr(_t("required"), nullptr))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_optional:
					if(!is_form_control(this))
					{
						return select_no_match;
					}
					if(get_attr(_t("required"), nullptr))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_read_only:
					/* Non-form elements are :read-only by default in the spec;
					 * form controls are :read-only when readonly or disabled,
					 * or when the input type is not editable (submit/button/
					 * checkbox/radio/hidden/etc.). We approximate by treating
					 * text-ish inputs and textareas as the only editable set. */
					{
						const tchar_t* tn = m_tag.c_str();
						bool editable = false;
						if(!t_strcasecmp(tn, _t("textarea")))
						{
							editable = true;
						} else if(!t_strcasecmp(tn, _t("input")))
						{
							const tchar_t* ty = get_attr(_t("type"), nullptr);
							if(!ty) ty = _t("text");
							editable = (!t_strcasecmp(ty, _t("text")) ||
										!t_strcasecmp(ty, _t("search")) ||
										!t_strcasecmp(ty, _t("tel")) ||
										!t_strcasecmp(ty, _t("url")) ||
										!t_strcasecmp(ty, _t("email")) ||
										!t_strcasecmp(ty, _t("password")) ||
										!t_strcasecmp(ty, _t("number")));
						}
						if(!editable)
						{
							/* contenteditable also makes an element editable. */
							const tchar_t* ce = get_attr(_t("contenteditable"), nullptr);
							if(ce && t_strcasecmp(ce, _t("false")))
							{
								editable = true;
							}
						}
						if(editable)
						{
							if(get_attr(_t("readonly"), nullptr) || get_attr(_t("disabled"), nullptr))
							{
								/* fall through: read-only matches */
							} else
							{
								return select_no_match;
							}
						}
					}
					break;
				case pseudo_class_read_write:
					{
						const tchar_t* tn = m_tag.c_str();
						bool editable = false;
						if(!t_strcasecmp(tn, _t("textarea")))
						{
							editable = true;
						} else if(!t_strcasecmp(tn, _t("input")))
						{
							const tchar_t* ty = get_attr(_t("type"), nullptr);
							if(!ty) ty = _t("text");
							editable = (!t_strcasecmp(ty, _t("text")) ||
										!t_strcasecmp(ty, _t("search")) ||
										!t_strcasecmp(ty, _t("tel")) ||
										!t_strcasecmp(ty, _t("url")) ||
										!t_strcasecmp(ty, _t("email")) ||
										!t_strcasecmp(ty, _t("password")) ||
										!t_strcasecmp(ty, _t("number")));
						}
						if(!editable)
						{
							const tchar_t* ce = get_attr(_t("contenteditable"), nullptr);
							if(ce && t_strcasecmp(ce, _t("false")))
							{
								editable = true;
							}
						}
						if(!editable) return select_no_match;
						if(get_attr(_t("readonly"), nullptr) || get_attr(_t("disabled"), nullptr))
						{
							return select_no_match;
						}
					}
					break;
				case pseudo_class_empty:
					/* :empty — no element children and no text children.
					 * Comments and CDATA (m_skip=true from construction) do
					 * not affect emptiness per the spec. We approximate by
					 * iterating children and ignoring those whose original
					 * skip flag was set at construction time; text nodes
					 * with any content (even whitespace) make it non-empty. */
					{
						bool empty = true;
						for(size_t ci = 0; ci < get_children_count(); ci++)
						{
							element::ptr c = get_child((int)ci);
							if(!c) continue;
							const tchar_t* cn = c->get_tagName();
							/* Pseudo-elements are not real children. */
							if(cn && cn[0] == _t(':')) continue;
							/* el_comment / el_cdata set m_skip at construction;
							 * el_text may also be marked skip later by line
							 * layout, so we can't rely on skip() alone. Instead
							 * we treat any child that produces non-empty text
							 * OR has children as breaking emptiness. */
							if(c->get_children_count() > 0)
							{
								empty = false;
								break;
							}
							tstring txt;
							c->get_text(txt);
							if(!txt.empty())
							{
								empty = false;
								break;
							}
							/* Comment/CDATA text is stored via set_data and
							 * surfaces through get_text(); they still count
							 * as empty for :empty purposes. Detect them by
							 * the constructor-set skip flag BEFORE line
							 * layout touches it. */
							if(c->skip()) continue;
							empty = false;
							break;
						}
						if(!empty) return select_no_match;
					}
					break;
				case pseudo_class_any_link:
					if(!is_link_element(this))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_default:
					/* :default — the default UI element among a group of
					 * similar elements. We support the two forms pages
					 * actually ship: checkbox/radio with the checked
					 * attribute, and <option> with the selected attribute.
					 * The submit button of a form is not modelled. */
					if(!is_checked_element(this))
					{
						return select_no_match;
					}
					break;
				case pseudo_class_lang:
					{
						trim( selector_param );

						if( !get_document()->match_lang( selector_param ) )
						{
							return select_no_match;
						}
					}
					break;
				case pseudo_class_root:
					/* :root = the document root element; without this, sheets
					 * that define custom properties on :root silently lose
					 * every --var declaration. */
					if(parent())
					{
						return select_no_match;
					}
					break;
				default:
					if(std::find(m_pseudo_classes.begin(), m_pseudo_classes.end(), i->val) == m_pseudo_classes.end())
					{
						return select_no_match;
					}
					break;
				}
			} else
			{
				res |= select_match_pseudo_class;
			}
			break;
		}
	}
	return res;
}

litehtml::element::ptr litehtml::html_tag::find_ancestor(const css_selector& selector, bool apply_pseudo, bool* is_pseudo)
{
	/* Iterative, not recursive: a descendant-combinator selector matched against
	 * a deep live tree used to push one find_ancestor frame per ancestor and
	 * overflow the engine thread stack (w3.org member grid). Walking up in a
	 * loop keeps stack O(1) in tree depth. */
	element::ptr el_parent = parent();
	/* Iteration cap as a cycle guard: a corrupt parent chain (child link pointing
	 * back at an ancestor) would otherwise spin here forever now that the walk is
	 * iterative. Real documents are far shallower than this; a tight cap also
	 * bounds the per-selector ancestor cost on large pages (w3.org). */
	int guard = 0;
	while(el_parent && guard++ < 48)
	{
		int res = el_parent->select(selector, apply_pseudo);
		if(res != select_no_match)
		{
			if(is_pseudo)
			{
				*is_pseudo = (res & select_match_pseudo_class) != 0;
			}
			return el_parent;
		}
		el_parent = el_parent->parent();
	}
	return nullptr;
}

int litehtml::html_tag::get_floats_height(element_float el_float) const
{
	if(is_floats_holder())
	{
		int h = 0;

		bool process = false;

		for(const auto& fb : m_floats_left)
		{
			process = false;
			switch(el_float)
			{
			case float_none:
				process = true;
				break;
			case float_left:
				if (fb.clear_floats == clear_left || fb.clear_floats == clear_both)
				{
					process = true;
				}
				break;
			case float_right:
				if (fb.clear_floats == clear_right || fb.clear_floats == clear_both)
				{
					process = true;
				}
				break;
			}
			if(process)
			{
				if(el_float == float_none)
				{
					h = std::max(h, fb.pos.bottom());
				} else
				{
					h = std::max(h, fb.pos.top());
				}
			}
		}


		for(const auto fb : m_floats_right)
		{
			process = false;
			switch(el_float)
			{
			case float_none:
				process = true;
				break;
			case float_left:
				if (fb.clear_floats == clear_left || fb.clear_floats == clear_both)
				{
					process = true;
				}
				break;
			case float_right:
				if (fb.clear_floats == clear_right || fb.clear_floats == clear_both)
				{
					process = true;
				}
				break;
			}
			if(process)
			{
				if(el_float == float_none)
				{
					h = std::max(h, fb.pos.bottom());
				} else
				{
					h = std::max(h, fb.pos.top());
				}
			}
		}

		return h;
	}
	element::ptr el_parent = parent();
	if (el_parent)
	{
		int h = el_parent->get_floats_height(el_float);
		return h - m_pos.y;
	}
	return 0;
}

int litehtml::html_tag::get_left_floats_height() const
{
	if(is_floats_holder())
	{
		int h = 0;
		if(!m_floats_left.empty())
		{
			for (const auto& fb : m_floats_left)
			{
				h = std::max(h, fb.pos.bottom());
			}
		}
		return h;
	}
	element::ptr el_parent = parent();
	if (el_parent)
	{
		int h = el_parent->get_left_floats_height();
		return h - m_pos.y;
	}
	return 0;
}

int litehtml::html_tag::get_right_floats_height() const
{
	if(is_floats_holder())
	{
		int h = 0;
		if(!m_floats_right.empty())
		{
			for(const auto& fb : m_floats_right)
			{
				h = std::max(h, fb.pos.bottom());
			}
		}
		return h;
	}
	element::ptr el_parent = parent();
	if (el_parent)
	{
		int h = el_parent->get_right_floats_height();
		return h - m_pos.y;
	}
	return 0;
}

int litehtml::html_tag::get_line_left( int y )
{
	if(is_floats_holder())
	{
		if(m_cahe_line_left.is_valid && m_cahe_line_left.hash == y)
		{
			return m_cahe_line_left.val;
		}

		int w = 0;
		for(const auto& fb : m_floats_left)
		{
			if (y >= fb.pos.top() && y < fb.pos.bottom())
			{
				w = std::max(w, fb.pos.right());
				if (w < fb.pos.right())
				{
					break;
				}
			}
		}
		m_cahe_line_left.set_value(y, w);
		return w;
	}
	element::ptr el_parent = parent();
	if (el_parent)
	{
		int w = el_parent->get_line_left(y + m_pos.y);
		if (w < 0)
		{
			w = 0;
		}
		return w - (w ? m_pos.x : 0);
	}
	return 0;
}

int litehtml::html_tag::get_line_right( int y, int def_right )
{
	if(is_floats_holder())
	{
		if(m_cahe_line_right.is_valid && m_cahe_line_right.hash == y)
		{
			if(m_cahe_line_right.is_default)
			{
				return def_right;
			} else
			{
				return std::min(m_cahe_line_right.val, def_right);
			}
		}

		int w = def_right;
		m_cahe_line_right.is_default = true;
		for(const auto& fb : m_floats_right)
		{
			if(y >= fb.pos.top() && y < fb.pos.bottom())
			{
				w = std::min(w, fb.pos.left());
				m_cahe_line_right.is_default = false;
				if(w > fb.pos.left())
				{
					break;
				}
			}
		}
		m_cahe_line_right.set_value(y, w);
		return w;
	}
	element::ptr el_parent = parent();
	if (el_parent)
	{
		int w = el_parent->get_line_right(y + m_pos.y, def_right + m_pos.x);
		return w - m_pos.x;
	}
	return 0;
}


void litehtml::html_tag::get_line_left_right( int y, int def_right, int& ln_left, int& ln_right )
{
	if(is_floats_holder())
	{
		ln_left		= get_line_left(y);
		ln_right	= get_line_right(y, def_right);
	} else
	{
		element::ptr el_parent = parent();
		if (el_parent)
		{
			el_parent->get_line_left_right(y + m_pos.y, def_right + m_pos.x, ln_left, ln_right);
		}
		ln_right -= m_pos.x;
		/* Only translate ln_left when it marks a real float edge. A zero means
		 * "no float up the chain" and must stay zero: subtracting a negative
		 * m_pos.x (e.g. a full-bleed `margin-left:-50vw` ancestor) would
		 * otherwise inflate it into a bogus left inset for every descendant.
		 * This mirrors the `(w ? m_pos.x : 0)` guard in get_line_left(). */
		if(ln_left != 0)
		{
			ln_left -= m_pos.x;
			if(ln_left < 0)
			{
				ln_left = 0;
			}
		}
	}
}

int litehtml::html_tag::fix_line_width( int max_width, element_float flt )
{
	int ret_width = 0;
	if(!m_boxes.empty())
	{
		elements_vector els;
		m_boxes.back()->get_elements(els);
		bool was_cleared = false;
		if(!els.empty() && els.front()->get_clear() != clear_none)
		{
			if(els.front()->get_clear() == clear_both)
			{
				was_cleared = true;
			} else
			{
				if(	(flt == float_left	&& els.front()->get_clear() == clear_left) ||
					(flt == float_right	&& els.front()->get_clear() == clear_right) )
				{
					was_cleared = true;
				}
			}
		}

		if(!was_cleared)
		{
			m_boxes.pop_back();

			for(elements_vector::iterator i = els.begin(); i != els.end(); i++)
			{
				int rw = place_element((*i), max_width);
				if(rw > ret_width)
				{
					ret_width = rw;
				}
			}
		} else
		{
			int line_top = 0;
			if(m_boxes.back()->get_type() == box_line)
			{
				line_top = m_boxes.back()->top();
			} else
			{
				line_top = m_boxes.back()->bottom();
			}

			int line_left	= 0;
			int line_right	= max_width;
			get_line_left_right(line_top, max_width, line_left, line_right);

			if(m_boxes.back()->get_type() == box_line)
			{
				if(m_boxes.size() == 1 && m_list_style_type != list_style_type_none && m_list_style_position == list_style_position_inside)
				{
					int sz_font = get_font_size();
					line_left += sz_font;
				}

				if(m_css_text_indent.val() != 0)
				{
					bool line_box_found = false;
					for(box::vector::iterator iter = m_boxes.begin(); iter < m_boxes.end(); iter++)
					{
						if((*iter)->get_type() == box_line)
						{
							line_box_found = true;
							break;
						}
					}
					if(!line_box_found)
					{
						line_left += m_css_text_indent.calc_percent(max_width);
					}
				}

			}

			elements_vector els;
			m_boxes.back()->new_width(line_left, line_right, els);
			for(auto& el : els)
			{
				int rw = place_element(el, max_width);
				if(rw > ret_width)
				{
					ret_width = rw;
				}
			}
		}
	}

	return ret_width;
}

void litehtml::html_tag::add_float(const element::ptr &el, int x, int y)
{
	if(is_floats_holder())
	{
		floated_box fb;
		fb.pos.x		= el->left() + x;
		fb.pos.y		= el->top()  + y;
		fb.pos.width	= el->width();
		fb.pos.height	= el->height();
		fb.float_side	= el->get_float();
		fb.clear_floats	= el->get_clear();
		fb.el			= el;

		if(fb.float_side == float_left)
		{
			if(m_floats_left.empty())
			{
				m_floats_left.push_back(fb);
			} else
			{
				bool inserted = false;
				for(floated_box::vector::iterator i = m_floats_left.begin(); i != m_floats_left.end(); i++)
				{
					if(fb.pos.right() > i->pos.right())
					{
						m_floats_left.insert(i, std::move(fb));
						inserted = true;
						break;
					}
				}
				if(!inserted)
				{
					m_floats_left.push_back(std::move(fb));
				}
			}
			m_cahe_line_left.invalidate();
		} else if(fb.float_side == float_right)
		{
			if(m_floats_right.empty())
			{
				m_floats_right.push_back(std::move(fb));
			} else
			{
				bool inserted = false;
				for(floated_box::vector::iterator i = m_floats_right.begin(); i != m_floats_right.end(); i++)
				{
					if(fb.pos.left() < i->pos.left())
					{
						m_floats_right.insert(i, std::move(fb));
						inserted = true;
						break;
					}
				}
				if(!inserted)
				{
					m_floats_right.push_back(fb);
				}
			}
			m_cahe_line_right.invalidate();
		}
	} else
	{
		element::ptr el_parent = parent();
		if (el_parent)
		{
			el_parent->add_float(el, x + m_pos.x, y + m_pos.y);
		}
	}
}

int litehtml::html_tag::find_next_line_top( int top, int width, int def_right )
{
	if(is_floats_holder())
	{
		int new_top = top;
		int_vector points;

		for(const auto& fb : m_floats_left)
		{
			if(fb.pos.top() >= top)
			{
				if(find(points.begin(), points.end(), fb.pos.top()) == points.end())
				{
					points.push_back(fb.pos.top());
				}
			}
			if (fb.pos.bottom() >= top)
			{
				if (find(points.begin(), points.end(), fb.pos.bottom()) == points.end())
				{
					points.push_back(fb.pos.bottom());
				}
			}
		}

		for (const auto& fb : m_floats_right)
		{
			if (fb.pos.top() >= top)
			{
				if (find(points.begin(), points.end(), fb.pos.top()) == points.end())
				{
					points.push_back(fb.pos.top());
				}
			}
			if (fb.pos.bottom() >= top)
			{
				if (find(points.begin(), points.end(), fb.pos.bottom()) == points.end())
				{
					points.push_back(fb.pos.bottom());
				}
			}
		}

		if(!points.empty())
		{
			sort(points.begin(), points.end());
			new_top = points.back();

			for(auto pt : points)
			{
				int pos_left	= 0;
				int pos_right	= def_right;
				get_line_left_right(pt, def_right, pos_left, pos_right);

				if(pos_right - pos_left >= width)
				{
					new_top = pt;
					break;
				}
			}
		}
		return new_top;
	}
	element::ptr el_parent = parent();
	if (el_parent)
	{
		int new_top = el_parent->find_next_line_top(top + m_pos.y, width, def_right + m_pos.x);
		return new_top - m_pos.y;
	}
	return 0;
}

void litehtml::html_tag::parse_background()
{
	const tchar_t* bg_color = get_style_property_own(_t("background-color"));
	const tchar_t* bg_position = get_style_property_own(_t("background-position"));
	const tchar_t* bg_size = get_style_property_own(_t("background-size"));
	const tchar_t* bg_attachment = get_style_property_own(_t("background-attachment"));
	const tchar_t* bg_repeat = get_style_property_own(_t("background-repeat"));
	const tchar_t* bg_clip = get_style_property_own(_t("background-clip"));
	const tchar_t* bg_origin = get_style_property_own(_t("background-origin"));
	const tchar_t* bg_image = get_style_property_own(_t("background-image"));
	const tchar_t* bg_baseurl = get_style_property_own(_t("background-image-baseurl"));
	const tchar_t* box_shadow = get_style_property_own(_t("box-shadow"));
	const tchar_t* mask = get_style_property_own(_t("mask"));
	if(!mask) mask = get_style_property_own(_t("-webkit-mask"));
	const tchar_t* mask_image = get_style_property_own(_t("mask-image"));
	if(!mask_image) mask_image = get_style_property_own(_t("-webkit-mask-image"));
	const tchar_t* mask_clip = get_style_property_own(_t("mask-clip"));
	if(!mask_clip) mask_clip = get_style_property_own(_t("-webkit-mask-clip"));
	const tchar_t* mask_composite = get_style_property_own(_t("mask-composite"));
	if(!mask_composite) mask_composite = get_style_property_own(_t("-webkit-mask-composite"));

	m_bg.m_color = bg_color ? web_color::from_string(bg_color, get_document()->container()) : web_color(0, 0, 0, 0);
	m_bg.m_box_shadow = (box_shadow && t_strcasecmp(box_shadow, _t("none"))) ? box_shadow : _t("");
	m_bg.m_mask = (mask && t_strcasecmp(mask, _t("none"))) ? mask : _t("");
	/* Modern sites commonly express the two-layer ring mask entirely through
	 * longhands. Keep the relevant computed values together for the software
	 * painter, which currently implements the content-box exclude/xor subset. */
	if(m_bg.m_mask.empty() && mask_image && t_strcasecmp(mask_image, _t("none")))
	{
		m_bg.m_mask = mask_image;
		if(mask_clip)
		{
			m_bg.m_mask += _t(" ");
			m_bg.m_mask += mask_clip;
		}
	}
	if(!m_bg.m_mask.empty() && mask_composite)
	{
		m_bg.m_mask += _t(" ");
		m_bg.m_mask += mask_composite;
	}
	m_bg.m_position.x.set_value(0, css_units_percentage);
	m_bg.m_position.y.set_value(0, css_units_percentage);
	m_bg.m_position.width.predef(background_size_auto);
	m_bg.m_position.height.predef(background_size_auto);
	m_bg.m_attachment = background_attachment_scroll;
	m_bg.m_repeat = background_repeat_repeat;
	m_bg.m_clip = background_box_border;
	m_bg.m_origin = background_box_padding;
	m_bg.m_image.clear();
	m_bg.m_baseurl.clear();

	if(!bg_position && !bg_size && !bg_attachment && !bg_repeat &&
		!bg_clip && !bg_origin && !bg_image && !bg_baseurl)
	{
		return;
	}

	// parse background-position
	const tchar_t* str = bg_position;
	if(str && str[0])
	{
		string_vector res;
		split_string(str, res, _t(" \t"));
		if(res.size() > 0)
		{
			if(res.size() == 1)
			{
				if( value_in_list(res[0].c_str(), _t("left;right;center")) )
				{
					m_bg.m_position.x.fromString(res[0], _t("left;right;center"));
					m_bg.m_position.y.set_value(50, css_units_percentage);
				} else if( value_in_list(res[0].c_str(), _t("top;bottom;center")) )
				{
					m_bg.m_position.y.fromString(res[0], _t("top;bottom;center"));
					m_bg.m_position.x.set_value(50, css_units_percentage);
				} else
				{
					m_bg.m_position.x.fromString(res[0], _t("left;right;center"));
					m_bg.m_position.y.set_value(50, css_units_percentage);
				}
			} else
			{
				if(value_in_list(res[0].c_str(), _t("left;right")))
				{
					m_bg.m_position.x.fromString(res[0], _t("left;right;center"));
					m_bg.m_position.y.fromString(res[1], _t("top;bottom;center"));
				} else if(value_in_list(res[0].c_str(), _t("top;bottom")))
				{
					m_bg.m_position.x.fromString(res[1], _t("left;right;center"));
					m_bg.m_position.y.fromString(res[0], _t("top;bottom;center"));
				} else if(value_in_list(res[1].c_str(), _t("left;right")))
				{
					m_bg.m_position.x.fromString(res[1], _t("left;right;center"));
					m_bg.m_position.y.fromString(res[0], _t("top;bottom;center"));
				}else if(value_in_list(res[1].c_str(), _t("top;bottom")))
				{
					m_bg.m_position.x.fromString(res[0], _t("left;right;center"));
					m_bg.m_position.y.fromString(res[1], _t("top;bottom;center"));
				} else
				{
					m_bg.m_position.x.fromString(res[0], _t("left;right;center"));
					m_bg.m_position.y.fromString(res[1], _t("top;bottom;center"));
				}
			}

			if(m_bg.m_position.x.is_predefined())
			{
				switch(m_bg.m_position.x.predef())
				{
				case 0:
					m_bg.m_position.x.set_value(0, css_units_percentage);
					break;
				case 1:
					m_bg.m_position.x.set_value(100, css_units_percentage);
					break;
				case 2:
					m_bg.m_position.x.set_value(50, css_units_percentage);
					break;
				}
			}
			if(m_bg.m_position.y.is_predefined())
			{
				switch(m_bg.m_position.y.predef())
				{
				case 0:
					m_bg.m_position.y.set_value(0, css_units_percentage);
					break;
				case 1:
					m_bg.m_position.y.set_value(100, css_units_percentage);
					break;
				case 2:
					m_bg.m_position.y.set_value(50, css_units_percentage);
					break;
				}
			}
		} else
		{
			m_bg.m_position.x.set_value(0, css_units_percentage);
			m_bg.m_position.y.set_value(0, css_units_percentage);
		}
	}

	str = bg_size;
	if(str && str[0])
	{
		string_vector res;
		split_string(str, res, _t(" \t"));
		if(!res.empty())
		{
			m_bg.m_position.width.fromString(res[0], background_size_strings);
			if(res.size() > 1)
			{
				m_bg.m_position.height.fromString(res[1], background_size_strings);
			} else
			{
				m_bg.m_position.height.predef(background_size_auto);
			}
		} else
		{
			m_bg.m_position.width.predef(background_size_auto);
			m_bg.m_position.height.predef(background_size_auto);
		}
	}

	document* doc = get_document();

	doc->cvt_units(m_bg.m_position.x,		m_font_size);
	doc->cvt_units(m_bg.m_position.y,		m_font_size);
	doc->cvt_units(m_bg.m_position.width,	m_font_size);
	doc->cvt_units(m_bg.m_position.height,	m_font_size);

	// parse background_attachment
	if(bg_attachment)
	{
		m_bg.m_attachment = (background_attachment) value_index(
			bg_attachment,
			background_attachment_strings,
			background_attachment_scroll);
	}

	// parse background_attachment
	if(bg_repeat)
	{
		m_bg.m_repeat = (background_repeat) value_index(
			bg_repeat,
			background_repeat_strings,
			background_repeat_repeat);
	}

	// parse background_clip
	if(bg_clip)
	{
		m_bg.m_clip = (background_box) value_index(
			bg_clip,
			background_box_strings,
			background_box_border);
	}

	// parse background_origin
	if(bg_origin)
	{
		m_bg.m_origin = (background_box) value_index(
			bg_origin,
			background_box_strings,
			background_box_content);
	}

	// parse background-image
	if(bg_image && bg_image[0])
	{
		if(!t_strncasecmp(bg_image, _t("linear-gradient("), 16) ||
		   !t_strncasecmp(bg_image, _t("conic-gradient("), 15))
		{
			/* Paint syntax, not a URL: keep the whole function string so the
			 * container can rasterise it; parse_css_url would drop it. */
			m_bg.m_image = bg_image;
		} else
		{
			css::parse_css_url(bg_image, m_bg.m_image);
			if(bg_baseurl)
			{
				m_bg.m_baseurl = bg_baseurl;
			}
		}
	}

	if(!m_bg.m_image.empty() &&
	   t_strncasecmp(m_bg.m_image.c_str(), _t("linear-gradient("), 16) &&
	   t_strncasecmp(m_bg.m_image.c_str(), _t("conic-gradient("), 15))
	{
		doc->container()->load_image(m_bg.m_image.c_str(), m_bg.m_baseurl.empty() ? 0 : m_bg.m_baseurl.c_str(), true);
	}
}

void litehtml::html_tag::add_positioned(const element::ptr &el)
{
	/* Bubble the participant up to the nearest ancestor STACKING CONTEXT, not
	 * merely the nearest positioned ancestor. A z-index:auto positioned box is
	 * transparent for stacking, so el's z-index has to compete at the real
	 * stacking context (CSS 2.1 Appendix E). Coordinates are recovered at paint
	 * time: draw_children_box descends the box tree through every non-stacking-
	 * context box, accumulating m_pos per level, so no offset is stored here. */
	if (is_stacking_context())
	{
		m_positioned.push_back(el);
	} else
	{
		element::ptr el_parent = parent();
		if (el_parent)
		{
			el_parent->add_positioned(el);
		} else
		{
			m_positioned.push_back(el);
		}
	}
}

bool litehtml::html_tag::is_stacking_participant() const
{
	/* Positioned boxes always take part in the positioned painting phase; a
	 * static flex/grid item does too when it carries an explicit z-index. */
	if (m_el_position != element_position_static)
	{
		return true;
	}
	if (!m_z_index_auto && have_parent())
	{
		style_display pd = parent()->get_display();
		if (pd == display_flex || pd == display_inline_flex ||
			pd == display_grid || pd == display_inline_grid)
		{
			return true;
		}
	}
	/* A static box that is nevertheless a stacking context (opacity<1,
	 * isolation:isolate) paints atomically "as if positioned with z-index:0"
	 * (CSS 2.1 App. E step 8). Routing it through the positioned phase is
	 * also the only way its own positioned descendants ever get painted:
	 * fetch_positioned parks them in ITS m_positioned, and the draw_block
	 * walk never calls draw_stacking_context on a plain block. */
	if (have_parent() && (m_isolate || m_opacity < 1.0f))
	{
		return true;
	}
	return false;
}

bool litehtml::html_tag::is_stacking_context() const
{
	/* Real painting boundaries only. A positioned element with z-index:auto is
	 * NOT one, so its z-indexed descendants escape to the nearest ancestor
	 * stacking context instead of being trapped inside it. */
	if (!have_parent())
	{
		return true;
	}
	if (m_el_position == element_position_fixed)
	{
		return true;
	}
	if (m_opacity < 1.0f)
	{
		return true;
	}
	if (m_isolate)
	{
		return true;
	}
	if (!m_z_index_auto)
	{
		if (m_el_position != element_position_static)
		{
			return true;
		}
		if (have_parent())
		{
			style_display pd = parent()->get_display();
			if (pd == display_flex || pd == display_inline_flex ||
				pd == display_grid || pd == display_inline_grid)
			{
				return true;
			}
		}
	}
	return false;
}

void litehtml::html_tag::calc_outlines( int parent_width )
{
	/* Flex items set m_pct_cb_width: per spec their percentage padding/margin
	 * resolve against the flex container content box, while render() hands them
	 * their resolved main size as parent_width. */
	int pcb = m_pct_cb_width > 0 ? m_pct_cb_width : parent_width;

	m_padding.left	= get_document()->cvt_units(m_css_padding.left,	m_font_size, pcb);
	m_padding.right	= get_document()->cvt_units(m_css_padding.right,	m_font_size, pcb);

	m_borders.left	= m_css_borders.left.width.calc_percent(pcb);
	m_borders.right	= m_css_borders.right.width.calc_percent(pcb);

	m_margins.left	= get_document()->cvt_units(m_css_margins.left,	m_font_size, pcb);
	m_margins.right	= get_document()->cvt_units(m_css_margins.right,	m_font_size, pcb);

	m_margins.top		= get_document()->cvt_units(m_css_margins.top,		m_font_size, pcb);
	m_margins.bottom	= get_document()->cvt_units(m_css_margins.bottom,	m_font_size, pcb);

	m_padding.top		= get_document()->cvt_units(m_css_padding.top,		m_font_size, pcb);
	m_padding.bottom	= get_document()->cvt_units(m_css_padding.bottom,	m_font_size, pcb);
}

void litehtml::html_tag::calc_auto_margins(int parent_width)
{
	if (get_element_position() != element_position_absolute && (m_display == display_block || m_display == display_table))
	{
		if (m_css_margins.left.is_predefined() && m_css_margins.right.is_predefined())
		{
			int el_width = m_pos.width + m_borders.left + m_borders.right + m_padding.left + m_padding.right;
			if (el_width <= parent_width)
			{
				m_margins.left = (parent_width - el_width) / 2;
				m_margins.right = (parent_width - el_width) - m_margins.left;
			}
			else
			{
				m_margins.left = 0;
				m_margins.right = 0;
			}
		}
		else if (m_css_margins.left.is_predefined() && !m_css_margins.right.is_predefined())
		{
			int el_width = m_pos.width + m_borders.left + m_borders.right + m_padding.left + m_padding.right + m_margins.right;
			m_margins.left = parent_width - el_width;
			if (m_margins.left < 0) m_margins.left = 0;
		}
		else if (!m_css_margins.left.is_predefined() && m_css_margins.right.is_predefined())
		{
			int el_width = m_pos.width + m_borders.left + m_borders.right + m_padding.left + m_padding.right + m_margins.left;
			m_margins.right = parent_width - el_width;
			if (m_margins.right < 0) m_margins.right = 0;
		}
	}
}

void litehtml::html_tag::parse_attributes()
{
	for(auto& el : m_children)
	{
		el->parse_attributes();
	}
}

void litehtml::html_tag::get_text( tstring& text )
{
	for (auto& el : m_children)
	{
		el->get_text(text);
	}
}

bool litehtml::html_tag::is_body()  const
{
	return false;
}

void litehtml::html_tag::set_data( const tchar_t* data )
{

}

void litehtml::html_tag::get_inline_boxes( position::vector& boxes )
{
	litehtml::box* old_box = 0;
	position pos;
	for(auto& el : m_children)
	{
		if(!el->skip())
		{
			if(el->m_box)
			{
				if(el->m_box != old_box)
				{
					if(old_box)
					{
						if(boxes.empty())
						{
							pos.x		-= m_padding.left + m_borders.left;
							pos.width	+= m_padding.left + m_borders.left;
						}
						boxes.push_back(pos);
					}
					old_box		= el->m_box;
					pos.x		= el->left() + el->margin_left();
					pos.y		= el->top() - m_padding.top - m_borders.top;
					pos.width	= 0;
					pos.height	= 0;
				}
				pos.width	= el->right() - pos.x - el->margin_right() - el->margin_left();
				pos.height	= std::max(pos.height, el->height() + m_padding.top + m_padding.bottom + m_borders.top + m_borders.bottom);
			} else if(el->get_display() == display_inline)
			{
				position::vector sub_boxes;
				el->get_inline_boxes(sub_boxes);
				if(!sub_boxes.empty())
				{
					sub_boxes.rbegin()->width += el->margin_right();
					if(boxes.empty())
					{
						if(m_padding.left + m_borders.left > 0)
						{
							position padding_box = (*sub_boxes.begin());
							padding_box.x		-= m_padding.left + m_borders.left + el->margin_left();
							padding_box.width	= m_padding.left + m_borders.left + el->margin_left();
							boxes.push_back(padding_box);
						}
					}

					sub_boxes.rbegin()->width += el->margin_right();

					boxes.insert(boxes.end(), sub_boxes.begin(), sub_boxes.end());
				}
			}
		}
	}
	if(pos.width || pos.height)
	{
		if(boxes.empty())
		{
			pos.x		-= m_padding.left + m_borders.left;
			pos.width	+= m_padding.left + m_borders.left;
		}
		boxes.push_back(pos);
	}
	if(!boxes.empty())
	{
		if(m_padding.right + m_borders.right > 0)
		{
			boxes.back().width += m_padding.right + m_borders.right;
		}
	}
}

bool litehtml::html_tag::on_mouse_over()
{
	bool ret = false;

	element::ptr el = this;
	while(el)
	{
		if(el->set_pseudo_class(_t("hover"), true))
		{
			ret = true;
		}
		el = el->parent();
	}

	return ret;
}

bool litehtml::html_tag::find_styles_changes( position::vector& redraw_boxes, int x, int y )
{
	if(m_display == display_inline_text)
	{
		return false;
	}

	bool ret = false;
	bool apply = false;
	for (used_selector::vector::iterator iter = m_used_styles.begin(); iter != m_used_styles.end() && !apply; iter++)
	{
		if(iter->m_selector->is_media_valid())
		{
			int res = select(*(iter->m_selector), true);
			if( (res == select_no_match && iter->m_used) || (res == select_match && !iter->m_used) )
			{
				apply = true;
			}
		}
	}

	if(apply)
	{
		if(m_display == display_inline ||  m_display == display_table_row)
		{
			position::vector boxes;
			get_inline_boxes(boxes);
			for(position::vector::iterator pos = boxes.begin(); pos != boxes.end(); pos++)
			{
				pos->x	+= x;
				pos->y	+= y;
				redraw_boxes.push_back(*pos);
			}
		} else
		{
			position pos = m_pos;
			if(m_el_position != element_position_fixed)
			{
				pos.x += x;
				pos.y += y;
			}
			pos += m_padding;
			pos += m_borders;
			redraw_boxes.push_back(pos);
		}

		ret = true;
		refresh_styles();
		parse_styles();
	}
	for (auto& el : m_children)
	{
		if(!el->skip())
		{
			if(m_el_position != element_position_fixed)
			{
				if(el->find_styles_changes(redraw_boxes, x + m_pos.x, y + m_pos.y))
				{
					ret = true;
				}
			} else
			{
				if(el->find_styles_changes(redraw_boxes, m_pos.x, m_pos.y))
				{
					ret = true;
				}
			}
		}
	}
	return ret;
}

bool litehtml::html_tag::on_mouse_leave()
{
	bool ret = false;

	element::ptr el = this;
	while(el)
	{
		if(el->set_pseudo_class(_t("hover"), false))
		{
			ret = true;
		}
		if(el->set_pseudo_class(_t("active"), false))
		{
			ret = true;
		}
		el = el->parent();
	}

	return ret;
}

bool litehtml::html_tag::on_lbutton_down()
{
    bool ret = false;

	element::ptr el = this;
    while (el)
    {
        if (el->set_pseudo_class(_t("active"), true))
        {
            ret = true;
        }
        el = el->parent();
    }

    return ret;
}

bool litehtml::html_tag::on_lbutton_up()
{
	bool ret = false;

	element::ptr el = this;
    while (el)
    {
        if (el->set_pseudo_class(_t("active"), false))
        {
            ret = true;
        }
        el = el->parent();
    }

    on_click();

	return ret;
}

void litehtml::html_tag::on_click()
{
	if (have_parent())
	{
		element::ptr el_parent = parent();
		if (el_parent)
		{
			el_parent->on_click();
		}
	}
}

const litehtml::tchar_t* litehtml::html_tag::get_cursor()
{
	return get_style_property(_t("cursor"), true, 0);
}

static const int font_size_table[8][7] =
{
	{ 9,    9,     9,     9,    11,    14,    18},
	{ 9,    9,     9,    10,    12,    15,    20},
	{ 9,    9,     9,    11,    13,    17,    22},
	{ 9,    9,    10,    12,    14,    18,    24},
	{ 9,    9,    10,    13,    16,    20,    26},
	{ 9,    9,    11,    14,    17,    21,    28},
	{ 9,   10,    12,    15,    17,    23,    30},
	{ 9,   10,    13,    16,    18,    24,    32}
};


void litehtml::html_tag::init_font()
{
	init_font(
		get_style_property_own(_t("font-size")),
		get_style_property_own(_t("font-family")),
		get_style_property_own(_t("font-weight")),
		get_style_property_own(_t("font-style")),
		get_style_property_own(_t("text-decoration"))
	);
}

void litehtml::html_tag::init_font(const tchar_t* own_font_size, const tchar_t* own_name, const tchar_t* own_weight, const tchar_t* own_style, const tchar_t* own_decoration)
{
	uint64_t start_ms = sys_tic_ms(0);
	bool inherit_fast = false;
	static const tchar_t* s_inherit = _t("inherit");

	// initialize font size
	const tchar_t* str = own_font_size;
	if(str && !t_strcasecmp(str, s_inherit))
	{
		str = 0;
	}

	int parent_sz = 0;
	int doc_font_size = 16; // Default to 16 if document not available
	document* doc = get_document();
	if (doc && doc->container())
	{
		doc_font_size = doc->container()->get_default_font_size();
	}

	element::ptr el_parent = parent();
	if (el_parent)
	{
		parent_sz = el_parent->get_font_size();
		bool inherit_font =
			(!own_font_size || !t_strcasecmp(own_font_size, s_inherit)) &&
			(!own_name || !t_strcasecmp(own_name, s_inherit)) &&
			(!own_weight || !t_strcasecmp(own_weight, s_inherit)) &&
			(!own_style || !t_strcasecmp(own_style, s_inherit)) &&
			(!own_decoration || !t_strcasecmp(own_decoration, s_inherit));
		if(inherit_font)
		{
			inherit_fast = true;
			m_font_size = parent_sz;
			m_font = el_parent->get_font(&m_font_metrics);
			litehtml::profile_init_font(inherit_fast, start_ms);
			return;
		}
	} else
	{
		parent_sz = doc_font_size;
	}


	if(!str)
	{
		m_font_size = parent_sz;
	} else
	{
		m_font_size = parent_sz;

		css_length sz;
		sz.fromString(str, font_size_strings);
		if(sz.is_predefined())
		{
			int idx_in_table = doc_font_size - 9;
			if(idx_in_table >= 0 && idx_in_table <= 7)
			{
				if(sz.predef() >= fontSize_xx_small && sz.predef() <= fontSize_xx_large)
				{
					m_font_size = font_size_table[idx_in_table][sz.predef()];
				} else
				{
					m_font_size = doc_font_size;
				}
			} else			
			{
				switch(sz.predef())
				{
				case fontSize_xx_small:
					m_font_size = doc_font_size * 3 / 5;
					break;
				case fontSize_x_small:
					m_font_size = doc_font_size * 3 / 4;
					break;
				case fontSize_small:
					m_font_size = doc_font_size * 8 / 9;
					break;
				case fontSize_large:
					m_font_size = doc_font_size * 6 / 5;
					break;
				case fontSize_x_large:
					m_font_size = doc_font_size * 3 / 2;
					break;
				case fontSize_xx_large:
					m_font_size = doc_font_size * 2;
					break;
				default:
					m_font_size = doc_font_size;
					break;
				}
			}
		} else
		{
			if(sz.units() == css_units_percentage)
			{
				m_font_size = sz.calc_percent(parent_sz);
			} else if(sz.units() == css_units_none)
			{
				/* CSS permits a unitless zero length ("font-size:0", which
				 * GitHub uses to hide directory table headers); any other
				 * unitless value is invalid here and inherits. Treating 0 as
				 * inherit kept the hidden headers' glyphs at full size. */
				m_font_size = (sz.val() == 0) ? 0 : parent_sz;
			} else if(sz.units() >= css_units_cqw && sz.units() <= css_units_cqmax)
			{
				/* Container-query units on font-size: cvt_units maps them to a
				 * percentage of the containing block, which for font-size would
				 * mean "% of the parent font size" - nonsense. Use the spec's
				 * no-container fallback (small viewport) instead. */
				css_length vp;
				vp.set_value(sz.val(), (sz.units() == css_units_cqh || sz.units() == css_units_cqb) ? css_units_vh : css_units_vw);
				m_font_size = doc ? doc->cvt_units(vp, parent_sz) : parent_sz;
			} else
			{
				if (doc)
				{
					m_font_size = doc->cvt_units(sz, parent_sz);
				} else
				{
					m_font_size = parent_sz;
				}
			}
		}
	}

	m_font = 0;
	if (doc)
	{
		if(!el_parent && doc->root() == this)
		{
			/* Root element: remember a sub-pixel-precise font size for the rem
			 * unit resolver. m_font_size is an int, so a viewport-scaled root
			 * like "font-size:0.67px" would truncate to 0 and make every rem
			 * length fall back to the 16px default (blowing layouts up ~16x). */
			double precise = (double)m_font_size;
			if(str)
			{
				css_length rfs;
				rfs.fromString(str, font_size_strings);
				if(!rfs.is_predefined())
				{
					switch(rfs.units())
					{
					case css_units_px:         precise = rfs.val(); break;
					case css_units_pt:         precise = rfs.val() * 96.0 / 72.0; break;
					case css_units_percentage: precise = rfs.val() / 100.0 * (double)parent_sz; break;
					case css_units_em:         precise = rfs.val() * (double)parent_sz; break;
					default: break; /* other units: keep the int-derived value */
					}
				}
			}
			doc->set_root_font_size(precise);
		}
		const tchar_t* name			= get_style_property(_t("font-family"),		true,	_t("inherit"));
		const tchar_t* weight		= get_style_property(_t("font-weight"),		true,	_t("normal"));
		const tchar_t* style		= get_style_property(_t("font-style"),		true,	_t("normal"));
		const tchar_t* decoration	= get_style_property(_t("text-decoration"),	true,	_t("none"));
		m_font = doc->get_font(name, m_font_size, weight, style, decoration, &m_font_metrics);
	}
	litehtml::profile_init_font(inherit_fast, start_ms);
}

bool litehtml::html_tag::is_break() const
{
	return false;
}

void litehtml::html_tag::set_tagName( const tchar_t* tag )
{
	tstring s_val = tag;
	for(size_t i = 0; i < s_val.length(); i++)
	{
		s_val[i] = ascii_tolower_char(s_val[i]);
	}
	m_tag = s_val;
}

/* Supported CSS transform subset: a list of rotate()/translate()/translateX()/
 * translateY()/translate3d() functions - enough for the border-trick chevrons
 * modern design systems rotate into place (w3.org breadcrumb separators and nav
 * carets) and for translate3d-positioned carousels (apple.com home gallery),
 * which we project to 2D by dropping Z. Any other function
 * (scale/matrix/skew/...) aborts the parse and leaves the identity, which beats
 * drawing a half-understood transform. */
void litehtml::html_tag::parse_transform_list(const tchar_t* val)
{
	m_transform.clear();
	/* Record the string form so the transition trigger can detect changes
	 * regardless of whether this parse came from parse_styles, an animated
	 * writeback (set_animated_transform), or a snap-back-to-CSS restore. */
	m_transform_str = val ? val : _t("");
	const tchar_t* p = val;
	while(p && *p)
	{
		while(*p == ' ' || *p == '\t') p++;
		if(!*p) break;
		int type = -1;
		if(!t_strncmp(p, _t("rotate("), 7))			{ type = 0; p += 7; }
		else if(!t_strncmp(p, _t("translateX("), 11))	{ type = 2; p += 11; }
		else if(!t_strncmp(p, _t("translateY("), 11))	{ type = 3; p += 11; }
		/* translate3d(x,y,z): carousels (apple.com home gallery) position slides
		 * with translate3d. We are a 2D engine, so take x,y and drop z; that is
		 * far better than clearing the transform (which left every slide stacked
		 * at the origin). The earlier paint crash blamed on translate3d was in
		 * fact a mario VM use-after-free, unrelated to this parser. */
		else if(!t_strncmp(p, _t("translate3d("), 12))	{ type = 1; p += 12; }
		else if(!t_strncmp(p, _t("translate("), 10))	{ type = 1; p += 10; }
		else if(!t_strncmp(p, _t("scaleX("), 7))		{ type = 5; p += 7; }
		else if(!t_strncmp(p, _t("scaleY("), 7))		{ type = 6; p += 7; }
		else if(!t_strncmp(p, _t("scale("), 6))			{ type = 4; p += 6; }
		else if(!t_strncmp(p, _t("skewX("), 6))			{ type = 8; p += 6; }
		else if(!t_strncmp(p, _t("skewY("), 6))			{ type = 9; p += 6; }
		else if(!t_strncmp(p, _t("skew("), 5))			{ type = 7; p += 5; }
		else if(!t_strncmp(p, _t("matrix("), 7))			{ type = 10; p += 7; }
		else { return; }  /* unknown function: stop parsing, keep what we have */
		tstring args;
		while(*p && *p != ')') args += *p++;
		if(*p == ')') p++;
		transform_fn fn;
		fn.type = type;
		if(type == 0 || type == 7 || type == 8 || type == 9)
		{
			/* Angle-valued functions: rotate, skew, skewX, skewY */
			if(type == 7)
			{
				/* skew(ax, ay): two angles */
				string_vector toks;
				split_string(args, toks, _t(", "));
				if(toks.size() >= 1)
				{
					char* end = 0;
					fn.deg = (float)strtod(toks[0].c_str(), &end);
					if(end && !t_strncmp(end, _t("rad"), 3))		fn.deg *= 57.2957795f;
					else if(end && !t_strncmp(end, _t("turn"), 4))	fn.deg *= 360.0f;
					else if(end && !t_strncmp(end, _t("grad"), 4))	fn.deg *= 0.9f;
				}
				if(toks.size() >= 2)
				{
					char* end = 0;
					fn.sx = (float)strtod(toks[1].c_str(), &end);  /* reuse sx for ay */
					if(end && !t_strncmp(end, _t("rad"), 3))		fn.sx *= 57.2957795f;
					else if(end && !t_strncmp(end, _t("turn"), 4))	fn.sx *= 360.0f;
					else if(end && !t_strncmp(end, _t("grad"), 4))	fn.sx *= 0.9f;
				}
			}
			else
			{
				char* end = 0;
				fn.deg = (float)strtod(args.c_str(), &end);
				if(end && !t_strncmp(end, _t("rad"), 3))		fn.deg *= 57.2957795f;
				else if(end && !t_strncmp(end, _t("turn"), 4))	fn.deg *= 360.0f;
				else if(end && !t_strncmp(end, _t("grad"), 4))	fn.deg *= 0.9f;
			}
		}
		else if(type == 4 || type == 5 || type == 6)
		{
			/* Scale functions */
			string_vector toks;
			split_string(args, toks, _t(", "));
			if(toks.size() >= 1) fn.sx = (float)strtod(toks[0].c_str(), nullptr);
			if(toks.size() >= 2) fn.sy = (float)strtod(toks[1].c_str(), nullptr);
			else fn.sy = fn.sx;  /* scale(s) means scale(s,s) */
		}
		else if(type == 10)
		{
			/* matrix(a,b,c,d,e,f) */
			string_vector toks;
			split_string(args, toks, _t(", "));
			for(int i = 0; i < 6 && i < (int)toks.size(); i++)
				fn.mat[i] = (float)strtod(toks[i].c_str(), nullptr);
		}
		else
		{
			/* Translate functions */
			string_vector toks;
			split_string(args, toks, _t(", "));
			if(toks.size() >= 1) fn.x.fromString(toks[0].c_str());
			if(toks.size() >= 2) fn.y.fromString(toks[1].c_str());
		}
		m_transform.push_back(fn);
	}
}

bool litehtml::html_tag::compute_transform_matrix(const position& box, float m[6]) const
{
	if(m_transform.empty()) return false;
	float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
	for(size_t i = 0; i < m_transform.size(); i++)
	{
		const transform_fn& fn = m_transform[i];
		float fa = 1, fb = 0, fc = 0, fd = 1, fe = 0, ff = 0;
		switch(fn.type)
		{
		case 0:  /* rotate(deg) */
		{
			float th = fn.deg * 3.14159265358979f / 180.0f;
			float cs = cosf(th), sn = sinf(th);
			fa = cs; fb = sn; fc = -sn; fd = cs;
			break;
		}
		case 1:  /* translate(x,y) / translate3d(x,y,z) */
		{
			css_length lx = fn.x, ly = fn.y;
			fe = (float)get_document()->cvt_units(lx, m_font_size, box.width);
			ff = (float)get_document()->cvt_units(ly, m_font_size, box.height);
			break;
		}
		case 2:  /* translateX(x) */
		{
			css_length lx = fn.x;
			fe = (float)get_document()->cvt_units(lx, m_font_size, box.width);
			break;
		}
		case 3:  /* translateY(y) */
		{
			css_length lx = fn.x;
			ff = (float)get_document()->cvt_units(lx, m_font_size, box.height);
			break;
		}
		case 4:  /* scale(sx,sy) */
			fa = fn.sx; fd = fn.sy;
			break;
		case 5:  /* scaleX(sx) */
			fa = fn.sx;
			break;
		case 6:  /* scaleY(sy) */
			fd = fn.sy;
			break;
		case 7:  /* skew(ax,ay) */
		{
			float tx = tanf(fn.deg * 3.14159265358979f / 180.0f);
			float ty = tanf(fn.sx  * 3.14159265358979f / 180.0f);  /* sx reused for ay */
			fc = tx; fb = ty;
			break;
		}
		case 8:  /* skewX(ax) */
			fc = tanf(fn.deg * 3.14159265358979f / 180.0f);
			break;
		case 9:  /* skewY(ay) */
			fb = tanf(fn.deg * 3.14159265358979f / 180.0f);
			break;
		case 10: /* matrix(a,b,c,d,e,f) */
			fa = fn.mat[0]; fb = fn.mat[1]; fc = fn.mat[2];
			fd = fn.mat[3]; fe = fn.mat[4]; ff = fn.mat[5];
			break;
		default:
			break;
		}
		/* m = m * fn: the new function acts in the local frame of the product */
		float na = a * fa + c * fb;
		float nb = b * fa + d * fb;
		float nc = a * fc + c * fd;
		float nd = b * fc + d * fd;
		float ne = a * fe + c * ff + e;
		float nf = b * fe + d * ff + f;
		a = na; b = nb; c = nc; d = nd; e = ne; f = nf;
	}
	/* conjugate by the transform-origin (box centre), then move from the
	 * box-local frame into device space */
	float ox = box.width * 0.5f, oy = box.height * 0.5f;
	float es = e + ox - (a * ox + c * oy);
	float fs = f + oy - (b * ox + d * oy);
	m[0] = a; m[1] = b; m[2] = c; m[3] = d;
	m[4] = es + box.x - (a * box.x + c * box.y);
	m[5] = fs + box.y - (b * box.x + d * box.y);
	return true;
}

void litehtml::html_tag::draw_background( uint_ptr hdc, int x, int y, const position* clip )
{
	position pos = m_pos;
	pos.x	+= x;
	pos.y	+= y;

	position el_pos = pos;
	el_pos += m_padding;
	el_pos += m_borders;

	if(m_display != display_inline && m_display != display_table_row)
	{
		if(el_pos.does_intersect(clip))
		{
			position border_box = pos;
			border_box += m_padding;
			border_box += m_borders;

			/* Compute the CSS transform once and wrap BOTH the background fill
			 * and the borders in it, so a transformed element paints its whole
			 * box (not just its border edges) through the matrix. The container
			 * applies the matrix as a polygon transform in software; content
			 * (text/children) is not transformed in this phase. */
			float xform[6];
			bool have_xform = compute_transform_matrix(border_box, xform);
			if(have_xform)
			{
				get_document()->container()->push_paint_transform(xform);
			}

			const background* bg = get_background();
			if(bg)
			{
				background_paint bg_paint;
				init_background_paint(pos, bg_paint, bg);

				/* Partial opacity: fade the background fill. */
				bg_paint.color.alpha = opacity_scale_alpha(bg_paint.color.alpha, m_opacity_cum);

				get_document()->container()->draw_background(hdc, bg_paint);
			}

			borders bdr = m_css_borders;
			bdr.radius = m_css_borders.radius.calc_percents(border_box.width, border_box.height);

			/* Partial opacity: fade each border edge colour. */
			if(m_opacity_cum < 1.0f)
			{
				bdr.left.color.alpha	= opacity_scale_alpha(bdr.left.color.alpha, m_opacity_cum);
				bdr.top.color.alpha		= opacity_scale_alpha(bdr.top.color.alpha, m_opacity_cum);
				bdr.right.color.alpha	= opacity_scale_alpha(bdr.right.color.alpha, m_opacity_cum);
				bdr.bottom.color.alpha	= opacity_scale_alpha(bdr.bottom.color.alpha, m_opacity_cum);
			}

			get_document()->container()->draw_borders(hdc, bdr, border_box, have_parent() ? false : true);
			if(have_xform)
			{
				get_document()->container()->pop_paint_transform();
			}
		}
	} else
	{
		const background* bg = get_background();

		position::vector boxes;
		get_inline_boxes(boxes);

		background_paint bg_paint;
		position content_box;

		for(position::vector::iterator box = boxes.begin(); box != boxes.end(); box++)
		{
			box->x	+= x;
			box->y	+= y;

			if(box->does_intersect(clip))
			{
				content_box = *box;
				content_box -= m_borders;
				content_box -= m_padding;

				if(bg)
				{
					init_background_paint(content_box, bg_paint, bg);
				}

				css_borders bdr;

				// set left borders radius for the first box
				if(box == boxes.begin())
				{
					bdr.radius.bottom_left_x	= m_css_borders.radius.bottom_left_x;
					bdr.radius.bottom_left_y	= m_css_borders.radius.bottom_left_y;
					bdr.radius.top_left_x		= m_css_borders.radius.top_left_x;
					bdr.radius.top_left_y		= m_css_borders.radius.top_left_y;
				}

				// set right borders radius for the last box
				if(box == boxes.end() - 1)
				{
					bdr.radius.bottom_right_x	= m_css_borders.radius.bottom_right_x;
					bdr.radius.bottom_right_y	= m_css_borders.radius.bottom_right_y;
					bdr.radius.top_right_x		= m_css_borders.radius.top_right_x;
					bdr.radius.top_right_y		= m_css_borders.radius.top_right_y;
				}

				
				bdr.top		= m_css_borders.top;
				bdr.bottom	= m_css_borders.bottom;
				if(box == boxes.begin())
				{
					bdr.left	= m_css_borders.left;
				}
				if(box == boxes.end() - 1)
				{
					bdr.right	= m_css_borders.right;
				}


				if(bg)
				{
					bg_paint.border_radius = bdr.radius.calc_percents(bg_paint.border_box.width, bg_paint.border_box.width);
					bg_paint.color.alpha = opacity_scale_alpha(bg_paint.color.alpha, m_opacity_cum);
					get_document()->container()->draw_background(hdc, bg_paint);
				}
				borders b = bdr;
				b.radius = bdr.radius.calc_percents(box->width, box->height);
				if(m_opacity_cum < 1.0f)
				{
					b.left.color.alpha	= opacity_scale_alpha(b.left.color.alpha, m_opacity_cum);
					b.top.color.alpha		= opacity_scale_alpha(b.top.color.alpha, m_opacity_cum);
					b.right.color.alpha	= opacity_scale_alpha(b.right.color.alpha, m_opacity_cum);
					b.bottom.color.alpha	= opacity_scale_alpha(b.bottom.color.alpha, m_opacity_cum);
				}
				get_document()->container()->draw_borders(hdc, b, *box, false);
			}
		}
	}
}

int litehtml::html_tag::render_inline(const element::ptr &container, int max_width)
{
	int ret_width = 0;
	int rw = 0;

	white_space ws = get_white_space();
	bool skip_spaces = false;
	if (ws == white_space_normal ||
		ws == white_space_nowrap ||
		ws == white_space_pre_line)
	{
		skip_spaces = true;
	}
	bool was_space = false;
	std::vector<element::ptr> layout_children;
	collect_box_children(this, layout_children);

	for (auto& el : layout_children)
	{
		// Check if element is valid
		if (!el) continue;

		// skip spaces to make rendering a bit faster
		if (skip_spaces)
		{
			if (el->is_white_space())
			{
				if (was_space)
				{
					el->skip(true);
					continue;
				}
				else
				{
					was_space = true;
				}
			}
			else
			{
				was_space = false;
			}
		}

		rw = container->place_element( el, max_width );
		if(rw > ret_width)
		{
			ret_width = rw;
		}
	}
	return ret_width;
}

int litehtml::html_tag::place_element(const element::ptr &el, int max_width)
{
	if(!el) return 0;
	if(el->get_display() == display_none) return 0;

	if(el->get_display() == display_inline)
	{
		/* A replaced element with display:inline (Tailwind's `inline` on the
		 * rokid.com hero <img>) is still an atomic inline-level box per CSS 2.1
		 * 9.2.2: it takes part in the line layout like an inline-block. The
		 * generic render_inline only walks children, and element::render_inline
		 * is an empty stub for leaves, so routing a replaced leaf there left it
		 * zero-sized forever. Fall through to the line placement below. */
		if(!el->is_replaced())
		{
			return el->render_inline(this, max_width);
		}
	}

	element_position el_position = el->get_element_position();

	if(el_position == element_position_absolute || el_position == element_position_fixed)
	{
		int line_top = 0;
		if(!m_boxes.empty())
		{
			if(m_boxes.back()->get_type() == box_line)
			{
				line_top = m_boxes.back()->top();
				if(!m_boxes.back()->is_empty())
				{
					line_top += line_height();
				}
			} else
			{
				line_top = m_boxes.back()->bottom();
			}
		}

		el->render(0, line_top, max_width);
		el->m_pos.x	+= el->content_margins_left();
		el->m_pos.y	+= el->content_margins_top();

		return 0;
	}

	int ret_width = 0;

	switch(el->get_float())
	{
	case float_left:
		{
			int line_top = 0;
			if(!m_boxes.empty())
			{
				if(m_boxes.back()->get_type() == box_line)
				{
					line_top = m_boxes.back()->top();
				} else
				{
					line_top = m_boxes.back()->bottom();
				}
			}
			line_top		= get_cleared_top(el, line_top);
			int line_left	= 0;
			int line_right	= max_width;
			get_line_left_right(line_top, max_width, line_left, line_right);

			el->render(line_left, line_top, line_right);
			if(el->right() > line_right)
			{
				int new_top = find_next_line_top(el->top(), el->width(), max_width);
				el->m_pos.x = get_line_left(new_top) + el->content_margins_left();
				el->m_pos.y = new_top + el->content_margins_top();
			}
			add_float(el, 0, 0);
			ret_width = fix_line_width(max_width, float_left);
			if(!ret_width)
			{
				ret_width = el->right();
			}
		}
		break;
	case float_right:
		{
			int line_top = 0;
			if(!m_boxes.empty())
			{
				if(m_boxes.back()->get_type() == box_line)
				{
					line_top = m_boxes.back()->top();
				} else
				{
					line_top = m_boxes.back()->bottom();
				}
			}
			line_top		= get_cleared_top(el, line_top);
			int line_left	= 0;
			int line_right	= max_width;
			get_line_left_right(line_top, max_width, line_left, line_right);

			el->render(0, line_top, line_right);

			if(line_left + el->width() > line_right)
			{
				int new_top = find_next_line_top(el->top(), el->width(), max_width);
				el->m_pos.x = get_line_right(new_top, max_width) - el->width() + el->content_margins_left();
				el->m_pos.y = new_top + el->content_margins_top();
			} else
			{
				el->m_pos.x = line_right - el->width() + el->content_margins_left();
			}
			add_float(el, 0, 0);
			ret_width = fix_line_width(max_width, float_right);

			if(!ret_width)
			{
				line_left	= 0;
				line_right	= max_width;
				get_line_left_right(line_top, max_width, line_left, line_right);

				ret_width = ret_width + (max_width - line_right);
			}
		}
		break;
	default:
		{
			line_context line_ctx;
			line_ctx.top = 0;
			if (!m_boxes.empty())
			{
				line_ctx.top = m_boxes.back()->top();
			}
			line_ctx.left = 0;
			line_ctx.right = max_width;
			line_ctx.fix_top();
			get_line_left_right(line_ctx.top, max_width, line_ctx.left, line_ctx.right);

			switch(el->get_display())
			{
			case display_inline_block:
			case display_inline_grid:
			case display_inline_flex:
			case display_inline:	/* replaced inline only, see place_element head */
				ret_width = el->render(line_ctx.left, line_ctx.top, line_ctx.right);
				break;
			case display_block:		
			case display_flex:
			case display_grid:
				if(el->is_replaced() || el->is_floats_holder())
				{
					element::ptr el_parent = el->parent();
					el->m_pos.width = el->get_css_width().calc_percent(line_ctx.right - line_ctx.left);
					el->m_pos.height = el->get_css_height().calc_percent(el_parent ? el_parent->m_pos.height : 0);
				}
				el->calc_outlines(line_ctx.right - line_ctx.left);
				break;
			case display_inline_text:
				{
					litehtml::size sz;
					el->get_content_size(sz, line_ctx.right);
					el->m_pos = sz;
				}
				break;
			default:
				ret_width = 0;
				break;
			}

			bool add_box = true;
			if(!m_boxes.empty())
			{
				if(m_boxes.back()->can_hold(el, m_white_space))
				{
					add_box = false;
				}
			}
			if(add_box)
			{
				new_box(el, max_width, line_ctx);
			} else if(!m_boxes.empty())
			{
				line_ctx.top = m_boxes.back()->top();
			}

			if (line_ctx.top != line_ctx.calculatedTop)
			{
				line_ctx.left = 0;
				line_ctx.right = max_width;
				line_ctx.fix_top();
				get_line_left_right(line_ctx.top, max_width, line_ctx.left, line_ctx.right);
			}

			/* Parent/child margin collapsing (CSS 2.1 8.3.1): 'shift' is how far the
			 * child's box is pulled up so its top margin is carried by THIS box
			 * instead (render_box then absorbs it into m_margins.top). A box that
			 * establishes a new block formatting context never collapses with its
			 * children, so its first child keeps the margin inside. */
			int  margin_shift = 0;
			int  pre_margin   = 0;
			bool block_child  = !el->is_inline_box();
			if(block_child)
			{
				pre_margin = el->margin_top();
				if(m_boxes.size() == 1)
				{
					if(collapse_top_margin() && m_overflow == overflow_visible)
					{
						margin_shift = pre_margin;
					}
				} else
				{
					int prev_margin = m_boxes[m_boxes.size() - 2]->bottom_margin();
					margin_shift = std::min(prev_margin, pre_margin);
				}
				if(margin_shift > 0)
				{
					line_ctx.top -= margin_shift;
					m_boxes.back()->y_shift(-margin_shift);
				}
			}

			switch(el->get_display())
			{
			case display_table:
			case display_list_item:
				ret_width = el->render(line_ctx.left, line_ctx.top, line_ctx.width());
				break;
			case display_block:
			case display_flex:
			case display_grid:
			case display_table_cell:
			case display_table_caption:
			case display_table_row:
				if(el->is_replaced() || el->is_floats_holder())
				{
					ret_width = el->render(line_ctx.left, line_ctx.top, line_ctx.width()) + line_ctx.left + (max_width - line_ctx.right);
				} else
				{
					ret_width = el->render(0, line_ctx.top, max_width);
				}
				break;
			default:
				ret_width = 0;
				break;
			}

			/* The child's top margin can GROW while it renders: render_box absorbs
			 * the collapsed-through margins of its own first descendants, and its
			 * final m_pos.y already sits that far below line_ctx.top. The shift
			 * above only knew the child's own CSS margin, so pull the box up by the
			 * growth as well - otherwise the margin is counted inside this box AND
			 * re-absorbed as our own, once per ancestor level. workspace.google.com's
			 * .base_main{margin-top:64px} under five plain wrappers opened a 320px
			 * hole below the header this way. */
			if(block_child && el->margin_top() > pre_margin)
			{
				int post_margin = el->margin_top();
				int new_shift = 0;
				if(m_boxes.size() == 1)
				{
					if(collapse_top_margin() && m_overflow == overflow_visible)
					{
						new_shift = post_margin;
					}
				} else
				{
					int prev_margin = m_boxes[m_boxes.size() - 2]->bottom_margin();
					new_shift = std::min(prev_margin, post_margin);
				}
				if(new_shift > margin_shift)
				{
					/* the box does not hold el yet (add_element below), so its
					 * y_shift moves only the box top - move the element too */
					int extra = new_shift - margin_shift;
					line_ctx.top -= extra;
					m_boxes.back()->y_shift(-extra);
					el->m_pos.y -= extra;
				}
			}

			m_boxes.back()->add_element(el);

			/* A collapsible space must not extend the shrink-to-fit width: if it
			 * ends the line it is stripped by line_box::finish (CSS Text 3 §4.1.3),
			 * and if a word follows, that word's right() covers it. Counting it
			 * here made every float/inline-block whose markup has a newline
			 * before the closing tag one space wider than its max-content, so
			 * the flex base size measured for github's floated action <li>s
			 * came up 4px short each and the Star button wrapped to a new row. */
			if(el->is_inline_box() && !el->skip() && !el->is_white_space())
			{
				ret_width = el->right() + (max_width - line_ctx.right);
			}
		}
		break;
	}

	return ret_width;
}

bool litehtml::html_tag::set_pseudo_class( const tchar_t* pclass, bool add )
{
	bool ret = false;
	if(add)
	{
		if(std::find(m_pseudo_classes.begin(), m_pseudo_classes.end(), pclass) == m_pseudo_classes.end())
		{
			m_pseudo_classes.push_back(pclass);
			ret = true;
		}
	} else
	{
		string_vector::iterator pi = std::find(m_pseudo_classes.begin(), m_pseudo_classes.end(), pclass);
		if(pi != m_pseudo_classes.end())
		{
			m_pseudo_classes.erase(pi);
			ret = true;
		}
	}
	return ret;
}

/* ---- CSS animation override methods (Phase 2) ---------------------------- */

void litehtml::html_tag::set_anim_override(const tchar_t* prop, const tchar_t* value)
{
	if(!prop || !value) return;
	tstring key = prop;
	lcase(key);
	m_anim_overrides[key] = value;
}

void litehtml::html_tag::clear_anim_override(const tchar_t* prop)
{
	if(!prop) return;
	tstring key = prop;
	lcase(key);
	m_anim_overrides.erase(key);
}

void litehtml::html_tag::clear_anim_overrides()
{
	m_anim_overrides.clear();
}

const litehtml::tchar_t* litehtml::html_tag::anim_override(const tchar_t* prop) const
{
	if(!prop) return nullptr;
	tstring key = prop;
	lcase(key);
	string_map::const_iterator it = m_anim_overrides.find(key);
	if(it == m_anim_overrides.end()) return nullptr;
	return it->second.c_str();
}

void litehtml::html_tag::set_animated_opacity(float op)
{
	if(op < 0.0f) op = 0.0f;
	if(op > 1.0f) op = 1.0f;
	/* Same script-reveal guard as parse_styles: html/body opacity:0 is a
	 * hydration gate that a JS-limited engine can never flip, so treat it
	 * as revealed. */
	if(op <= 0.0f && (m_tag == _t("html") || m_tag == _t("body"))) op = 1.0f;
	m_opacity = op;
	propagate_opacity_cum();
}

void litehtml::html_tag::propagate_opacity_cum()
{
	element::ptr el_parent = parent();
	float parent_cum = el_parent ? el_parent->get_opacity_cum() : 1.0f;
	m_opacity_cum = parent_cum * m_opacity;
	for(auto& child : m_children)
	{
		if(child->is_html_tag())
		{
			static_cast<html_tag*>(child)->propagate_opacity_cum();
		}
	}
}

void litehtml::html_tag::set_animated_transform(const tchar_t* val)
{
	if(!val) return;
	/* Store the raw override string (so a later parse_styles keeps it) and
	 * re-parse into m_transform now: draw_background composes the paint
	 * matrix from m_transform, and tick_animations runs on the engine thread
	 * without re-running the full cascade, so the parse must happen here for
	 * the transform to visibly animate frame to frame. */
	set_anim_override(_t("transform"), val);
	parse_transform_list(val);
}

bool litehtml::html_tag::subtree_within_budget(int limit) const
{
	/* Bounded iterative DFS counting this element and every descendant. We
	 * stop the moment the count exceeds `limit` so the guard costs at most
	 * O(limit) and never walks a huge subtree just to reject it. Phase 3.1
	 * uses this to decide whether an animation-driven relayout of this
	 * element's geometry is cheap enough to run every frame; oversized
	 * subtrees are degraded to a discrete switch instead. */
	if(limit <= 0) return false;
	int count = 0;
	std::vector<litehtml::element::ptr> stack;
	stack.push_back(const_cast<litehtml::html_tag*>(this));
	while(!stack.empty())
	{
		litehtml::element::ptr cur = stack.back();
		stack.pop_back();
		if(!cur) continue;
		count++;
		if(count > limit) return false;
		for(size_t i = 0; i < cur->get_children_count(); i++)
		{
			litehtml::element::ptr c = cur->get_child((int)i);
			if(c) stack.push_back(c);
		}
	}
	return true;
}

bool litehtml::html_tag::set_class( const tchar_t* pclass, bool add )
{
	string_vector classes;
	bool changed = false;

	split_string( pclass, classes, _t(" ") );

	if(add)
	{
		for( auto & _class : classes  )
		{
			if(std::find(m_class_values.begin(), m_class_values.end(), _class) == m_class_values.end())
			{
				m_class_values.push_back( std::move( _class ) );
				changed = true;
			}
		}
	} else
	{
		for( const auto & _class : classes )
		{
			auto end = std::remove(m_class_values.begin(), m_class_values.end(), _class);

			if(end != m_class_values.end())
			{
				m_class_values.erase(end, m_class_values.end());
				changed = true;
			}
		}
	}

	if( changed )
	{
		tstring class_string;
		join_string(class_string, m_class_values, _t(" "));
		set_attr(_t("class"), class_string.c_str());

		return true;
	}
	else
	{
		return false;
	}

}

int litehtml::html_tag::line_height() const
{
	return m_line_height;
}

bool litehtml::html_tag::is_line_height_normal() const
{
	return m_lh_predefined;
}

float litehtml::html_tag::line_height_factor() const
{
	return m_lh_factor;
}

bool litehtml::html_tag::is_replaced() const
{
	return false;
}

int litehtml::html_tag::finish_last_box(bool end_of_render)
{
	int line_top = 0;

	if(!m_boxes.empty())
	{
		m_boxes.back()->finish(end_of_render);

		if(m_boxes.back()->is_empty())
		{
			line_top = m_boxes.back()->top();
			m_boxes.pop_back();
		}

		if(!m_boxes.empty())
		{
			line_top = m_boxes.back()->bottom();
		}
	}
	return line_top;
}

int litehtml::html_tag::new_box(const element::ptr &el, int max_width, line_context& line_ctx)
{
	line_ctx.top = get_cleared_top(el, finish_last_box());

	line_ctx.left = 0;
	line_ctx.right = max_width;
	line_ctx.fix_top();
	get_line_left_right(line_ctx.top, max_width, line_ctx.left, line_ctx.right);

	if(el->is_inline_box() || el->is_floats_holder())
	{
		if (el->width() > line_ctx.right - line_ctx.left)
		{
			line_ctx.top = find_next_line_top(line_ctx.top, el->width(), max_width);
			line_ctx.left = 0;
			line_ctx.right = max_width;
			line_ctx.fix_top();
			get_line_left_right(line_ctx.top, max_width, line_ctx.left, line_ctx.right);
		}
	}

	int first_line_margin = 0;
	if(m_boxes.empty() && m_list_style_type != list_style_type_none && m_list_style_position == list_style_position_inside)
	{
		int sz_font = get_font_size();
		first_line_margin = sz_font;
	}

	if(el->is_inline_box())
	{
		int text_indent = 0;
		if(m_css_text_indent.val() != 0)
		{
			bool line_box_found = false;
			for(box::vector::iterator iter = m_boxes.begin(); iter != m_boxes.end(); iter++)
			{
				if((*iter)->get_type() == box_line)
				{
					line_box_found = true;
					break;
				}
			}
			if(!line_box_found)
			{
				text_indent = m_css_text_indent.calc_percent(max_width);
			}
		}

		font_metrics fm;
		get_font(&fm);
		m_boxes.emplace_back(std::unique_ptr<line_box>(new line_box(line_ctx.top, line_ctx.left + first_line_margin + text_indent,
			line_ctx.right, line_height(), fm, m_text_align, m_balanced_line_width)));
	} else
	{
		m_boxes.emplace_back(std::unique_ptr<block_box>(new block_box(line_ctx.top, line_ctx.left, line_ctx.right)));
	}

	return line_ctx.top;
}

int litehtml::html_tag::get_cleared_top(const element::ptr &el, int line_top) const
{
	switch(el->get_clear())
	{
	case clear_left:
		{
			int fh = get_left_floats_height();
			if(fh && fh > line_top)
			{
				line_top = fh;
			}
		}
		break;
	case clear_right:
		{
			int fh = get_right_floats_height();
			if(fh && fh > line_top)
			{
				line_top = fh;
			}
		}
		break;
	case clear_both:
		{
			int fh = get_floats_height();
			if(fh && fh > line_top)
			{
				line_top = fh;
			}
		}
		break;
	default:
		if(el->get_float() != float_none)
		{
			int fh = get_floats_height(el->get_float());
			if(fh && fh > line_top)
			{
				line_top = fh;
			}
		}
		break;
	}
	return line_top;
}

litehtml::style_display litehtml::html_tag::get_display() const
{
	return m_display;
}

litehtml::element_float litehtml::html_tag::get_float() const
{
	return m_float;
}

bool litehtml::html_tag::is_floats_holder() const
{
	if(	m_display == display_inline_block || 
		m_display == display_inline_flex || 
		m_display == display_flex || 
		m_display == display_grid || 
		m_display == display_inline_grid || 
		m_display == display_table_cell || 
		!have_parent() ||
		is_body() || 
		m_float != float_none ||
		m_el_position == element_position_absolute ||
		m_el_position == element_position_fixed ||
		m_overflow > overflow_visible)
	{
		return true;
	}
	/* A flex/grid item establishes an independent formatting context: floats
	 * inside it never escape into the container. github's repo header keeps
	 * its action buttons as floated <li> under an inline <ul> inside a flex
	 * item; without this the floats registered with an ancestor holder and
	 * the Star button landed under the repo title. */
	element::ptr p = parent();
	if(p)
	{
		style_display pd = p->get_display();
		if(pd == display_flex || pd == display_inline_flex || pd == display_grid || pd == display_inline_grid)
		{
			return true;
		}
	}
	return false;
}

bool litehtml::html_tag::is_first_child_inline(const element::ptr& el) const
{
	if(!m_children.empty())
	{
		for (const auto& this_el : m_children)
		{
			if (!this_el->is_white_space())
			{
				if (el == this_el)
				{
					return true;
				}
				if (this_el->get_display() == display_inline)
				{
					if (this_el->have_inline_child())
					{
						return false;
					}
				} else
				{
					return false;
				}
			}
		}
	}
	return false;
}

bool litehtml::html_tag::is_last_child_inline(const element::ptr& el)
{
	if(!m_children.empty())
	{
		for (auto this_el = m_children.rbegin(); this_el < m_children.rend(); ++this_el)
		{
			if (!(*this_el)->is_white_space())
			{
				if (el == (*this_el))
				{
					return true;
				}
				if ((*this_el)->get_display() == display_inline)
				{
					if ((*this_el)->have_inline_child())
					{
						return false;
					}
				} else
				{
					return false;
				}
			}
		}
	}
	return false;
}

litehtml::white_space litehtml::html_tag::get_white_space() const
{
	return m_white_space;
}

litehtml::text_align litehtml::html_tag::get_text_align() const
{
	return m_text_align;
}

litehtml::text_transform litehtml::html_tag::get_text_transform() const
{
	return m_text_transform;
}

litehtml::vertical_align litehtml::html_tag::get_vertical_align() const
{
	return m_vertical_align;
}

litehtml::css_length litehtml::html_tag::get_css_left() const
{
	return m_css_offsets.left;
}

litehtml::css_length litehtml::html_tag::get_css_right() const
{
	return m_css_offsets.right;
}

litehtml::css_length litehtml::html_tag::get_css_top() const
{
	return m_css_offsets.top;
}

litehtml::css_length litehtml::html_tag::get_css_bottom() const
{
	return m_css_offsets.bottom;
}


litehtml::css_offsets litehtml::html_tag::get_css_offsets() const
{
	return m_css_offsets;
}

litehtml::element_clear litehtml::html_tag::get_clear() const
{
	return m_clear;
}

litehtml::css_length litehtml::html_tag::get_css_width() const
{
	return m_css_width;
}

litehtml::css_length litehtml::html_tag::get_css_height() const
{
	return m_css_height;
}

size_t litehtml::html_tag::get_children_count() const
{
	return m_children.size();
}

litehtml::element::ptr litehtml::html_tag::get_child( int idx ) const
{
	return m_children[idx];
}

void litehtml::html_tag::set_css_width( css_length& w )
{
	m_css_width = w;
}

void litehtml::html_tag::apply_vertical_align()
{
	if(!m_boxes.empty())
	{
		int add = 0;
		int content_height	= m_boxes.back()->bottom();

		if(m_pos.height > content_height)
		{
			switch(m_vertical_align)
			{
			case va_middle:
				add = (m_pos.height - content_height) / 2;
				break;
			case va_bottom:
				add = m_pos.height - content_height;
				break;
			default:
				add = 0;
				break;
			}
		}

		if(add)
		{
			for(size_t i = 0; i < m_boxes.size(); i++)
			{
				m_boxes[i]->y_shift(add);
			}
		}
	}
}

litehtml::element_position litehtml::html_tag::get_element_position(css_offsets* offsets) const
{
	if(offsets && m_el_position != element_position_static)
	{
		*offsets = m_css_offsets;
	}
	return m_el_position;
}

void litehtml::html_tag::init_background_paint(position pos, background_paint &bg_paint, const background* bg)
{
	if(!bg) return;

	bg_paint = *bg;
	position content_box	= pos;
	position padding_box	= pos;
	bg_paint.content_box = content_box;
	padding_box += m_padding;
	position border_box		= padding_box;
	border_box += m_borders;

	switch(bg->m_clip)
	{
	case litehtml::background_box_padding:
		bg_paint.clip_box = padding_box;
		break;
	case litehtml::background_box_content:
		bg_paint.clip_box = content_box;
		break;
	default:
		bg_paint.clip_box = border_box;
		break;
	}

	switch(bg->m_origin)
	{
	case litehtml::background_box_border:
		bg_paint.origin_box = border_box;
		break;
	case litehtml::background_box_content:
		bg_paint.origin_box = content_box;
		break;
	default:
		bg_paint.origin_box = padding_box;
		break;
	}

	if(!bg_paint.image.empty())
	{
		get_document()->container()->get_image_size(bg_paint.image.c_str(), bg_paint.baseurl.c_str(), bg_paint.image_size);
		if(bg_paint.image_size.width && bg_paint.image_size.height)
		{
			litehtml::size img_new_sz = bg_paint.image_size;
			double img_ar_width		= (double) bg_paint.image_size.width / (double) bg_paint.image_size.height;
			double img_ar_height	= (double) bg_paint.image_size.height / (double) bg_paint.image_size.width;


			if(bg->m_position.width.is_predefined())
			{
				switch(bg->m_position.width.predef())
				{
				case litehtml::background_size_contain:
					if( (int) ((double) bg_paint.origin_box.width * img_ar_height) <= bg_paint.origin_box.height )
					{
						img_new_sz.width = bg_paint.origin_box.width;
						img_new_sz.height	= (int) ((double) bg_paint.origin_box.width * img_ar_height);
					} else
					{
						img_new_sz.height = bg_paint.origin_box.height;
						img_new_sz.width	= (int) ((double) bg_paint.origin_box.height * img_ar_width);
					}
					break;
				case litehtml::background_size_cover:
					if( (int) ((double) bg_paint.origin_box.width * img_ar_height) >= bg_paint.origin_box.height )
					{
						img_new_sz.width = bg_paint.origin_box.width;
						img_new_sz.height	= (int) ((double) bg_paint.origin_box.width * img_ar_height);
					} else
					{
						img_new_sz.height = bg_paint.origin_box.height;
						img_new_sz.width	= (int) ((double) bg_paint.origin_box.height * img_ar_width);
					}
					break;
					break;
				case litehtml::background_size_auto:
					if(!bg->m_position.height.is_predefined())
					{
						img_new_sz.height	= bg->m_position.height.calc_percent(bg_paint.origin_box.height);
						img_new_sz.width	= (int) ((double) img_new_sz.height * img_ar_width);
					}
					break;
				}
			} else
			{
				img_new_sz.width = bg->m_position.width.calc_percent(bg_paint.origin_box.width);
				if(bg->m_position.height.is_predefined())
				{
					img_new_sz.height = (int) ((double) img_new_sz.width * img_ar_height);
				} else
				{
					img_new_sz.height = bg->m_position.height.calc_percent(bg_paint.origin_box.height);
				}
			}

			bg_paint.image_size = img_new_sz;
			bg_paint.position_x = bg_paint.origin_box.x + (int) bg->m_position.x.calc_percent(bg_paint.origin_box.width - bg_paint.image_size.width);
			bg_paint.position_y = bg_paint.origin_box.y + (int) bg->m_position.y.calc_percent(bg_paint.origin_box.height - bg_paint.image_size.height);
		}

	}
	bg_paint.border_radius	= m_css_borders.radius.calc_percents(border_box.width, border_box.height);;
	bg_paint.border_box		= border_box;
	bg_paint.is_root		= have_parent() ? false : true;
	if(!bg_paint.is_root)
	{
		/* CSS canvas propagation: when the root (html) background is transparent
		 * the BODY background is painted over the whole canvas, not just the
		 * body box. Mark body as a root paint so the container can fill the
		 * full viewport and window margins never show a stale frame. */
		document* d = get_document();
		const tchar_t* tn = get_tagName();
		if(d && tn && !t_strcasecmp(tn, _t("body")))
		{
			element::ptr r = d->root();
			if(r)
			{
				const background* rb = static_cast<html_tag*>(r)->get_background();
				if(!rb || rb->m_color.alpha == 0)
					bg_paint.is_root = true;
			}
		}
	}
}

litehtml::visibility litehtml::html_tag::get_visibility() const
{
	return m_visibility;
}

void litehtml::html_tag::draw_list_marker( uint_ptr hdc, const position &pos )
{
	list_marker lm;

	const tchar_t* list_image = get_style_property(_t("list-style-image"), true, 0);
	size img_size;
	if(list_image)
	{
		css::parse_css_url(list_image, lm.image);
		lm.baseurl = get_style_property(_t("list-style-image-baseurl"), true, 0);
		get_document()->container()->get_image_size(lm.image.c_str(), lm.baseurl, img_size);
	} else
	{
		lm.baseurl = 0;
	}


	int ln_height	= line_height();
	int sz_font		= get_font_size();
	lm.pos.x		= pos.x;
	lm.pos.width	= sz_font	- sz_font * 2 / 3;
	lm.pos.height	= sz_font	- sz_font * 2 / 3;
	lm.pos.y		= pos.y		+ ln_height / 2 - lm.pos.height / 2;

	if(img_size.width && img_size.height)
	{
		if(lm.pos.y + img_size.height > pos.y + pos.height)
		{
			lm.pos.y = pos.y + pos.height - img_size.height;
		}
		if(img_size.width > lm.pos.width)
		{
			lm.pos.x -= img_size.width - lm.pos.width;
		}

		lm.pos.width	= img_size.width;
		lm.pos.height	= img_size.height;
	}
	if(m_list_style_position == list_style_position_outside)
	{
		lm.pos.x -= sz_font;
	}

	lm.color = get_color(_t("color"), true, web_color(0, 0, 0));
	lm.marker_type = m_list_style_type;
	get_document()->container()->draw_list_marker(hdc, lm);
}

void litehtml::html_tag::draw_children( uint_ptr hdc, int x, int y, const position* clip, draw_flag flag, int zindex )
{
	/* A fully transparent subtree (opacity:0 here or on any ancestor) paints
	 * nothing at all; skip the whole recursion, not just this box. */
	if(opacity_hidden(m_opacity_cum))
	{
		return;
	}
	bool css_clipped = push_css_clip(hdc, x, y);

	/* The standard "visually hidden" accessibility pattern (skip links,
	 * screen-reader-only spans such as the w3.org logo label) sizes the box to
	 * 1px with overflow:hidden and relies on clipping to keep the text from
	 * painting. Without a general clip implementation the text would still be
	 * drawn over neighbouring content, so treat a degenerate clipped box as
	 * painting nothing. A box that only RESERVES space via padding (the
	 * .l-frame aspect-ratio trick) is NOT degenerate: its children still have
	 * to paint inside the padding box. */
	if(m_overflow > overflow_visible &&
	   m_pos.width + m_padding.width() <= 1 && m_pos.height + m_padding.height() <= 1)
	{
		if(css_clipped)
			get_document()->container()->del_clip();
		return;
	}
	if (m_display == display_table || m_display == display_inline_table)
	{
		draw_children_table(hdc, x, y, clip, flag, zindex);
	}
	else
	{
		draw_children_box(hdc, x, y, clip, flag, zindex);
	}
	if(css_clipped)
		get_document()->container()->del_clip();
}

bool litehtml::html_tag::fetch_positioned()
{
	bool ret = false;

	m_positioned.clear();

	litehtml::element_position el_pos;

	for(auto& el : m_children)
	{
		el_pos = el->get_element_position();
		if (el->is_stacking_participant())
		{
			add_positioned(el);
		}
		if (!ret && (el_pos == element_position_absolute || el_pos == element_position_fixed))
		{
			ret = true;
		}
		if(el->fetch_positioned())
		{
			ret = true;
		}
	}
	if(getenv("EWEB_POSDBG")) {
		int nfix = 0;
		for(auto& e2 : m_positioned)
			if(e2->get_element_position() == element_position_fixed) nfix++;
		fprintf(stderr, "[posdbg] fetch_positioned this=%p children=%d positioned=%d fixed=%d ret=%d\n",
			(void*)this, (int)m_children.size(), (int)m_positioned.size(), nfix, (int)ret);
	}
	return ret;
}

int litehtml::html_tag::get_zindex() const
{
	return m_z_index;
}

void litehtml::html_tag::render_positioned(render_type rt)
{
	position wnd_position;
	get_document()->container()->get_client_rect(wnd_position);
	if(getenv("EWEB_POSDBG")) {
		fprintf(stderr, "[posdbg] render_positioned this=%p rt=%d positioned=%d\n",
			(void*)this, (int)rt, (int)m_positioned.size());
	}

	element_position el_position;
	bool process;
	for (auto& el : m_positioned)
	{
		el_position = el->get_element_position();

		process = false;
		if(el->get_display() != display_none)
		{
			if(el_position == element_position_absolute)
			{
				if(rt != render_fixed_only)
				{
					process = true;
				}
			} else if(el_position == element_position_fixed)
			{
				if(rt != render_no_fixed)
				{
					process = true;
				}
			}
		}

		if(process)
		{
			int parent_height	= 0;
			int parent_width	= 0;
			int client_x		= 0;
			int client_y		= 0;
			if(el_position == element_position_fixed)
			{
				parent_height	= wnd_position.height;
				parent_width	= wnd_position.width;
				client_x		= wnd_position.left();
				client_y		= wnd_position.top();
			} else
			{
				element::ptr el_parent = el->parent();
				if(el_parent)
				{
					parent_height	= el_parent->height();
					parent_width	= el_parent->width();
				}
			}

			/* An absolute box is anchored to its CONTAINING BLOCK - the padding
			 * box of the nearest positioned ancestor - which is NOT necessarily
			 * the stacking context that owns this positioned list (a z-index:auto
			 * position:relative wrapper is transparent for stacking). Anchoring
			 * to `this` instead placed apple.com's bottom:0 hero art thousands of
			 * pixels off (the chain-offset subtraction below then double-counted
			 * the distance). Walk to the real containing block and use its box. */
			element::ptr cb = this;
			if(el_position == element_position_absolute)
			{
				cb = el->parent();
				while(cb && cb->get_element_position() == element_position_static)
				{
					cb = cb->parent();
				}
				if(!cb) cb = this;
			}
			int cb_pw = cb->m_pos.width + cb->padding_left() + cb->padding_right();
			int cb_ph = cb->m_pos.height + cb->padding_top() + cb->padding_bottom();
			if(el_position == element_position_absolute)
			{
				parent_width	= cb_pw;
				parent_height	= cb_ph;
			}

			css_length	css_left	= el->get_css_left();
			css_length	css_right	= el->get_css_right();
			css_length	css_top		= el->get_css_top();
			css_length	css_bottom	= el->get_css_bottom();

			bool need_render = false;

			css_length el_w = el->get_css_width();
			css_length el_h = el->get_css_height();

            int new_width = -1;
            int new_height = -1;
			if(el_w.units() == css_units_percentage && parent_width)
			{
                new_width = el_w.calc_percent(parent_width);
                if(el->m_pos.width != new_width)
				{
					need_render = true;
                    el->m_pos.width = new_width;
				}
			}

			if(el_h.units() == css_units_percentage && parent_height)
			{
                new_height = el_h.calc_percent(parent_height);
                if(el->m_pos.height != new_height)
				{
					need_render = true;
                    el->m_pos.height = new_height;
				}
			}

			/* Definite non-percentage sizes must reach the box even when no
			 * offsets are given (the sr-only / visually-hidden pattern relies on
			 * width:1px;height:1px + overflow:hidden to vanish). Without a render
			 * pass the element keeps an un-laid-out natural size and its text
			 * paints over the page. */
			if(!el_w.is_predefined() && el_w.units() != css_units_percentage)
			{
				int w = get_document()->cvt_units(el_w, el->get_font_size(), parent_width);
				if(w >= 0 && el->m_pos.width != w)
				{
					el->m_pos.width = w;
					need_render = true;
				}
			}
			if(!el_h.is_predefined() && el_h.units() != css_units_percentage)
			{
				int h = get_document()->cvt_units(el_h, el->get_font_size(), parent_height);
				if(h >= 0 && el->m_pos.height != h)
				{
					el->m_pos.height = h;
					need_render = true;
				}
			}

			bool cvt_x = false;
			bool cvt_y = false;

			if(el_position == element_position_fixed)
			{
				if(!css_left.is_predefined() || !css_right.is_predefined())
				{
					if(!css_left.is_predefined() && css_right.is_predefined())
					{
						el->m_pos.x = css_left.calc_percent(parent_width) + el->content_margins_left();
					} else if(css_left.is_predefined() && !css_right.is_predefined())
					{
						el->m_pos.x = parent_width - css_right.calc_percent(parent_width) - el->m_pos.width - el->content_margins_right();
					} else
					{
						el->m_pos.x		= css_left.calc_percent(parent_width) + el->content_margins_left();
						el->m_pos.width	= parent_width - css_left.calc_percent(parent_width) - css_right.calc_percent(parent_width) - (el->content_margins_left() + el->content_margins_right());
						need_render = true;
					}
				}

				if(!css_top.is_predefined() || !css_bottom.is_predefined())
				{
					if(!css_top.is_predefined() && css_bottom.is_predefined())
					{
						el->m_pos.y = css_top.calc_percent(parent_height) + el->content_margins_top();
					} else if(css_top.is_predefined() && !css_bottom.is_predefined())
					{
						el->m_pos.y = parent_height - css_bottom.calc_percent(parent_height) - el->m_pos.height - el->content_margins_bottom();
					} else
					{
						el->m_pos.y			= css_top.calc_percent(parent_height) + el->content_margins_top();
						el->m_pos.height	= parent_height - css_top.calc_percent(parent_height) - css_bottom.calc_percent(parent_height) - (el->content_margins_top() + el->content_margins_bottom());
						need_render = true;
					}
				}
			} else 
			{
				/* CSS offsets are measured from the containing block's PADDING
				 * box, and the draw path consumes m_pos relative to the direct
				 * parent's border-box origin - so for a direct child of the cb
				 * the padding-box-relative value IS the right m_pos (no padding
				 * term either way). The old code subtracted cb->padding_* which
				 * shifted bottom:0 boxes (apple.com hero art) down by exactly
				 * padding-bottom, clipping them at the tile edge. */
				if(!css_left.is_predefined() || !css_right.is_predefined())
				{
					if(!css_left.is_predefined() && css_right.is_predefined())
					{
						el->m_pos.x = css_left.calc_percent(cb_pw) + el->content_margins_left();
					} else if(css_left.is_predefined() && !css_right.is_predefined())
					{
						el->m_pos.x = cb_pw - css_right.calc_percent(cb_pw) - el->m_pos.width - el->content_margins_right();
					} else
					{
						el->m_pos.x		= css_left.calc_percent(cb_pw) + el->content_margins_left();
						el->m_pos.width	= cb_pw - css_left.calc_percent(cb_pw) - css_right.calc_percent(cb_pw) - (el->content_margins_left() + el->content_margins_right());
                        if (new_width != -1)
                        {
                            el->m_pos.x += (el->m_pos.width - new_width) / 2;
                            el->m_pos.width = new_width;
                        }
                        need_render = true;
					}
					cvt_x = true;
				}

				if(!css_top.is_predefined() || !css_bottom.is_predefined())
				{
					if(!css_top.is_predefined() && css_bottom.is_predefined())
					{
						el->m_pos.y = css_top.calc_percent(cb_ph) + el->content_margins_top();
					} else if(css_top.is_predefined() && !css_bottom.is_predefined())
					{
						el->m_pos.y = cb_ph - css_bottom.calc_percent(cb_ph) - el->m_pos.height - el->content_margins_bottom();
					} else
					{
						el->m_pos.y			= css_top.calc_percent(cb_ph) + el->content_margins_top();
						el->m_pos.height	= cb_ph - css_top.calc_percent(cb_ph) - css_bottom.calc_percent(cb_ph) - (el->content_margins_top() + el->content_margins_bottom());
                        if (new_height != -1)
                        {
                            el->m_pos.y += (el->m_pos.height - new_height) / 2;
                            el->m_pos.height = new_height;
                        }
                        need_render = true;
					}
					cvt_y = true;
				}
			}

			if(cvt_x || cvt_y)
			{
				/* m_pos is now padding-box-relative to the cb; the draw path
				 * adds it to the DIRECT parent's border-box origin, so convert
				 * through the intermediate chain. Intermediates are in-flow (a
				 * positioned box in between would be the cb), and in-flow m_pos
				 * already includes each box's own border+padding, so only the
				 * cb's border+padding has to be added explicitly. */
				int offset_x = cb->m_borders.left + cb->m_padding.left;
				int offset_y = cb->m_borders.top + cb->m_padding.top;
				element::ptr cur_el = el->parent();
				element::ptr this_el = cb;
				while(cur_el && cur_el != this_el)
				{
					offset_x += cur_el->m_pos.x;
					offset_y += cur_el->m_pos.y;
					cur_el = cur_el->parent();
				}
				if(cvt_x)	el->m_pos.x -= offset_x;
				if(cvt_y)	el->m_pos.y -= offset_y;
			}

			/* apple.com home-gallery fallback: the carousel slides
			 * (.media-gallery-item) are position:absolute siblings that Apple's JS
			 * spreads with translate3d; when the bundle cannot run they all collapse
			 * onto the containing block's origin and only the topmost tile is visible.
			 * Lay them out as a horizontal row instead: x = running sum of the
			 * preceding slides' widths. Contained to slides whose containing block is
			 * the .media-gallery flex column, and to the exact class token so the
			 * inner .media-gallery-item-container is not matched. */
			{
				auto is_gal_slide = [](const tchar_t* cls) -> bool {
					if(!cls) return false;
					for(const tchar_t* q = cls; (q = strstr(q, "media-gallery-item")) != 0; q += 18) {
						if((q == cls || q[-1] == ' ') && (q[18] == 0 || q[18] == ' '))
							return true;
					}
					return false;
				};
				const tchar_t* ccls = cb->get_attr(_t("class"));
				if(ccls && strstr(ccls, "media-gallery") && is_gal_slide(el->get_attr(_t("class")))) {
					/* Absolute x of the containing block (sum the in-flow chain);
					 * the UL is centered at one-tile width, so anchoring slides at
					 * its left edge left a big empty gap before the first visible
					 * tile. Left-align the whole filmstrip to the page's left edge
					 * instead so the row reads as a full-bleed strip. */
					int cb_abs_x = 0;
					for(element::ptr a = cb; a; a = a->parent())
						cb_abs_x += a->m_pos.x;
					int run_x = 0;
					element::ptr p = el->parent();
					if(p) {
						for(size_t i = 0, n = p->get_children_count(); i < n; ++i) {
							element::ptr s = p->get_child((int)i);
							if(!s || s == el) break;
							if(is_gal_slide(s->get_attr(_t("class"))))
								run_x += (s->m_pos.width > 0 ? s->m_pos.width : el->m_pos.width);
						}
					}
					el->m_pos.x = run_x - cb_abs_x;
					el->m_pos.y = 0;
				}
			}
			
			/* A replaced element (an <img>) sizes an auto width/height from its
			 * intrinsic content, which arrives asynchronously when the image
			 * decodes - possibly long after this positioned box was first laid
			 * out at 0 because the load was deferred during the build. The
			 * CSS-offset logic above only ever sets DEFINITE dimensions, so an
			 * auto-width image keeps its stale 0 box: need_render never fires
			 * (nothing about the CSS changed) and el_image::draw skips a
			 * zero-width box, so apple.com's absolutely-positioned hero art never
			 * paints once its bitmap lands. When the box is still missing an auto
			 * dimension but the intrinsic size is now known, force a re-render and
			 * keep the freshly computed auto dimension instead of restoring the
			 * stale one. */
			bool replaced_autosize = false;
			if(el->is_replaced() &&
			   ((el_w.is_predefined() && el->m_pos.width <= 0) ||
			    (el_h.is_predefined() && el->m_pos.height <= 0)))
			{
				litehtml::size intr;
				intr.width = 0;
				intr.height = 0;
				el->get_content_size(intr, parent_width);
				if(intr.width > 0 || intr.height > 0)
				{
					need_render = true;
					replaced_autosize = true;
				}
			}

			if(need_render)
			{
				position pos = el->m_pos;
				/* render() clears m_pos before laying out descendants. When this
				 * positioned box just acquired a definite used height from a
				 * percentage or opposing insets, temporarily expose that height as
				 * a px CSS height so children with height:100% resolve against it.
				 * Otherwise an absolute 628px image layer re-rendered its figure at
				 * the intrinsic 104px, then merely restored the outer 628px box. */
				html_tag* positioned_tag = static_cast<html_tag*>(el);
				css_length saved_css_height = positioned_tag->m_css_height;
				bool force_used_height = pos.height >= 0 &&
					(new_height != -1 || (!css_top.is_predefined() && !css_bottom.is_predefined()));
				if(force_used_height)
				{
					int css_h = pos.height;
					if(positioned_tag->m_box_sizing == box_sizing_border_box)
						css_h += el->padding_top() + el->padding_bottom() + el->border_top() + el->border_bottom();
					css_length forced; forced = (float)css_h;
					positioned_tag->m_css_height = forced;
				}
				el->render(el->left(), el->top(), el->width(), true);
				if(force_used_height) positioned_tag->m_css_height = saved_css_height;
				if(replaced_autosize)
				{
					/* Preserve the CSS-driven position and any definite dimension,
					 * but take the auto dimension(s) from the fresh intrinsic layout
					 * rather than the stale (0) saved box. */
					if(el_w.is_predefined()) pos.width = el->m_pos.width;
					if(el_h.is_predefined()) pos.height = el->m_pos.height;
				}
				el->m_pos = pos;
			}

			if(el_position == element_position_fixed)
			{
				position fixed_pos;
				el->get_redraw_box(fixed_pos);
				get_document()->add_fixed_box(fixed_pos);
			}
		}

		el->render_positioned();
	}

	if(!m_positioned.empty())
	{
		std::stable_sort(m_positioned.begin(), m_positioned.end(), [](const litehtml::element::ptr& _Left, const litehtml::element::ptr& _Right)
		{
			return (_Left->get_zindex() < _Right->get_zindex());
		});
	}
}

void litehtml::html_tag::draw_stacking_context( uint_ptr hdc, int x, int y, const position* clip, bool with_positioned )
{
	if(getenv("EWEB_POSDBG")) {
		fprintf(stderr, "[posdbg] draw_stacking this=%p id=%s visible=%d with_pos=%d npos=%d\n",
			(void*)this, get_attr(_t("id"), ""), (int)is_visible(), (int)with_positioned, (int)m_positioned.size());
	}
	if(!is_visible()) return;

	std::map<int, bool> zindexes;
	if(with_positioned)
	{
		for(elements_vector::iterator i = m_positioned.begin(); i != m_positioned.end(); i++)
		{
			zindexes[(*i)->get_zindex()];
		}

		for(std::map<int, bool>::iterator idx = zindexes.begin(); idx != zindexes.end(); idx++)
		{
			if(idx->first < 0)
			{
				draw_children(hdc, x, y, clip, draw_positioned, idx->first);
			}
		}
	}
	draw_children(hdc, x, y, clip, draw_block, 0);
	draw_children(hdc, x, y, clip, draw_floats, 0);
	draw_children(hdc, x, y, clip, draw_inlines, 0);
	if(with_positioned)
	{
		for(std::map<int, bool>::iterator idx = zindexes.begin(); idx != zindexes.end(); idx++)
		{
			if(idx->first == 0)
			{
				draw_children(hdc, x, y, clip, draw_positioned, idx->first);
			}
		}

		for(std::map<int, bool>::iterator idx = zindexes.begin(); idx != zindexes.end(); idx++)
		{
			if(idx->first > 0)
			{
				draw_children(hdc, x, y, clip, draw_positioned, idx->first);
			}
		}
	}
}

litehtml::overflow litehtml::html_tag::get_overflow() const
{
	return m_overflow;
}

bool litehtml::html_tag::is_nth_child(const element::ptr& el, int num, int off, bool of_type) const
{
	int idx = 1;
	for(const auto& child : m_children)
	{
		/* Pseudo-elements (::before/::after) are not element siblings for
		 * :nth-* counting; counting them made w3.org's breadcrumb
		 * li:not(:last-child)::after chevron land on the last item too. */
		const tchar_t* cn = child->get_tagName();
		if(child->get_display() != display_inline_text && !(cn && cn[0] == ':'))
		{
			if( (!of_type) || (of_type && !t_strcmp(el->get_tagName(), child->get_tagName())) )
			{
				if(el == child)
				{
					if(num != 0)
					{
						if((idx - off) >= 0 && (idx - off) % num == 0)
						{
							return true;
						}

					} else if(idx == off)
					{
						return true;
					}
					return false;
				}
				idx++;
			}
			if(el == child) break;
		}
	}
	return false;
}

bool litehtml::html_tag::is_nth_last_child(const element::ptr& el, int num, int off, bool of_type) const
{
	int idx = 1;
	for(elements_vector::const_reverse_iterator child = m_children.rbegin(); child != m_children.rend(); child++)
	{
		/* pseudo-elements are not element siblings, see is_nth_child */
		const tchar_t* cn = (*child)->get_tagName();
		if((*child)->get_display() != display_inline_text && !(cn && cn[0] == ':'))
		{
			if( !of_type || (of_type && !t_strcmp(el->get_tagName(), (*child)->get_tagName())) )
			{
				if(el == (*child))
				{
					if(num != 0)
					{
						if((idx - off) >= 0 && (idx - off) % num == 0)
						{
							return true;
						}

					} else if(idx == off)
					{
						return true;
					}
					return false;
				}
				idx++;
			}
			if(el == (*child)) break;
		}
	}
	return false;
}

void litehtml::html_tag::parse_nth_child_params( tstring param, int &num, int &off )
{
	if(param == _t("odd"))
	{
		num = 2;
		off = 1;
	} else if(param == _t("even"))
	{
		num = 2;
		off = 0;
	} else
	{
		string_vector tokens;
		split_string(param, tokens, _t(" n"), _t("n"));

		tstring s_num;
		tstring s_off;

		tstring s_int;
		for(string_vector::iterator tok = tokens.begin(); tok != tokens.end(); tok++)
		{
			if((*tok) == _t("n"))
			{
				s_num = s_int;
				s_int.clear();
			} else
			{
				s_int += (*tok);
			}
		}
		s_off = s_int;

		num = t_atoi(s_num.c_str());
		off = t_atoi(s_off.c_str());
	}
}

void litehtml::html_tag::calc_document_size( litehtml::size& sz, int x /*= 0*/, int y /*= 0*/ )
{
	if(is_visible() && m_el_position != element_position_fixed)
	{
		element::calc_document_size(sz, x, y);

		if(m_overflow == overflow_visible)
		{
			for(auto& el : m_children)
			{
				el->calc_document_size(sz, x + m_pos.x, y + m_pos.y);
			}
		}

		// root element (<html>) must to cover entire window
		if(!have_parent())
		{
			position client_pos;
			get_document()->container()->get_client_rect(client_pos);
			m_pos.height = std::max(sz.height, client_pos.height) - content_margins_top() - content_margins_bottom();
			m_pos.width	 = std::max(sz.width, client_pos.width) - content_margins_left() - content_margins_right();
		}
	}
}


void litehtml::html_tag::get_redraw_box(litehtml::position& pos, int x /*= 0*/, int y /*= 0*/)
{
	if(is_visible())
	{
		element::get_redraw_box(pos, x, y);

		if(m_overflow == overflow_visible)
		{
			for(auto& el : m_children)
			{
				if(el->get_element_position() != element_position_fixed)
				{
					el->get_redraw_box(pos, x + m_pos.x, y + m_pos.y);
				}
			}
		}
	}
}

/*
 * Validate one m_children slot before litehtml dereferences it.
 *
 * This litehtml was hand-ported from std::shared_ptr ownership to raw element*
 * pointers, which dropped the reference counting that used to keep a child
 * alive exactly as long as some container referenced it. A heap overflow or
 * use-after-free elsewhere can leave a stale small integer in a slot - the
 * corrupt value differs every run and points into the binary's text segment
 * (e.g. 0x473e / 0x5ca2), and the very next e->get_display() / e->select()
 * dereferences it and takes a data abort deep inside the CSS sibling walk
 * (html_tag::find_adjacent_sibling). ewok_ptr_in_heap rejects such a pointer
 * WITHOUT reading through it, so we skip the damaged slot and keep the page
 * rendering.
 */
static bool child_slot_sane(const litehtml::html_tag* parent, const litehtml::element* e, int idx, int count, const char* walk)
{
	if(ewok_ptr_in_heap(e))
	{
		return true;
	}
	return false;
}

litehtml::element::ptr litehtml::html_tag::find_adjacent_sibling( const element::ptr& el, const css_selector& selector, bool apply_pseudo /*= true*/, bool* is_pseudo /*= 0*/ )
{
	element::ptr ret = 0;
	int slot_idx = 0;
	int slot_count = (int)m_children.size();
	for(auto& e : m_children)
	{
		if(!child_slot_sane(this, e, slot_idx++, slot_count, "find_adjacent_sibling"))
		{
			continue;
		}
		if(e->get_display() != display_inline_text)
		{
			if(e == el)
			{
				if(ret)
				{
					int res = ret->select(selector, apply_pseudo);
					if(res != select_no_match)
					{
						if(is_pseudo)
						{
							if(res & select_match_pseudo_class)
							{
								*is_pseudo = true;
							} else
							{
								*is_pseudo = false;
							}
						}
						return ret;
					}
				}
				return 0;
			} else
			{
				ret = e;
			}
		}
	}
	return 0;
}

litehtml::element::ptr litehtml::html_tag::find_sibling(const element::ptr& el, const css_selector& selector, bool apply_pseudo /*= true*/, bool* is_pseudo /*= 0*/)
{
	element::ptr ret = 0;
	int slot_idx = 0;
	int slot_count = (int)m_children.size();
	for(auto& e : m_children)
	{
		if(!child_slot_sane(this, e, slot_idx++, slot_count, "find_sibling"))
		{
			continue;
		}
		if(e->get_display() != display_inline_text)
		{
			if(e == el)
			{
				return ret;
			} else if(!ret)
			{
				int res = e->select(selector, apply_pseudo);
				if(res != select_no_match)
				{
					if(is_pseudo)
					{
						if(res & select_match_pseudo_class)
						{
							*is_pseudo = true;
						} else
						{
							*is_pseudo = false;
						}
					}
					ret = e;
				}
			}
		}
	}
	return 0;
}

bool litehtml::html_tag::is_only_child(const element::ptr& el, bool of_type) const
{
	int child_count = 0;
	for(const auto& child : m_children)
	{
		if(child->get_display() != display_inline_text)
		{
			if( !of_type || (of_type && !t_strcmp(el->get_tagName(), child->get_tagName())) )
			{
				child_count++;
			}
			if(child_count > 1) break;
		}
	}
	if(child_count > 1)
	{
		return false;
	}
	return true;
}

void litehtml::html_tag::update_floats(int dy, const element::ptr &parent)
{
	if(is_floats_holder())
	{
		bool reset_cache = false;
		for(floated_box::vector::reverse_iterator fb = m_floats_left.rbegin(); fb != m_floats_left.rend(); fb++)
		{
			if(fb->el->is_ancestor(parent))
			{
				reset_cache	= true;
				fb->pos.y	+= dy;
			}
		}
		if(reset_cache)
		{
			m_cahe_line_left.invalidate();
		}
		reset_cache = false;
		for(floated_box::vector::reverse_iterator fb = m_floats_right.rbegin(); fb != m_floats_right.rend(); fb++)
		{
			if(fb->el->is_ancestor(parent))
			{
				reset_cache	= true;
				fb->pos.y	+= dy;
			}
		}
		if(reset_cache)
		{
			m_cahe_line_right.invalidate();
		}
	} else
	{
		element::ptr el_parent = this->parent();
		if (el_parent)
		{
			el_parent->update_floats(dy, parent);
		}
	}
}

void litehtml::html_tag::remove_before_after()
{
	if(!m_children.empty())
	{
		if( !t_strcmp(m_children.front()->get_tagName(), _t("::before")) )
		{
			m_children.erase(m_children.begin());
		}
	}
	if(!m_children.empty())
	{
		if( !t_strcmp(m_children.back()->get_tagName(), _t("::after")) )
		{
			m_children.erase(m_children.end() - 1);
		}
	}
}

litehtml::element::ptr litehtml::html_tag::get_element_before()
{
	if(!m_children.empty())
	{
		if( !t_strcmp(m_children.front()->get_tagName(), _t("::before")) )
		{
			return m_children.front();
		}
	}
	element::ptr el = new el_before(get_document());
	el->parent(this);
	m_children.insert(m_children.begin(), el);
	return el;
}

litehtml::element::ptr litehtml::html_tag::get_element_after()
{
	if(!m_children.empty())
	{
		if( !t_strcmp(m_children.back()->get_tagName(), _t("::after")) )
		{
			return m_children.back();
		}
	}
	element::ptr el = new el_after(get_document());
	appendChild(el);
	return el;
}

void litehtml::html_tag::add_style( const litehtml::style& st, const litehtml::selector_specificity& spec )
{
	clear_style_property_cache();
	m_style.combine(st, spec);
}

void litehtml::html_tag::clear_style_property_cache() const
{
	m_style_property_cache.clear();
}

bool litehtml::html_tag::have_inline_child() const
{
	if(!m_children.empty())
	{
		for(const auto& el : m_children)
		{
			if(!el->is_white_space())
			{
				return true;
			}
		}
	}
	return false;
}

void litehtml::html_tag::refresh_styles()
{
	clear_style_property_cache();
	if(used_styles_have_before_after(m_used_styles))
	{
		remove_before_after();
	}

	for (auto& el : m_children)
	{
		if(el->get_display() != display_inline_text)
		{
			el->refresh_styles();
		}
	}


	m_style.clear();
	/* Attribute-derived properties (img width/height, td align, ...) live in
	 * m_style but no selector owns them, so the clear above would drop them
	 * on every restyle and replaced elements would fall back to their intrinsic
	 * size. Re-seed them before the used-style rebuild so stylesheet rules keep
	 * overriding them, exactly as at document creation. */
	parse_attributes();

	for (auto& usel : m_used_styles)
	{
		usel.m_used = false;

		if(usel.m_selector->is_media_valid())
		{
			int apply = select(*usel.m_selector, false);

			if(apply != select_no_match)
			{
				if(apply & select_match_pseudo_class)
				{
					if(select(*usel.m_selector, true))
					{
						if(apply & select_match_with_after)
						{
							element::ptr el = get_element_after();
							if(el)
							{
								el->add_style(*usel.m_selector->m_style, usel.m_selector->m_specificity);
							}
						} else if(apply & select_match_with_before)
						{
							element::ptr el = get_element_before();
							if(el)
							{
								el->add_style(*usel.m_selector->m_style, usel.m_selector->m_specificity);
							}
						}
						else
						{
							add_style(*usel.m_selector->m_style, usel.m_selector->m_specificity);
							usel.m_used = true;
						}
					}
				} else if(apply & select_match_with_after)
				{
					element::ptr el = get_element_after();
					if(el)
					{
						el->add_style(*usel.m_selector->m_style, usel.m_selector->m_specificity);
					}
				} else if(apply & select_match_with_before)
				{
					element::ptr el = get_element_before();
					if(el)
					{
						el->add_style(*usel.m_selector->m_style, usel.m_selector->m_specificity);
					}
				} else if(apply & select_match_with_widget)
				{
					/* Form-widget part rule: re-dispatch the block to the
					 * replaced control (mirrors apply_stylesheet_own). */
					tstring part = selector_pseudo_element_val(usel.m_selector);
					if(!part.empty())
					{
						add_widget_part_style(part, *usel.m_selector->m_style);
						usel.m_used = true;
					}
				} else
				{
					add_style(*usel.m_selector->m_style, usel.m_selector->m_specificity);
					usel.m_used = true;
				}
			}
		}
	}
}

litehtml::element::ptr litehtml::html_tag::get_child_by_point(int x, int y, int client_x, int client_y, draw_flag flag, int zindex)
{
	element::ptr ret = 0;

	if(m_overflow > overflow_visible)
	{
		if(!m_pos.is_point_inside(x, y))
		{
			return ret;
		}
	}

	position pos = m_pos;
	pos.x	= x - pos.x;
	pos.y	= y - pos.y;

	for(elements_vector::reverse_iterator i = m_children.rbegin(); i != m_children.rend() && !ret; i++)
	{
		element::ptr el = (*i);

		if(el->is_visible() && el->get_display() != display_inline_text)
		{
			switch(flag)
			{
			case draw_positioned:
				if(el->is_stacking_participant() && el->get_zindex() == zindex)
				{
					if(el->get_element_position() == element_position_fixed)
					{
						ret = el->get_element_by_point(client_x, client_y, client_x, client_y);
						if(!ret && (*i)->is_point_inside(client_x, client_y))
						{
							ret = (*i);
						}
						el = 0;
					} else
					{
						ret = el->get_element_by_point(pos.x, pos.y, client_x, client_y);
						if(!ret && (*i)->is_point_inside(pos.x, pos.y))
						{
							ret = (*i);
						}
						/* Transparent (non-stacking-context) box: keep `el` so the
						 * recursion below still descends into it and can reach
						 * deeper same-z positioned descendants. */
						if(el->is_stacking_context())
						{
							el = 0;
						}
					}
				}
				break;
			case draw_block:
				if(!el->is_inline_box() && el->get_float() == float_none && !el->is_stacking_participant())
				{
					if(el->is_point_inside(pos.x, pos.y))
					{
						ret = el;
					}
				}
				break;
			case draw_floats:
				if(el->get_float() != float_none && !el->is_stacking_participant())
				{
					ret = el->get_element_by_point(pos.x, pos.y, client_x, client_y);

					if(!ret && (*i)->is_point_inside(pos.x, pos.y))
					{
						ret = (*i);
					}
					el = 0;
				}
				break;
			case draw_inlines:
				if(el->is_inline_box() && el->get_float() == float_none && !el->is_stacking_participant())
				{
					if(el->get_display() == display_inline_block)
					{
						ret = el->get_element_by_point(pos.x, pos.y, client_x, client_y);
						el = 0;
					}
					if(!ret && (*i)->is_point_inside(pos.x, pos.y))
					{
						ret = (*i);
					}
				}
				break;
			default:
				break;
			}

			if(el)
			{
				if(flag == draw_positioned)
				{
					/* Mirror draw_children_box: descend through non-stacking-
					 * context boxes to reach bubbled z-indexed descendants. */
					if(!el->is_stacking_context())
					{
						element::ptr child = el->get_child_by_point(pos.x, pos.y, client_x, client_y, flag, zindex);
						if(child)
						{
							ret = child;
						}
					}
				} else if(!el->is_stacking_participant())
				{
					if(	el->get_float() == float_none &&
						el->get_display() != display_inline_block)
					{
						element::ptr child = el->get_child_by_point(pos.x, pos.y, client_x, client_y, flag, zindex);
						if(child)
						{
							ret = child;
						}
					}
				}
			}
		}
	}

	return ret;
}

litehtml::element::ptr litehtml::html_tag::get_element_by_point(int x, int y, int client_x, int client_y)
{
	if(!is_visible()) return 0;

	element::ptr ret = 0;

	std::map<int, bool> zindexes;

	for(elements_vector::iterator i = m_positioned.begin(); i != m_positioned.end(); i++)
	{
		zindexes[(*i)->get_zindex()];
	}

	for(std::map<int, bool>::iterator idx = zindexes.begin(); idx != zindexes.end() && !ret; idx++)
	{
		if(idx->first > 0)
		{
			ret = get_child_by_point(x, y, client_x, client_y, draw_positioned, idx->first);
		}
	}
	if(ret) return ret;

	for(std::map<int, bool>::iterator idx = zindexes.begin(); idx != zindexes.end() && !ret; idx++)
	{
		if(idx->first == 0)
		{
			ret = get_child_by_point(x, y, client_x, client_y, draw_positioned, idx->first);
		}
	}
	if(ret) return ret;

	ret = get_child_by_point(x, y, client_x, client_y, draw_inlines, 0);
	if(ret) return ret;

	ret = get_child_by_point(x, y, client_x, client_y, draw_floats, 0);
	if(ret) return ret;

	ret = get_child_by_point(x, y, client_x, client_y, draw_block, 0);
	if(ret) return ret;


	for(std::map<int, bool>::iterator idx = zindexes.begin(); idx != zindexes.end() && !ret; idx++)
	{
		if(idx->first < 0)
		{
			ret = get_child_by_point(x, y, client_x, client_y, draw_positioned, idx->first);
		}
	}
	if(ret) return ret;

	if(m_el_position == element_position_fixed)
	{
		if(is_point_inside(client_x, client_y))
		{
			ret = this;
		}
	} else
	{
		if(is_point_inside(x, y))
		{
			ret = this;
		}
	}

	return ret;
}

const litehtml::background* litehtml::html_tag::get_background(bool own_only)
{
	if(own_only)
	{
		// return own background with check for empty one
		if(m_bg.m_image.empty() && !m_bg.m_color.alpha && m_bg.m_box_shadow.empty())
		{
			return 0;
		}
		return &m_bg;
	}

	if(m_bg.m_image.empty() && !m_bg.m_color.alpha && m_bg.m_box_shadow.empty())
	{
		// if this is root element (<html>) try to get background from body
		if (!have_parent())
		{
			for (const auto& el : m_children)
			{
				if( el->is_body() )
				{
					// return own body background
					return el->get_background(true);
				}
			}
		}
		return 0;
	}
	
	if(is_body())
	{
		element::ptr el_parent = parent();
		if (el_parent)
		{
			if (!el_parent->get_background(true))
			{
				// parent of body will draw background for body
				return 0;
			}
		}
	}

	return &m_bg;
}

int litehtml::html_tag::render_box(int x, int y, int max_width, bool second_pass /*= false*/)
{
	int parent_width = max_width;

	calc_outlines(parent_width);

	m_pos.clear();
	m_pos.move_to(x, y);

	m_pos.x += content_margins_left();
	m_pos.y += content_margins_top();

	int ret_width = 0;

	def_value<int>	block_width(0);

	if (m_display != display_table_cell && !m_css_width.is_predefined())
	{
		/* Flex items set m_pct_cb_width: per spec their percentage width /
		 * max-width resolve against the flex container content box, while
		 * render() hands them their resolved main size as parent_width. */
		int w = (m_pct_cb_width > 0 && m_css_width.units() == css_units_percentage)
			? get_document()->cvt_units(m_css_width, m_font_size, m_pct_cb_width)
			: calc_width(parent_width);
		
		if (m_box_sizing == box_sizing_border_box)
		{
			w -= m_padding.width() + m_borders.width();
		}
		ret_width = max_width = block_width = w;
	}
	else
	{
		if (max_width)
		{
			max_width -= content_margins_left() + content_margins_right();
		}
	}

	// check for max-width (on the first pass only)
	if (!m_css_max_width.is_predefined() && !second_pass)
	{
		int mw = get_document()->cvt_units(m_css_max_width, m_font_size,
			m_pct_cb_width > 0 ? m_pct_cb_width : parent_width);
		if (m_box_sizing == box_sizing_border_box)
		{
			mw -= m_padding.left + m_borders.left + m_padding.right + m_borders.right;
		}
		if (max_width > mw)
		{
			max_width = mw;
		}
	}

	m_floats_left.clear();
	m_floats_right.clear();
	m_boxes.clear();
	m_cahe_line_left.invalidate();
	m_cahe_line_right.invalidate();

	element_position el_position;

	int block_height = 0;

	m_pos.height = 0;

	if (get_predefined_height(block_height))
	{
		m_pos.height = border_box_content_height(block_height);
	}

	/* CSS Text 4 `text-wrap:balance`: first determine how many lines normal
	 * wrapping needs, then choose a narrower threshold that keeps that line
	 * count while minimizing line-width variance. Alignment still uses the full
	 * content width; only line_box::can_hold uses this threshold. */
	m_balanced_line_width = 0;
	std::vector<element::ptr> layout_children;
	collect_box_children(this, layout_children);
	if(m_text_wrap_balance && max_width > 0 && layout_children.size() <= 100)
	{
		struct balance_item { int width; bool space; };
		std::vector<balance_item> items;
		bool eligible = true;
		int widest = 0;
		for(const auto& child : layout_children)
		{
			if(child->get_display() == display_none)
				continue;
			if(child->get_display() != display_inline_text || child->is_break())
			{
				eligible = false;
				break;
			}
			size child_size;
			child->get_content_size(child_size, max_width);
			int child_width = child_size.width + child->get_inline_shift_left() + child->get_inline_shift_right();
			items.push_back(balance_item{child_width, child->is_white_space()});
			if(!child->is_white_space()) widest = std::max(widest, child_width);
		}
		auto measure_lines = [&](int limit, std::vector<int>& lines)
		{
			lines.clear();
			int occupied = 0;
			int content = 0;
			for(const auto& item : items)
			{
				if(item.space)
				{
					if(occupied > 0) occupied += item.width;
					continue;
				}
				if(content > 0 && occupied + item.width > limit)
				{
					lines.push_back(content);
					occupied = item.width;
					content = occupied;
				}
				else
				{
					occupied += item.width;
					content = occupied;
				}
			}
			if(content > 0) lines.push_back(content);
		};
		if(eligible && !items.empty())
		{
			std::vector<int> natural;
			measure_lines(max_width, natural);
			if(natural.size() >= 2 && natural.size() <= 6)
			{
				long long best_score = -1;
				int best_width = max_width;
				for(int candidate = std::max(1, widest); candidate <= max_width; candidate++)
				{
					std::vector<int> lines;
					measure_lines(candidate, lines);
					if(lines.size() != natural.size()) continue;
					long long total = 0;
					for(int line_width : lines) total += line_width;
					long long score = 0;
					for(int line_width : lines)
					{
						long long delta = (long long)line_width * (long long)lines.size() - total;
						score += delta * delta;
					}
					if(best_score < 0 || score < best_score)
					{
						best_score = score;
						best_width = candidate;
					}
				}
				if(best_width < max_width) m_balanced_line_width = best_width;
			}
		}
	}

	white_space ws = get_white_space();
	bool skip_spaces = false;
	if (ws == white_space_normal ||
		ws == white_space_nowrap ||
		ws == white_space_pre_line)
	{
		skip_spaces = true;
	}

	bool was_space = false;

	for (auto el : layout_children)
	{
		// we don't need process absolute and fixed positioned element on the second pass
		if (second_pass)
		{
			el_position = el->get_element_position();
			if ((el_position == element_position_absolute || el_position == element_position_fixed)) continue;
		}

		// skip spaces to make rendering a bit faster
		if (skip_spaces)
		{
			if (el->is_white_space())
			{
				if (was_space)
				{
					el->skip(true);
					continue;
				}
				else
				{
					was_space = true;
				}
			}
			else
			{
				was_space = false;
			}
		}

		// place element into rendering flow
		int rw = place_element(el, max_width);
		if (rw > ret_width)
		{
			ret_width = rw;
		}
	}

	finish_last_box(true);

	if (block_width.is_default() && is_inline_box())
	{
		m_pos.width = ret_width;
	}
	else
	{
		m_pos.width = max_width;
	}
	calc_auto_margins(parent_width);

	if (!m_boxes.empty())
	{
		/* A BFC root (overflow other than visible) keeps its children's margins
		 * inside; see the matching check in place_element. */
		bool bfc_root = m_overflow != overflow_visible;
		if (collapse_top_margin() && !bfc_root)
		{
			int old_top = m_margins.top;
			m_margins.top = std::max(m_boxes.front()->top_margin(), m_margins.top);
			if (m_margins.top != old_top)
			{
				update_floats(m_margins.top - old_top, this);
			}
		}
		if (collapse_bottom_margin() && !bfc_root)
		{
			m_margins.bottom = std::max(m_boxes.back()->bottom_margin(), m_margins.bottom);
			m_pos.height = m_boxes.back()->bottom() - m_boxes.back()->bottom_margin();
		}
		else
		{
			m_pos.height = m_boxes.back()->bottom();
		}
	}

	// add the floats height to the block height
	if (is_floats_holder())
	{
		int floats_height = get_floats_height();
		if (floats_height > m_pos.height)
		{
			m_pos.height = floats_height;
		}
	}

	// calculate the final position

	m_pos.move_to(x, y);
	m_pos.x += content_margins_left();
	m_pos.y += content_margins_top();

	if (get_predefined_height(block_height))
	{
		m_pos.height = border_box_content_height(block_height);
	}

	int min_height = 0;
	if (!m_css_min_height.is_predefined() && m_css_min_height.units() == css_units_percentage)
	{
		element::ptr el_parent = parent();
		if (el_parent)
		{
			if (el_parent->get_predefined_height(block_height))
			{
				min_height = m_css_min_height.calc_percent(block_height);
			}
		}
	}
	else
	{
		min_height = (int)m_css_min_height.val();
	}
	if (min_height != 0 && m_box_sizing == box_sizing_border_box)
	{
		min_height -= m_padding.top + m_borders.top + m_padding.bottom + m_borders.bottom;
		if (min_height < 0) min_height = 0;
	}

	if (m_display == display_list_item)
	{
		const tchar_t* list_image = get_style_property(_t("list-style-image"), true, 0);
		if (list_image)
		{
			tstring url;
			css::parse_css_url(list_image, url);

			size sz;
			const tchar_t* list_image_baseurl = get_style_property(_t("list-style-image-baseurl"), true, 0);
			get_document()->container()->get_image_size(url.c_str(), list_image_baseurl, sz);
			if (min_height < sz.height)
			{
				min_height = sz.height;
			}
		}

	}

	if (min_height > m_pos.height)
	{
		m_pos.height = min_height;
	}

	int min_width = m_css_min_width.calc_percent(parent_width);

	if (min_width != 0 && m_box_sizing == box_sizing_border_box)
	{
		min_width -= m_padding.left + m_borders.left + m_padding.right + m_borders.right;
		if (min_width < 0) min_width = 0;
	}

	if (min_width != 0)
	{
		if (min_width > m_pos.width)
		{
			m_pos.width = min_width;
		}
		if (min_width > ret_width)
		{
			ret_width = min_width;
		}
	}

	ret_width += content_margins_left() + content_margins_right();

	// re-render with new width
	if (ret_width < max_width && !second_pass && have_parent())
	{
		/* width:fit-content / max-content / min-content on an in-flow block:
		 * shrink-to-fit exactly like a float, then let margin:auto centre the
		 * narrowed box (the first-pass calc_auto_margins saw the full width). */
		bool intrinsic_width = m_css_width.is_predefined() && m_css_width.predef() > 0;
		if (m_display == display_inline_block ||
			intrinsic_width ||
			m_css_width.is_predefined() &&
			(m_float != float_none ||
			m_display == display_table ||
			m_el_position == element_position_absolute ||
			m_el_position == element_position_fixed
			)
			)
		{
			render(x, y, ret_width, true);
			m_pos.width = ret_width - (content_margins_left() + content_margins_right());
			if (intrinsic_width && m_display == display_block)
			{
				calc_auto_margins(parent_width);
				m_pos.x = x + content_margins_left();
			}
		}
	}

	if (is_floats_holder() && !second_pass)
	{
		for (const auto& fb : m_floats_left)
		{
			fb.el->apply_relative_shift(fb.el->parent()->calc_width(m_pos.width));
		}
	}


	return ret_width;
}

int litehtml::html_tag::render_table(int x, int y, int max_width, bool second_pass /*= false*/)
{
	if (!m_grid) return 0;

	int parent_width = max_width;

	calc_outlines(parent_width);

	m_pos.clear();
	m_pos.move_to(x, y);

	m_pos.x += content_margins_left();
	m_pos.y += content_margins_top();

	def_value<int>	block_width(0);

	if (!m_css_width.is_predefined())
	{
		max_width = block_width = calc_width(parent_width) - m_padding.width() - m_borders.width();
	}
	else
	{
		if (max_width)
		{
			max_width -= content_margins_left() + content_margins_right();
		}
	}

	// Calculate table spacing
	int table_width_spacing = 0;
	if (m_border_collapse == border_collapse_separate)
	{
		table_width_spacing = m_border_spacing_x * (m_grid->cols_count() + 1);
	}
	else
	{
		table_width_spacing = 0;

		if (m_grid->cols_count())
		{
			table_width_spacing -= std::min(border_left(), m_grid->column(0).border_left);
			table_width_spacing -= std::min(border_right(), m_grid->column(m_grid->cols_count() - 1).border_right);
		}

		for (int col = 1; col < m_grid->cols_count(); col++)
		{
			table_width_spacing -= std::min(m_grid->column(col).border_left, m_grid->column(col - 1).border_right);
		}
	}


	// Calculate the minimum content width (MCW) of each cell: the formatted content may span any number of lines but may not overflow the cell box. 
	// If the specified 'width' (W) of the cell is greater than MCW, W is the minimum cell width. A value of 'auto' means that MCW is the minimum 
	// cell width.
	// 
	// Also, calculate the "maximum" cell width of each cell: formatting the content without breaking lines other than where explicit line breaks occur.

	if (m_grid->cols_count() == 1 && !block_width.is_default())
	{
		for (int row = 0; row < m_grid->rows_count(); row++)
		{
			table_cell* cell = m_grid->cell(0, row);
			if (cell && cell->el)
			{
				cell->min_width = cell->max_width = cell->el->render(0, 0, max_width - table_width_spacing);
				cell->el->m_pos.width = cell->min_width - cell->el->content_margins_left() - cell->el->content_margins_right();
			}
		}
	}
	else
	{
		for (int row = 0; row < m_grid->rows_count(); row++)
		{
			for (int col = 0; col < m_grid->cols_count(); col++)
			{
				table_cell* cell = m_grid->cell(col, row);
				if (cell && cell->el)
				{
					if (!m_grid->column(col).css_width.is_predefined() && m_grid->column(col).css_width.units() != css_units_percentage)
					{
						int css_w = m_grid->column(col).css_width.calc_percent(block_width);
						int el_w = cell->el->render(0, 0, css_w);
						cell->min_width = cell->max_width = std::max(css_w, el_w);
						cell->el->m_pos.width = cell->min_width - cell->el->content_margins_left() - cell->el->content_margins_right();
					}
					else
					{
						// calculate minimum content width
						cell->min_width = cell->el->render(0, 0, 1);
						// calculate maximum content width
						cell->max_width = cell->el->render(0, 0, max_width - table_width_spacing);
					}
				}
			}
		}
	}

	// For each column, determine a maximum and minimum column width from the cells that span only that column. 
	// The minimum is that required by the cell with the largest minimum cell width (or the column 'width', whichever is larger). 
	// The maximum is that required by the cell with the largest maximum cell width (or the column 'width', whichever is larger).

	for (int col = 0; col < m_grid->cols_count(); col++)
	{
		m_grid->column(col).max_width = 0;
		m_grid->column(col).min_width = 0;
		for (int row = 0; row < m_grid->rows_count(); row++)
		{
			if (m_grid->cell(col, row)->colspan <= 1)
			{
				m_grid->column(col).max_width = std::max(m_grid->column(col).max_width, m_grid->cell(col, row)->max_width);
				m_grid->column(col).min_width = std::max(m_grid->column(col).min_width, m_grid->cell(col, row)->min_width);
			}
		}
	}

	// For each cell that spans more than one column, increase the minimum widths of the columns it spans so that together, 
	// they are at least as wide as the cell. Do the same for the maximum widths. 
	// If possible, widen all spanned columns by approximately the same amount.

	for (int col = 0; col < m_grid->cols_count(); col++)
	{
		for (int row = 0; row < m_grid->rows_count(); row++)
		{
			if (m_grid->cell(col, row)->colspan > 1)
			{
				int max_total_width = m_grid->column(col).max_width;
				int min_total_width = m_grid->column(col).min_width;
				for (int col2 = col + 1; col2 < col + m_grid->cell(col, row)->colspan; col2++)
				{
					max_total_width += m_grid->column(col2).max_width;
					min_total_width += m_grid->column(col2).min_width;
				}
				if (min_total_width < m_grid->cell(col, row)->min_width)
				{
					m_grid->distribute_min_width(m_grid->cell(col, row)->min_width - min_total_width, col, col + m_grid->cell(col, row)->colspan - 1);
				}
				if (max_total_width < m_grid->cell(col, row)->max_width)
				{
					m_grid->distribute_max_width(m_grid->cell(col, row)->max_width - max_total_width, col, col + m_grid->cell(col, row)->colspan - 1);
				}
			}
		}
	}

	// If the 'table' or 'inline-table' element's 'width' property has a computed value (W) other than 'auto', the used width is the 
	// greater of W, CAPMIN, and the minimum width required by all the columns plus cell spacing or borders (MIN). 
	// If the used width is greater than MIN, the extra width should be distributed over the columns.
	//
	// If the 'table' or 'inline-table' element has 'width: auto', the used width is the greater of the table's containing block width, 
	// CAPMIN, and MIN. However, if either CAPMIN or the maximum width required by the columns plus cell spacing or borders (MAX) is 
	// less than that of the containing block, use max(MAX, CAPMIN).


	int table_width = 0;
	int min_table_width = 0;
	int max_table_width = 0;

	if (!block_width.is_default())
	{
		table_width = m_grid->calc_table_width(block_width - table_width_spacing, false, min_table_width, max_table_width);
	}
	else
	{
		table_width = m_grid->calc_table_width(max_width - table_width_spacing, true, min_table_width, max_table_width);
	}

	min_table_width += table_width_spacing;
	max_table_width += table_width_spacing;
	table_width += table_width_spacing;
	m_grid->calc_horizontal_positions(m_borders, m_border_collapse, m_border_spacing_x);

	bool row_span_found = false;

	// render cells with computed width
	for (int row = 0; row < m_grid->rows_count(); row++)
	{
		m_grid->row(row).height = 0;
		for (int col = 0; col < m_grid->cols_count(); col++)
		{
			table_cell* cell = m_grid->cell(col, row);
			if (cell->el)
			{
				int span_col = col + cell->colspan - 1;
				if (span_col >= m_grid->cols_count())
				{
					span_col = m_grid->cols_count() - 1;
				}
				int cell_width = m_grid->column(span_col).right - m_grid->column(col).left;

				if (cell->el->m_pos.width != cell_width - cell->el->content_margins_left() - cell->el->content_margins_right())
				{
					cell->el->render(m_grid->column(col).left, 0, cell_width);
					cell->el->m_pos.width = cell_width - cell->el->content_margins_left() - cell->el->content_margins_right();
				}
				else
				{
					cell->el->m_pos.x = m_grid->column(col).left + cell->el->content_margins_left();
				}

				if (cell->rowspan <= 1)
				{
					m_grid->row(row).height = std::max(m_grid->row(row).height, cell->el->height());
				}
				else
				{
					row_span_found = true;
				}

			}
		}
	}

	if (row_span_found)
	{
		for (int col = 0; col < m_grid->cols_count(); col++)
		{
			for (int row = 0; row < m_grid->rows_count(); row++)
			{
				table_cell* cell = m_grid->cell(col, row);
				if (cell->el)
				{
					int span_row = row + cell->rowspan - 1;
					if (span_row >= m_grid->rows_count())
					{
						span_row = m_grid->rows_count() - 1;
					}
					if (span_row != row)
					{
						int h = 0;
						for (int i = row; i <= span_row; i++)
						{
							h += m_grid->row(i).height;
						}
						if (h < cell->el->height())
						{
							m_grid->row(span_row).height += cell->el->height() - h;
						}
					}
				}
			}
		}
	}

	// Calculate vertical table spacing
	int table_height_spacing = 0;
	if (m_border_collapse == border_collapse_separate)
	{
		table_height_spacing = m_border_spacing_y * (m_grid->rows_count() + 1);
	}
	else
	{
		table_height_spacing = 0;

		if (m_grid->rows_count())
		{
			table_height_spacing -= std::min(border_top(), m_grid->row(0).border_top);
			table_height_spacing -= std::min(border_bottom(), m_grid->row(m_grid->rows_count() - 1).border_bottom);
		}

		for (int row = 1; row < m_grid->rows_count(); row++)
		{
			table_height_spacing -= std::min(m_grid->row(row).border_top, m_grid->row(row - 1).border_bottom);
		}
	}


	// calculate block height
	int block_height = 0;
	if (get_predefined_height(block_height))
	{
		block_height -= m_padding.height() + m_borders.height();
	}

	// calculate minimum height from m_css_min_height
	int min_height = 0;
	if (!m_css_min_height.is_predefined() && m_css_min_height.units() == css_units_percentage)
	{
		element::ptr el_parent = parent();
		if (el_parent)
		{
			int parent_height = 0;
			if (el_parent->get_predefined_height(parent_height))
			{
				min_height = m_css_min_height.calc_percent(parent_height);
			}
		}
	}
	else
	{
		min_height = (int)m_css_min_height.val();
	}

	int extra_row_height = 0;
	int minimum_table_height = std::max(block_height, min_height);

	m_grid->calc_rows_height(minimum_table_height - table_height_spacing, m_border_spacing_y);
	m_grid->calc_vertical_positions(m_borders, m_border_collapse, m_border_spacing_y);

	int table_height = 0;

	// place cells vertically
	for (int col = 0; col < m_grid->cols_count(); col++)
	{
		for (int row = 0; row < m_grid->rows_count(); row++)
		{
			table_cell* cell = m_grid->cell(col, row);
			if (cell->el)
			{
				int span_row = row + cell->rowspan - 1;
				if (span_row >= m_grid->rows_count())
				{
					span_row = m_grid->rows_count() - 1;
				}
				cell->el->m_pos.y = m_grid->row(row).top + cell->el->content_margins_top();
				cell->el->m_pos.height = m_grid->row(span_row).bottom - m_grid->row(row).top - cell->el->content_margins_top() - cell->el->content_margins_bottom();
				table_height = std::max(table_height, m_grid->row(span_row).bottom);
				cell->el->apply_vertical_align();
			}
		}
	}

	if (m_border_collapse == border_collapse_collapse)
	{
		if (m_grid->rows_count())
		{
			table_height -= std::min(border_bottom(), m_grid->row(m_grid->rows_count() - 1).border_bottom);
		}
	}
	else
	{
		table_height += m_border_spacing_y;
	}

	m_pos.width = table_width;

	calc_auto_margins(parent_width);

	m_pos.move_to(x, y);
	m_pos.x += content_margins_left();
	m_pos.y += content_margins_top();
	m_pos.width = table_width;
	m_pos.height = table_height;

	return max_table_width;
}

void litehtml::html_tag::draw_children_box(uint_ptr hdc, int x, int y, const position* clip, draw_flag flag, int zindex)
{
	position pos = m_pos;
	pos.x += x;
	pos.y += y;

	document* doc = get_document();

	if (m_overflow > overflow_visible)
	{
		position border_box = pos;
		border_box += m_padding;
		border_box += m_borders;

		/* CSS clips overflow at the PADDING edge, not the content edge. Boxes
		 * whose space comes from padding alone (the .l-frame aspect-ratio
		 * trick: content height 0 + padding-bottom, absolutely positioned
		 * media inside) would otherwise clip every child away - the w3.org
		 * card images never painted at all. */
		position pad_box = pos;
		pad_box += m_padding;

		border_radiuses bdr_radius = m_css_borders.radius.calc_percents(border_box.width, border_box.height);

		bdr_radius -= m_borders;

		doc->container()->set_clip(pad_box, bdr_radius, true, true);
	}

	position browser_wnd;
	doc->container()->get_client_rect(browser_wnd);

	element::ptr el = 0;
	for (auto& item : m_children)
	{
		el = item;
		if (el->is_visible())
		{
			switch (flag)
			{
			case draw_positioned:
				if (el->is_stacking_participant() && el->get_zindex() == zindex)
				{
					if (el->get_element_position() == element_position_fixed)
					{
						if(getenv("EWEB_POSDBG")) {
							fprintf(stderr, "[posdbg] DRAW fixed el=%p id=%s cls=%s mpos=%d,%d %dx%d wnd=%d,%d z=%d\n",
								(void*)el, el->get_attr(_t("id"), ""), el->get_attr(_t("class"), ""),
								el->m_pos.x, el->m_pos.y, el->m_pos.width, el->m_pos.height,
								browser_wnd.x, browser_wnd.y, el->get_zindex());
						}
						el->draw(hdc, browser_wnd.x, browser_wnd.y, clip);
						el->draw_stacking_context(hdc, browser_wnd.x, browser_wnd.y, clip, true);
						el = 0;
					}
					else
					{
						bool el_sc = el->is_stacking_context();
						el->draw(hdc, pos.x, pos.y, clip);
						/* A z-index:auto positioned box is not a stacking context:
						 * paint its own in-flow content here (with_positioned is
						 * false) and let its z-indexed descendants be painted by
						 * this stacking context at their own z levels. For such a
						 * transparent box keep `el` set so the recursion below also
						 * descends into it and reaches deeper same-z (z-index:auto)
						 * positioned descendants - nested position:relative wrappers
						 * are extremely common and must not swallow their children.
						 * A real stacking context is atomic, so consume it. */
						el->draw_stacking_context(hdc, pos.x, pos.y, clip, el_sc);
						if (el_sc)
						{
							el = 0;
						}
					}
				}
				break;
			case draw_block:
				if (!el->is_inline_box() && el->get_float() == float_none && !el->is_stacking_participant())
				{
					el->draw(hdc, pos.x, pos.y, clip);
				}
				break;
			case draw_floats:
				if (el->get_float() != float_none && !el->is_stacking_participant())
				{
					el->draw(hdc, pos.x, pos.y, clip);
					el->draw_stacking_context(hdc, pos.x, pos.y, clip, false);
					el = 0;
				}
				break;
			case draw_inlines:
				if (el->is_inline_box() && el->get_float() == float_none && !el->is_stacking_participant())
				{
					el->draw(hdc, pos.x, pos.y, clip);
					if (el->get_display() == display_inline_block)
					{
						el->draw_stacking_context(hdc, pos.x, pos.y, clip, false);
						el = 0;
					}
				}
				break;
			default:
				break;
			}

			if (el)
			{
				if (flag == draw_positioned)
				{
					/* Descend through every box that is not a stacking context
					 * (static boxes and z-index:auto positioned boxes) so that
					 * z-indexed descendants which bubbled up to this stacking
					 * context are still found at their own z level. */
					if (!el->is_stacking_context())
					{
						el->draw_children(hdc, pos.x, pos.y, clip, flag, zindex);
					}
				}
				else
				{
					if (el->get_float() == float_none &&
						el->get_display() != display_inline_block &&
						!el->is_stacking_participant())
					{
						el->draw_children(hdc, pos.x, pos.y, clip, flag, zindex);
					}
				}
			}
		}
	}

	if (m_overflow > overflow_visible)
	{
		doc->container()->del_clip();
	}
}

void litehtml::html_tag::draw_children_table(uint_ptr hdc, int x, int y, const position* clip, draw_flag flag, int zindex)
{
	if (!m_grid) return;

	position pos = m_pos;
	pos.x += x;
	pos.y += y;
	for (int row = 0; row < m_grid->rows_count(); row++)
	{
		if (flag == draw_block)
		{
			m_grid->row(row).el_row->draw_background(hdc, pos.x, pos.y, clip);
		}
		for (int col = 0; col < m_grid->cols_count(); col++)
		{
			table_cell* cell = m_grid->cell(col, row);
			if (cell->el)
			{
				if (flag == draw_block)
				{
					cell->el->draw(hdc, pos.x, pos.y, clip);
				}
				cell->el->draw_children(hdc, pos.x, pos.y, clip, flag, zindex);
			}
		}
	}
}
