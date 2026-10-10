/* openfi_at 单元测试 —— 用【真机上抓到的报文格式】(含各种变体) */
#define _GNU_SOURCE
#include "../src/openfi_at.h"
#include "t.h"

#include <stdio.h>
#include <string.h>

/* 常见回显 + 空行 */
static const char *R_CSQ =
	"AT+CSQ\r\r\n"
	"+CSQ: 20,99\r\n"
	"\r\n"
	"OK\r\n";

/* 不支持的命令 → CME ERROR 4 */
static const char *R_CME4 =
	"AT+QCFG=\"band\"\r\r\n"
	"+CME ERROR: 4\r\n";

/* 带 URC 干扰行(模组经常在响应前塞一条主动上报) */
static const char *R_URC =
	"AT+QENG=\"servingcell\"\r\r\n"
	"+QIND: \"acT\",\"NR5G-SA\"\r\n"
	"+QENG: \"servingcell\",\"NOCONN\",\"NR5G-SA\",\"TDD\",460,00,123456789,266,504990,41,5,-81,-10,-73,16,-\r\n"
	"OK\r\n";

/* 带引号、空字段、负数的多字段行 */
static const char *R_COPS =
	"AT+COPS?\r\r\n"
	"+COPS: 0,0,\"CHINA MOBILE\",7\r\n"
	"OK\r\n";

/* 最后没有 OK(超时) */
static const char *R_TIMEOUT = "AT+CSQ\r\r\n";

/* 一坨没有回显的裸响应(有些模组配了 ATE0) */
static const char *R_NOECHO = "+CPIN: READY\r\nOK\r\n";

static const char *R_CMS = "AT+CMGS=\"10086\"\r\n+CMS ERROR: 500\r\n";

int main(void)
{
	struct at_resp r;
	char fields[AT_MAX_FIELDS][64];

	printf("── 基本:CSQ ──\n");
	T_EQ_INT(at_parse(R_CSQ, &r), 0);
	T_EQ_INT(r.result, AT_RES_OK);
	T_OK(at_is_ok(&r));
	T_EQ_INT(r.n, 1);                                 /* 回显和空行被丢掉 */
	T_EQ_STR(at_find(&r, "+CSQ:"), "+CSQ: 20,99");
	T_EQ_INT(at_csv_int(at_find(&r, "+CSQ:"), 0, -1), 20);
	T_EQ_INT(at_csv_int(at_find(&r, "+CSQ:"), 1, -1), 99);
	T_EQ_INT(at_csv_int(at_find(&r, "+CSQ:"), 2, -1), -1);   /* 越界→默认 */

	printf("── CME ERROR ──\n");
	T_EQ_INT(at_parse(R_CME4, &r), 0);
	T_EQ_INT(r.result, AT_RES_CME);
	T_EQ_INT(r.err_code, 4);
	T_EQ_STR(at_result_str(r.result), "CME ERROR");
	T_OK(!at_is_ok(&r));

	printf("── URC 干扰行 + 长字段行 ──\n");
	T_EQ_INT(at_parse(R_URC, &r), 0);
	T_EQ_INT(r.result, AT_RES_OK);
	T_EQ_INT(r.n, 2);                                  /* URC 保留但不干扰查找 */
	T_OK(at_find(&r, "+QENG:") != NULL);
	T_EQ_INT(at_csv_int(at_find(&r, "+QENG:"), 0, -1), -1);  /* "servingcell" 不是数字 */
	{
		const char *l = at_find(&r, "+QENG:");
		int n = at_csv(l, fields, AT_MAX_FIELDS);
		T_OK(n >= 16);
		T_EQ_STR(fields[0], "servingcell");
		T_EQ_STR(fields[1], "NOCONN");
		T_EQ_INT(at_csv_int(l, 8, -1), 504990);        /* ARFCN */
		T_EQ_INT(at_csv_int(l, 11, 0), -81);           /* 负数字段 */
	}

	printf("── 带引号的字符串字段 ──\n");
	T_EQ_INT(at_parse(R_COPS, &r), 0);
	T_EQ_INT(at_csv_int(at_find(&r, "+COPS:"), 3, -1), 7);
	{
		const char *l = at_find(&r, "+COPS:");
		at_csv(l, fields, AT_MAX_FIELDS);
		T_EQ_STR(fields[2], "CHINA MOBILE");           /* 引号被去掉 */
	}

	printf("── 超时 / 无回显 ──\n");
	T_EQ_INT(at_parse(R_TIMEOUT, &r), 0);
	T_EQ_INT(r.result, AT_RES_TIMEOUT);
	T_EQ_STR(at_result_str(r.result), "timeout");
	T_EQ_INT(at_parse(R_NOECHO, &r), 0);
	T_EQ_INT(r.result, AT_RES_OK);
	T_EQ_STR(at_find(&r, "+CPIN:"), "+CPIN: READY");

	printf("── CMS ERROR(发短信失败)──\n");
	T_EQ_INT(at_parse(R_CMS, &r), 0);
	T_EQ_INT(r.result, AT_RES_CMS);
	T_EQ_INT(r.err_code, 500);

	printf("── 边界 ──\n");
	T_EQ_INT(at_parse("", &r), 0);
	T_EQ_INT(r.result, AT_RES_TIMEOUT);
	T_EQ_INT(r.n, 0);
	T_OK(at_find(&r, "+X:") == NULL);
	T_EQ_STR(at_result_str(AT_RES_IO), "io error");
	T_EQ_INT(at_csv(NULL, fields, AT_MAX_FIELDS), 0);
	T_EQ_INT(at_csv_int(NULL, 0, 42), 42);

	T_SUMMARY();
}
