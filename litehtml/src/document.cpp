#include "html.h"
#include "document.h"
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef LITEHTML_LIFETIME_DEBUG
/* Authoritative, non-dereferencing liveness lookup for html_tag objects,
 * maintained by the html_tag ctor/dtor (see html_tag.cpp). Safe to call on a
 * stale handle whose memory the mario VM has recycled. */
bool litehtml_tag_is_live(const void* p);
#endif

namespace litehtml {
void reset_parse_style_profile();
void dump_parse_style_profile();
void reset_dom_internal_profile();
void dump_dom_internal_profile();
void profile_apply_stylesheet(uint32_t selector_count, uint64_t start_ms);
void profile_select(uint64_t start_ms);
void profile_select_element(uint64_t start_ms);
void profile_get_style_property(bool cache_hit, uint32_t parent_steps, uint64_t start_ms);
void profile_init_font(bool inherit_fast, uint64_t start_ms);
void profile_text_parse(uint32_t transform_ms, uint32_t measure_ms, uint64_t start_ms);
void profile_get_font(bool cache_hit, uint64_t start_ms);
void profile_cvt_units(uint64_t start_ms);
void profile_color_parse(uint64_t start_ms);
}

namespace {

static inline bool font_metrics_ptr_writable_doc(litehtml::font_metrics* fm)
{
	if(!fm)
	{
		return false;
	}
	uintptr_t addr = (uintptr_t)fm;
	return addr >= 0x180000;
}

struct create_node_profile_t
{
	uint32_t calls;
	uint32_t element_nodes;
	uint32_t text_nodes;
	uint32_t attrs_ms;
	uint32_t create_element_ms;
	uint32_t children_ms;
	uint32_t text_split_ms;
};

static create_node_profile_t g_create_node_profile = {};
static const bool g_create_node_profile_supported = true;

struct dom_internal_profile_t
{
	uint32_t apply_stylesheet_calls;
	uint32_t apply_stylesheet_selectors;
	uint32_t apply_stylesheet_ms;
	uint32_t select_calls;
	uint32_t select_ms;
	uint32_t select_element_calls;
	uint32_t select_element_ms;
	uint32_t get_style_calls;
	uint32_t get_style_cache_hits;
	uint32_t get_style_parent_steps;
	uint32_t get_style_ms;
	uint32_t init_font_calls;
	uint32_t init_font_inherit_hits;
	uint32_t init_font_ms;
	uint32_t text_parse_calls;
	uint32_t text_transform_ms;
	uint32_t text_measure_ms;
	uint32_t text_parse_ms;
	uint32_t get_font_calls;
	uint32_t get_font_cache_hits;
	uint32_t get_font_cache_misses;
	uint32_t get_font_ms;
	uint32_t cvt_units_calls;
	uint32_t cvt_units_ms;
	uint32_t color_parse_calls;
	uint32_t color_parse_ms;
};

static dom_internal_profile_t g_dom_internal_profile = {};

static inline void add_dom_internal_time(uint32_t& slot, uint64_t start_ms)
{
	slot += (uint32_t)(sys_tic_ms(0) - start_ms);
}

template<typename T, typename... Args>
static T* litehtml_alloc(const char* label, Args... args)
{
	void* mem = malloc(sizeof(T));
	if(!mem)
	{
		return nullptr;
	}
	return new (mem) T(args...);
}

static inline void reset_create_node_profile()
{
	if(!g_create_node_profile_supported)
	{
		return;
	}
	memset(&g_create_node_profile, 0, sizeof(g_create_node_profile));
}

static inline void add_create_node_time(uint32_t& slot, uint64_t start_ms)
{
	if(!g_create_node_profile_supported)
	{
		return;
	}
	slot += (uint32_t)(sys_tic_ms(0) - start_ms);
}

static inline void dump_create_node_profile()
{
	if(!g_create_node_profile_supported)
	{
		return;
	}
}

}
#include "stylesheet.h"
#include "html_tag.h"
#include "el_text.h"
#include "el_para.h"
#include "el_space.h"
#include "el_body.h"
#include "el_image.h"
#include "el_svg.h"
#include "el_table.h"
#include "el_td.h"
#include "el_link.h"
#include "el_title.h"
#include "el_style.h"
#include "el_script.h"
#include "el_comment.h"
#include "el_cdata.h"
#include "el_base.h"
#include "el_anchor.h"
#include "el_break.h"
#include "el_div.h"
#include "el_font.h"
#include "el_tr.h"
#include <math.h>
#include <stdio.h>
#include <algorithm>
#include "gumbo/gumbo.h"
#include "utf8_strings.h"

litehtml::document::document(litehtml::document_container* objContainer, litehtml::context* ctx)
{
	m_container	= objContainer;
	m_context	= ctx;
	m_root		= nullptr;
	m_over_element = nullptr;
	m_size.width = 0;
	m_size.height = 0;
	m_def_color = web_color(0, 0, 0);
	m_last_font_valid = false;
	m_tables_dirty = false;
	m_last_font_size = 0;
	m_last_font = 0;
	m_step_epoch = 0;
	m_step_phase = 0;
	m_step_deadline = 0;
	m_step_visits = 0;
	m_step_stamped = 0;
	m_step_apply_ms = 0;
	m_step_parse_ms = 0;
	m_step_start = 0;
	m_step_exhausted = false;
}

void litehtml::reset_dom_internal_profile()
{
	memset(&g_dom_internal_profile, 0, sizeof(g_dom_internal_profile));
}

void litehtml::dump_dom_internal_profile()
{
}

void litehtml::profile_apply_stylesheet(uint32_t selector_count, uint64_t start_ms)
{
	g_dom_internal_profile.apply_stylesheet_calls++;
	g_dom_internal_profile.apply_stylesheet_selectors += selector_count;
	add_dom_internal_time(g_dom_internal_profile.apply_stylesheet_ms, start_ms);
}

void litehtml::profile_select(uint64_t start_ms)
{
	g_dom_internal_profile.select_calls++;
	add_dom_internal_time(g_dom_internal_profile.select_ms, start_ms);
}

