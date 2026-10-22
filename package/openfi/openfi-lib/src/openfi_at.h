/* OpenFi 后端公共库 —— AT 响应解析(纯函数,本地可测)
 *
 * 5G 模组的所有信息都是从 AT 口读出来的。协议本身很简单:
 *     发一条命令 → 收若干行 → 最后一行是 OK / ERROR / +CME ERROR: n
 *
 * 【为什么要单独抽出来】
 *   串口 I/O(termios)只有目标机上能跑,但【解析】是纯字符串处理 ——
 *   抽出来之后,把真机抓到的报文喂进去,就能在开发机上把解析逻辑测透。
 *   实测过:模组返回的格式有各种变体(带/不带空格、引号、空字段、
 *   前置 URC 干扰行),靠人眼在设备上试是试不完的。
 */
#ifndef OPENFI_AT_H
#define OPENFI_AT_H

#include <stddef.h>

#define AT_MAX_LINES 128
/*
 * 单行上限。★ 原来是 256,这是个实打实的 bug:
 *   AT+CMGR / AT+CMGL 返回的短信是 PDU 十六进制串,
 *   一条 156 字节的短信就是 312 个字符,255 字节的上限更是 510 个字符。
 *   256 的缓冲装不下,at_parse 里"超长行直接丢"→ 长短信永远读不出来。
 *   定成 512 恰好容纳 255 字节(510 字符)的 PDU。
 */
#define AT_LINE_MAX  512
#define AT_MAX_FIELDS 24

/* 最终结果 */
enum at_result {
	AT_RES_OK = 0,		/* 收到 OK */
	AT_RES_ERROR,		/* 收到 ERROR */
	AT_RES_CME,		/* 收到 +CME ERROR: n */
	AT_RES_CMS,		/* 收到 +CMS ERROR: n */
	AT_RES_TIMEOUT,		/* 超时/没收到终止行 */
	AT_RES_IO,		/* 串口读写失败 */
};

struct at_resp {
	char lines[AT_MAX_LINES][AT_LINE_MAX];
	int  n;
	enum at_result result;
	int  err_code;		/* CME/CMS 的错误码,没有则 0 */
};

/*
 * 解析一整段串口输出。
 * buf 里可以包含命令回显(以 AT 开头的行会被丢掉)、URC 干扰行、空行。
 * 返回 0 成功, -1 参数错。
 */
int at_parse(const char *buf, struct at_resp *r);

/* 结果 → 字符串("OK" / "ERROR" / "CME ERROR 4" / "timeout" …) */
const char *at_result_str(enum at_result res);

/* 在响应里找以 prefix 开头的那一行(如 "+CSQ:"),返回行指针或 NULL。
 * 会跳过命令回显和 URC。 */
const char *at_find(const struct at_resp *r, const char *prefix);

/* 把 prefix 行冒号后面的部分按逗号切成字段(会去引号和空格)。
 * 返回字段个数。line 传 at_find 的结果。 */
int at_csv(const char *line, char fields[][64], int max_fields);

/* 取第 idx 个字段的整数值;空字段/非数字 → 返回 def */
long at_csv_int(const char *line, int idx, long def);

/* 判断整段响应是否"成功"(OK),辅助用 */
int at_is_ok(const struct at_resp *r);

#endif
