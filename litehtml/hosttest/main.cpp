/* Host-side litehtml harness: loads a real page + its stylesheet on macOS and
 * dumps computed styles / geometry so render gaps can be diagnosed without
 * QEMU. Not part of the cross build. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include "litehtml/html.h"
#include "litehtml/document.h"
#include "litehtml/context.h"
#include "litehtml/html_tag.h"
#include <ewoksys/kernel_tic.h>

static std::string read_file(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static int utf8_len(const char* s)
{
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n;
}

struct test_container : public litehtml::document_container
{
    int vw = 1200, vh = 900;
    int marker_calls = 0;
    int bg_calls = 0;

    litehtml::uint_ptr create_font(const litehtml::tchar_t* faceName, int size, int weight,
        litehtml::font_style italic, unsigned int decoration, litehtml::font_metrics* fm) override
    {
        (void)faceName; (void)weight; (void)italic; (void)decoration;
        if (fm) {
            fm->height = size;
            fm->ascent = (int)(size * 0.8);
            fm->descent = (int)(size * 0.2);
            fm->x_height = (int)(size * 0.5);
        }
        return (litehtml::uint_ptr)(intptr_t)(size ? size : 16);
    }
    void delete_font(litehtml::uint_ptr hFont) override { (void)hFont; }
    int text_width(const litehtml::tchar_t* text, litehtml::uint_ptr hFont) override
    {
        return (int)(utf8_len(text) * (int)(intptr_t)hFont * 0.55);
    }
    void draw_text(litehtml::uint_ptr hdc, const litehtml::tchar_t* text, litehtml::uint_ptr hFont,
        litehtml::web_color color, const litehtml::position& pos) override
    {
        (void)hdc; (void)text; (void)hFont; (void)color; (void)pos;
    }
    int pt_to_px(int pt) override { return (int)(pt * 96 / 72); }
    int get_default_font_size() const override { return 16; }
    const litehtml::tchar_t* get_default_font_name() const override { return "sans"; }
    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override
    {
        (void)hdc;
        if (marker_calls < 40)
            printf("MARKER type=%d pos=(%d,%d %dx%d)\n", (int)marker.marker_type,
                marker.pos.x, marker.pos.y, marker.pos.width, marker.pos.height);
        marker_calls++;
    }
    void load_image(const litehtml::tchar_t* src, const litehtml::tchar_t* baseurl, bool redraw_on_ready) override
    { (void)src; (void)baseurl; (void)redraw_on_ready; }
    void get_image_size(const litehtml::tchar_t* src, const litehtml::tchar_t* baseurl, litehtml::size& sz) override
    { (void)src; (void)baseurl; sz.width = 0; sz.height = 0; }
    void draw_background(litehtml::uint_ptr hdc, const litehtml::background_paint& bg) override
    {
        (void)hdc;
        if (bg_calls < 80 && bg.color.alpha)
            printf("BG #%02x%02x%02x%02x box=(%d,%d %dx%d)\n", bg.color.alpha, bg.color.red,
                bg.color.green, bg.color.blue, bg.border_box.x, bg.border_box.y,
                bg.border_box.width, bg.border_box.height);
        bg_calls++;
    }
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders, const litehtml::position& draw_pos, bool root) override
    { (void)hdc; (void)borders; (void)draw_pos; (void)root; }
    void set_caption(const litehtml::tchar_t* caption) override { (void)caption; }
    void set_base_url(const litehtml::tchar_t* base_url) override { (void)base_url; }
    void link(litehtml::document* doc, const litehtml::element::ptr& el) override { (void)doc; (void)el; }
    void on_anchor_click(const litehtml::tchar_t* url, const litehtml::element::ptr& el) override { (void)url; (void)el; }
    void set_cursor(const litehtml::tchar_t* cursor) override { (void)cursor; }
    void transform_text(litehtml::tstring& text, litehtml::text_transform tt) override { (void)text; (void)tt; }
    void import_css(litehtml::tstring& text, const litehtml::tstring& url, litehtml::tstring& baseurl) override
    {
        (void)baseurl;
        /* Fixture paths are relative to the repo root; run lhtest from there.
         * print.css is media=print: the device never applies it, so neither
         * do we. */
        if (url.find("core.css") != std::string::npos)
            text = read_file("build/lhdump/core.css");
        else if (url.find("advanced.css") != std::string::npos)
            text = read_file("build/lhdump/adv.css");
        else if (url.find("print.css") != std::string::npos)
            text = "";
    }
    void set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius, bool valid_x, bool valid_y) override
    { (void)pos; (void)bdr_radius; (void)valid_x; (void)valid_y; }
    void del_clip() override {}
    void get_client_rect(litehtml::position& client) const override
    { client.x = 0; client.y = 0; client.width = vw; client.height = vh; }
    litehtml::element* create_element(const litehtml::tchar_t* tag_name,
        const litehtml::string_map& attributes, litehtml::document* doc) override
    { (void)tag_name; (void)attributes; (void)doc; return 0; }
    void get_media_features(litehtml::media_features& media) const override
    {
        media.width = vw; media.height = vh;
        media.device_width = vw; media.device_height = vh;
        media.color = 8; media.monochrome = 0; media.color_index = 0; media.resolution = 96;
    }
    void get_language(litehtml::tstring& language, litehtml::tstring& culture) const override
    { language = "en"; culture = "us"; }
};

