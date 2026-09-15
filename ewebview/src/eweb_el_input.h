// Replaced form-control elements for litehtml, drawn through the porting HAL.
//
// Ported from widget++'s el_input: the xwin/graph drawing is replaced by the
// eweb_gfx table, so the element knows nothing about the platform. The hdc
// litehtml hands to draw() is an eweb_surface_t* the engine is rendering into.
//
// Covers the whole form-control family the container factory maps here:
// text-like <input>/<textarea>, <input type=button|submit|reset>/<button>,
// <input type=checkbox|radio>, <select> and <input type=hidden>. Each draws a
// recognisable widget (box, border, label text, arrow / tick / dot) instead of
// the old featureless solid rectangle.

#pragma once

#include <litehtml.h>
#include <ewebview_port.h>

#include <map>
#include <string>

enum EWebInputType {
    EWEB_INPUT_TEXT,      /* <input type=text|password|search|...>            */
    EWEB_INPUT_TEXTAREA,  /* <textarea>                                       */
    EWEB_INPUT_BUTTON,    /* <input type=button|submit|reset>, <button>       */
    EWEB_INPUT_CHECKBOX,  /* <input type=checkbox>                            */
    EWEB_INPUT_RADIO,     /* <input type=radio>                               */
    EWEB_INPUT_SELECT,    /* <select>                                         */
    EWEB_INPUT_RANGE,     /* <input type=range>                               */
    EWEB_INPUT_HIDDEN     /* <input type=hidden>                              */
};

class eweb_el_input : public litehtml::html_tag
{
public:
    eweb_el_input(
        litehtml::document* doc,
        const eweb_port_t* port,
        EWebInputType inputType);
    virtual ~eweb_el_input(void);

    virtual int      line_height() const override;
    virtual bool     is_replaced() const override;
    virtual void     get_content_size(litehtml::size& sz, int max_width) override;
    virtual litehtml::style_display    get_display() const override;
    virtual litehtml::element_position get_element_position(litehtml::css_offsets* offsets = 0) const override;
    virtual int      render(int x, int y, int max_width, bool second_pass = false) override;
    virtual void     draw(litehtml::uint_ptr hdc, int x, int y, const litehtml::position* clip) override;
    virtual void     draw_stacking_context(litehtml::uint_ptr hdc, int x, int y, const litehtml::position* clip, bool with_positioned) override;
    virtual void     parse_styles(bool is_reparse) override;
    virtual void     add_widget_part_style(const litehtml::tstring& part, const litehtml::style& st) override;
    /* Hand the engine an opaque handle to this control so a mouse hit on the
     * element (or any ancestor-walk from a text child) can drive focus and
     * activation. Returns `this`. */
    virtual void*    eweb_form_widget() override { return this; }
    virtual void     on_click();

    /* ---- interaction state (driven by the engine) ---- */
    /* True when this control currently holds keyboard focus. draw() renders
     * the focus ring / caret off this flag. */
    void setFocused(bool f) { m_focused = f; }
    bool isFocused() const { return m_focused; }
    /* True when the control can receive keyboard focus (everything except
     * hidden and disabled). */
    bool isFocusable();

    /* ---- live value / checked state (single source of truth) ---- */
    /* The committed-or-being-edited value. Text controls prefer the
     * in-progress edit buffer; everything else falls back to the attribute. */
    std::string value();
    void setValue(const std::string& v);
    bool isChecked();
    void setChecked(bool c);

    /* ---- text editing (engine-driven) ---- */
    /* True for the text-like family that owns an edit buffer + caret. */
    bool isTextEditing() const;
    void insertText(const char* utf8);
    void deleteSelection();
    void deleteBack();
    void deleteForward();
    /* dir: -1 left, +1 right, -2 home, +2 end. extend grows the selection
     * from the anchor instead of collapsing it. */
    void moveCaret(int dir, bool extend);
    void selectAll();
    void setSelectionRange(int s, int e);
    int  selStart() const;
    int  selEnd() const;
    std::string selectedText() const;
    void selectWordAt(int localX);
    /* Place the caret (dropping any selection) at a click point given in the
     * control's local coordinates. */
    void setCaretFromPoint(int localX, int localY);