void litehtml::profile_select_element(uint64_t start_ms)
{
	g_dom_internal_profile.select_element_calls++;
	add_dom_internal_time(g_dom_internal_profile.select_element_ms, start_ms);
}

void litehtml::profile_get_style_property(bool cache_hit, uint32_t parent_steps, uint64_t start_ms)
{
	g_dom_internal_profile.get_style_calls++;
	if(cache_hit)
	{
		g_dom_internal_profile.get_style_cache_hits++;
	}
	g_dom_internal_profile.get_style_parent_steps += parent_steps;
	add_dom_internal_time(g_dom_internal_profile.get_style_ms, start_ms);
}

void litehtml::profile_init_font(bool inherit_fast, uint64_t start_ms)
{
	g_dom_internal_profile.init_font_calls++;
	if(inherit_fast)
	{
		g_dom_internal_profile.init_font_inherit_hits++;
	}
	add_dom_internal_time(g_dom_internal_profile.init_font_ms, start_ms);
}

void litehtml::profile_text_parse(uint32_t transform_ms, uint32_t measure_ms, uint64_t start_ms)
{
	g_dom_internal_profile.text_parse_calls++;
	g_dom_internal_profile.text_transform_ms += transform_ms;
	g_dom_internal_profile.text_measure_ms += measure_ms;
	add_dom_internal_time(g_dom_internal_profile.text_parse_ms, start_ms);
}

void litehtml::profile_get_font(bool cache_hit, uint64_t start_ms)
{
	g_dom_internal_profile.get_font_calls++;
	if(cache_hit)
	{
		g_dom_internal_profile.get_font_cache_hits++;
	}
	else
	{
		g_dom_internal_profile.get_font_cache_misses++;
	}
	add_dom_internal_time(g_dom_internal_profile.get_font_ms, start_ms);
}

void litehtml::profile_cvt_units(uint64_t start_ms)
{
	g_dom_internal_profile.cvt_units_calls++;
	add_dom_internal_time(g_dom_internal_profile.cvt_units_ms, start_ms);
}

void litehtml::profile_color_parse(uint64_t start_ms)
{
	g_dom_internal_profile.color_parse_calls++;
	add_dom_internal_time(g_dom_internal_profile.color_parse_ms, start_ms);
}

litehtml::document::~document()
{
	m_over_element = 0;
	if(m_container)
	{
		for(fonts_map::iterator f = m_fonts.begin(); f != m_fonts.end(); f++)
		{
			m_container->delete_font(f->second.font);
		}
	}
	// Clean up DOM tree
	if(m_root)
	{
		delete m_root;
		m_root = nullptr;
	}
}

litehtml::document::ptr litehtml::document::createFromString( const tchar_t* str, litehtml::document_container* objPainter, litehtml::context* ctx, litehtml::css* user_styles)
{
	return createFromUTF8(litehtml_to_utf8(str), objPainter, ctx, user_styles);
}

litehtml::document::ptr litehtml::document::createFromUTF8(const char* str, litehtml::document_container* objPainter, litehtml::context* ctx, litehtml::css* user_styles)
{
	uint32_t len = str ? (uint32_t)strlen(str) : 0;
	reset_dom_internal_profile();
	GumboOutput* output = gumbo_parse_with_options(&kGumboDefaultOptions, (const char*) str, len);

	litehtml::document::ptr doc = litehtml_alloc<litehtml::document>("document", objPainter, ctx);
	if(!output)
	{
		// OOM during gumbo parsing - return NULL so caller detects failure
		if(doc) delete doc;
		return nullptr;
	}
	if(!doc)
	{
		gumbo_destroy_output(&kGumboDefaultOptions, output);
		return nullptr;
	}

	// Create litehtml::elements.
	elements_vector root_elements;
	reset_create_node_profile();
	doc->create_node(output->root, root_elements);
	if (!root_elements.empty())
	{
		doc->m_root = root_elements.back();
		root_elements.pop_back();
	}
	for(auto& el : root_elements)
	{
		if(el)
		{
			delete el;
		}
	}
	root_elements.clear();
	gumbo_destroy_output(&kGumboDefaultOptions, output);

	if (doc->m_root)
	{
		doc->container()->get_media_features(doc->m_media);

		/* Master sheets were parsed with doc==0: hook their @media lists up
		 * and evaluate them before matching, or every @media-scoped selector
		 * stays inactive. */
		doc->register_master_media_lists();

		doc->m_root->apply_stylesheet(ctx->master_css());

		doc->m_root->parse_attributes();

		media_query_list::ptr media;
		for (css_text::vector::iterator css = doc->m_css.begin(); css != doc->m_css.end(); css++)
		{
			if (!css->media.empty())
			{
				media = media_query_list::create_from_string(css->media, doc);
			}
			else
			{
				media = 0;
			}
			doc->m_styles.parse_stylesheet(css->text.c_str(), css->baseurl.c_str(), doc, media);
		}
		// Sort css selectors using CSS rules.
		doc->m_styles.sort_selectors();

		if (!doc->m_media_lists.empty())
		{
			doc->update_media_lists(doc->m_media);
		}

		doc->m_root->apply_stylesheet(doc->m_styles);

		if (user_styles)
		{
			doc->m_root->apply_stylesheet(*user_styles);
		}

		reset_parse_style_profile();
		doc->m_root->parse_styles();
		dump_parse_style_profile();
		dump_dom_internal_profile();

		// Now the m_tabular_elements is filled with tabular elements.
		// We have to check the tabular elements for missing table elements
		// and create the anonymous boxes in visual table layout
		doc->fix_tables_layout();

		// Fanaly initialize elements
		doc->m_root->init();
	}

	return doc;
}

