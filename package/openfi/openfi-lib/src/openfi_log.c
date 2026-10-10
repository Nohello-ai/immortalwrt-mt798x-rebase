#define _GNU_SOURCE
#include "openfi_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_tag = "openfi";
static int g_debug;

void olog_init(const char *tag)
{
	const char *d = getenv("OPENFI_DEBUG");

	if (tag && *tag)
		g_tag = tag;
	g_debug = (d && *d && strcmp(d, "0") != 0);
	openlog(g_tag, LOG_PID, LOG_DAEMON);
}

void olog(int prio, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	if (prio == LOG_DEBUG && !g_debug)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	syslog(prio, "%s", buf);
}
