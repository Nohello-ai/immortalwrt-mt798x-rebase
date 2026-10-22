/*
 * openfi-sms —— OpenFi 6C 短信收发
 *
 * 用法:
 *   openfi-sms list [N]         列出短信(默认 50 条,新的在前)JSON
 *   openfi-sms read <id>        看一条(顺带标为已读)
 *   openfi-sms send <号码> <内容>  发一条
 *   openfi-sms delete <id>      删一条
 *   openfi-sms delete all       全删
 *   openfi-sms unread           未读条数(纯数字,给界面)
 *   openfi-sms collect          后台收短信(procd 拉起)
 *   openfi-sms selftest         自检
 *
 * 设计:
 *   · 短信用"一条一个文件"存(/etc/openfi-sms),掉电不坏
 *   · 收:定时问模组要【未读】短信,解出 PDU 存本地,然后从模组里删掉,
 *        这样不会把模组的存储塞满;不用 URC 是因为 URC 可能丢
 *   · 发:自己编 PDU 再走 AT+CMGS —— 不依赖模组的中文编码能力
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <sys/select.h>	/* musl 要求显式包含,glibc 会间接带进来 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "openfi_at.h"
#include "openfi_at_io.h"
#include "openfi_json.h"
#include "openfi_log.h"
#include "openfi_sms.h"
#include "openfi_sms_store.h"
#include "openfi_sysfs.h"
#include "openfi_uci.h"

static volatile sig_atomic_t g_stop;

static void on_term(int sig) { (void)sig; g_stop = 1; }

static void copy_str(char *dst, size_t n, const char *src)
{
	size_t l;

	if (!dst || !n)
		return;
	if (!src)
		src = "";
	l = strlen(src);
	if (l >= n)
		l = n - 1;
	memcpy(dst, src, l);
	dst[l] = '\0';
}

/* ---------------- 模组侧 ---------------- */

struct sms_cfg {
	char at_port[128];
	char sca[64];			/* 短信中心;空 = 用模组里存的 */
	int  baud;
	int  timeout_ms;
	int  period;
};

/*
 * 解析 AT 口。
 *
 * 【为什么不能直接用 uci 里的值】
 *   openfi_modem 的默认配置写的是 'auto' —— 约定值,不是设备路径。
 *   直接拿去 open() 会报 "打不开 auto"(真机上就是这么炸的)。
 *
 * 【为什么不去自己探测】
 *   串口一次只能一个人用。openfi-modem 的采样进程每 15 秒就用一次,
 *   我们再去逐个口试 AT,就是在跟它抢(虽然现在有 flock,但没必要)。
 *   它已经把【真正在用的口】写在 /var/run/openfi-modem.json 里了,读就行。
 *
 * 顺序:
 *   ① 显式配了 /dev/xxx     → 用它
 *   ② 配置是空或 'auto'      → 读 modem 的采样结果
 *   ③ 还是拿不到             → 自己按常见顺序探一遍(最后手段)
 */
