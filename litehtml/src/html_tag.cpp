#include "html.h"
#include "html_tag.h"
#include "document.h"
#include "iterators.h"
#include "stylesheet.h"
#include "table.h"
#include <algorithm>
#include <locale>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "el_before_after.h"

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
	m_lh_predefined			= false;
	m_line_height			= 0;
	m_visibility			= visibility_visible;
	m_border_spacing_x		= 0;
	m_border_spacing_y		= 0;
	m_border_collapse		= border_collapse_separate;
}

litehtml::html_tag::~html_tag()
{
#ifdef LITEHTML_LIFETIME_DEBUG
	litehtml_lifetime_remove(this);
#endif
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

bool litehtml::html_tag::appendChild(const element::ptr &el)
{
	if(el)
	{
		el->parent(this);
		m_children.push_back(el);
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
	if(ref && ref->parent() == this)
	{
		for(size_t i = 0; i < m_children.size(); i++)
		{
			if(m_children[i] == ref)
			{
				el->parent(this);
				m_children.insert(m_children.begin() + i, el);
				return true;
			}
		}
	}
	/* No ref, or a ref that is not our child: append, exactly like the DOM. */
	return appendChild(el);
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
	if(stylesheet.has_before_after())
	{
		remove_before_after();
	}

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
						el->add_style(*sel->m_style);
					}
				} else if(apply & select_match_with_before)
				{
					ensure_used_style();
					element::ptr el = get_element_before();
					if(el)
					{
						el->add_style(*sel->m_style);
					}
				} else
				{
					add_style(*sel->m_style);
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

void litehtml::html_tag::draw( uint_ptr hdc, int x, int y, const position* clip )
{
	position pos = m_pos;
	pos.x	+= x;
	pos.y	+= y;

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

void litehtml::html_tag::expand_css_functions()
{
	bool has_func = false;
	for(props_map::const_iterator it = m_style.properties().begin(); it != m_style.properties().end(); ++it)
	{
		const tstring& v = it->second.m_value;
		if(v.find(_t("var(")) != tstring::npos || v.find(_t("clamp(")) != tstring::npos ||
		   v.find(_t("min(")) != tstring::npos || v.find(_t("max(")) != tstring::npos)
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
	for(props_map::const_iterator it = old.begin(); it != old.end(); ++it)
	{
		const tstring& name = it->first;
		const tstring& raw = it->second.m_value;
		if(name.length() > 2 && name[0] == _t('-') && name[1] == _t('-'))
		{
			/* keep the raw definition: it is re-resolved per element */
			m_style.add_property(name.c_str(), raw.c_str(), NULL, it->second.m_important);
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
		if(v.find(_t("clamp(")) != tstring::npos || v.find(_t("min(")) != tstring::npos || v.find(_t("max(")) != tstring::npos)
		{
			if(doc && !eval_math_funcs(v, doc, m_font_size))
			{
				continue;
			}
		}
		m_style.add_property(name.c_str(), v.c_str(), NULL, it->second.m_important);
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
	const tchar_t* own_position = own_style_ref_ptr(own_refs.position);
	const tchar_t* own_overflow = own_style_ref_ptr(own_refs.overflow);
	const tchar_t* own_display = own_style_ref_ptr(own_refs.display);
	const tchar_t* own_box_sizing = own_style_ref_ptr(own_refs.box_sizing);
	const tchar_t* own_text_align = own_style_ref_ptr(own_refs.text_align);
	const tchar_t* own_text_transform = own_style_ref_ptr(own_refs.text_transform);

	m_el_position	= (element_position)	value_index((own_position && t_strcasecmp(own_position, _t("inherit"))) ? own_position : _t("static"),			element_position_strings,	element_position_fixed);
	m_overflow		= (overflow)			value_index((own_overflow && t_strcasecmp(own_overflow, _t("inherit"))) ? own_overflow : _t("visible"),		overflow_strings,			overflow_visible);
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
		m_display = display_inline;
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
	if(profile_enabled)
	{
		parse_style_profile_add(g_parse_style_profile.basic_ms, part_start);
		part_start = sys_tic_ms(0);
	}

	if(m_el_position != element_position_static)
	{
		const tchar_t* val = own_style_ref_ptr(own_refs.z_index);
		if(val)
		{
			m_z_index = t_atoi(val);
		}
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
			m_line_height = el_parent->line_height();
			m_lh_predefined = false;
		}
		else
		{
			m_line_height = m_font_metrics.height;
			m_lh_predefined = true;
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
	}

	/* display:contents generates no box of its own: queue the child lift for
	 * the next document::render (the single safe point - no child-list
	 * iteration is in flight there). */
	if(m_display == display_contents && !m_contents_spliced)
	{
		m_contents_spliced = true;
		if(doc) doc->queue_contents_splice(this);
	}

	if(profile_enabled)
		part_start = sys_tic_ms(0);
	m_css_text_indent.fromString(	get_style_property(_t("text-indent"),	true,	_t("0")),	_t("0"));

	const tchar_t* own_width = get_style_property_own(_t("width"));
	if(own_width) {
		m_css_width.fromString(own_width, _t("auto"));
	} else {
		css_length_set_predef0(m_css_width);
	}

	const tchar_t* own_height = get_style_property_own(_t("height"));
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
		m_line_height = el_parent->line_height();
		m_lh_predefined = false;
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
}

int litehtml::html_tag::render( int x, int y, int max_width, bool second_pass )
{
	if (m_display == display_table || m_display == display_inline_table)
	{
		return render_table(x, y, max_width, second_pass);
	}

	if (m_display == display_flex || m_display == display_inline_flex)
	{
		return render_flex(x, y, max_width, second_pass);
	}

	if (m_display == display_grid || m_display == display_inline_grid)
	{
		return render_grid(x, y, max_width, second_pass);
	}

	return render_box(x, y, max_width, second_pass);
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
				 * opposite - it matches exactly the parentless element. */
				if(!el_parent && selector != pseudo_class_root)
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
	element::ptr el_parent = parent();
	if (!el_parent)
	{
		return nullptr;
	}
	int res = el_parent->select(selector, apply_pseudo);
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
		return el_parent;
	}
	return el_parent->find_ancestor(selector, apply_pseudo, is_pseudo);
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

	m_bg.m_color = bg_color ? web_color::from_string(bg_color, get_document()->container()) : web_color(0, 0, 0, 0);
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
		css::parse_css_url(bg_image, m_bg.m_image);
		if(bg_baseurl)
		{
			m_bg.m_baseurl = bg_baseurl;
		}
	}

	if(!m_bg.m_image.empty())
	{
		doc->container()->load_image(m_bg.m_image.c_str(), m_bg.m_baseurl.empty() ? 0 : m_bg.m_baseurl.c_str(), true);
	}
}

void litehtml::html_tag::add_positioned(const element::ptr &el)
{
	if (m_el_position != element_position_static || (!have_parent()))
	{
		m_positioned.push_back(el);
	} else
	{
		element::ptr el_parent = parent();
		if (el_parent)
		{
			el_parent->add_positioned(el);
		}
	}
}

void litehtml::html_tag::calc_outlines( int parent_width )
{
	m_padding.left	= get_document()->cvt_units(m_css_padding.left,	m_font_size, parent_width);
	m_padding.right	= get_document()->cvt_units(m_css_padding.right,	m_font_size, parent_width);

	m_borders.left	= m_css_borders.left.width.calc_percent(parent_width);
	m_borders.right	= m_css_borders.right.width.calc_percent(parent_width);

	m_margins.left	= get_document()->cvt_units(m_css_margins.left,	m_font_size, parent_width);
	m_margins.right	= get_document()->cvt_units(m_css_margins.right,	m_font_size, parent_width);

	m_margins.top		= get_document()->cvt_units(m_css_margins.top,		m_font_size, parent_width);
	m_margins.bottom	= get_document()->cvt_units(m_css_margins.bottom,	m_font_size, parent_width);

	m_padding.top		= get_document()->cvt_units(m_css_padding.top,		m_font_size, parent_width);
	m_padding.bottom	= get_document()->cvt_units(m_css_padding.bottom,	m_font_size, parent_width);
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
			const background* bg = get_background();
			if(bg)
			{
				background_paint bg_paint;
				init_background_paint(pos, bg_paint, bg);

				get_document()->container()->draw_background(hdc, bg_paint);
			}
			position border_box = pos;
			border_box += m_padding;
			border_box += m_borders;

			borders bdr = m_css_borders;
			bdr.radius = m_css_borders.radius.calc_percents(border_box.width, border_box.height);

			get_document()->container()->draw_borders(hdc, bdr, border_box, have_parent() ? false : true);
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
					get_document()->container()->draw_background(hdc, bg_paint);
				}
				borders b = bdr;
				b.radius = bdr.radius.calc_percents(box->width, box->height);
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

	for (auto& el : m_children)
	{
		// Check if element is valid
		if (!el) continue;

		// display:contents elements generate no box of their own
		if (el->get_display() == display_contents) continue;

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
		return el->render_inline(this, max_width);
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

			if(!el->is_inline_box())
			{
				if(m_boxes.size() == 1)
				{
					if(collapse_top_margin())
					{
						int shift = el->margin_top();
						if(shift >= 0)
						{
							line_ctx.top -= shift;
							m_boxes.back()->y_shift(-shift);
						}
					}
				} else
				{
					int shift = 0;
					int prev_margin = m_boxes[m_boxes.size() - 2]->bottom_margin();

					if(prev_margin > el->margin_top())
					{
						shift = el->margin_top();
					} else
					{
						shift = prev_margin;
					}
					if(shift >= 0)
					{
						line_ctx.top -= shift;
						m_boxes.back()->y_shift(-shift);
					}
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

			m_boxes.back()->add_element(el);

			if(el->is_inline_box() && !el->skip())
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
		m_boxes.emplace_back(std::unique_ptr<line_box>(new line_box(line_ctx.top, line_ctx.left + first_line_margin + text_indent, line_ctx.right, line_height(), fm, m_text_align)));
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
	/* The standard "visually hidden" accessibility pattern (skip links,
	 * screen-reader-only spans such as the w3.org logo label) sizes the box to
	 * 1px with overflow:hidden and relies on clipping to keep the text from
	 * painting. Without a general clip implementation the text would still be
	 * drawn over neighbouring content, so treat a degenerate clipped box as
	 * painting nothing. */
	if(m_overflow > overflow_visible && m_pos.width <= 1 && m_pos.height <= 1)
	{
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
}

bool litehtml::html_tag::fetch_positioned()
{
	bool ret = false;

	m_positioned.clear();

	litehtml::element_position el_pos;

	for(auto& el : m_children)
	{
		el_pos = el->get_element_position();
		if (el_pos != element_position_static)
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
				if(!css_left.is_predefined() || !css_right.is_predefined())
				{
					if(!css_left.is_predefined() && css_right.is_predefined())
					{
						el->m_pos.x = css_left.calc_percent(parent_width) + el->content_margins_left() - m_padding.left;
					} else if(css_left.is_predefined() && !css_right.is_predefined())
					{
						el->m_pos.x = m_pos.width + m_padding.right - css_right.calc_percent(parent_width) - el->m_pos.width - el->content_margins_right();
					} else
					{
						el->m_pos.x		= css_left.calc_percent(parent_width) + el->content_margins_left() - m_padding.left;
						el->m_pos.width	= m_pos.width + m_padding.left + m_padding.right - css_left.calc_percent(parent_width) - css_right.calc_percent(parent_width) - (el->content_margins_left() + el->content_margins_right());
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
						el->m_pos.y = css_top.calc_percent(parent_height) + el->content_margins_top() - m_padding.top;
					} else if(css_top.is_predefined() && !css_bottom.is_predefined())
					{
						el->m_pos.y = m_pos.height + m_padding.bottom - css_bottom.calc_percent(parent_height) - el->m_pos.height - el->content_margins_bottom();
					} else
					{
						el->m_pos.y			= css_top.calc_percent(parent_height) + el->content_margins_top() - m_padding.top;
						el->m_pos.height	= m_pos.height + m_padding.top + m_padding.bottom - css_top.calc_percent(parent_height) - css_bottom.calc_percent(parent_height) - (el->content_margins_top() + el->content_margins_bottom());
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
				int offset_x = 0;
				int offset_y = 0;
				element::ptr cur_el = el->parent();
				element::ptr this_el = this;
				while(cur_el && cur_el != this_el)
				{
					offset_x += cur_el->m_pos.x;
					offset_y += cur_el->m_pos.y;
					cur_el = cur_el->parent();
				}
				if(cvt_x)	el->m_pos.x -= offset_x;
				if(cvt_y)	el->m_pos.y -= offset_y;
			}

			if(need_render)
			{
				position pos = el->m_pos;
				el->render(el->left(), el->top(), el->width(), true);
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
		if(child->get_display() != display_inline_text)
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
		if((*child)->get_display() != display_inline_text)
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

void litehtml::html_tag::add_style( const litehtml::style& st )
{
	clear_style_property_cache();
	m_style.combine(st);
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
								el->add_style(*usel.m_selector->m_style);
							}
						} else if(apply & select_match_with_before)
						{
							element::ptr el = get_element_before();
							if(el)
							{
								el->add_style(*usel.m_selector->m_style);
							}
						}
						else
						{
							add_style(*usel.m_selector->m_style);
							usel.m_used = true;
						}
					}
				} else if(apply & select_match_with_after)
				{
					element::ptr el = get_element_after();
					if(el)
					{
						el->add_style(*usel.m_selector->m_style);
					}
				} else if(apply & select_match_with_before)
				{
					element::ptr el = get_element_before();
					if(el)
					{
						el->add_style(*usel.m_selector->m_style);
					}
				} else
				{
					add_style(*usel.m_selector->m_style);
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
				if(el->is_positioned() && el->get_zindex() == zindex)
				{
					if(el->get_element_position() == element_position_fixed)
					{
						ret = el->get_element_by_point(client_x, client_y, client_x, client_y);
						if(!ret && (*i)->is_point_inside(client_x, client_y))
						{
							ret = (*i);
						}
					} else
					{
						ret = el->get_element_by_point(pos.x, pos.y, client_x, client_y);
						if(!ret && (*i)->is_point_inside(pos.x, pos.y))
						{
							ret = (*i);
						}
					}
					el = 0;
				}
				break;
			case draw_block:
				if(!el->is_inline_box() && el->get_float() == float_none && !el->is_positioned())
				{
					if(el->is_point_inside(pos.x, pos.y))
					{
						ret = el;
					}
				}
				break;
			case draw_floats:
				if(el->get_float() != float_none && !el->is_positioned())
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
				if(el->is_inline_box() && el->get_float() == float_none && !el->is_positioned())
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

			if(el && !el->is_positioned())
			{
				if(flag == draw_positioned)
				{
					element::ptr child = el->get_child_by_point(pos.x, pos.y, client_x, client_y, flag, zindex);
					if(child)
					{
						ret = child;
					}
				} else
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
		if(m_bg.m_image.empty() && !m_bg.m_color.alpha)
		{
			return 0;
		}
		return &m_bg;
	}

	if(m_bg.m_image.empty() && !m_bg.m_color.alpha)
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
		int w = calc_width(parent_width);
		
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
		int mw = get_document()->cvt_units(m_css_max_width, m_font_size, parent_width);
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
		m_pos.height = block_height;
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

	for (auto el : m_children)
	{
		// display:contents elements generate no box of their own
		if (el->get_display() == display_contents) continue;

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
		if (collapse_top_margin())
		{
			int old_top = m_margins.top;
			m_margins.top = std::max(m_boxes.front()->top_margin(), m_margins.top);
			if (m_margins.top != old_top)
			{
				update_floats(m_margins.top - old_top, this);
			}
		}
		if (collapse_bottom_margin())
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
		m_pos.height = block_height;
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
		if (m_display == display_inline_block ||
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

		border_radiuses bdr_radius = m_css_borders.radius.calc_percents(border_box.width, border_box.height);

		bdr_radius -= m_borders;
		bdr_radius -= m_padding;

		doc->container()->set_clip(pos, bdr_radius, true, true);
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
				if (el->is_positioned() && el->get_zindex() == zindex)
				{
					if (el->get_element_position() == element_position_fixed)
					{
						el->draw(hdc, browser_wnd.x, browser_wnd.y, clip);
						el->draw_stacking_context(hdc, browser_wnd.x, browser_wnd.y, clip, true);
					}
					else
					{
						el->draw(hdc, pos.x, pos.y, clip);
						el->draw_stacking_context(hdc, pos.x, pos.y, clip, true);
					}
					el = 0;
				}
				break;
			case draw_block:
				if (!el->is_inline_box() && el->get_float() == float_none && !el->is_positioned())
				{
					el->draw(hdc, pos.x, pos.y, clip);
				}
				break;
			case draw_floats:
				if (el->get_float() != float_none && !el->is_positioned())
				{
					el->draw(hdc, pos.x, pos.y, clip);
					el->draw_stacking_context(hdc, pos.x, pos.y, clip, false);
					el = 0;
				}
				break;
			case draw_inlines:
				if (el->is_inline_box() && el->get_float() == float_none && !el->is_positioned())
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
					if (!el->is_positioned())
					{
						el->draw_children(hdc, pos.x, pos.y, clip, flag, zindex);
					}
				}
				else
				{
					if (el->get_float() == float_none &&
						el->get_display() != display_inline_block &&
						!el->is_positioned())
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
