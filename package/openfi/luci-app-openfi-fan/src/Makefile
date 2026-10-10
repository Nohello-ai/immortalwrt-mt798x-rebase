#
# openfi-fan 的构建。交叉编译时由 OpenWrt 包传入 CC/CFLAGS/LDFLAGS。
#
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99
LDFLAGS ?=
# 链接顺序要注意:库必须放在源文件后面
LIBS    ?= -lopenfi

openfi-fan: openfi-fan.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f openfi-fan

.PHONY: clean