    /* ---- activation (engine-driven) ---- */
    void activate(int localX, int localY);
    void keyActivate();
    void setRangeFromX(int localX);
    void toggleDropdown();
    void moveOption(int dir);
    void chooseActiveOption();
    bool isDropdownOpen() const { return m_dropdownOpen; }
    int  activeOption() const { return m_activeOption; }

    /* ---- engine interaction surface (default-action layer) ---- */
    /* The control family, so the engine can branch its default actions. */
    EWebInputType inputType() const { return m_inputType; }
    /* Text caret placement from a click given in the control's BORDER-box local
     * coordinates (relative to get_placement()'s origin). These fold in the
     * text inset draw() uses, so the engine never touches padding/borders. */
    void placeCaretAt(int localX, int localY);
    /* Extend the selection to a border-box-local X, keeping the anchor (drag). */
    void dragSelectTo(int localX);
    /* Double-click: select the word under a border-box-local X. */
    void selectWordAtLocal(int localX);
    /* <select> popup contents + geometry, exposed for the engine-drawn dropdown
     * overlay (draw and hit-test share popupRowHeight() so they agree). */
    int  dropdownRowCount() { return optionCount(); }
    std::string dropdownRowText(int i);
    int  selectedOption() { return selectedOptionIndex(); }
    void setActiveOption(int i) { m_activeOption = i; }
    /* Move the committed selection by `dir` while the popup is closed (arrow
     * keys on a focused <select>). */
    void stepSelectedOption(int dir);
    /* Nudge a range slider's value by `dir` (arrow keys on a focused slider). */
    void stepRange(int dir);
    /* number inputs: the UA spinner arrows at the right edge. spinnerHit()
     * reports whether a border-box-local point lands on an arrow and which
     * way it steps (+1 up / -1 down); stepNumber() applies it clamped to
     * min/max and re-syncs the edit buffer. */
    bool isNumberInput();
    bool spinnerHit(int localX, int localY, int* dir);
    void stepNumber(int dir);
    /* Row height (logical px) of the dropdown overlay, from the control font. */
    int  popupRowHeight();
    /* Paint the expanded option list at (x,y) in the target surface, `width`
     * wide, `visibleRows` tall using `rowH`. Highlights activeOption() and
     * marks selectedOption(). */
    void drawDropdown(eweb_surface_t* s, int x, int y, int width, int rowH, int visibleRows);

private:
    /* Left inset from the border box to where the text/caret starts. */
    int textInsetLeft() const { return m_padding.left + m_borders.left + 4; }
    const eweb_port_t* m_port;
    EWebInputType m_inputType;
    bool m_focused;   /* holds keyboard focus (engine-driven) */

    /* Live edit buffer for text-like controls. m_hasEdit says the buffer is
     * authoritative (the user or script typed); otherwise the value attribute
     * (or <textarea> text) is the source. Offsets are byte offsets. */
    std::string m_editValue;
    bool m_hasEdit;
    int  m_caret;        /* caret byte offset */
    int  m_selAnchor;    /* selection anchor byte offset */
    bool m_selActive;    /* anchor != caret -> a real selection */
    int  m_textScrollX;  /* horizontal pan for over-long single-line text */
    int  m_textScrollY;  /* vertical pan for textarea (linear model: unused) */
    bool m_checked;      /* live checkbox/radio state */
    bool m_hasChecked;
    bool m_dropdownOpen; /* select expanded (overlay drawn by the engine) */
    int  m_activeOption; /* highlighted <option> index while expanded */

