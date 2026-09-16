#pragma once
#include "html_tag.h"

namespace litehtml
{
	class el_script : public element
	{
		tstring m_text;
		/* el_script derives from element (NOT html_tag), and element::set_attr /
		 * get_attr are no-ops, so a <script> could not remember its own
		 * attributes. The DOM bridge relies on them round-tripping: a dynamically
		 * injected `var s = createElement("script"); s.src = "..."; body.appendChild(s)`
		 * must be recognised as an EXTERNAL script (jsDynamicScriptInserted reads
		 * get_attr("src")) so the engine fetches and runs it. Give el_script a
		 * minimal attribute store of its own, mirroring html_tag's lower-cased keys.
		 * Parsed <script> tags never reach here (extract_scripts strips them from
		 * the HTML first), so this only affects createElement'd script nodes. */
		string_map m_script_attrs;
	public:
		el_script(litehtml::document* doc);
		virtual ~el_script();

		virtual void			parse_attributes() override;
		virtual bool			appendChild(const ptr &el) override;
		virtual const tchar_t*	get_tagName() const override;
		/* el_script derives from element (not html_tag), whose select_all is a
		 * no-op, so a createElement'd/stand-in <script> was invisible to
		 * document.getElementsByTagName("script") and document.scripts: SDKs that
		 * locate their own tag that way (taobao baxia) dereferenced undefined and
		 * aborted before installing their request signer. Match the right-hand tag
		 * (plus exact/exists attribute tests) against our fixed tag name; scripts
		 * have no element children to recurse into (appendChild captures text). */
		virtual void			select_all(const css_selector& selector, elements_vector& res) override;
		virtual element::ptr	select_one(const css_selector& selector) override;

		virtual void			set_attr(const tchar_t* name, const tchar_t* val) override;
		virtual const tchar_t*	get_attr(const tchar_t* name, const tchar_t* def = 0) override;
		virtual void			remove_attr(const tchar_t* name) override;
	};
}
