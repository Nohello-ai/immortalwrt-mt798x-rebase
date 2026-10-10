/* OpenFi 后端公共库 —— 日志
 *
 * 走 syslog(OpenWrt 的 logd 会收),tag 固定,方便 `logread -e openfi` 过滤。
 * 同时支持把日志级别调高看细节(环境变量 OPENFI_DEBUG=1)。
 */
#ifndef OPENFI_LOG_H
#define OPENFI_LOG_H

#include <syslog.h>

void olog_init(const char *tag);
void olog(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* 惯用简写 */
#define olog_dbg(...)  olog(LOG_DEBUG, __VA_ARGS__)
#define olog_info(...) olog(LOG_INFO,  __VA_ARGS__)
#define olog_warn(...) olog(LOG_WARNING, __VA_ARGS__)
#define olog_err(...)  olog(LOG_ERR,   __VA_ARGS__)

#endif
