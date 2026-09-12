PROJS_ROOT_DIR=..
include $(PROJS_ROOT_DIR)/make.inc

BUILD_DIR = $(EWOKOS_SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

# The mario VM sources live in the mario_js submodule at the browser repo
# root; only the browser-side natives (DOM/event/web/canvas bridges) live
# here. Everything is archived into one libmario.a so xBrowser links a
# single -lmario.
MARIO_VM = ../mario_js

NATIVE_PATH = $(MARIO_VM)/lang/js/native
NATIVES_PATH_BUILTIN = $(NATIVE_PATH)/builtin
NATIVES_PATH = $(NATIVE_PATH)/natives

CFLAGS += -I $(NATIVES_PATH_BUILTIN) \
	-I $(NATIVES_PATH) \
	-I $(MARIO_VM)/mario \
	-I natives \
	-MMD -MP

# VM core + JS compiler (see mario_js/Makefile and lang/js/lang.mk for the
# canonical object list; bin/ holds the standalone interpreter and is not
# part of the library).
VM_OBJS = \
	$(ARCH)/vm/mario/mario.o \
	$(ARCH)/vm/mario/lex/mario_lex.o \
	$(ARCH)/vm/mario/bcdump/bcdump.o \
	$(ARCH)/vm/lang/js/compiler.o

NATIVE_OBJS = \
	$(ARCH)/vm/lang/js/native/natives_all.o \
	\
	$(ARCH)/vm/lang/js/native/builtin/natives_builtin.o \
	$(ARCH)/vm/lang/js/native/builtin/Object/native_Object.o \
	$(ARCH)/vm/lang/js/native/builtin/Error/native_Error.o \
	$(ARCH)/vm/lang/js/native/builtin/Number/native_Number.o \
	$(ARCH)/vm/lang/js/native/builtin/BigInt/native_BigInt.o \
	$(ARCH)/vm/lang/js/native/builtin/ArrayBuffer/native_ArrayBuffer.o \
	$(ARCH)/vm/lang/js/native/builtin/DataView/native_DataView.o \
	$(ARCH)/vm/lang/js/native/builtin/TypedArray/native_TypedArray.o \
	$(ARCH)/vm/lang/js/native/builtin/Console/native_Console.o \
	$(ARCH)/vm/lang/js/native/builtin/String/native_String.o \
	$(ARCH)/vm/lang/js/native/builtin/Array/native_Array.o \
	$(ARCH)/vm/lang/js/native/builtin/Promise/native_Promise.o \
	$(ARCH)/vm/lang/js/native/builtin/Map/native_Map.o \
	$(ARCH)/vm/lang/js/native/builtin/Set/native_Set.o \
	$(ARCH)/vm/lang/js/native/builtin/Symbol/native_Symbol.o \
	$(ARCH)/vm/lang/js/native/builtin/Proxy/native_Proxy.o \
	$(ARCH)/vm/lang/js/native/builtin/Reflect/native_Reflect.o \
	$(ARCH)/vm/lang/js/native/builtin/WeakRef/native_WeakRef.o \
	$(ARCH)/vm/lang/js/native/builtin/FinalizationRegistry/native_FinalizationRegistry.o \
	$(ARCH)/vm/lang/js/native/builtin/SharedArrayBuffer/native_SharedArrayBuffer.o \
	$(ARCH)/vm/lang/js/native/builtin/Atomics/native_Atomics.o \
	$(ARCH)/vm/lang/js/native/builtin/RegExp/native_RegExp.o \
	\
	$(ARCH)/vm/lang/js/native/natives/natives.o \
	$(ARCH)/vm/lang/js/native/natives/JSON/JSON.o \
	$(ARCH)/vm/lang/js/native/natives/JSON/native_JSON.o \
	$(ARCH)/vm/lang/js/native/natives/Date/native_Date.o \
	$(ARCH)/vm/lang/js/native/natives/Math/native_Math.o

# Browser natives: DOM/event/web/canvas bridges consumed by WidgetWebview.
BROWSER_NATIVE_OBJS = \
	$(ARCH)/natives/js_dom.o \
	$(ARCH)/natives/js_event.o \
	$(ARCH)/natives/js_web.o \
	$(ARCH)/natives/js_canvas.o

TASK_OBJS = $(VM_OBJS) $(NATIVE_OBJS) $(BROWSER_NATIVE_OBJS)

TASK = $(TARGET_DIR)/lib/libmario.a

# Public headers, installed as <mario/*.h>. js_natives_priv.h stays private.
MARIO_HEADERS = $(MARIO_VM)/mario/mario.h \
	natives/js_dom.h \
	natives/js_event.h \
	natives/js_web.h \
	natives/js_canvas.h

all: $(TASK)

$(TASK): $(TASK_OBJS)
	@mkdir -p $(TARGET_DIR)/lib
	@mkdir -p $(TARGET_DIR)/include/mario
	$(AR) crs $(TASK) $(TASK_OBJS)
	cp $(MARIO_HEADERS) $(TARGET_DIR)/include/mario/

$(ARCH)/vm/%.o: $(MARIO_VM)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(TASK_OBJS:.o=.d)

clean:
	rm -rf $(ARCH)
	rm -f $(TASK)