static const char* disp_name(litehtml::style_display d)
{
    switch (d) {
    case litehtml::display_block: return "block";
    case litehtml::display_inline: return "inline";
    case litehtml::display_inline_block: return "inline-block";
    case litehtml::display_inline_table: return "inline-table";
    case litehtml::display_list_item: return "list-item";
    case litehtml::display_table: return "table";
    case litehtml::display_table_caption: return "table-caption";
    case litehtml::display_table_cell: return "table-cell";
    case litehtml::display_table_column: return "table-column";
    case litehtml::display_table_column_group: return "table-column-group";
    case litehtml::display_table_footer_group: return "table-footer-group";
    case litehtml::display_table_header_group: return "table-header-group";
    case litehtml::display_table_row: return "table-row";
    case litehtml::display_table_row_group: return "table-row-group";
    case litehtml::display_flex: return "flex";
    case litehtml::display_inline_flex: return "inline-flex";
    case litehtml::display_none: return "none";
    default: return "?";
    }
}

static bool want_node(litehtml::element* el)
{
    const char* tag = el->get_tagName();
    const char* cls = el->get_attr("class", "");
    if (!strcmp(tag, "ul") || !strcmp(tag, "ol") || !strcmp(tag, "li") || !strcmp(tag, "nav") ||
        !strcmp(tag, "header") || !strcmp(tag, "input") || !strcmp(tag, "button") ||
        !strcmp(tag, "html") || !strcmp(tag, "body") || !strcmp(tag, "div") || !strcmp(tag, "main") ||
        !strcmp(tag, "section") || !strcmp(tag, "footer") || !strcmp(tag, "h1") || !strcmp(tag, "h2") ||
        !strcmp(tag, "h3") || !strcmp(tag, "p") || !strcmp(tag, "figure"))
        return true;
    if (strstr(cls, "visuallyhidden") || strstr(cls, "skip-link") || strstr(cls, "banner") ||
        strstr(cls, "global-nav") || strstr(cls, "clean-list"))
        return true;
    return false;
}

static void dump(litehtml::element::ptr el, int depth, int max_depth)
{
    if (depth > max_depth) return;
    if (want_node(el)) {
        litehtml::position& pos = el->get_position();
        const char* tag = el->get_tagName();
        const char* cls = el->get_attr("class", "");
        const char* lst = el->get_style_property("list-style-type", true, "-");
        const char* posn = el->get_style_property("position", false, "-");
        const char* w = el->get_style_property("width", false, "-");
        const char* h = el->get_style_property("height", false, "-");
        const char* ovf = el->get_style_property("overflow", false, "-");
        printf("%*s<%s%s%s> disp=%s pos=%s lst=%s ovf=%s w=%s h=%s box=(%d,%d %dx%d)\n",
            depth * 2, "", tag, cls[0] ? " class=" : "", cls,
            disp_name(el->get_display()), posn, lst, ovf, w, h,
            pos.x, pos.y, pos.width, pos.height);
    }
    for (size_t i = 0; i < el->get_children_count(); i++)
        dump(el->get_child((int)i), depth + 1, max_depth);
}

static int g_wide_shown = 0;

