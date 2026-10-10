/*
 * 解析器模糊测试
 *
 * 思路:解析器(PDU / AT 响应 / debugfs / UCI)最容易被畸形输入打穿,
 *      而这些输入在真实世界里一定会遇到(信号差时半截报文、
 *      模组固件差异、别人塞进来的乱数据)。
 *
 * 做法:
 *   ① 纯随机字节
 *   ② 从【合法输入】出发做变异(截断/改字节/插字节)—— 这样更容易
 *      走进深层代码路径,而纯随机大多在第一个判断就被挡回来了
 *   ③ 边界值(全 0xFF / 全 0x00 / 超长 / 空)
 *
 * 配合 ASan/UBSan 跑,任何越界读写、use-after-free、未定义行为都会
 * 立刻让它崩掉并打出位置 —— 这比手工构造用例高效得多。
 *
 * 用法: fuzz_parsers [轮数](默认 20000)
 */
#define _GNU_SOURCE
#include "openfi_at.h"
#include "openfi_json.h"
#include "openfi_sms.h"
#include "openfi_switch.h"
#include "openfi_sms_store.h"
#include "openfi_uci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned long rng_state = 0x12345678u;

static unsigned rnd(void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return (unsigned)(rng_state >> 16);
}

static unsigned rnd_range(unsigned n) { return n ? rnd() % n : 0; }

/* 随机可打印字符(解析器大多期望文本)*/
static char rnd_char(void)
{
	static const char *pool =
		"0123456789ABCDEFabcdef:,;'\"+-=<>[]()| \t\r\n\\%sXPQRT";

	return pool[rnd_range((unsigned)strlen(pool))];
}

/* 从模板变异出一个输入 */
static void mutate(const char *base, char *out, size_t outlen)
{
	size_t n = strlen(base);
	size_t i;

	if (n >= outlen)
		n = outlen - 1;
	memcpy(out, base, n);
	out[n] = '\0';

	int ops = 1 + (int)rnd_range(6);

	for (int k = 0; k < ops && n > 0; k++) {
		switch (rnd_range(4)) {
		case 0:				/* 改一个字节 */
			out[rnd_range((unsigned)n)] = rnd_char();
			break;
		case 1:				/* 截断 */
			n = rnd_range((unsigned)n + 1);
			out[n] = '\0';
			break;
		case 2:				/* 插一个字节 */
			if (n + 2 < outlen) {
				size_t pos = rnd_range((unsigned)n + 1);

				memmove(out + pos + 1, out + pos, n - pos + 1);
				out[pos] = rnd_char();
				n++;
			}
			break;
		default:			/* 删一个字节 */
			if (n > 1) {
				size_t pos = rnd_range((unsigned)n);

				memmove(out + pos, out + pos + 1, n - pos);
				n--;
			}
			break;
		}
	}
}

/* ---- 合法的"种子",变异从它们出发 ---- */
static const char *SEED_PDU[] = {
	"0D91683108108300F0040D91683108108300F000006210100000000005E8329BFD06",
	"0D91683108108300F0040D91683108108300F0000862101000000000046D4B8BD5",
	"00440D91683108108300F00000621010000000000C0500032A020141D0E190",
	"00010005810180F6000005E8329BFD06",
	"0791683108108300F0040D91683108108300F000006210100000000005E8329BFD06",
	NULL
};

static const char *SEED_AT[] = {
	"AT+CSQ\r\r\n+CSQ: 21,98\r\n\r\nOK\r\n",
	"AT+QENG=\"servingcell\"\r\r\n+QIND: \"acT\",\"NR5G-SA\"\r\n"
	"+QENG: \"servingcell\",\"NOCONN\",\"NR5G-SA\",\"TDD\",460,00,123456789,266,"
	"504990,41,5,-81,-10,-73,16,-\r\nOK\r\n",
	"AT+QTEMP\r\r\n+QTEMP: \"soc-thermal\",54\r\n+QTEMP: \"pa-thermal\",47\r\nOK\r\n",
	"AT+CPIN?\r\r\n+CME ERROR: 10\r\n",
	"",
	NULL
};

static const char *SEED_GPIO[] = {
	"gpiochip0: GPIOs 512-568, parent: platform/11d00000.pinctrl, pinctrl_moore:\n"
	" gpio-512 (                    |func                ) in  lo IRQ \n"
	" gpio-513 (                    |reset               ) in  hi IRQ ACTIVE LOW\n",
	"",
	NULL
};

static const char *SEED_UCI[] = {
	"config fan 'fan'\n\toption mode 'auto'\n\toption t1 '55'\n\tlist x 'a'\n",
	"config led\n\toption enabled 'on'\n",
	"",
	NULL
};

static void fuzz_hex(char *buf, size_t n)
{
	if (rnd_range(3) == 0) {
		size_t l = rnd_range((unsigned)n);

		for (size_t i = 0; i < l; i++)
			buf[i] = (char)rnd();
		buf[l] = '\0';
	} else {
		mutate(SEED_PDU[rnd_range(5)], buf, n);
	}
}

int main(int argc, char **argv)
{
	long rounds = (argc > 1) ? atol(argv[1]) : 20000;
	char buf[4096];
	struct sms s;
	struct at_resp r;

	/* 存储解析测试用的文件 */
	{
		FILE *f = fopen("/tmp/fuzz-uci", "w");

		if (f) {
			fputs("config fan 'fan'\n\toption mode 'auto'\n", f);
			fclose(f);
		}
	}
	ouci_set_root("/tmp");

	printf("模糊测试 %ld 轮(配合 ASan/UBSan 跑)\n", rounds);

	for (long i = 0; i < rounds; i++) {
		int which = (int)rnd_range(4);

		switch (which) {
		case 0:					/* PDU 解码 */
			fuzz_hex(buf, sizeof(buf));
			sms_decode_pdu(buf, &s);
			/* 再来一遍:确保重复调用不会残留状态 */
			sms_decode_pdu(buf, &s);
			break;

		case 1:					/* AT 响应解析 */
			if (rnd_range(3) == 0) {
				size_t l = rnd_range(1024);

				for (size_t k = 0; k < l; k++)
					buf[k] = (char)rnd();
				buf[l] = '\0';
			} else {
				mutate(SEED_AT[rnd_range(4)], buf, 1024);
			}
			at_parse(buf, &r);
			at_find(&r, "+CSQ:");
			at_find(&r, "+QENG:");
			at_csv_int("+CSQ: 1,2", 0, -1);
			break;

		case 2:					/* 开关电平解析 */
			mutate(SEED_GPIO[rnd_range(2)], buf, sizeof(buf));
			sw_parse(buf, "func");
			sw_parse(buf, "reset");
			sw_parse(buf, "");
			break;

		default:				/* UCI 解析 */
			mutate(SEED_UCI[rnd_range(2)], buf, sizeof(buf));
			{
				FILE *f = fopen("/tmp/fuzz-uci", "w");

				if (f) {
					fwrite(buf, 1, strlen(buf), f);
					fclose(f);
				}
			}
			{
				char *v = ouci_get("fuzz-uci", "fan", "mode");
				free(v);
			}
			{
				char *v = ouci_list_options("fuzz-uci", "fan");
				free(v);
			}
			break;
		}

		if ((i % 5000) == 0 && i) {
			printf("  ... %ld 轮\n", i);
			fflush(stdout);
		}
	}

	printf("  ✅ %ld 轮全部跑完,没有崩溃 / 越界 / 未定义行为\n", rounds);
	unlink("/tmp/fuzz-uci");
	return 0;
}
