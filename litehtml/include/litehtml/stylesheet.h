#pragma once
#include <set>
#include "style.h"
#include "css_selector.h"

namespace litehtml
{
	class document_container;

	class css
	{
		public:
		// Bucket index over the right-most simple selector of every rule so that
		// apply_stylesheet only tests selectors that can possibly match an element
		// (keyed by required class / id / tag). kind: 1=class, 2=id, 3=tag.
		struct selector_index
		{
			typedef std::pair<int, tstring>	key_t;
			typedef std::pair<key_t, std::vector<int> >	bucket_t;
			bool						built;
			unsigned int			epoch;
			std::vector<unsigned int>	stamps;
			std::vector<bucket_t>	buckets;	// sorted by first
			std::vector<int>		universal;

			selector_index()
			{
				built = false;
				epoch = 0;
			}
			unsigned int begin_visit()
			{
				if(++epoch == 0)
				{
					for(size_t i = 0; i < stamps.size(); i++)
					{
						stamps[i] = 0;
					}
					epoch = 1;
				}
				return epoch;
			}
		};
		// Binary search over the sorted bucket vector; returns 0 when absent.
		// (The embedded std::map operator[] insert path is not trustworthy.)
		static inline std::vector<int>* find_selector_bucket(selector_index& ix, const selector_index::key_t& k)
		{
			size_t lo = 0;
			size_t hi = ix.buckets.size();
			while(lo < hi)
			{
				size_t mid = (lo + hi) >> 1;
				const selector_index::key_t& mk = ix.buckets[mid].first;
				if(mk.first < k.first || (mk.first == k.first && mk.second < k.second))
				{
					lo = mid + 1;
				}
				else
				{
					hi = mid;
				}
			}
			if(lo < ix.buckets.size())
			{
				const selector_index::key_t& mk = ix.buckets[lo].first;
				if(mk.first == k.first && !(mk.second < k.second) && !(k.second < mk.second))
				{
					return &ix.buckets[lo].second;
				}
			}
			return 0;
		}
	private:
		css_selector::vector	m_selectors;
		bool					m_has_before_after;
		/* Every attribute name any parsed selector tests ([data-x=...],
		 * [aria-...]); the DOM bridge consults it to decide whether an
		 * attribute mutation must re-run the cascade. */
		std::set<tstring>		m_attr_names;
		mutable selector_index	m_index;
	public:
		css()
		{
			m_has_before_after = false;
		}
		
		
		~css()
		{
			clear();
		}
		
		const css_selector::vector& selectors() const
		{
			return m_selectors;
		}
		
		void clear()
		{
			/* Raw-pointer ownership (css_selector::ptr / style::ptr /
			 * media_query_list::ptr are all raw): each selector owns its m_left
			 * chain (freed by ~css_selector), while one style is shared by every
			 * selector of a comma list and one media list by every rule of an
			 * @media block. Collect the shared pointers first, then free each
			 * unique one exactly once. Dropping the vector alone leaked the whole
			 * parsed sheet on every context reset / document teardown. */
			std::set<style::ptr> styles;
			std::set<media_query_list::ptr> medias;
			for(size_t i = 0; i < m_selectors.size(); i++)
			{
				css_selector::ptr s = m_selectors[i];
				for(css_selector::ptr l = s; l; l = l->m_left)
				{
					if(l->m_style) styles.insert(l->m_style);
					if(l->m_media_query) medias.insert(l->m_media_query);
				}
				delete s;
			}
			for(std::set<style::ptr>::iterator it = styles.begin(); it != styles.end(); ++it)
			{
				delete *it;
			}
			for(std::set<media_query_list::ptr>::iterator it = medias.begin(); it != medias.end(); ++it)
			{
				delete *it;
			}
			m_selectors.clear();
			m_has_before_after = false;
			m_attr_names.clear();
			m_index.built = false;
			m_index.buckets.clear();
			m_index.universal.clear();
			m_index.stamps.clear();
		}

		bool	uses_attribute(const tstring& name) const
		{
			return m_attr_names.find(name) != m_attr_names.end();
		}

		void	parse_stylesheet(const tchar_t* str, const tchar_t* baseurl, document* doc, const media_query_list::ptr& media);
		void	sort_selectors();
		selector_index& get_selector_index() const;
		void	invalidate_index()
		{
			m_index.built = false;
		}
		bool	has_before_after() const
		{
			return m_has_before_after;
		}
		static void	parse_css_url(const tstring& str, tstring& url);

	private:
		void	parse_atrule(const tstring& text, const tchar_t* baseurl, document* doc, const media_query_list::ptr& media);
		/* @supports condition evaluation (feature queries). */
		bool	supports_condition_holds(const tstring& cond);
		void	add_selector(css_selector::ptr selector);
		bool	parse_selectors(const tstring& txt, const litehtml::style::ptr& styles, const media_query_list::ptr& media);

	};

	inline void litehtml::css::add_selector( css_selector::ptr selector )
	{
		selector->m_order = (int) m_selectors.size();
		m_index.built = false;
		/* Record every attribute the selector chain tests, ancestors included:
		 * mutating such an attribute on an ancestor must restyle the subtree. */
		for(css_selector::ptr s = selector; s; s = s->m_left)
		{
			for(css_attribute_selector::vector::const_iterator at = s->m_right.m_attrs.begin(); at != s->m_right.m_attrs.end(); ++at)
			{
				m_attr_names.insert(at->attribute);
			}
		}
		if(!m_has_before_after)
		{
			for(css_attribute_selector::vector::const_iterator it = selector->m_right.m_attrs.begin(); it != selector->m_right.m_attrs.end(); ++it)
			{
				if(it->condition == select_pseudo_element && (it->val == _t("before") || it->val == _t("after")))
				{
					m_has_before_after = true;
					break;
				}
			}
		}
		m_selectors.push_back(selector);
	}

}
