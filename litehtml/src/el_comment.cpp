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
