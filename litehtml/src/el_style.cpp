#include "html.h"
#include "el_style.h"
#include "document.h"


litehtml::el_style::el_style(litehtml::document* doc) : litehtml::element(doc)
{

}

litehtml::el_style::~el_style()
{

}

void litehtml::el_style::parse_attributes()
{
	register_if_changed();
}

/* Contribute this <style>'s text to the document sheet set exactly once per
 * distinct payload. document::add_stylesheet decides where the sheet goes: it
 * queues into m_css before the creation-time sweep and parses straight into
 * m_styles afterwards, so the very same call covers an SSR <style> and one JS
 * injects at runtime (gestalt's per-field `#label-x > div { color: transparent }`
 * built with createElement('style') + appendChild). The m_registered/m_reg_text
 * guard is load-bearing: parse_attributes re-runs on every restyle
 * (html_tag::reapply_style_cascade calls it), so without it a runtime sheet
 * would be re-parsed into m_styles on each pass and grow the selector set
 * without bound. */
void litehtml::el_style::register_if_changed()
{
	tstring text;
	for(auto& el : m_children)
	{
		el->get_text(text);
	}
	if(m_registered && text == m_reg_text)
	{
		return;
	}
	document* doc = get_document();
	if(doc)
	{
		doc->add_stylesheet(text.c_str(), 0, get_attr(_t("media")));
	}
	m_reg_text = text;
	m_registered = true;
}

bool litehtml::el_style::appendChild(const ptr &el)
{
	m_children.push_back(el);
	/* A runtime text swap (JS setting style.textContent on an attached <style>)
	 * replaces the sheet, so re-register it. During the initial parse
	 * m_registered is still false and parse_attributes owns registration, which
	 * keeps a parser-split text run from being registered piecemeal. */
	if(m_registered)
	{
		register_if_changed();
	}
	return true;
}

const litehtml::tchar_t* litehtml::el_style::get_tagName() const
{
	return _t("style");
}
