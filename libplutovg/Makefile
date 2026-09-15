PROJS_ROOT_DIR=..
include $(PROJS_ROOT_DIR)/make.inc

BUILD_DIR = $(SDK_DIR)
TARGET_DIR = $(BUILD_DIR)

# Vendored from EwokOS system/gui/libs/libsvg: this is the rasteriser half
# (plutovg, FreeType smooth raster, premultiplied ARGB surfaces). The core
# calls it directly for inline <svg> anti-aliased fills (see
# EWebContainer::draw_svg); the sdl2 port's SVG parser (plutosvg + svg.c,
# under ewebview/porting/sdl2/libsvg) also produces plutovg paths and
# canvases, so it links against libplutovg.a as well.
#
# Under OS_TYPE=ewokos this directory is NOT built - the system tree already
# ships the same library into the shared build dir (see ../Makefile).
#
# Plain C99. The STBI_* defines trim the bundled stb_image to the formats
# SVG embedded images actually use, matching the EwokOS build.
CFLAGS += -I. -std=c99 \
	-DPLUTOVG_BUILD_STATIC -DPLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD \
	-DSTBI_NO_THREAD_LOCALS -DSTBI_NO_FAILURE_STRINGS -DSTBI_NO_JPEG \
	-DSTBI_NO_PNG -DSTBI_NO_BMP -DSTBI_NO_PSD -DSTBI_NO_TGA -DSTBI_NO_GIF \
	-DSTBI_NO_HDR -DSTBI_NO_PIC -DSTBI_NO_PNM -DSTBI_NO_STDIO

PLUTOVG_OBJS = \
	$(ARCH)/plutovg-blend.o \
	$(ARCH)/plutovg-canvas.o \
	$(ARCH)/plutovg-font.o \
	$(ARCH)/plutovg-matrix.o \
	$(ARCH)/plutovg-paint.o \
	$(ARCH)/plutovg-path.o \
	$(ARCH)/plutovg-rasterize.o \
	$(ARCH)/plutovg-surface.o \
	$(ARCH)/plutovg-ft-math.o \
	$(ARCH)/plutovg-ft-raster.o \
	$(ARCH)/plutovg-ft-stroker.o

LIBPLUTOVG = $(TARGET_DIR)/lib/libplutovg.a

all: $(LIBPLUTOVG)

$(LIBPLUTOVG): $(PLUTOVG_OBJS) Makefile
	@mkdir -p $(TARGET_DIR)/lib
	@mkdir -p $(TARGET_DIR)/include
	$(AR) rcs $(LIBPLUTOVG) $(PLUTOVG_OBJS)
	cp plutovg.h $(TARGET_DIR)/include/

$(ARCH)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The FT smooth raster relies on signed-overflow wrapping in its 32-bit
# fixed-point cell math on 32-bit targets (same guard EwokOS applies).
ifeq ($(ARCH),x86)
$(ARCH)/plutovg-ft-raster.o: CFLAGS += -O0 -fwrapv -fno-strict-overflow
endif

clean:
	rm -rf $(ARCH)
	rm -f $(LIBPLUTOVG)
