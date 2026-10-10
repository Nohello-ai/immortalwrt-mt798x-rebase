#
# openfi-lib —— 同一个 Makefile 支持两种用法:
#
#   交叉编译(由 OpenWrt 包调用):
#       $(MAKE) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" AR="$(TARGET_AR)" \
#               JSON_CFLAGS="" LDFLAGS="$(TARGET_LDFLAGS)"
#       ★ JSON_CFLAGS 传空 —— 交叉编译时 json-c 的头在 sysroot 里,
#         TARGET_CFLAGS 已经带上了,【绝不能】再让 pkg-config 去找宿主机的 json-c
#
#   本地开发 / 测试(开发机上直接 make):
#       make test
#
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99
JSON_CFLAGS ?= $(shell pkg-config --cflags json-c 2>/dev/null)
LDFLAGS ?=

SRCS := openfi_uci.c openfi_json.c openfi_log.c openfi_sysfs.c openfi_at.c openfi_at_io.c openfi_curve.c openfi_fanctl.c openfi_led.c openfi_temp.c openfi_switch.c
OBJS := $(SRCS:.c=.o)
LIB  := libopenfi.a

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(JSON_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(LIB)
	rm -rf /tmp/openfi-test-conf /tmp/openfi-test-sys

.PHONY: all clean
