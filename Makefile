ifeq ($(PORTING),)
export PORTING = sdl2
endif

DIRS = libtinyhttpsc libwebp jsnative litehtml

# libsvg (vendored plutosvg+plutovg) backs the core's anti-aliased inline
# <svg> fill and the sdl2 port's SVG decode; it must build before ewebview
# (the core includes the installed <plutovg.h>). Under OS_TYPE=ewokos the
# system tree already ships the same library into the shared build dir,
# so building it here would overwrite the system archive and headers.
ifneq ($(OS_TYPE),ewokos)
DIRS += libsvg
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
