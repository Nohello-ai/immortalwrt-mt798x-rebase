#
# openfi-sms 的构建(交叉编译时由 OpenWrt 包传 CC/CFLAGS/LDFLAGS)
#
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99
LDFLAGS ?=
LIBS    ?= -lopenfi

openfi-sms: main.c openfi_sms.c openfi_sms_store.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

clean:
	rm -f openfi-sms

.PHONY: clean
