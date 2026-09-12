DIRS = libwebp litehtml jsnative ewebview

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
