PROJS_ROOT_DIR=../..
include $(PROJS_ROOT_DIR)/make.inc

BUILD_DIR = $(SDK_DIR)
TARGET_DIR = $(BUILD_DIR)

# hlbrowser: a headless (non-SDL) ewebview driver.
#
# Drives the engine directly through the headless port (port_headless.c) with
# no window system anywhere in the process. It exists primarily so the engine
# can run under AddressSanitizer on the host: the SDL2 shell cannot, because
# the libSDL2 dyld load deadlocks against the ASan runtime. Static linking
# only pulls port_sdl2.o out of libewebview.a when eweb_port_sdl2 is
# referenced, and nothing here references it, so no SDL libraries are needed.

CFLAGS += -I $(TARGET_DIR)/include -I $(PROJS_ROOT_DIR)/libtinyhttpsc/include

LDFLAGS += -L $(TARGET_DIR)/lib

ifeq ($(shell uname -s),Darwin)
LDFLAGS += -Wl,-dead_strip
endif

# Same engine archive set as sdlbrowser, minus the SDL2 companions. Link order
# matters: ewebview references litehtml/mario/webp/tinyhttpsc symbols, svg
# references plutovg. libewebview.a is C++14 internally, so link with $(CXX).
LIBS = -lewebview -lsvg -lplutovg -llitehtml -lmario_jsn -leasm -lwebp -ltinyhttpsc \
       -lm -lpthread -lz

# macOS appearance detection (AppleInterfaceStyle) uses CoreFoundation.
ifeq ($(shell uname -s),Darwin)
LIBS += -framework CoreFoundation
endif

OBJS = $(ARCH)/main_headless.o $(ARCH)/port_headless.o $(ARCH)/compat.o

PROJ_LIBS = $(TARGET_DIR)/lib/libewebview.a \
            $(TARGET_DIR)/lib/libsvg.a \
            $(TARGET_DIR)/lib/libplutovg.a \
            $(TARGET_DIR)/lib/liblitehtml.a \
            $(TARGET_DIR)/lib/libmario_jsn.a \
            $(TARGET_DIR)/lib/libeasm.a \
            $(TARGET_DIR)/lib/libwebp.a \
            $(TARGET_DIR)/lib/libtinyhttpsc.a

TARGET = $(TARGET_DIR)/bin/hlbrowser

all: $(TARGET)

$(TARGET): $(OBJS) $(PROJ_LIBS)
	@mkdir -p $(dir $@)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

$(ARCH)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# EwokOS compat stubs (gumbo setjmp, proc_usleep, ewok_ptr_in_heap): shared
# verbatim with sdlbrowser; the file has no SDL dependency.
$(ARCH)/compat.o: ../sdlbrowser/compat.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(ARCH)
	rm -f $(TARGET)

.PHONY: all clean
