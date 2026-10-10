/* 自带 JSON 输出的测试 —— 重点是转义(踩过坑:裸控制字符会让解析失败) */
#define _GNU_SOURCE
#include "../src/openfi_json.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 把 stdout 抓回来 */
static char *capture(void (*fn)(void))
{
	static char buf[4096];
	int saved = dup(1);
	int p[2];

	if (pipe(p) != 0)
		return NULL;
	fflush(stdout);
	dup2(p[1], 1);
	fn();
	fflush(stdout);
	dup2(saved, 1);
	close(saved);
	close(p[1]);
	ssize_t n = read(p[0], buf, sizeof(buf) - 1);
	close(p[0]);
	buf[n > 0 ? n : 0] = '\0';
	return buf;
}

static void emit_basic(void)
{
	struct ojson *o = oj_new();

	oj_bool(o, "ok", 1);
	oj_int(o, "temp", 52);
	oj_str(o, "reason", "curve");
	oj_null(o, "wifi");
	oj_bool(o, "off", 0);
	oj_emit(o);
}

static void emit_escape(void)
{
	struct ojson *o = oj_new();

	oj_str(o, "quote", "he said \"hi\"");
	oj_str(o, "bs", "a\\b");
	oj_str(o, "nl", "line1\nline2");
	oj_str(o, "tab", "a\tb");
	oj_str(o, "ctrl", "x\x01y");       /* 裸控制字符必须被转义 */
	oj_str(o, "nullv", NULL);
	oj_raw(o, "nested", "{\"a\":1}");
	oj_emit(o);
}

int main(void)
{
	char *s;

	printf("── 基本输出 ──\n");
	s = capture(emit_basic);
	T_EQ_STR(s, "{\"ok\":true,\"temp\":52,\"reason\":\"curve\",\"wifi\":null,\"off\":false}\n");

	printf("── 转义(以前吃过亏:裸控制字符让 JSON 解析失败)──\n");
	s = capture(emit_escape);
	T_OK(s && strstr(s, "\\\"hi\\\"") != NULL);       /* 引号 */
	T_OK(s && strstr(s, "a\\\\b") != NULL);           /* 反斜杠 */
	T_OK(s && strstr(s, "line1\\nline2") != NULL);    /* 换行 */
	T_OK(s && strstr(s, "a\\tb") != NULL);            /* 制表 */
	T_OK(s && strstr(s, "\\u0001") != NULL);          /* 控制字符 */
	T_OK(s && strstr(s, "\"nullv\":null") != NULL);
	T_OK(s && strstr(s, "\"nested\":{\"a\":1}") != NULL);
	T_OK(s && strchr(s, '\x01') == NULL);             /* 绝不允许裸控制字符 */

	printf("── 长时间/大量字段不越界 ──\n");
	{
		struct ojson *o;
		char key[32];

		int saved = dup(1);
		FILE *devnull = fopen("/dev/null", "w");

		dup2(fileno(devnull), 1);
		o = oj_new();
		for (int i = 0; i < 200; i++) {
			snprintf(key, sizeof(key), "k%d", i);
			oj_int(o, key, i);
		}
		oj_emit(o);
		fflush(stdout);
		dup2(saved, 1);
		close(saved);
		fclose(devnull);
		printf("     (200 个字段已输出,没崩)\n");
	}

	printf("── 超长字符串 ──\n");
	{
		struct ojson *o = oj_new();
		char big[3000];

		memset(big, 'x', sizeof(big) - 1);
		big[sizeof(big) - 1] = '\0';
		int saved = dup(1);
		FILE *devnull = fopen("/dev/null", "w");

		dup2(fileno(devnull), 1);
		oj_str(o, "big", big);
		oj_emit(o);
		fflush(stdout);
		dup2(saved, 1);
		close(saved);
		fclose(devnull);
		printf("     (3000 字节字符串已输出,没崩)\n");
	}

	printf("── 空指针不崩 ──\n");
	oj_emit(NULL);
	oj_str(NULL, "k", "v");
	oj_int(NULL, "k", 1);
	oj_bool(NULL, "k", 1);
	oj_null(NULL, "k");
	oj_raw(NULL, "k", "{}");
	{
		struct ojson *o = oj_new();
		oj_str(o, NULL, "v");        /* 空键 */
		oj_emit(o);
	}

	printf("── oj_fail 输出格式 ──\n");
	{
		int saved = dup(1);
		int pp[2];

		pipe(pp);
		fflush(stdout);
		dup2(pp[1], 1);
		oj_fail("测试错误 %d", 42);
		fflush(stdout);
		dup2(saved, 1);
		close(saved); close(pp[1]);
		char b[256];
		ssize_t n = read(pp[0], b, sizeof(b) - 1);
		close(pp[0]);
		b[n > 0 ? n : 0] = '\0';
		T_OK(strstr(b, "\"ok\":false") != NULL);
		T_OK(strstr(b, "测试错误 42") != NULL);
	}

	T_SUMMARY();
}
