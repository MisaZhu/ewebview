export ewebview_root := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

ifeq ($(ARCH),)
export ARCH=aarch64
endif

ifeq ($(HW),)
export HW=virt
endif

SDK_DIR= $(ewebview_root)/build_$(ARCH)/$(HW)

include $(ewebview_root)/sdl2.rule

LDFLAGS = -L $(SDK_DIR)/lib

# SDL2 (plus companion SDL2_image/_ttf/_gfx) headers for the native sdl2 port:
# port_sdl2.c includes <SDL.h>, <SDL_image.h>, <SDL_ttf.h>,
# <SDL2_gfxPrimitives.h> and <SDL2_rotozoom.h>. Homebrew installs all of them
# into the same include/SDL2 dir, so sdl2-config's --cflags (portable across
# Apple Silicon /opt/homebrew, Intel /usr/local and Linux distros) covers them
# and adds -D_THREAD_SAFE. Guarded so a host without sdl2-config still parses.
SDL2_CONFIG := $(shell command -v sdl2-config 2>/dev/null)
ifneq ($(SDL2_CONFIG),)
CFLAGS_EXTRA   += $(shell $(SDL2_CONFIG) --cflags)
CXXFLAGS_EXTRA += $(shell $(SDL2_CONFIG) --cflags)
endif

CFLAGS += -I include -I $(SDK_DIR)/include $(CFLAGS_EXTRA)
CXXFLAGS += -I include -I $(SDK_DIR)/include $(CXXFLAGS_EXTRA)
