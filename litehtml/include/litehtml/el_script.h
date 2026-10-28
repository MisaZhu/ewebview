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
		/* appendChild() captures the script body into the private m_text and keeps no
		 * children, so the inherited element::get_text (a no-op) handed
		 * jsDynamicScriptInserted an EMPTY body: every dynamically inserted INLINE
		 * <script> (next/script `children` payloads, e.g. rokid's renderComponents
		 * bootstrap that constructs the site Header) was silently dropped. Expose
		 * the captured body through get_text. */
		virtual void			get_text(tstring& text) override;
		/* The DOM gives an inline <script> exactly one text child; expose the
		 * captured body node through the child accessors so childNodes /
		 * firstChild match a real browser (React hydration walks it and
		 * desyncs with #418 when firstChild is null). */
		virtual size_t			get_children_count() const override;
		virtual element::ptr	get_child(int idx) const override;
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
		/* The DOM bridge's attribute channel (attributes NamedNodeMap,
		 * getAttributeNode, attr snapshots) reads eweb_attrs(); the inherited
		 * html_tag store does not exist here (el_script derives from element),
		 * so without this override every <script> looked attribute-LESS to JS
		 * even though getAttribute worked. React 19's head-singleton hydration
		 * adopts <script async src> nodes as hoistable resources by reading
		 * their attributes; an empty view leaves the head walk unable to match
		 * them and the hydration pointer desyncs (rokid #418). */
		virtual const string_map*	eweb_attrs() override { return &m_script_attrs; }
	};
}
