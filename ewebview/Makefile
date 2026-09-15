PROJS_ROOT_DIR=..
include $(PROJS_ROOT_DIR)/make.inc

BUILD_DIR = $(SDK_DIR)
TARGET_DIR = $(BUILD_DIR)

# The core (src/) is platform-independent C++14: every platform call goes
# through the eweb_port_t HAL tables declared in porting/include. The reference
# EwokOS port (porting/src/) is plain C99. Both land in ONE archive:
# port_ewokos.o is only pulled in when the embedder actually references
# eweb_port_ewokos(), so a foreign platform can link libewebview.a and supply
# its own port without dragging graph/font/tinyhttpsc symbols in.
#
# <litehtml.h> and <mario/*.h> come from $(TARGET_DIR)/include, installed by
# the sibling litehtml/mario Makefiles, so ewebview must build after them
# (see DIRS in ../Makefile). -I include comes from make.inc.
CXXFLAGS += -I porting/include -MMD -MP
CFLAGS   += -I porting/include -MMD -MP

# port_sdl2.c (the sdl2 PORT_OBJS below) includes <SDL.h> plus the companion
# SDL2_image/_ttf/_gfx headers, which Homebrew installs under include/SDL2.
# make.inc leaves CFLAGS_EXTRA empty, so pull those dirs (and -D_THREAD_SAFE)
# from sdl2-config here - portable across Apple Silicon /opt/homebrew, Intel
# /usr/local and Linux distros. Scoped to PORTING=sdl2 and guarded on
# sdl2-config existing, so other ports and hosts without SDL2 still build.
ifeq ($(PORTING),sdl2)
SDL2_CONFIG := $(shell command -v sdl2-config 2>/dev/null)
ifneq ($(SDL2_CONFIG),)
SDL2_CFLAGS := $(shell $(SDL2_CONFIG) --cflags)
CFLAGS   += $(SDL2_CFLAGS)
CXXFLAGS += $(SDL2_CFLAGS)
endif
endif

EWEBVIEW_OBJS = \
	$(ARCH)/src/ewebview.o \
	$(ARCH)/src/EWebContainer.o \
	$(ARCH)/src/EWebCanvas.o \
	$(ARCH)/src/EWebCanvasGlue.o \
	$(ARCH)/src/EWebJs.o \
	$(ARCH)/src/EWebCookies.o \
	$(ARCH)/src/eweb_el_input.o

PORT_OBJS = $(ARCH)/porting/$(PORTING)/port_$(PORTING).o

TASK_OBJS = $(EWEBVIEW_OBJS) $(PORT_OBJS)

TASK = $(TARGET_DIR)/lib/libewebview.a

all: $(TASK)

$(TASK): $(TASK_OBJS)
	@mkdir -p $(TARGET_DIR)/lib
	@mkdir -p $(TARGET_DIR)/include
	$(AR) crs $(TASK) $(TASK_OBJS)
	cp include/ewebview.h $(TARGET_DIR)/include/
	cp porting/include/ewebview_port.h $(TARGET_DIR)/include/

$(ARCH)/src/%.o: src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PORT_OBJS): $(ARCH)/porting/$(PORTING)/%.o: porting/$(PORTING)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Header edits must rebuild the objects that include them (see the note in
# ../litehtml/Makefile); -MMD -MP writes a .d beside each .o.
-include $(TASK_OBJS:.o=.d)

clean:
	rm -rf $(ARCH)
	rm -f $(TASK)