litehtml::uint_ptr litehtml::document::add_font( const tchar_t* name, int size, const tchar_t* weight, const tchar_t* style, const tchar_t* decoration, font_metrics* fm )
{
	uint_ptr ret = 0;

	if( !name || (name && !t_strcasecmp(name, _t("inherit"))) )
	{
		name = m_container->get_default_font_name();
	}

	if(size < 0)
	{
		size = container()->get_default_font_size();
	}

	tchar_t strSize[20];
	t_itoa(size, strSize, 20, 10);

	tstring key = name;
	key += _t(":");
	key += strSize;
	key += _t(":");
	key += weight;
	key += _t(":");
	key += style;
	key += _t(":");
	key += decoration;

	if(m_fonts.find(key) == m_fonts.end())
	{
		font_style fs = (font_style) value_index(style, font_style_strings, fontStyleNormal);
		int	fw = value_index(weight, font_weight_strings, -1);
		if(fw >= 0)
		{
			switch(fw)
			{
			case litehtml::fontWeightBold:
				fw = 700;
				break;
			case litehtml::fontWeightBolder:
				fw = 600;
				break;
			case litehtml::fontWeightLighter:
				fw = 300;
				break;
			default:
				fw = 400;
				break;
			}
		} else
		{
			fw = t_atoi(weight);
			if(fw < 100)
			{
				fw = 400;
			}
		}

		unsigned int decor = 0;

		if(decoration)
		{
			std::vector<tstring> tokens;
			split_string(decoration, tokens, _t(" "));
			for(std::vector<tstring>::iterator i = tokens.begin(); i != tokens.end(); i++)
			{
				if(!t_strcasecmp(i->c_str(), _t("underline")))
				{
					decor |= font_decoration_underline;
				} else if(!t_strcasecmp(i->c_str(), _t("line-through")))
				{
					decor |= font_decoration_linethrough;
				} else if(!t_strcasecmp(i->c_str(), _t("overline")))
				{
					decor |= font_decoration_overline;
				}
			}
		}

		font_item fi= {0};

		fi.font = m_container->create_font(name, size, fw, fs, decor, &fi.metrics);
		m_fonts[key] = fi;
		ret = fi.font;
		if(font_metrics_ptr_writable_doc(fm))
		{
			*fm = fi.metrics;
		}
	}
	return ret;
}

litehtml::uint_ptr litehtml::document::get_font( const tchar_t* name, int size, const tchar_t* weight, const tchar_t* style, const tchar_t* decoration, font_metrics* fm )
{
	uint64_t start_ms = sys_tic_ms(0);
	if( !name || (name && !t_strcasecmp(name, _t("inherit"))) )
	{
		name = m_container->get_default_font_name();
	}

	if(size < 0)
	{
		size = container()->get_default_font_size();
	}

	if(m_last_font_valid &&
		m_last_font_size == size &&
		m_last_font_name == name &&
		m_last_font_weight == weight &&
		m_last_font_style == style &&
		m_last_font_decoration == decoration)
	{
		if(fm)
		{
			*fm = m_last_font_metrics;
		}
		profile_get_font(true, start_ms);
		return m_last_font;
	}

	tchar_t strSize[20];
	t_itoa(size, strSize, 20, 10);

	tstring key = name;
	key += _t(":");
	key += strSize;
	key += _t(":");
	key += weight;
	key += _t(":");
	key += style;
	key += _t(":");
	key += decoration;

	fonts_map::iterator el = m_fonts.find(key);

	if(el != m_fonts.end())
	{
		if(fm)
		{
			*fm = el->second.metrics;
		}
		m_last_font_valid = true;
		m_last_font_name = name;
		m_last_font_weight = weight;
		m_last_font_style = style;
		m_last_font_decoration = decoration;
		m_last_font_size = size;
		m_last_font = el->second.font;
		m_last_font_metrics = el->second.metrics;
		profile_get_font(true, start_ms);
		return el->second.font;
	}
	uint_ptr font = add_font(name, size, weight, style, decoration, fm);
	if(font)
	{
		fonts_map::iterator cached = m_fonts.find(key);
		if(cached != m_fonts.end())
		{
			m_last_font_valid = true;
			m_last_font_name = name;
			m_last_font_weight = weight;
			m_last_font_style = style;
			m_last_font_decoration = decoration;
			m_last_font_size = size;
			m_last_font = cached->second.font;
			m_last_font_metrics = cached->second.metrics;
		}
	}
	profile_get_font(false, start_ms);
	return font;
}

int litehtml::document::render( int max_width, render_type rt )
{
	int ret = 0;
	/* Lift display:contents children into their grandparents before any layout
	 * walk starts (parse_styles queued them; no child iteration is in flight
	 * here, so the vector moves are safe). */
	if(!m_contents_splice.empty())
	{
		for(auto* el : m_contents_splice)
			el->splice_contents_children();
		m_contents_splice.clear();
	}
	/* Viewport units (vw/vh/vmin/vmax) resolve against m_media, snapshotted
	 * from the container at create time - often before the widget had a client
	 * size - and media_changed() only refreshes it when the page carries @media
	 * rules. Refresh every layout so 100vh tracks the live viewport (and window
	 * resizes) even on pages without @media. */
	container()->get_media_features(m_media);
	if(m_root)
	{
		/* A post-creation style update can flip a cell's computed display; the
		 * grid captured in init() would keep the stale column set, so rebuild
		 * the tabular grids before this layout pass measures them. */
		if(m_tables_dirty)
		{
			m_tables_dirty = false;
			m_root->init();
		}
		if(rt == render_fixed_only)
		{
			m_fixed_boxes.clear();
			m_root->render_positioned(rt);
		} else
		{
			ret = m_root->render(0, 0, max_width);
			if(m_root->fetch_positioned())
			{
				m_fixed_boxes.clear();
				m_root->render_positioned(rt);
			}
			m_size.width	= 0;
			m_size.height	= 0;
			m_root->calc_document_size(m_size);
		}
	}
	return ret;
}

void litehtml::document::draw( uint_ptr hdc, int x, int y, const position* clip )
{
	if(m_root)
	{
		m_root->draw(hdc, x, y, clip);
		m_root->draw_stacking_context(hdc, x, y, clip, true);
	}
}

