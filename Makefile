ifeq ($(PORTING),)
export PORTING = sdl2
endif

DIRS = libtinyhttpsc libwebp jsnative litehtml ewebview 

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