    /* Raw declaration blocks matched by widget part pseudo-element rules
     * (input[type=range]::-webkit-slider-thumb {...}), keyed by the lowercased
     * part name. parse_styles turns each into a childless html_tag so the
     * values resolve (var(), inherited custom properties) exactly like the
     * ::before/::after elements do, and draw() reads the computed look. */
    std::map<std::string, litehtml::style> m_widget_styles;
    litehtml::element::ptr m_part_thumb;
    litehtml::element::ptr m_part_track;

    static uint32_t make_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

    /* The text a text-like control shows: the edit buffer when present, else
     * the value attribute / <textarea> text. Never the placeholder. */
    std::string textValue() const;
    /* textValue() with password masking applied for drawing / measuring. */
    std::string displayText() const;
    bool isPassword() const;
    /* Byte offset of the codepoint boundary before / after `pos`. */
    int utf8Prev(int pos) const;
    int utf8Next(int pos) const;
    int textLen() const;
    /* Pixel width of displayText()[0:off] (mask-aware) for caret placement. */
    int offsetToX(int off);
    int xToOffset(int localX);
    /* Keep the caret inside the visible text box after edits / moves. */
    void ensureCaretVisible(int boxWidth);
    /* Push the live text into the value attribute so getAttribute and a no-JS
     * form submit agree with what is on screen. */
    void syncValueAttr();
    /* Draw text with a horizontal pan (single-line edit fields). */
    void draw_scrolled_text(eweb_surface_t* s, const litehtml::position& box,
                            const std::string& text, uint32_t color, int scrollX);

    /* <select> option helpers (document order, <option> children only). */
    int  optionCount();
    litehtml::element::ptr optionAt(int i);
    int  selectedOptionIndex();
    void selectOptionIndex(int i);

    void resolve_widget_parts();
    const litehtml::element::ptr& widget_part(bool thumb) const;
    bool part_color(bool thumb, litehtml::web_color& c) const;
    int  part_dim(bool thumb, const char* prop, int defval) const;
    float range_fraction();
    void extra_box_size(int& w, int& h) const;
    /* The page cascade declares padding/border on the axis, i.e. it replaces
     * the UA control chrome there; gates the +24/24 intrinsic emulation in
     * get_content_size (a styled button sizes from its label alone). */
    bool styled_horizontal() const;
    bool styled_vertical() const;
    /* Font line height: the single-line content height of a styled control
     * (the UA 24px face height only applies to unstyled ones). */
    int  content_line_height() const;
    /* True when a <button> carries real element children (an <img> thumbnail,
     * an inline <svg> glyph): such a control is a layout CONTAINER, not an
     * atomic replaced widget - the children need boxes and paint, and the
     * button's height must come from them (a history thumb sized only by
     * aspect-ratio/children would otherwise collapse to the UA 24px face and
     * clip its <img> to nothing). Text-only buttons stay on the widget path.
     * When true, is_replaced/render/draw/get_content_size delegate to
     * html_tag so flex/block layout and child paint run normally. */
    bool container_mode() const;

    /* The user-visible label: value/placeholder attribute for <input>, the
     * selected <option> text for <select>, the element text otherwise. */
    std::string label(bool* is_placeholder = 0);
    int label_width(const std::string& text);
    void draw_text_box(eweb_surface_t* s, const litehtml::position& box,
                       const std::string& text, uint32_t color, bool center);
    /* Paint the page-authored box of a styled control: CSS background via
     * init_background_paint plus the border box (width/colour/radius) the
     * html_tag::draw path would have drawn - draw() is overridden here, so
     * neither comes for free. Mirrors el_image::draw. */
    void draw_page_box(litehtml::uint_ptr hdc, int x, int y, const litehtml::position* clip);
    /* True when the page (cascade) declares any box styling for this control
     * - background/border/border-radius. When set, draw() defers the box look
     * to litehtml::draw_background (CSS colours, borders, opacity) instead of
     * painting the neutral UA white-fill/gray-outline widget chrome, so styled
     * fields (Google's transparent border:none input, Material buttons) don't
     * get a spurious extra rectangle. */
    bool page_styled_box() const;
};
