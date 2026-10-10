#
# openfi-modem 的构建(交叉编译时由 OpenWrt 包传 CC/CFLAGS/LDFLAGS)
#
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99
LDFLAGS ?=
LIBS    ?= -lopenfi

openfi-modem: main.c openfi_modem.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

clean:
	rm -f openfi-modem

.PHONY: clean
