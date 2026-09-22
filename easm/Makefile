# easm - embed WebAssembly runtime for ewebview
#
# Core sources (decode/validate/interp/arith/runtime) are always compiled.
# JIT selection: jit_a64.c (real aarch64 JIT) on Apple Silicon (macOS aarch64),
# jit_stub.c (interpreter-only fallback) elsewhere.
#
# WASI backends: wasi.c references BOTH platform hooks (its EA_BACKEND switch
# names ea_wasi_plat_posix / ea_wasi_plat_ewok), so both translation units must
# always link regardless of OS_TYPE; wasi_ewok.c is portable C with a RAM-fs
# default, so it builds cleanly off-target too.

PROJS_ROOT_DIR=..
include $(PROJS_ROOT_DIR)/make.inc

BUILD_DIR = $(SDK_DIR)
TARGET_DIR = $(BUILD_DIR)
TARGET = $(TARGET_DIR)/lib/libeasm.a

# Exported headers
PUBLIC_HDR_DIR = $(TARGET_DIR)/include/easm
PUBLIC_HDRS = easm.h wasi.h opcodes.h

CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
          -Wno-unused-variable -Wno-tautological-compare -Wno-format \
          -Wno-sign-compare -I src -MMD -MP $(CFLAGS_EXTRA)

# Core sources (always compiled)
CORE_SRC = src/decode.c src/validate.c src/runtime.c src/interp.c \
           src/arith.c src/simd_exec.c src/util.c

# WASI backends. wasi.c references BOTH platform hooks (its EA_BACKEND switch
# names ea_wasi_plat_posix / ea_wasi_plat_ewok), so both translation units must
# always link regardless of OS_TYPE; wasi_ewok.c is portable C with a RAM-fs
# default, so it builds cleanly off-target too.
WASI_SRC = src/wasi.c src/wasi_posix.c src/wasi_ewok.c

# JIT backend
UNAME_S := $(shell uname -s)
JIT_SRC = src/jit_stub.c   # fallback
ifneq ($(OS_TYPE),ewokos)
ifneq ($(ARCH),)
ifeq ($(ARCH),aarch64)
ifneq ($(UNAME_S),Linux)   # skip JIT on Linux even if aarch64 (no MAP_JIT guarantee)
JIT_SRC = src/jit_a64.c
endif
endif
endif
endif

SRC = $(CORE_SRC) $(WASI_SRC) $(JIT_SRC)
OBJ = $(SRC:src/%.c=build/%.o)

all: $(TARGET)

build:
	mkdir -p build

build/%.o: src/%.c src/easm.h src/opcodes.h src/a64_emit.h | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	@mkdir -p $(TARGET_DIR)/lib
	@mkdir -p $(PUBLIC_HDR_DIR)
	$(AR) crs $(TARGET) $(OBJ)
	@for h in $(PUBLIC_HDRS); do \
		cp src/$$h $(PUBLIC_HDR_DIR)/$$h; \
	done

-include $(OBJ:.o=.d)

clean:
	rm -rf build
	rm -f $(TARGET)
	rm -rf $(PUBLIC_HDR_DIR)