static int resolve_at_port(char *out, size_t n, int baud, int timeout_ms)
{
	char buf[1024];
	FILE *f;
	size_t got;
	char *hit, *end;

	/* ② 复用 modem 采样进程的结果 */
	f = fopen("/var/run/openfi-modem.json", "r");
	if (f) {
		got = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[got] = '\0';

		/* 找 "at_port":"/dev/ttyUSB2" —— 这个文件是我们自己写的,
		   格式固定,不值得为它引入一个 JSON 解析器 */
		hit = strstr(buf, "\"at_port\":\"");
		if (hit) {
			hit += strlen("\"at_port\":\"");
			end = strchr(hit, '"');
			if (end && (size_t)(end - hit) < n && end > hit) {
				memcpy(out, hit, (size_t)(end - hit));
				out[end - hit] = '\0';
				if (out[0] == '/')
					return 0;
			}
		}
	}

	/* ③ 最后手段:按常见顺序探一遍(和 modem 包一样的思路) */
	{
		static const char *common[] = {
			"/dev/ttyUSB2", "/dev/ttyUSB3", "/dev/ttyUSB1",
			"/dev/ttyUSB0", "/dev/ttyACM0", "/dev/ttyACM1",
		};
		struct at_port tmp;
		size_t i;
		char extra[512];

		for (i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
			if (at_open(&tmp, common[i], baud) == 0) {
				struct at_resp r;

				if (at_cmd(&tmp, "AT", timeout_ms, &r) == 0 &&
				    r.result == AT_RES_OK) {
					at_close(&tmp);
					copy_str(out, n, common[i]);
					return 0;
				}
				at_close(&tmp);
			}
		}
		/* at_list_ports 能列出系统里实际存在的串口 */
		if (at_list_ports(extra, sizeof(extra)) > 0) {
			/* 形如 "ttyUSB0 ttyUSB2 ...",简化:只在里面找第一个能应答的 */
			char *tok = strtok(extra, " \n");
			while (tok) {
				char path[128];

				snprintf(path, sizeof(path), "/dev/%s", tok);
				if (at_open(&tmp, path, baud) == 0) {
					struct at_resp r;

					if (at_cmd(&tmp, "AT", timeout_ms, &r) == 0 &&
					    r.result == AT_RES_OK) {
						at_close(&tmp);
						copy_str(out, n, path);
						return 0;
					}
					at_close(&tmp);
				}
				tok = strtok(NULL, " \n");
			}
		}
	}
	return -1;
}

/*
 * need_port: 只有真的要跟模组说话的命令(send / collect / selftest)才传 1。
 *
 * 【为什么必须区分 —— 真机上页面卡死就是这个原因】
 *   list / read / unread / delete 都只读本地存储,本来【不需要碰串口】。
 *   但早期版本无条件调 resolve_at_port(),而它在拿不到 modem 缓存时
 *   会【逐个尝试打开串口】:
 *       6 个候选口 × 每个在锁上等 12 秒 = 最长 72 秒
 *   于是 LuCI 页面的 fs.exec('/usr/bin/openfi-sms list 100') 一直不返回,
 *   页面就一直转圈(表现:短信页完全空白 / 永远加载中)。
 *
 *   锁是采样进程持有的,拿不到很正常 —— 所以这些纯本地操作压根不该去抢。
 */
static void cfg_load(struct sms_cfg *c, int need_port)
{
	char *p = ouci_get("openfi_sms", "sms", "at_port");

	memset(c, 0, sizeof(*c));

	c->baud = (int)ouci_get_int("openfi_sms", "sms", "baud", 115200);
	c->timeout_ms = (int)ouci_get_int("openfi_sms", "sms", "timeout_ms", 2000);

	if (!need_port) {
		/* 用不到串口:不去解析、不去抢锁 */
		c->at_port[0] = '\0';
	} else if (p && *p && strcmp(p, "auto") != 0 && p[0] == '/') {
		/* ① 显式路径 */
		copy_str(c->at_port, sizeof(c->at_port), p);
	} else {
		/* ② / ③ 复用 modem 采样结果,或自己探 */
		if (resolve_at_port(c->at_port, sizeof(c->at_port),
				    c->baud, c->timeout_ms) != 0) {
			/* 实在拿不到就先留空,open_port 会失败并打日志,
			   比塞一个假路径进去更容易看出问题 */
			c->at_port[0] = '\0';
		}
	}
	free(p);
	p = ouci_get("openfi_sms", "sms", "sca");
	copy_str(c->sca, sizeof(c->sca), p ? p : "");
	free(p);
	c->period = (int)ouci_get_int("openfi_sms", "sms", "period", 20);
	if (c->period < 10)
		c->period = 10;
}

static int open_port(const struct sms_cfg *c, struct at_port *p)
{
	return at_open(p, c->at_port, c->baud);
}

/* ---------------- 收 ---------------- */

