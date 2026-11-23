#include "html.h"
#include "el_comment.h"

litehtml::el_comment::el_comment(litehtml::document* doc) : litehtml::element(doc)
{
	m_skip = true;
}

litehtml::el_comment::~el_comment()
{

}

void litehtml::el_comment::get_text( tstring& text )
{
	text += m_text;
}

bool litehtml::el_comment::is_comment() const
{
	return true;
}

litehtml::element::ptr litehtml::el_comment::clone_node(bool deep)
{
	/* A comment node has no children, so deep and shallow clones are identical.
	 * Built with malloc + placement-new exactly like litehtml_alloc and
	 * jsCreateComment, keeping the matching free() on teardown balanced. */
	void* mem = malloc(sizeof(el_comment));
	if(!mem) return nullptr;
	el_comment* c = new (mem) el_comment(m_doc);
	c->m_text = m_text;
	return c;
}

void litehtml::el_comment::set_data( const tchar_t* data )
{
	if(data)
	{
		m_text += data;
	}
}

void litehtml::el_comment::set_data_replace( const tchar_t* data )
{
	/* CharacterData.data = x REPLACES the whole value; the parser's chunked
	 * append path stays in set_data(). React's streaming reveal runtime writes
	 * boundary markers this way ($RC: `a.previousSibling.data = "$~"`, $RV:
	 * `g.data = "$"`), so an append here would corrupt every marker. */
	m_text = data ? data : _t("");
}
