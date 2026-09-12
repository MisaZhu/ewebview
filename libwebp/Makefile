PROJS_ROOT_DIR=..
include $(PROJS_ROOT_DIR)/$(PORTING).inc

BUILD_DIR = $(SDK_DIR)
TARGET_DIR = $(BUILD_DIR)

# src/ holds the private headers (webp_internal.h, webp_vp8*.h); include/
# is what gets installed for the consumers of the library.
CFLAGS += -Isrc

LIB_WEBP_OBJS = $(ARCH)/src/webp.o \
	$(ARCH)/src/webp_huff.o \
	$(ARCH)/src/webp_vp8l.o \
	$(ARCH)/src/webp_alpha.o \
	$(ARCH)/src/webp_vp8.o \
	$(ARCH)/src/webp_vp8_recon.o
LIB_WEBP = $(TARGET_DIR)/lib/libwebp.a

all: $(LIB_WEBP)

$(LIB_WEBP): $(LIB_WEBP_OBJS) Makefile
	@mkdir -p $(TARGET_DIR)/lib
	@mkdir -p $(TARGET_DIR)/include
	$(AR) rcs $(LIB_WEBP) $(LIB_WEBP_OBJS)
	cp -r include/* $(TARGET_DIR)/include/

$(ARCH)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(ARCH)
	rm -f $(LIB_WEBP)