/*
 * 读模组上【指定索引】的一条短信并入库。
 *
 * 【为什么改成逐条读】
 *   原来是发一次 AT+CMGL=4 读全部。真机上这个做法根本不可用:
 *   一条 156 字节的短信,转成 PDU 十六进制是 312 个字符;
 *   49 条合起来约 17 KB,而 at_cmd 的响应缓冲只有 8 KB、
 *   at_parse 的单行上限只有 256 字符。于是后半截被静默截掉、
 *   长 PDU 被整行丢弃 —— 页面永远空白,还一条日志都不留。
 *   逐条读以后每条响应只有几百字符,缓冲永远够,短信再多也不受影响。
 *
 * 返回: 1 = 新入库, 0 = 已存在/空槽, -1 = 出错(已记日志)
 * stat_out: 模组侧的短信状态(0 = 未读,-1 = 未知)
 */
static int fetch_at_index(struct at_port *port, const struct sms_cfg *c,
			  int idx, int *stat_out)
{
	struct at_resp r;
	char cmd[24];
	const char *l;
	int i;

	if (stat_out)
		*stat_out = -1;

	snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idx);
	if (at_cmd(port, cmd, c->timeout_ms * 2, &r) != 0) {
		olog_warn("AT+CMGR=%d 无响应(%s)", idx, at_result_str(r.result));
		return -1;
	}
	/* 空槽回 ERROR 是正常的,不当错误刷日志 */
	if (r.result != AT_RES_OK)
		return 0;

	l = at_find(&r, "+CMGR:");
	if (!l) {
		olog_warn("AT+CMGR=%d 响应缺 +CMGR 头", idx);
		return -1;
	}
	if (stat_out)
		*stat_out = (int)at_csv_int(l, 0, -1);

	/* PDU 在包头之后:纯十六进制、长度 > 20 */
	for (i = 0; i < r.n; i++) {
		const char *s = r.lines[i];
		size_t sl = strlen(s);
		struct sms_msg m, old;

		if (s[0] == '+' || sl <= 20)
			continue;
		if (strspn(s, "0123456789abcdefABCDEF") != sl)
			continue;

		memset(&m, 0, sizeof(m));
		if (sms_decode_pdu(s, &m.pdu) != 0) {
			olog_warn("AT+CMGR=%d PDU 解码失败(%zu 字符)", idx, sl);
			return -1;
		}
		/* 内容 id:同一轮/下一轮再读到不会堆重复 */
		sms_store_content_id(&m, m.id, sizeof(m.id));
		if (sms_store_get(m.id, &old) == 0)
			return 0;
		m.received_at = (long)time(NULL);
		/* ★ 已读状态跟模组走。+CMGR 的 stat:0=未读,1=已读,2=未发,3=已发。
		 *   原来一律写 0,于是 49 条早就读过的短信在页面上全成了"未读"。 */
		m.read = (stat_out && *stat_out == 1) ? 1 : 0;
		if (sms_store_save(&m) != 0) {
			olog_warn("AT+CMGR=%d 入库失败", idx);
			return -1;
		}
		olog_info("收到短信 来自 %s:%s", m.pdu.addr, m.pdu.text);
		return 1;
	}
	olog_warn("AT+CMGR=%d 响应里找不到 PDU 行", idx);
	return -1;
}