int litehtml::document::cvt_units( const tchar_t* str, int fontSize, bool* is_percent/*= 0*/ ) const
{
	uint64_t start_ms = sys_tic_ms(0);
	if(!str)	return 0;
	
	css_length val;
	val.fromString(str);
	if(is_percent && val.units() == css_units_percentage && !val.is_predefined())
	{
		*is_percent = true;
	}
	int ret = cvt_units(val, fontSize);
	profile_cvt_units(start_ms);
	return ret;
}

int litehtml::document::cvt_units( css_length& val, int fontSize, int size ) const
{
	uint64_t start_ms = sys_tic_ms(0);
	if(val.is_predefined())
	{
		profile_cvt_units(start_ms);
		return 0;
	}
	int ret = 0;
	switch(val.units())
	{
	case css_units_percentage:
		ret = val.calc_percent(size);
		break;
	case css_units_em:
		ret = round_f(val.val() * fontSize);
		val.set_value((float) ret, css_units_px);
		break;
	case css_units_pt:
		ret = m_container->pt_to_px((int) val.val());
		val.set_value((float) ret, css_units_px);
		break;
	case css_units_in:
		ret = m_container->pt_to_px((int) (val.val() * 72));
		val.set_value((float) ret, css_units_px);
		break;
	case css_units_cm:
		ret = m_container->pt_to_px((int) (val.val() * 0.3937 * 72));
		val.set_value((float) ret, css_units_px);
		break;
	case css_units_mm:
		ret = m_container->pt_to_px((int) (val.val() * 0.3937 * 72) / 10);
		val.set_value((float) ret, css_units_px);
		break;
	case css_units_vw:
		ret = (int)((double)m_media.width * (double)val.val() / 100.0);
		break;
	case css_units_vh:
		ret = (int)((double)m_media.height * (double)val.val() / 100.0);
		break;
	case css_units_vmin:
		ret = (int)((double)std::min(m_media.height, m_media.width) * (double)val.val() / 100.0);
		break;
	case css_units_vmax:
		ret = (int)((double)std::max(m_media.height, m_media.width) * (double)val.val() / 100.0);
		break;
	case css_units_rem:
		/* root element font size; fall back to the container default while the
		 * root box has not been styled yet */
		{
			int root_sz = m_root ? m_root->get_font_size() : 0;
			if(root_sz <= 0)
			{
				root_sz = m_container->get_default_font_size();
			}
			ret = round_f(val.val() * root_sz);
			val.set_value((float) ret, css_units_px);
		}
		break;
	case css_units_ch:
		/* advance of "0"; approximated as half an em, which is what most
		 * proportional faces measure within a few percent */
		ret = round_f(val.val() * fontSize / 2);
		val.set_value((float) ret, css_units_px);
		break;
	default:
		ret = (int) val.val();
		break;
	}
	profile_cvt_units(start_ms);
	return ret;
}

int litehtml::document::width() const
{
	return m_size.width;
}

int litehtml::document::height() const
{
	return m_size.height;
}

void litehtml::document::add_stylesheet( const tchar_t* str, const tchar_t* baseurl, const tchar_t* media )
{
	if(str && str[0])
	{
		/* Master css changed: any chunked update in flight is stale (its epoch
		 * stamps would skip elements against the old stylesheet set). */
		abort_style_step();
		m_css.push_back(css_text(str, baseurl, media));
	}
}

bool litehtml::document::on_mouse_over( int x, int y, int client_x, int client_y, position::vector& redraw_boxes )
{
	if(!m_root)
	{
		return false;
	}

	element::ptr over_el = m_root->get_element_by_point(x, y, client_x, client_y);

	bool state_was_changed = false;

	if(over_el != m_over_element)
	{
		if(m_over_element)
		{
			if(m_over_element->on_mouse_leave())
			{
				state_was_changed = true;
			}
		}
		m_over_element = over_el;
	}

	const tchar_t* cursor = 0;

	if(m_over_element)
	{
		if(m_over_element->on_mouse_over())
		{
			state_was_changed = true;
		}
		cursor = m_over_element->get_cursor();
	}
	
	m_container->set_cursor(cursor ? cursor : _t("auto"));
	
	if(state_was_changed)
	{
		return m_root->find_styles_changes(redraw_boxes, 0, 0);
	}
	return false;
}

bool litehtml::document::on_mouse_leave( position::vector& redraw_boxes )
{
	if(!m_root)
	{
		return false;
	}
	if(m_over_element)
	{
		if(m_over_element->on_mouse_leave())
		{
			return m_root->find_styles_changes(redraw_boxes, 0, 0);
		}
	}
	return false;
}

bool litehtml::document::on_lbutton_down( int x, int y, int client_x, int client_y, position::vector& redraw_boxes )
{
	if(!m_root)
	{
		return false;
	}

	element::ptr over_el = m_root->get_element_by_point(x, y, client_x, client_y);

	bool state_was_changed = false;

	if(over_el != m_over_element)
	{
		if(m_over_element)
		{
			if(m_over_element->on_mouse_leave())
			{
				state_was_changed = true;
			}
		}
		m_over_element = over_el;
		if(m_over_element)
		{
			if(m_over_element->on_mouse_over())
			{
				state_was_changed = true;
			}
		}
	}

	const tchar_t* cursor = 0;

	if(m_over_element)
	{
		if(m_over_element->on_lbutton_down())
		{
			state_was_changed = true;
		}
		cursor = m_over_element->get_cursor();
	}

	m_container->set_cursor(cursor ? cursor : _t("auto"));

	if(state_was_changed)
	{
		return m_root->find_styles_changes(redraw_boxes, 0, 0);
	}

	return false;
}

bool litehtml::document::on_lbutton_up( int x, int y, int client_x, int client_y, position::vector& redraw_boxes )
{
	if(!m_root)
	{
		return false;
	}
	if(m_over_element)
	{
		if(m_over_element->on_lbutton_up())
		{
			return m_root->find_styles_changes(redraw_boxes, 0, 0);
		}
	}
	return false;
}

