ifeq ($(PORTING),)
export PORTING = sdl2
endif

PROJS_ROOT_DIR=../..
include $(PROJS_ROOT_DIR)/make.inc

BUILD_DIR = $(SDK_DIR)
TARGET_DIR = $(BUILD_DIR)

# sdlbrowser: a desktop SDL2 browser shell around libewebview.
#
# Links the ewebview core + SDL2 port (both inside libewebview.a) against
# litehtml / mario / webp / tinyhttpsc and the SDL2 companion libraries.
# The SDL2 port (port_sdl2.c) is already compiled into libewebview.a by
# ../../ewebview/Makefile when PORTING=sdl2, so we only need main.o here.
#
# pkg-config resolves SDL2 / SDL2_ttf / SDL2_image / SDL2_gfx include paths
# and link flags portably across Homebrew (macOS) and distro packages (Linux).

PKG_CONFIG := $(shell command -v pkg-config 2>/dev/null)

SDL2_CFLAGS :=
SDL2_LIBS   :=

ifneq ($(PKG_CONFIG),)
  # SDL2_image has no .pc on some installs; fall back to sdl2-config + manual -l.
  SDL2_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 SDL2_ttf SDL2_gfx 2>/dev/null)
  SDL2_LIBS   := $(shell $(PKG_CONFIG) --libs   sdl2 SDL2_ttf SDL2_gfx 2>/dev/null)
  # SDL2_image: try pkg-config first, else just -lSDL2_image.
  ifneq ($(shell $(PKG_CONFIG) --exists SDL2_image 2>/dev/null && echo yes),)
    SDL2_CFLAGS += $(shell $(PKG_CONFIG) --cflags SDL2_image)
    SDL2_LIBS   += $(shell $(PKG_CONFIG) --libs   SDL2_image)
  else
    SDL2_LIBS   += -lSDL2_image
  endif
else
  SDL2_CONFIG := $(shell command -v sdl2-config 2>/dev/null)
  ifneq ($(SDL2_CONFIG),)
    SDL2_CFLAGS := $(shell $(SDL2_CONFIG) --cflags)
    SDL2_LIBS   := $(shell $(SDL2_CONFIG) --libs) -lSDL2_ttf -lSDL2_image -lSDL2_gfx
  endif
endif

CFLAGS += $(SDL2_CFLAGS) -I $(TARGET_DIR)/include

LDFLAGS += -L $(TARGET_DIR)/lib

# Link order matters: ewebview references litehtml/mario/webp/tinyhttpsc
# symbols, and the SDL2 port inside libewebview.a references SDL2 companions.
# libewebview.a is C++14 internally, so link with $(CXX) to pull in libstdc++.
LIBS = -lewebview -llitehtml -lmario -lwebp -ltinyhttpsc \
       $(SDL2_LIBS) -lm -lpthread

OBJS = $(ARCH)/main.o $(ARCH)/compat.o

TARGET = $(TARGET_DIR)/bin/sdlbrowser

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)
	cp -r res $(dir $@)

$(ARCH)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(ARCH)
	rm -f $(TARGET)

.PHONY: all clean