/* 拉一轮短信 */
static int fetch_unread(const struct sms_cfg *c)
{
	struct at_port port;
	struct at_resp r;
	const char *l;
	int count = 0, imported = 0;
	int del_idx[64], n_del = 0;

	if (open_port(c, &port) != 0) {
		olog_warn("打不开 %s", c->at_port);
		return -1;
	}

	/* ★ 先切 PDU 模式 —— 命令必须和模式匹配。
	 *
	 *   原来这里直接发文本模式的 AT+CMGL="REC UNREAD",但从不设置模式。
	 *   而发送链路会把模组设成 PDU 模式(AT+CMGF=0) —— 于是只要发过
	 *   一条短信,读取就永远失败:PDU 模式下 "REC UNREAD" 是非法的,
	 *   模组直接回 CMS ERROR。 */
	at_cmd(&port, "AT+CMGF=0", c->timeout_ms, &r);

	/* 问存储容量:AT+CPMS? → +CPMS: "SM",<used>,<total>,...
	 * ★ 失败要说出来 —— 以前所有失败路径都是静默 return,真机上完全
	 *   看不出是"没有短信"还是"读不出来"。 */
	if (at_cmd(&port, "AT+CPMS?", c->timeout_ms, &r) != 0 || r.result != AT_RES_OK) {
		olog_warn("AT+CPMS? 失败(%s)", at_result_str(r.result));
		at_close(&port);
		return -1;
	}
	l = at_find(&r, "+CPMS:");
	if (l)
		count = (int)at_csv_int(l, 2, 0);	/* 第 3 个字段 = 总数 */

	if (count <= 0) {
		at_close(&port);
		return 0;
	}

	/* 逐条读 —— 内存占用和短信总数无关 */
	for (int i = 1; i <= count; i++) {
		int stat = -1;
		int n = fetch_at_index(&port, c, i, &stat);

		if (n > 0)
			imported++;
		/*
		 * ★ 清理策略:只要【本地已经有完整副本】,就删掉模组上那份。
		 *   - n > 0:刚导入成功,本地有 ✅
		 *   - n == 0:之前就存过(内容 id 命中),本地也有 ✅
		 *   - n < 0:导入失败 —— ★ 千万不能删,留着下轮重试
		 *
		 *   原来只删 stat==0(未读)的,而运营商下发的短信大多是
		 *   stat=1(已读),于是一条都删不掉。真机上 SIM 卡 49/50
		 *   一直是满的,再来的短信会被运营商直接拒收。
		 *   本地存的是完整副本(含正文),删模组那份不丢东西。
		 */
		if (n >= 0 && n_del < (int)(sizeof(del_idx) / sizeof(del_idx[0])))
			del_idx[n_del++] = i;
	}

	/* ★ 读完再统一删。边读边删会让后面的索引前移,漏读。 */
	for (int i = 0; i < n_del; i++) {
		char cmd[24];

		snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", del_idx[i]);
		if (at_cmd(&port, cmd, c->timeout_ms, &r) != 0 || r.result != AT_RES_OK)
			olog_warn("AT+CMGD=%d 失败(%s)", del_idx[i], at_result_str(r.result));
	}

	/* 顺便把短信中心号读出来(发的时候要用)*/
	if (!c->sca[0]) {
		if (at_cmd(&port, "AT+CSCA?", c->timeout_ms, &r) == 0) {
			const char *cl = at_find(&r, "+CSCA:");

			if (cl) {
				char fields[4][64];

				if (at_csv(cl, fields, 4) >= 1)
					ouci_set("openfi_sms", "sms", "sca", fields[0]);
			}
		}
	}

	at_close(&port);
	return imported;
}

/* ---------------- 发 ---------------- */

/* UTF-8 字符长度;不完整或非法返回 0(让调用方停下,不要跨过结尾) */
static int utf8_char_len(const unsigned char *p)
{
	int l;

	if (!p || !*p)
		return 0;
	if (*p < 0x80)
		l = 1;
	else if ((*p & 0xE0) == 0xC0)
		l = 2;
	else if ((*p & 0xF0) == 0xE0)
		l = 3;
	else if ((*p & 0xF8) == 0xF0)
		l = 4;
	else
		return 1;		/* 非法首字节,当单字节跳过 */
	for (int i = 1; i < l; i++)
		if (!p[i] || (p[i] & 0xC0) != 0x80)
			return 0;	/* 不完整 —— 不能 q += l 跨过结尾 */
	return l;
}

static int utf8_count(const char *text)
{
	const unsigned char *p = (const unsigned char *)text;
	int n = 0;

	while (p && *p) {
		int l = utf8_char_len(p);

		if (l <= 0)
			break;
		p += l;
		n++;
	}
	return n;
}