namespace {
/* Per-node cascade order mirrors document creation: master sheet, then
 * attribute-derived properties, then the document sheets, so presentation
 * attributes keep losing to author rules. The walk stops at nodes that are
 * already styled, which keeps re-parenting an existing subtree free. */
void style_detached_subtree_walk(litehtml::element* el,
                                 const litehtml::css& master,
                                 const litehtml::css& doc_css)
{
	/* The walk is entered from a live root (the node handed to
	 * appendChild/insertBefore) but recurses through every descendant, and a
	 * subtree can hang off a live ancestor whose sibling slot was freed and
	 * recycled by the mario VM (a stale handle shows up as a JS "prototype"
	 * blob). The embedder guards only vet that root, so re-check liveness at
	 * every step. */
	if(!el)
	{
		return;
	}
#ifdef LITEHTML_LIFETIME_DEBUG
	/* Gate on the lifetime registry BEFORE touching any member of el. The
	 * registry is a non-dereferencing pointer-set lookup maintained by the
	 * html_tag ctor/dtor, so it is both safe on a stale handle and
	 * authoritative: ~html_tag unregisters the node before the freed block is
	 * reused. The element magic is cleared in ~element too, but a block the
	 * mario VM has recycled can read back as "live" when the VM refills the
	 * magic offset - that is how a dead child of <body> slipped past a
	 * magic-only check and aborted in apply_stylesheet_own. Returning here also
	 * avoids get_children_count() on recycled memory, whose huge count would
	 * send the child loop below walking wild pointers. Live text/comment nodes
	 * are not html_tags and are absent from the registry, but they carry no tag
	 * name and no children, so skipping them costs nothing. */
	if(!litehtml_tag_is_live(el))
	{
		return;
	}
#else
	/* Production build: the registry is compiled out, so fall back to the
	 * element magic. Reading it is safe for any pointer that still lands in the
	 * mapped heap, which is the case for a recycled node. */
	if(!el->is_live_handle())
	{
		return;
	}
#endif
	if(el->sheets_applied())
	{
		return;
	}
	el->set_sheets_applied(true);
	const litehtml::tchar_t* tag = el->get_tagName();
	if(tag && tag[0])
	{
		litehtml::html_tag* t = static_cast<litehtml::html_tag*>(el);
		t->apply_stylesheet_own(master);
		t->parse_attributes();
		t->apply_stylesheet_own(doc_css);
	}
	int count = el->get_children_count();
	for(int i = 0; i < count; i++)
	{
		style_detached_subtree_walk(el->get_child(i), master, doc_css);
	}
}
} // namespace

void litehtml::document::queue_contents_splice(element* el)
{
	if(el) m_contents_splice.push_back(el);
}

void litehtml::document::style_detached_subtree(element* el)
{
	if(!el || !m_context)
	{
		return;
	}
	style_detached_subtree_walk(el, m_context->master_css(), m_styles);
}

litehtml::element::ptr litehtml::document::create_element(const tchar_t* tag_name, const string_map& attributes)
{
	element::ptr newTag = nullptr;
	// XContainer only customizes <input>; avoid unnecessary virtual dispatch
	// for every other tag during DOM construction.
	if(m_container && tag_name && !t_strcmp(tag_name, _t("input")))
	{
		newTag = m_container->create_element(tag_name, attributes, this);
	}
	if(!newTag)
	{
		if(!t_strcmp(tag_name, _t("br")))
		{
			newTag = litehtml_alloc<litehtml::el_break>("el_break", this);
		} else if(!t_strcmp(tag_name, _t("p")))
		{
			newTag = litehtml_alloc<litehtml::el_para>("el_para", this);
		} else if(!t_strcmp(tag_name, _t("img")))
		{
			newTag = litehtml_alloc<litehtml::el_image>("el_image", this);
		} else if(!t_strcmp(tag_name, _t("svg")))
		{
			newTag = litehtml_alloc<litehtml::el_svg>("el_svg", this);
		} else if(!t_strcmp(tag_name, _t("table")))
		{
			newTag = litehtml_alloc<litehtml::el_table>("el_table", this);
		} else if(!t_strcmp(tag_name, _t("td")) || !t_strcmp(tag_name, _t("th")))
		{
			newTag = litehtml_alloc<litehtml::el_td>("el_td", this);
		} else if(!t_strcmp(tag_name, _t("link")))
		{
			newTag = litehtml_alloc<litehtml::el_link>("el_link", this);
		} else if(!t_strcmp(tag_name, _t("title")))
		{
			newTag = litehtml_alloc<litehtml::el_title>("el_title", this);
		} else if(!t_strcmp(tag_name, _t("a")))
		{
			newTag = litehtml_alloc<litehtml::el_anchor>("el_anchor", this);
		} else if(!t_strcmp(tag_name, _t("tr")))
		{
			newTag = litehtml_alloc<litehtml::el_tr>("el_tr", this);
		} else if(!t_strcmp(tag_name, _t("style")))
		{
			newTag = litehtml_alloc<litehtml::el_style>("el_style", this);
		} else if(!t_strcmp(tag_name, _t("base")))
		{
			newTag = litehtml_alloc<litehtml::el_base>("el_base", this);
		} else if(!t_strcmp(tag_name, _t("body")))
		{
			newTag = litehtml_alloc<litehtml::el_body>("el_body", this);
		} else if(!t_strcmp(tag_name, _t("div")))
		{
			newTag = litehtml_alloc<litehtml::el_div>("el_div", this);
		} else if(!t_strcmp(tag_name, _t("script")))
		{
			newTag = litehtml_alloc<litehtml::el_script>("el_script", this);
		} else if(!t_strcmp(tag_name, _t("font")))
		{
			newTag = litehtml_alloc<litehtml::el_font>("el_font", this);
		} else
		{
			newTag = litehtml_alloc<litehtml::html_tag>("html_tag", this);
		}
	}
	if(newTag)
	{
		newTag->set_tagName(tag_name);
		for (string_map::const_iterator iter = attributes.begin(); iter != attributes.end(); iter++)
		{
			newTag->set_attr(iter->first.c_str(), iter->second.c_str());
		}
	}

	return newTag;
}

