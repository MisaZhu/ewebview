ifeq ($(PORTING),)
export PORTING = sdl2
endif

DIRS = libtinyhttpsc libwebp jsnative litehtml

ARCH ?= aarch64
HW ?= virt
BUILD_ROOT = build_$(ARCH)/$(HW)

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
	@echo "Build artifacts:"
	@format_size() { awk -v bytes="$$1" 'BEGIN { if (bytes >= 1073741824) printf "%.2f GiB", bytes / 1073741824; else if (bytes >= 1048576) printf "%.2f MiB", bytes / 1048576; else if (bytes >= 1024) printf "%.2f KiB", bytes / 1024; else printf "%d B", bytes; }'; }; \
	for file in \
		"$(BUILD_ROOT)/lib/libewebview.a" \
		"$(BUILD_ROOT)/lib/liblitehtml.a" \
		"$(BUILD_ROOT)/lib/libmario_jsn.a" \
		"$(BUILD_ROOT)/lib/libwebp.a" \
		"$(BUILD_ROOT)/lib/libtinyhttpsc.a"; do \
		if [ -f "$$file" ]; then \
			size=$$(stat -f%z "$$file" 2>/dev/null || stat -c%s "$$file"); \
			echo "  $$file ($$(format_size "$$size"))"; \
		fi; \
	done
	@if [ "$(PORTING)" = "sdl2" ] && [ -f "$(BUILD_ROOT)/bin/sdlbrowser" ]; then \
		size=$$(stat -f%z "$(BUILD_ROOT)/bin/sdlbrowser" 2>/dev/null || stat -c%s "$(BUILD_ROOT)/bin/sdlbrowser"); \
		echo "  $(BUILD_ROOT)/bin/sdlbrowser ($$(awk -v bytes="$$size" 'BEGIN { if (bytes >= 1073741824) printf "%.2f GiB", bytes / 1073741824; else if (bytes >= 1048576) printf "%.2f MiB", bytes / 1048576; else if (bytes >= 1024) printf "%.2f KiB", bytes / 1024; else printf "%d B", bytes; }'))"; \
	fi

basic_libs:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done

clean:
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