/*
 * 发一条 PDU。
 *
 * ★ 必须走这三步(以前直接 at_cmd 发 PDU,发送从来没成功过):
 *     ① 发 AT+CMGS=<TPDU字节数>,模组回 ">" 提示(不是 OK)
 *     ② 写 PDU 本体
 *     ③ 用 Ctrl-Z(0x1A)结束 —— 只发 CR 的话模组会一直等剩余数据
 *   然后才回 OK / ERROR。
 */
static int send_one_pdu(struct at_port *port, const struct sms_cfg *c,
			const char *pdu, int tpdu_len)
{
	struct at_resp r;
	char cmd[64];
	char out[1024];
	size_t used = 0;

	snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", tpdu_len);
	if (at_write_raw(port, cmd, strlen(cmd)) != 0 ||
	    at_write_raw(port, "\r", 1) != 0)
		return -1;

	/* 等 ">" 提示。有些模组会在提示前先吐回显,at_wait_prompt 只找 '>' */
	if (at_wait_prompt(port, c->timeout_ms) != 0) {
		olog_warn("等 > 提示超时,仍然尝试发 PDU");
	}

	if (at_write_raw(port, pdu, strlen(pdu)) != 0)
		return -1;
	/* ★ 0x1A = Ctrl-Z = "输入结束";0x1B(ESC)是放弃 */
	if (at_write_raw(port, "\x1a", 1) != 0)
		return -1;

	/* 读结果 */
	{
		fd_set rfds;
		struct timeval tv;
		long long remain = c->timeout_ms * 3;

		while (remain > 0) {
			FD_ZERO(&rfds);
			FD_SET(port->fd, &rfds);
			tv.tv_sec = (time_t)(remain / 1000);
			tv.tv_usec = (suseconds_t)((remain % 1000) * 1000);
			int rc = select(port->fd + 1, &rfds, NULL, NULL, &tv);

			if (rc < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (rc == 0)
				break;
			{
				ssize_t n = read(port->fd, out + used,
						 sizeof(out) - used - 1);

				if (n <= 0)
					break;
				used += (size_t)n;
				out[used] = '\0';
				if (strstr(out, "OK") || strstr(out, "ERROR") ||
				    strstr(out, "+CMS ERROR") || strstr(out, "+CME ERROR"))
					break;
			}
			remain -= 50;
		}
	}
	if (at_parse(out, &r) != 0)
		return -1;
	return (r.result == AT_RES_OK) ? 0 : -1;
}

static int send_sms(const struct sms_cfg *c, const char *num, const char *text)
{
	struct at_port port;
	struct at_resp r;
	char pdu[SMS_HEX_MAX];
	char chunk[SMS_TEXT_MAX];
	struct sms_msg m;
	int use_ucs2, nseg, per_seg, single_max;
	int seg = 0, rc = 0;
	int ref = (int)(time(NULL) & 0xFF);
	const unsigned char *q = (const unsigned char *)text;
	int chars_total;

	if (!text || !*text)
		return oj_fail("内容为空");
	if (open_port(c, &port) != 0)
		return oj_fail("打不开串口 %s", c->at_port);

	/* ── 先定编码:纯 GSM 7 位能表达就用 7 位,否则 UCS2 ── */
	{
		unsigned char tmp[SMS_HEX_MAX];
		int sept = 0;

		use_ucs2 = (sms_pack_7bit(text, tmp, sizeof(tmp), &sept) < 0);
	}

	/*
	 * ★ 段数必须和实际切段用【同一套尺寸】,否则 UDH 里的 total
	 *   和真实条数对不上,接收端要么丢掉 seq>total 的分片,
	 *   要么永远等一条不存在的分段(整条短信不显示)。
	 *   以前:段数用字节数按 153 估、切段却固定 67 —— 两者必然打架
	 *   (实测 200 个 ASCII 字符:估算 2 段、实际切 3 段)。
	 */
	chars_total = utf8_count(text);
	single_max = use_ucs2 ? 70 : 160;
	per_seg = use_ucs2 ? 67 : 153;
	nseg = (chars_total <= single_max) ? 1
	     : (chars_total + per_seg - 1) / per_seg;

	/* 按同一尺寸切段 */
	int limit = (nseg > 1) ? per_seg : single_max;

	/* 先切一遍并核对条数 —— 万一和估算不符,以实际为准 */
	{
		const unsigned char *p = q;
		int n = 0;

		while (*p) {
			int chars = 0;

			while (*p && chars < limit) {
				int l = utf8_char_len(p);

				if (l <= 0)
					break;
				p += l;
				chars++;
			}
			if (chars == 0)
				break;
			n++;
		}
		if (n > 1 && n != nseg) {
			olog_warn("段数估算 %d 与实际 %d 不符,按实际来", nseg, n);
			nseg = n;
		}
	}

	/* 设 PDU 模式 */
	at_cmd(&port, "AT+CMGF=0", c->timeout_ms, &r);

	while (*q && rc == 0) {
		int take = 0, chars = 0;

		while (*q && chars < limit) {
			int l = utf8_char_len(q);

			if (l <= 0) {
				/* 不完整的 UTF-8:到此为止,不要把指针跨过结尾 */
				olog_warn("正文末尾有不完整字符,已截断");
				break;
			}
			if (take + l + 1 >= (int)sizeof(chunk))
				break;
			for (int i = 0; i < l; i++)
				chunk[take++] = (char)q[i];
			q += l;
			chars++;
		}
		chunk[take] = '\0';
		if (chars == 0)
			break;

		seg++;
		if (sms_encode_pdu(c->sca, num, chunk, -1,
				   nseg > 1 ? nseg : 0,
				   nseg > 1 ? seg : 0, ref,
				   pdu, sizeof(pdu)) != 0) {
			rc = oj_fail("PDU 编码失败(第 %d 段)", seg);
			break;
		}
		/* ★ 长度是 TPDU 字节数,不含短信中心字段 */
		{
			int tpdu = sms_tpdu_len(c->sca, pdu);

			if (tpdu <= 0) {
				rc = oj_fail("PDU 长度算不出来");
				break;
			}
			if (send_one_pdu(&port, c, pdu, tpdu) != 0) {
				rc = oj_fail("发送失败(第 %d/%d 段)", seg, nseg);
				break;
			}
		}
		olog_info("已发出第 %d/%d 段(%d 个字符)", seg, nseg, chars);
	}

	at_close(&port);

	if (rc == 0) {
		/* 存一份到"已发送" */
		memset(&m, 0, sizeof(m));
		copy_str(m.pdu.addr, sizeof(m.pdu.addr), num);
		copy_str(m.pdu.text, sizeof(m.pdu.text), text);
		m.pdu.dcs = use_ucs2 ? SMS_DCS_UCS2 : SMS_DCS_7BIT;
		m.outgoing = 1;
		m.read = 1;
		m.received_at = (long)time(NULL);
		sms_store_save(&m);

		struct ojson *o = oj_new();

		oj_bool(o, "ok", 1);
		oj_str(o, "to", num);
		oj_int(o, "segments", nseg);
		oj_emit(o);
	}
	return rc;
}

/* ---------------- 输出 ---------------- */

static void emit_msg_json(const struct sms_msg *m, int with_text)
{
	struct ojson *o = oj_new();

	oj_str(o, "id", m->id);
	oj_str(o, "addr", m->pdu.addr);
	oj_str(o, "ts", m->pdu.ts);
	oj_bool(o, "read", m->read);
	oj_bool(o, "outgoing", m->outgoing);
	oj_int(o, "received_at", m->received_at);
	if (with_text)
		oj_str(o, "text", m->pdu.text);
	else
		oj_int(o, "text_len", (long)strlen(m->pdu.text));
	if (m->pdu.concat_total > 1)
		oj_int(o, "part", m->pdu.concat_seq);
	oj_emit(o);
}

static void usage(void)
{
	fprintf(stderr,
		"用法: openfi-sms <命令>\n"
		"  list [N]          列出短信(默认 50,新在前)\n"
		"  read <id>         看一条并标为已读\n"
		"  send <号码> <内容> 发一条\n"
		"  delete <id>|all   删除\n"
		"  unread            未读条数\n"
		"  collect           后台收短信\n"
		"  selftest          自检\n");
}

int main(int argc, char **argv)
{
	struct sms_cfg cfg;

	olog_init("openfi-sms");
	{
		const char *e = getenv("OPENFI_SYSFS_ROOT");

		if (e && *e)
			osys_set_root(e);
		e = getenv("OPENFI_UCI_ROOT");
		if (e && *e)
			ouci_set_root(e);
		e = getenv("OPENFI_SMS_DIR");
		if (e && *e)
			sms_store_set_dir(e);
	}
	if (argc < 2) {
		usage();
		return 1;
	}
	const char *act = argv[1];

	/*
	 * ★ 只有真的要跟模组说话的命令才去解析串口。
	 *   list / read / unread / delete 是纯本地存储操作,解析串口会让它们
	 *   在采样进程的锁上最多等 72 秒 —— 网页那边就是"一直转圈"。
	 */
	{
		int need_port = (!strcmp(act, "send") ||
				 !strcmp(act, "collect") ||
				 !strcmp(act, "selftest"));

		cfg_load(&cfg, need_port);
	}
	sms_store_init();

	if (!strcmp(act, "list")) {
		int max = (argc > 2) ? atoi(argv[2]) : 50;
		static struct sms_msg all[512];
		const int arr_max = (int)(sizeof(all) / sizeof(all[0]));
		const size_t cap = 64 * 1024;
		struct ojson *o;
		char *arr;
		size_t used = 0;
		int total, n, i;
		int truncated = 0;

		/*
		 * ★ 请求条数必须先收敛到【接收数组的上限】。
		 *   sms_store_list 只会填 min(库存, max) 条,而这里传进来的是
		 *   命令行参数 —— 库存 520 条 + `list 600` 就会往 512 项的数组里
		 *   写 520 条,直接踩坏相邻内存。
		 */
		if (max <= 0)
			max = 50;
		if (max > arr_max)
			max = arr_max;

		total = sms_store_list(all, max);
		n = (total < max) ? total : max;
		if (n > arr_max)
			n = arr_max;

		o = oj_new();
		arr = malloc(cap);
		if (!arr) {
			oj_free(o);
			return 1;
		}
		arr[0] = '\0';

		/*
		 * ★ snprintf 返回的是"假如不截断会写多少字节",不是"实际写了多少"。
		 *   所以不能写成 used += snprintf(arr+used, cap-used, ...) ——
		 *   一旦总长超过 cap,cap-used 会下溢成一个巨大的 size_t,
		 *   下一次 snprintf 就往分配区之外猛写(实测越界 287 字节)。
		 *   正确做法:【先判后加】。
		 */
		{
			int w = snprintf(arr + used, cap - used, "[");

			if (w < 0 || (size_t)w >= cap - used) {
				used = 0;
				truncated = 1;
			} else {
				used += (size_t)w;
			}
		}

		for (i = 0; i < n && !truncated; i++) {
			struct ojson *one = oj_new();
			char *js;

			oj_str(one, "id", all[i].id);
			oj_str(one, "addr", all[i].pdu.addr);
			oj_str(one, "ts", all[i].pdu.ts);
			oj_bool(one, "read", all[i].read);
			oj_bool(one, "outgoing", all[i].outgoing);
			oj_int(one, "received_at", all[i].received_at);
			oj_str(one, "text", all[i].pdu.text);
			js = oj_render(one);
			oj_free(one);
			if (!js)
				continue;

			int w = snprintf(arr + used, cap - used, "%s%s",
					 i ? "," : "", js);

			free(js);
			if (w < 0 || (size_t)w >= cap - used) {
				/* 放不下了 —— 把已经写好的部分收尾,别硬塞 */
				truncated = 1;
				n = i;
				break;
			}
			used += (size_t)w;
		}

		if (truncated)
			olog_warn("短信太多,列表被截断到 %d 条", n);
		{
			int w = snprintf(arr + used, cap - used, "]");

			if (w < 0 || (size_t)w >= cap - used)
				arr[used ? used - 1 : 0] = '\0';	/* 极端情况:去掉尾逗号 */
			else
				used += (size_t)w;
		}

		oj_int(o, "total", total);
		oj_int(o, "unread", sms_store_unread());
		oj_int(o, "shown", n);
		oj_bool(o, "truncated", truncated);
		oj_raw(o, "messages", arr);
		free(arr);
		oj_emit(o);
		return 0;
	}

	if (!strcmp(act, "read")) {
		struct sms_msg m;

		if (argc < 3)
			return oj_fail("要指定 id");
		if (!sms_id_ok(argv[2]))
			return oj_fail("id 不合法");
		if (sms_store_get(argv[2], &m) != 0)
			return oj_fail("找不到这条短信");
		emit_msg_json(&m, 1);
		sms_store_mark(argv[2], 1);
		return 0;
	}

	if (!strcmp(act, "unread")) {
		printf("%d\n", sms_store_unread());
		return 0;
	}

	if (!strcmp(act, "delete")) {
		if (argc < 3)
			return oj_fail("要指定 id 或 all");
		if (!strcmp(argv[2], "all")) {
			int n = sms_store_clear();
			struct ojson *o = oj_new();

			oj_bool(o, "ok", 1);
			oj_int(o, "deleted", n);
			oj_emit(o);
			return 0;
		}
		if (sms_store_delete(argv[2]) != 0)
			return oj_fail("删除失败(没有这条?)");
		struct ojson *o = oj_new();

		oj_bool(o, "ok", 1);
		oj_emit(o);
		return 0;
	}

	if (!strcmp(act, "send")) {
		if (argc < 4)
			return oj_fail("用法: send <号码> <内容>");
		return send_sms(&cfg, argv[2], argv[3]);
	}

	if (!strcmp(act, "selftest")) {
		int bad = 0;
		struct at_port port;

		printf("openfi-sms 自检\n");
		printf("  存储目录: %s(%d 条,未读 %d)\n", sms_store_dir(),
		       sms_store_count(), sms_store_unread());
		if (sms_store_init() != 0) {
			printf("  ❌ 存储目录不可用\n");
			bad++;
		}
		printf("  AT 口: %s @ %d\n", cfg.at_port, cfg.baud);
		if (open_port(&cfg, &port) == 0) {
			struct at_resp r;

			printf("  ✅ 串口可打开\n");
			if (at_cmd(&port, "AT+CPMS?", cfg.timeout_ms, &r) == 0 && r.n > 0)
				printf("  模组存储: %s\n", r.lines[0]);
			at_close(&port);
		} else {
			printf("  ⚠️ 串口打不开(模组没插或口不对)\n");
		}
		printf("\n  %s\n", bad ? "自检发现问题" : "自检通过");
		return bad ? 1 : 0;
	}

	if (!strcmp(act, "collect")) {
		signal(SIGTERM, on_term);
		signal(SIGINT, on_term);
		signal(SIGHUP, on_term);
		olog_info("收短信进程启动,周期 %ds,AT 口 %s", cfg.period, cfg.at_port);
		while (!g_stop) {
			int n = fetch_unread(&cfg);

			if (n > 0)
				olog_info("本轮入库 %d 条", n);
			for (int i = 0; i < cfg.period * 10 && !g_stop; i++)
				usleep(100 * 1000);
		}
		return 0;
	}

	if (!strcmp(act, "-h") || !strcmp(act, "--help")) {
		usage();
		return 0;
	}
	usage();
	return 1;
}
