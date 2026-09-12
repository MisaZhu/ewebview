#pragma once
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
		mutable selector_index	m_index;
	public:
		css()
		{
			m_has_before_after = false;
		}
		
		
		~css()
		{

		}

		const css_selector::vector& selectors() const
		{
			return m_selectors;
		}

		void clear()
		{
			m_selectors.clear();
			m_has_before_after = false;
			m_index.built = false;
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
