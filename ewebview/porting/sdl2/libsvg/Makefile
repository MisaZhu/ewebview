PROJS_ROOT_DIR=../../../..
include $(PROJS_ROOT_DIR)/make.inc

BUILD_DIR = $(SDK_DIR)
TARGET_DIR = $(BUILD_DIR)

# SVG parser half of the vendored EwokOS libsvg, kept inside the sdl2 port
# tree because it is the only consumer: port_sdl2.c decodes <img src="*.svg">
# by parsing the document with plutosvg and re-rasterising it through the
# svg_doc_t/svg_image_t wrapper at blit-time destination size (so scaled SVG
# stays smooth). The ewokos port goes through graph_image_new_from_data on
# the host-provided libsvg and does not touch this archive.
#
# Depends on libplutovg.a (top-level) for the actual rasteriser; plutosvg.h
# picks up <plutovg.h> from $(TARGET_DIR)/include via make.inc's -I flag, so
# libplutovg must build first (see DIRS in the top-level Makefile).
#
# Under OS_TYPE=ewokos this directory is NOT built - the system tree already
# ships an equivalent libsvg.a into the shared build dir.
CFLAGS += -I. -std=c99 -DPLUTOSVG_BUILD_STATIC

LIBSVG_OBJS = \
	$(ARCH)/plutosvg.o \
	$(ARCH)/svg.o

LIBSVG = $(TARGET_DIR)/lib/libsvg.a

all: $(LIBSVG)

$(LIBSVG): $(LIBSVG_OBJS) Makefile
	@mkdir -p $(TARGET_DIR)/lib
	@mkdir -p $(TARGET_DIR)/include
	$(AR) rcs $(LIBSVG) $(LIBSVG_OBJS)
	cp svg.h plutosvg.h $(TARGET_DIR)/include/

$(ARCH)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(ARCH)
	rm -f $(LIBSVG)