void litehtml::document::get_fixed_boxes( position::vector& fixed_boxes )
{
	fixed_boxes = m_fixed_boxes;
}

void litehtml::document::add_fixed_box( const position& pos )
{
	m_fixed_boxes.push_back(pos);
}

bool litehtml::document::media_changed()
{
	if(!m_media_lists.empty())
	{
		container()->get_media_features(m_media);
		if (update_media_lists(m_media))
		{
			/* Full unbounded reparse below: drop any chunked step in flight so
			 * its epoch stamps cannot make parse_styles skip elements. */
			abort_style_step();
			m_root->refresh_styles();
			m_root->parse_styles();
			return true;
		}
	}
	return false;
}

bool litehtml::document::lang_changed()
{
	if(!m_media_lists.empty())
	{
		tstring culture;
		container()->get_language(m_lang, culture);
		if(!culture.empty())
		{
			m_culture = m_lang + _t('-') + culture;
		}
		else
		{
			m_culture.clear();
		}
		abort_style_step(); /* see media_changed(): unbounded reparse follows */
		m_root->refresh_styles();
		m_root->parse_styles();
		return true;
	}
	return false;
}

bool litehtml::document::update_media_lists(const media_features& features)
{
	bool update_styles = false;
	for(media_query_list::vector::iterator iter = m_media_lists.begin(); iter != m_media_lists.end(); iter++)
	{
		if((*iter)->apply_media_features(features))
		{
			update_styles = true;
		}
	}
	return update_styles;
}

void litehtml::document::update_master_styles()
{
	/* Legacy unbounded entry point: run the chunked update to completion. */
	while(!update_master_styles_step(sys_tic_ms(0) + 10000))
	{
	}
}

bool litehtml::document::style_step_exhausted()
{
	if(m_step_exhausted)
	{
		return true;
	}
	/* Sampling the clock on every element visit is measurable on a 1k-node
	 * tree; check it once per 64 visits instead. */
	if((++m_step_visits & 63) == 0 && sys_tic_ms(0) >= m_step_deadline)
	{
		m_step_exhausted = true;
	}
	return m_step_exhausted;
}

void litehtml::document::style_step_stamp(element* el)
{
	if(el)
	{
		el->m_step_stamp = m_step_epoch;
		/* A fresh stamp means only this element's own work has completed; its
		 * subtree has not been covered yet. Clear any m_step_done left over
		 * from the previous phase or epoch, otherwise the element would be
		 * pruned on resume and its children never visited. */
		el->m_step_done = false;
		m_step_stamped++;
	}
}

bool litehtml::document::update_master_styles_step(uint64_t deadline_ms)
{
	if(!m_root || !m_context)
	{
		return true;
	}
	if(m_step_phase == 0)
	{
		reset_dom_internal_profile();
		reset_parse_style_profile();
		m_step_start = sys_tic_ms(0);
		m_step_apply_ms = 0;
		m_step_parse_ms = 0;
		/* Stylesheets may have been (re)loaded since document creation; their
		 * @media lists carry doc==0 at parse time, so re-register and
		 * evaluate them here before the apply walk consults is_media_valid(). */
		register_master_media_lists();
		m_root->refresh_styles();
		/* Fresh epoch pair per update: apply uses E, parse uses E+1, so stamps
		 * never alias across phases or against a previous update. */
		m_step_epoch += 2;
		m_step_phase = 1;
	}
	m_step_deadline = deadline_ms;
	m_step_exhausted = false;
	m_step_visits = 0;

	if(m_step_phase == 1)
	{
		uint64_t walk_start = sys_tic_ms(0);
		/* Single resumable walk: html_tag::apply_stylesheet applies the master
		 * sheet and m_styles per element while stepping, so stamps stay in one
		 * epoch and a paused subtree resumes at the frontier. A second walk
		 * with a bumped epoch (previous behaviour) made every resumed chunk
		 * redo the whole master pass from the root, so small time slices never
		 * reached the end of the tree. */
		m_root->apply_stylesheet(m_context->master_css());
		m_step_apply_ms += (uint32_t)(sys_tic_ms(0) - walk_start);
		if(m_step_exhausted)
		{
			return false;
		}
		/* The walk covered the whole tree: apply phase complete. */
		m_step_epoch++;
		m_step_phase = 2;
		m_step_exhausted = false;
		m_step_visits = 0;
	}
	if(m_step_phase == 2)
	{
		if(sys_tic_ms(0) >= m_step_deadline)
		{
			return false;
		}
		uint64_t walk_start = sys_tic_ms(0);
		m_root->parse_styles();
		m_step_parse_ms += (uint32_t)(sys_tic_ms(0) - walk_start);
		if(m_step_exhausted)
		{
			return false;
		}
	}
	m_step_phase = 0;
	/* Computed displays may have flipped (a late sheet hiding table cells, a
	 * media flip): table grids were built in init() against the old values. */
	m_tables_dirty = true;
	dump_parse_style_profile();
	dump_dom_internal_profile();
	litehtml::dump_apply_phase_profile();
	return true;
}

void litehtml::document::add_media_list( media_query_list::ptr list )
{
	if(list)
	{
		if(std::find(m_media_lists.begin(), m_media_lists.end(), list) == m_media_lists.end())
		{
			m_media_lists.push_back(list);
		}
	}
}

void litehtml::document::register_master_media_lists()
{
	if(!m_context)
	{
		return;
	}
	/* Tens of thousands of selectors can share a few thousand lists; the
	 * linear dedup in add_media_list would make this O(n^2) per restyle. */
	std::vector<media_query_list::ptr> seen(m_media_lists.begin(), m_media_lists.end());
	std::sort(seen.begin(), seen.end());
	const css_selector::vector& sels = m_context->master_css().selectors();
	for(css_selector::vector::const_iterator sel = sels.begin(); sel != sels.end(); sel++)
	{
		media_query_list::ptr mq = (*sel)->m_media_query;
		if(!mq)
		{
			continue;
		}
		std::vector<media_query_list::ptr>::iterator pos = std::lower_bound(seen.begin(), seen.end(), mq);
		if(pos == seen.end() || *pos != mq)
		{
			seen.insert(pos, mq);
			m_media_lists.push_back(mq);
		}
	}
	if(!m_media_lists.empty())
	{
		container()->get_media_features(m_media);
		update_media_lists(m_media);
	}
}

