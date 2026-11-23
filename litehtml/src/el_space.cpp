#include "html.h"
#include "document.h"
#include "el_space.h"
#include <cstdlib>
#include <new>

litehtml::el_space::el_space(const tchar_t* text, litehtml::document* doc) : el_text(text, doc)
{
}

litehtml::el_space::~el_space()
{

}

litehtml::element::ptr litehtml::el_space::clone_node(bool deep)
{
	/* Preserve the el_space type through clones (Angular ngIf transclusion
	 * clones parsed templates): el_text::clone_node would rebuild a plain
	 * el_text whose is_white_space() is false, rendering phantom line boxes
	 * for pure-whitespace runs. malloc + placement-new matches litehtml_alloc. */
	void* mem = malloc(sizeof(el_space));
	if(!mem) return nullptr;
	return new (mem) el_space(m_text.c_str(), m_doc);
}

bool litehtml::el_space::is_white_space() const
{
	white_space ws = get_white_space();
	if(	ws == white_space_normal || 
		ws == white_space_nowrap ||
		ws == white_space_pre_line )
	{
		return true;
	}
	return false;
}

bool litehtml::el_space::is_break() const
{
	white_space ws = get_white_space();
	if(	ws == white_space_pre ||
		ws == white_space_pre_line ||
		ws == white_space_pre_wrap)
	{
		if(m_text == _t("\n"))
		{
			return true;
		}
	}
	return false;
}
