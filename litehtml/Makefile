PROJS_ROOT_DIR=../..
include $(PROJS_ROOT_DIR)/make.inc

BUILD_DIR = $(EWOKOS_SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

CFLAGS += -Iinclude -Iinclude/litehtml -Iinclude/gumbo -I $(TARGET_DIR)/include -fno-rtti -nostdinc++ -MMD -MP
CXXFLAGS += -Iinclude -Iinclude/litehtml -Iinclude/gumbo -I $(TARGET_DIR)/include -fno-rtti -nostdinc++ -MMD -MP

LITEHTML_OBJS = \
	$(ARCH)/src/el_break.o \
	$(ARCH)/src/el_para.o \
	$(ARCH)/src/el_text.o \
	$(ARCH)/src/el_script.o \
	$(ARCH)/src/el_style.o \
	$(ARCH)/src/el_table.o \
	$(ARCH)/src/media_query.o \
	$(ARCH)/src/document.o \
	$(ARCH)/src/element.o \
	$(ARCH)/src/el_image.o \
	$(ARCH)/src/el_base.o \
	$(ARCH)/src/el_anchor.o \
	$(ARCH)/src/html_tag.o \
	$(ARCH)/src/flex_layout.o \
	$(ARCH)/src/el_comment.o \
	$(ARCH)/src/iterators.o \
	$(ARCH)/src/background.o \
	$(ARCH)/src/el_font.o \
	$(ARCH)/src/html.o \
	$(ARCH)/src/css_selector.o \
	$(ARCH)/src/el_td.o \
	$(ARCH)/src/el_tr.o \
	$(ARCH)/src/context.o \
	$(ARCH)/src/el_body.o \
	$(ARCH)/src/table.o \
	$(ARCH)/src/style.o \
	$(ARCH)/src/box.o \
	$(ARCH)/src/el_div.o \
	$(ARCH)/src/el_before_after.o \
	$(ARCH)/src/el_link.o \
	$(ARCH)/src/css_length.o \
	$(ARCH)/src/el_space.o \
	$(ARCH)/src/el_cdata.o \
	$(ARCH)/src/el_title.o \
	$(ARCH)/src/web_color.o \
	$(ARCH)/src/stylesheet.o

# Gumbo parser source files
GUMBO_OBJS = \
	$(ARCH)/src/gumbo/vector.o \
	$(ARCH)/src/gumbo/util.o \
	$(ARCH)/src/gumbo/utf8.o \
	$(ARCH)/src/gumbo/tokenizer.o \
	$(ARCH)/src/gumbo/string_piece.o \
	$(ARCH)/src/gumbo/tag.o \
	$(ARCH)/src/gumbo/string_buffer.o \
	$(ARCH)/src/gumbo/parser.o \
	$(ARCH)/src/gumbo/error.o \
	$(ARCH)/src/gumbo/char_ref.o \
	$(ARCH)/src/gumbo/attribute.o

TASK_OBJS = $(LITEHTML_OBJS) $(GUMBO_OBJS)

TASK = $(TARGET_DIR)/lib/liblitehtml.a

$(TASK): $(TASK_OBJS)
	$(AR) crs $(TASK) $(TASK_OBJS) 
	cp -r include/* $(TARGET_DIR)/include

# make.rule's pattern rules do not track #include edges, so editing a widely
# included header (element.h / html_tag.h) left objects such as document.o
# stale: the archive then mixed the old and new element vtable / sizeof(html_tag)
# and el_image was under-allocated and mis-dispatched, silently breaking every
# image. -MMD -MP writes a .d beside each .o; including them makes each object
# depend on its headers so a header edit recompiles all affected objects.
-include $(LITEHTML_OBJS:.o=.d) $(GUMBO_OBJS:.o=.d)

clean:
	rm -f $(TASK_OBJS)
	rm -f $(LITEHTML_OBJS:.o=.d) $(GUMBO_OBJS:.o=.d)
	rm -f $(TASK)