void litehtml::document::create_node(GumboNode* node, elements_vector& elements, int depth)
{
	if(!node) return;
	if(depth > 64) return;  // prevent stack overflow on deeply nested HTML
	g_create_node_profile.calls++;
	GumboNodeType node_type = node->type;
	switch (node_type)
	{
	case GUMBO_NODE_ELEMENT:
		{
			g_create_node_profile.element_nodes++;
			string_map attrs;
			GumboAttribute* attr;
			uint64_t attrs_start = sys_tic_ms(0);
			for (unsigned int i = 0; i < node->v.element.attributes.length; i++)
			{
				attr = (GumboAttribute*)node->v.element.attributes.data[i];
				if(!attr || !attr->name || !attr->value)
				{
					continue;
				}
				attrs[tstring(litehtml_from_utf8(attr->name))] = litehtml_from_utf8(attr->value);
			}
			add_create_node_time(g_create_node_profile.attrs_ms, attrs_start);


			element::ptr ret = nullptr;
			const char* tag = gumbo_normalized_tagname(node->v.element.tag);
			uint64_t create_start = sys_tic_ms(0);
			if (tag && tag[0])
			{
				ret = create_element(litehtml_from_utf8(tag), attrs);
			}
			else
			{
				if (node->v.element.original_tag.data && node->v.element.original_tag.length)
				{
					std::string strA;
					gumbo_tag_from_original_text(&node->v.element.original_tag);
					strA.append(node->v.element.original_tag.data, node->v.element.original_tag.length);
					ret = create_element(litehtml_from_utf8(strA.c_str()), attrs);
				}
			}
			add_create_node_time(g_create_node_profile.create_element_ms, create_start);
			if (ret)
			{
				elements_vector child;
				uint64_t children_start = sys_tic_ms(0);
				for (unsigned int i = 0; i < node->v.element.children.length; i++)
				{
					child.clear();
					GumboNode* child_node = static_cast<GumboNode*> (node->v.element.children.data[i]);
					if(!child_node)
					{
						continue;
					}
					create_node(child_node, child, depth + 1);
					for(auto& el : child)
					{
						ret->appendChild(el);
					}
				}
				add_create_node_time(g_create_node_profile.children_ms, children_start);
				elements.push_back(ret);
			}
		}
		break;
	case GUMBO_NODE_TEXT:
		{
			g_create_node_profile.text_nodes++;
			uint64_t text_start = sys_tic_ms(0);
			std::string str;
			std::string spaces;
			const char* str_in = node->v.text.text;
			if(!str_in) return;
			if(!str_in[0]) return;
			size_t str_in_len = strlen(str_in);
			if(node->parent &&
				node->parent->type == GUMBO_NODE_ELEMENT &&
				(node->parent->v.element.tag == GUMBO_TAG_STYLE ||
				 node->parent->v.element.tag == GUMBO_TAG_SCRIPT))
			{
				element::ptr text = litehtml_alloc<el_text>("el_text(rawtext)", str_in, this);
				if(text)
				{
					elements.push_back(text);
				}
				return;
			}
			str.reserve(str_in_len);
			spaces.reserve(str_in_len);
			unsigned char c;
			auto flush_text = [&]()
			{
				if (!str.empty())
				{
					element::ptr text = litehtml_alloc<el_text>("el_text", str.c_str(), this);
					if(text)
					{
						elements.push_back(text);
					}
					str.clear();
				}
			};
			auto flush_spaces = [&]()
			{
				if (!spaces.empty())
				{
					element::ptr space = litehtml_alloc<el_space>("el_space", spaces.c_str(), this);
					if(space)
					{
						elements.push_back(space);
					}
					spaces.clear();
				}
			};
			for (size_t i = 0; i < str_in_len; i++)
			{
				c = (unsigned char) str_in[i];
				if (c == ' ' || c == '\t' || c == '\r' || c == '\f')
				{
					flush_text();
					spaces += c;
				}
				else if (c == '\n')
				{
					flush_text();
					flush_spaces();
					spaces += c;
					flush_spaces();
				}
				// CJK character range - simplified for UTF-8
				else if (c >= 0xE4 && c <= 0xE9)
				{
					// Flush any previously accumulated non-CJK (ASCII) text
					// CJK and ASCII must be in separate el_text nodes
					if (!str.empty() && (unsigned char)str[0] < 0xE4) {
						flush_text();
					}
					flush_spaces();
					// Accumulate CJK character (3 bytes UTF-8)
					if (i + 2 < str_in_len) {
						str += c;
						str += str_in[++i];
						str += str_in[++i];
					} else {
						// Incomplete UTF-8 at end of input - add available bytes
						str += c;
						if (i + 1 < str_in_len) str += str_in[++i];
						if (i + 1 < str_in_len) str += str_in[++i];
					}
					// Don't flush yet - consecutive CJK chars batch together
					// They will be flushed when whitespace/ASCII/EOF follows
				}
				else
				{
					flush_spaces();
					str += c;
				}
			}
			flush_text();
			flush_spaces();
			add_create_node_time(g_create_node_profile.text_split_ms, text_start);
		}
		break;
	case GUMBO_NODE_CDATA:
		{
			element::ptr ret = litehtml_alloc<el_cdata>("el_cdata", this);
			if(ret)
			{
				if(node->v.text.text)
					ret->set_data(litehtml_from_utf8(node->v.text.text));
				elements.push_back(ret);
			}
		}
		break;
	case GUMBO_NODE_COMMENT:
		{
			element::ptr ret = litehtml_alloc<el_comment>("el_comment", this);
			if(ret)
			{
				if(node->v.text.text)
					ret->set_data(litehtml_from_utf8(node->v.text.text));
				elements.push_back(ret);
			}
		}
		break;
	case GUMBO_NODE_WHITESPACE:
		{
			std::string spaces;
			const char* str_in = node->v.text.text;
			if(!str_in || !str_in[0]) return;
			size_t str_in_len = strlen(str_in);
			spaces.reserve(str_in_len);
			auto flush_spaces = [&]()
			{
				if(!spaces.empty())
				{
					element::ptr space = litehtml_alloc<el_space>("el_space", spaces.c_str(), this);
					if(space)
					{
						elements.push_back(space);
					}
					spaces.clear();
				}
			};
			for(size_t i = 0; i < str_in_len; i++)
			{
				unsigned char c = (unsigned char)str_in[i];
				if(c == '\n')
				{
					flush_spaces();
					spaces += c;
					flush_spaces();
				}
				else
				{
					spaces += c;
				}
			}
			flush_spaces();
		}
		break;
	default:
		break;
	}
}

