#pragma once
#include "attributes.h"
#include <string>

namespace litehtml
{
	//////////////////////////////////////////////////////////////////////////
	// Defined here (not in css_selector.h) so property_value can carry it:
	// css_selector.h includes style.h, so the dependency has to point this way.
	struct selector_specificity
	{
		int		a;
		int		b;
		int		c;
		int		d;

		selector_specificity(int va = 0, int vb = 0, int vc = 0, int vd = 0)
		{
			a	= va;
			b	= vb;
			c	= vc;
			d	= vd;
		}

		void operator += (const selector_specificity& val)
		{
			a	+= val.a;
			b	+= val.b;
			c	+= val.c;
			d	+= val.d;
		}

		bool operator==(const selector_specificity& val) const
		{
			return (a == val.a && b == val.b && c == val.c && d == val.d);
		}

		bool operator!=(const selector_specificity& val) const
		{
			return (a != val.a || b != val.b || c != val.c || d != val.d);
		}

		bool operator > (const selector_specificity& val) const
		{
			if(a != val.a) return a > val.a;
			if(b != val.b) return b > val.b;
			if(c != val.c) return c > val.c;
			if(d != val.d) return d > val.d;
			return false;
		}

		bool operator >= (const selector_specificity& val) const
		{
			return ((*this) == val) || ((*this) > val);
		}

		bool operator <= (const selector_specificity& val) const
		{
			return !((*this) > val);
		}

		bool operator < (const selector_specificity& val) const
		{
			return ((*this) <= val) && ((*this) != val);
		}
	};

	/* Inline style= declarations outrank every selector: the unused 'a' slot
	 * (reserved for inline in calc_specificity) is set so they win the cascade
	 * against author/UA rules of any selector specificity. */
	const selector_specificity inline_style_specificity(1, 0, 0, 0);
	/* HTML presentation attributes participate in the author cascade with zero
	 * specificity, so any matching author selector can override them. */
	const selector_specificity presentation_attribute_specificity(0, 0, 0, 0);

	class property_value
	{
	public:
		tstring	m_value;
		bool			m_important;
		selector_specificity	m_specificity;

		property_value()
		{
			m_important = false;
			m_specificity = presentation_attribute_specificity;
		}
		property_value(const tchar_t* val, bool imp, const selector_specificity& spec = inline_style_specificity)
		{
			m_important = imp;
			m_value		= val;
			m_specificity = spec;
		}
		property_value(const property_value& val)
		{
			m_value		= val.m_value;
			m_important	= val.m_important;
			m_specificity = val.m_specificity;
		}

		property_value& operator=(const property_value& val)
		{
			m_value		= val.m_value;
			m_important	= val.m_important;
			m_specificity = val.m_specificity;
			return *this;
		}
	};

	typedef std::map<tstring, property_value>	props_map;

	class style
	{
	public:
		typedef style*		ptr;
		typedef std::vector<style::ptr>		vector;
	private:
		props_map			m_properties;
		static string_map	m_valid_values;
		static void init_valid_values();
	public:
		style();
		style(const style& val);
		virtual ~style();

		void operator=(const style& val)
		{
			m_properties = val.m_properties;
			m_cur_specificity = val.m_cur_specificity;
		}

		void add(const tchar_t* txt, const tchar_t* baseurl, const selector_specificity& spec = inline_style_specificity)
		{
			m_cur_specificity = spec;
			parse(txt, baseurl);
		}
		void add(const tchar_t* txt, size_t len, const tchar_t* baseurl, const selector_specificity& spec = inline_style_specificity)
		{
			m_cur_specificity = spec;
			parse(tstring(txt, len).c_str(), baseurl);
		}

		/* Four-argument form keeps the current declaration context, which is
		 * required by parser recursion and shorthand expansion. The explicit
		 * specificity overload starts a new declaration context. */
		void add_property(const tchar_t* name, const tchar_t* val, const tchar_t* baseurl, bool important);
		void add_property(const tchar_t* name, const tchar_t* val, const tchar_t* baseurl, bool important, const selector_specificity& spec);

		const tchar_t* get_property(const tchar_t* name) const
		{
			if(name)
			{
				props_map::const_iterator f = m_properties.find(name);
				if(f != m_properties.end())
				{
					return f->second.m_value.c_str();
				}
			}
			return 0;
		}

		void combine(const litehtml::style& src, const selector_specificity& spec = inline_style_specificity);
		void clear()
		{
			m_properties.clear();
		}
		bool empty() const
		{
			return m_properties.empty();
		}
		const props_map& properties() const
		{
			return m_properties;
		}

	private:
		/* Specificity of the rule currently being merged into this style. Set at
		 * the entry points (parse / add_property / combine) and read by
		 * add_parsed_property so the many internal shorthand-expansion calls need
		 * not each thread the value through. */
		selector_specificity	m_cur_specificity;
		void parse_property(const tstring& txt, const tchar_t* baseurl);
		void parse(const tchar_t* txt, const tchar_t* baseurl);
		void parse_short_border(const tstring& prefix, const tstring& val, bool important);
		void parse_short_background(const tstring& val, const tchar_t* baseurl, bool important);
		void parse_short_font(const tstring& val, bool important);
		void add_parsed_property(const tstring& name, const tstring& val, bool important);
		void remove_property(const tstring& name, bool important);
	};
}