static int g_max_right = 0;
static void scan_right(litehtml::element::ptr el, int ax, int ay)
{
    litehtml::position& pos = el->get_position();
    int absx = ax + pos.x;
    int right = absx + pos.width;
    if (right > g_max_right) {
        g_max_right = right;
        printf("NEWMAX right=%d <%s class=%s> disp=%s box=(%d,%d %dx%d) abs=(%d,%d)\n",
            right, el->get_tagName(), el->get_attr("class", ""), disp_name(el->get_display()),
            pos.x, pos.y, pos.width, pos.height, absx, ay + pos.y);
        for (litehtml::element* a = el->parent(); a; a = a->parent()) {
            litehtml::position& ap = a->get_position();
            printf("   ^ <%s class=%s> disp=%s box=(%d,%d %dx%d)\n",
                a->get_tagName(), a->get_attr("class", ""), disp_name(a->get_display()),
                ap.x, ap.y, ap.width, ap.height);
        }
    }
    for (size_t i = 0; i < el->get_children_count(); i++)
        scan_right(el->get_child((int)i), absx, ay + pos.y);
}
static void dump_wide(litehtml::element::ptr el, int depth, int vw, int ax, int ay)
{
    litehtml::position& pos = el->get_position();
    int absx = ax + pos.x, absy = ay + pos.y;
    if (pos.width > vw + 4 || absx + pos.width > vw + 4 || absx < -4) {
        const char* tag = el->get_tagName();
        const char* cls = el->get_attr("class", "");
        printf("%*sWIDE <%s class=%s> disp=%s box=(%d,%d %dx%d) abs=(%d,%d) w=%s minw=%s maxw=%s ws=%s\n",
            depth * 2, "", tag, cls, disp_name(el->get_display()),
            pos.x, pos.y, pos.width, pos.height, absx, absy,
            el->get_style_property("width", false, "-"),
            el->get_style_property("min-width", false, "-"),
            el->get_style_property("max-width", false, "-"),
            el->get_style_property("white-space", true, "-"));
        if (g_wide_shown < 6) {
            g_wide_shown++;
            for (litehtml::element* a = el->parent(); a; a = a->parent()) {
                litehtml::position& ap = a->get_position();
                printf("%*s  ^ <%s class=%s id=%s> disp=%s box=(%d,%d %dx%d) flexdir=%s justify=%s align=%s gap=%s\n",
                    depth * 2, "", a->get_tagName(), a->get_attr("class", ""), a->get_attr("id", ""),
                    disp_name(a->get_display()), ap.x, ap.y, ap.width, ap.height,
                    a->get_style_property("flex-direction", false, "-"),
                    a->get_style_property("justify-content", false, "-"),
                    a->get_style_property("align-items", false, "-"),
                    a->get_style_property("gap", false, "-"));
            }
        }
    }
    for (size_t i = 0; i < el->get_children_count(); i++)
        dump_wide(el->get_child((int)i), depth + 1, vw, absx, absy);
}

int main(int argc, char** argv)
{
    const char* html_path = argc > 1 ? argv[1] : "/tmp/w3.html";
    const char* defcss_path = argc > 2 ? argv[2] : "../../../apps/xBrowser/res/html/default.css";
    const char* css_path = argc > 3 ? argv[3] : "/tmp/w3core.css";
    int width = argc > 4 ? atoi(argv[4]) : 1200;
    int max_depth = argc > 5 ? atoi(argv[5]) : 100;
    bool chunked = (argc > 6 && !strcmp(argv[6], "chunk"));

    std::string html = read_file(html_path);
    std::string defcss = read_file(defcss_path);
    std::string css = read_file(css_path);
    if (html.empty()) { fprintf(stderr, "no html\n"); return 1; }

    test_container cont;
    cont.vw = width;
    litehtml::context ctx;
    litehtml::document::ptr doc = litehtml::document::createFromUTF8(html.c_str(), &cont, &ctx, 0);
    if (!doc) { fprintf(stderr, "create failed\n"); return 1; }
    if (!defcss.empty())
        ctx.load_master_stylesheet(defcss.c_str());
    printf("doc created, applying master css (%zu bytes)\n", css.size());
    ctx.load_master_stylesheet(css.c_str());
    if (chunked) {
        int rounds = 0;
        while (!doc->update_master_styles_step(kernel_tic_ms(0) + 2) && rounds < 100000)
            rounds++;
        printf("chunked update done in %d rounds, phase=%d\n", rounds, doc->style_step_phase());
    } else {
        doc->update_master_styles();
    }
    doc->render(width);
    printf("rendered %dx%d\n", doc->width(), doc->height());
    dump(doc->root(), 0, max_depth);
    printf("--- wide elements (viewport %d) ---\n", width);
    scan_right(doc->root(), 0, 0);
    dump_wide(doc->root(), 0, width, 0, 0);
    printf("--- draw pass ---\n");
    doc->draw((litehtml::uint_ptr)1, 0, 0, 0);
    printf("marker calls: %d\n", cont.marker_calls);
    return 0;
}
