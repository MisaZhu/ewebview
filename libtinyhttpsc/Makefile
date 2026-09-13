PROJS_ROOT_DIR=..
include $(PROJS_ROOT_DIR)/make.inc

BUILD_DIR = $(SDK_DIR)
TARGET_DIR = $(BUILD_DIR)

# Self-contained BearSSL HTTP/HTTPS client amalgamation. It only needs the
# SDK-installed libc/socket headers (sys/socket.h etc.); the BearSSL engine is
# embedded in src/BearHttpsClientOne.c, so there is no external TLS dependency.
CFLAGS += -Iinclude

LIB_OBJS = $(ARCH)/src/tinyhttpsc.o $(ARCH)/src/BearHttpsClientOne.o
LIB = $(TARGET_DIR)/lib/libtinyhttpsc.a

all: $(LIB)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(TARGET_DIR)/lib
	@mkdir -p $(TARGET_DIR)/include
	$(AR) rcs $(LIB) $(LIB_OBJS)
	cp -r include/* $(TARGET_DIR)/include/

$(ARCH)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(ARCH)
	rm -f $(LIB)