void litehtml::document::fix_tables_layout()
{
	size_t i = 0;
	while (i < m_tabular_elements.size())
	{
		element::ptr el_ptr = m_tabular_elements[i];
		
		// Check if pointer is valid
		if (!el_ptr)
		{
			i++;
			continue;
		}

		switch (el_ptr->get_display())
		{
		case display_inline_table:
		case display_table:
			fix_table_children(el_ptr, display_table_row_group, _t("table-row-group"));
			break;
		case display_table_footer_group:
		case display_table_row_group:
		case display_table_header_group:
			fix_table_parent(el_ptr, display_table, _t("table"));
			fix_table_children(el_ptr, display_table_row, _t("table-row"));
			break;
		case display_table_row:
			fix_table_parent(el_ptr, display_table_row_group, _t("table-row-group"));
			fix_table_children(el_ptr, display_table_cell, _t("table-cell"));
			break;
		case display_table_cell:
			fix_table_parent(el_ptr, display_table_row, _t("table-row"));
			break;
		// TODO: make table layout fix for table-caption, table-column etc. elements
		case display_table_caption:
		case display_table_column:
		case display_table_column_group:
		default:
			break;
		}
		i++;
	}
}

void litehtml::document::fix_table_children(element::ptr& el_ptr, style_display disp, const tchar_t* disp_str)
{
	elements_vector tmp;
	elements_vector::iterator first_iter = el_ptr->m_children.begin();
	elements_vector::iterator cur_iter = el_ptr->m_children.begin();

	auto flush_elements = [&]()
	{
		element::ptr annon_tag = litehtml_alloc<html_tag>("html_tag(table-child)", this);
		if(!annon_tag)
		{
			tmp.clear();
			return;
		}
		style st;
		st.add_property(_t("display"), disp_str, 0, false);
		annon_tag->add_style(st);
		annon_tag->parent(el_ptr);
		annon_tag->parse_styles();
		std::for_each(tmp.begin(), tmp.end(),
			[&annon_tag](element::ptr& el)
			{
				annon_tag->appendChild(el);
			}
		);
		first_iter = el_ptr->m_children.insert(first_iter, annon_tag);
		cur_iter = first_iter + 1;
		while (cur_iter != el_ptr->m_children.end() && (*cur_iter)->parent() != el_ptr)
		{
			cur_iter = el_ptr->m_children.erase(cur_iter);
		}
		first_iter = cur_iter;
		tmp.clear();
	};

	while (cur_iter != el_ptr->m_children.end())
	{
		if ((*cur_iter)->get_display() != disp)
		{
			if (!(*cur_iter)->is_white_space() || ((*cur_iter)->is_white_space() && !tmp.empty()))
			{
				if (tmp.empty())
				{
					first_iter = cur_iter;
				}
				tmp.push_back((*cur_iter));
			}
			cur_iter++;
		}
		else if (!tmp.empty())
		{
			flush_elements();
		}
		else
		{
			cur_iter++;
		}
	}
	if (!tmp.empty())
	{
		flush_elements();
	}
}

void litehtml::document::fix_table_parent(element::ptr& el_ptr, style_display disp, const tchar_t* disp_str)
{
	element::ptr parent = el_ptr->parent();

	if (!parent)
	{
		return;
	}

	if (parent->get_display() != disp)
	{
		elements_vector::iterator this_element = std::find_if(parent->m_children.begin(), parent->m_children.end(),
			[&](element::ptr& el)
			{
				if (el == el_ptr)
				{
					return true;
				}
				return false;
			}
		);
		if (this_element != parent->m_children.end())
		{
			style_display el_disp = el_ptr->get_display();
			elements_vector::iterator first = this_element;
			elements_vector::iterator last = this_element;
			elements_vector::iterator cur = this_element;

			// find first element with same display
			while (true)
			{
				if (cur == parent->m_children.begin()) break;
				cur--;
				if ((*cur)->is_white_space() || (*cur)->get_display() == el_disp)
				{
					first = cur;
				}
				else
				{
					break;
				}
			}

			// find last element with same display
			cur = this_element;
			while (true)
			{
				cur++;
				if (cur == parent->m_children.end()) break;

				if ((*cur)->is_white_space() || (*cur)->get_display() == el_disp)
				{
					last = cur;
				}
				else
				{
					break;
				}
			}

			// extract elements with the same display and wrap them with anonymous object
			element::ptr annon_tag = litehtml_alloc<html_tag>("html_tag(table-parent)", this);
			if(!annon_tag)
			{
				return;
			}
			style st;
			st.add_property(_t("display"), disp_str, 0, false);
			annon_tag->add_style(st);
			annon_tag->parent(parent);
			annon_tag->parse_styles();
			std::for_each(first, last + 1,
				[&annon_tag](element::ptr& el)
				{
					annon_tag->appendChild(el);
				}
			);
			first = parent->m_children.erase(first, last + 1);
			parent->m_children.insert(first, annon_tag);
		}
	}
}
