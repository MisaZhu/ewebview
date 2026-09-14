PROJS_ROOT_DIR=..
include $(PROJS_ROOT_DIR)/make.inc

BUILD_DIR = $(SDK_DIR)
TARGET_DIR = $(BUILD_DIR)

# Vendored from EwokOS system/gui/libs/libsvg: plutosvg (SVG document
# parser) on top of plutovg (anti-aliased vector rasteriser, FreeType
# smooth raster). The core uses plutovg directly for inline <svg> fills
# (EWebContainer::draw_svg); the sdl2 port uses svg_load_from_memory()
# for <img src="*.svg"> decode. Under OS_TYPE=ewokos this directory is
# NOT built - the system tree already ships the same library and the
# embedder links it there (see DIRS in ../Makefile).
#
# plutovg/plutosvg are plain C99; the STBI_* defines trim the bundled
# stb_image to the formats SVG embedded images actually use, matching
# the EwokOS build of this library.
CFLAGS += -I. -Iplutovg -std=c99 \
	-DPLUTOVG_BUILD_STATIC -DPLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD \
	-DSTBI_NO_THREAD_LOCALS -DSTBI_NO_FAILURE_STRINGS -DSTBI_NO_JPEG \
	-DSTBI_NO_PNG -DSTBI_NO_BMP -DSTBI_NO_PSD -DSTBI_NO_TGA -DSTBI_NO_GIF \
	-DSTBI_NO_HDR -DSTBI_NO_PIC -DSTBI_NO_PNM -DSTBI_NO_STDIO

PLUTOVG_OBJS = \
	$(ARCH)/plutovg/plutovg-blend.o \
	$(ARCH)/plutovg/plutovg-canvas.o \
	$(ARCH)/plutovg/plutovg-font.o \
	$(ARCH)/plutovg/plutovg-matrix.o \
	$(ARCH)/plutovg/plutovg-paint.o \
	$(ARCH)/plutovg/plutovg-path.o \
	$(ARCH)/plutovg/plutovg-rasterize.o \
	$(ARCH)/plutovg/plutovg-surface.o \
	$(ARCH)/plutovg/plutovg-ft-math.o \
	$(ARCH)/plutovg/plutovg-ft-raster.o \
	$(ARCH)/plutovg/plutovg-ft-stroker.o

LIBSVG_OBJS = $(ARCH)/svg.o $(ARCH)/plutosvg.o $(PLUTOVG_OBJS)

LIBSVG = $(TARGET_DIR)/lib/libsvg.a

all: $(LIBSVG)

$(LIBSVG): $(LIBSVG_OBJS) Makefile
	@mkdir -p $(TARGET_DIR)/lib
	@mkdir -p $(TARGET_DIR)/include
	$(AR) rcs $(LIBSVG) $(LIBSVG_OBJS)
	cp svg.h plutosvg.h plutovg/plutovg.h $(TARGET_DIR)/include/

$(ARCH)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The FT smooth raster relies on signed-overflow wrapping in its 32-bit
# fixed-point cell math on 32-bit targets (same guard EwokOS applies).
ifeq ($(ARCH),x86)
$(ARCH)/plutovg/plutovg-ft-raster.o: CFLAGS += -O0 -fwrapv -fno-strict-overflow
endif

clean:
	rm -rf $(ARCH)
	rm -f $(LIBSVG)
