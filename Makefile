ifeq ($(PORTING),)
export PORTING = sdl2
endif

DIRS = libtinyhttpsc libwebp jsnative litehtml

# libplutovg (rasteriser half of the vendored EwokOS libsvg) backs the
# core's anti-aliased inline <svg> fill in EWebContainer::draw_svg, so it
# must build before ewebview. The parser half (plutosvg + svg.c wrapper)
# lives under ewebview/porting/sdl2/libsvg because only port_sdl2.c uses
# it for <img src="*.svg"> decode; it must also precede ewebview since
# port_sdl2.c is compiled into libewebview.a and includes <svg.h>. Under
# OS_TYPE=ewokos the system tree already ships both archives into the
# shared build dir, so we skip building them here to avoid overwriting.
ifneq ($(OS_TYPE),ewokos)
DIRS += libplutovg
ifeq ($(PORTING),sdl2)
DIRS += ewebview/porting/sdl2/libsvg
endif
endif

DIRS += ewebview

ifeq ($(PORTING),sdl2)
DIRS += bin/sdlbrowser
endif

all: basic_libs
	@echo "all done."

basic_libs:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
