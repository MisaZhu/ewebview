ifeq ($(ewokos),)
# Default the EwokOS root to THIS repo, derived from make.inc's own location
# (projects/make.inc -> repo root) so it is always an absolute path.  The old
# "~/work/ewokos" default was handed to gcc verbatim - neither make nor gcc
# expands '~' - which broke every -I$(SDK_DIR)/... (e.g. FLTK's <X11/Xlib.h>
# under include/nx11) unless the caller exported ewokos= or passed
# ewokos=/abs/path explicitly.  ':=' (not '=') is required: it captures
# $(lastword $(MAKEFILE_LIST)) == make.inc at parse time, before the variable
# is re-expanded later from the including Makefile.
export ewokos := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
endif

export ewebview_root := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# All arch output dirs, so that clean removes objects of every arch,
# not only the one selected by $(ARCH).
ALL_ARCH_DIRS = aarch64 arm x86 riscv


ifeq ($(ARCH),)
export ARCH=aarch64
endif

ifeq ($(HW),)
export HW=virt
endif

ifeq ($(OS_TYPE),ewokos)
EWOKOS_ROOT_DIR=$(ewokos)
EWOKOS_SYS_DIR=$(EWOKOS_ROOT_DIR)/system
SDK_DIR= $(EWOKOS_SYS_DIR)/build_$(ARCH)/$(HW)
include $(EWOKOS_SYS_DIR)/platform/$(ARCH)/make.rule
else
SDK_DIR= $(ewebview_root)/build_$(ARCH)/$(HW)
include $(ewebview_root)/host.rule
endif


LDFLAGS = -L $(SDK_DIR)/lib $(LDFLAGS_EXTRA)
# LITEHTML_LIFETIME_DEBUG is NOT a verbose-logging switch (that is XBROWSER_DEBUG):
# it compiles in the html_tag lifetime registry and the authoritative liveness
# gates in apply_stylesheet/apply_stylesheet_own/style_detached_subtree_walk.
# Without it those gates fall back to the element magic, which a freed node whose
# block the mario VM recycled reads back as "live" - so the style walk matches a
# dangling html_tag and aborts in selector_key_lower's std::string copy (w3.org
# freezes the whole window). Keep it on in EVERY build, including production.
CFLAGS += -I include -I $(SDK_DIR)/include -DLITEHTML_LIFETIME_DEBUG $(CFLAGS_EXTRA)
CXXFLAGS += -I include -I $(SDK_DIR)/include -DLITEHTML_LIFETIME_DEBUG $(CXXFLAGS_EXTRA)
