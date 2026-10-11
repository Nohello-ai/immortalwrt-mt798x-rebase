/*
 * openfi-modem —— OpenFi 6C 5G 模组管理
 *
 * 用法:
 *   openfi-modem status        综合状态 JSON(读缓存,零副作用 —— LuCI 轮询用)
 *   openfi-modem refresh       主动查一遍模组并更新缓存
 *   openfi-modem collect       后台采样守护进程(定时 refresh,并给风扇写温度)
 *   openfi-modem at <命令>     透传一条 AT 命令(AT 终端用)
 *   openfi-modem at-port       只打印探测到的 AT 口(调试)
 *   openfi-modem reset         硬复位模组(拉 lte_reset GPIO)
 *   openfi-modem selftest      自检
 *
 * 设计要点:
 *   · 模组是独立的一台小电脑,所有信息都得从 AT 口问出来。
 *     串口一次只能一个人用,所以【由一个常驻进程统一采样】,
 *     其他查询读缓存 —— 免得 Web 页面和风扇守护进程同时抢串口。
 *   · 风扇要的模组温度也由这里写出去(/var/run/openfi-modem-temp),
 *     风扇就不必自己开一路串口。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "openfi_at.h"
#include "openfi_at_io.h"
#include "openfi_json.h"
#include "openfi_log.h"
#include "openfi_modem.h"
#include "openfi_sysfs.h"
#include "openfi_uci.h"

#define STATE_FILE   "/var/run/openfi-modem.json"
#define TEMP_FILE    "/var/run/openfi-modem-temp"
#define DEFAULT_PORT "/dev/ttyUSB2"	/* Quectel 系模组的 AT 口惯例 */

static volatile sig_atomic_t g_stop;

/*
 * 有界字符串拷贝。
 * 为什么不用 snprintf:当源缓冲区比目标大时,snprintf 会触发
 * -Wformat-truncation(而且我们开了 -Werror)。这里明确按目标长度截断,
 * 语义清楚,编译器也不会误报。
 */

static void on_term(int sig) { (void)sig; g_stop = 1; }

/* ---------------- AT 口探测 ---------------- */

/* 试一个口:能回 OK 就认它 */
static int probe_port(const char *path, int baud, int timeout_ms)
{
	struct at_port p;
	struct at_resp r;
	int ok;

	if (at_open(&p, path, baud) != 0)
		return 0;
	ok = (at_cmd(&p, "AT", timeout_ms, &r) == 0 && at_is_ok(&r));
	at_close(&p);
	return ok;
}

/*
 * 找 AT 口。
 * 顺序:UCI 指定的 → 常见候选(ttyUSB2/ttyUSB3/ttyUSB1/ttyUSB0/ttyACM0)
 *      → 再扫 /dev 里全部 ttyUSB 与 ttyACM 设备
 * 只要有一个口回 OK 就采用它 —— 比按 USB 描述符硬编码更耐固件变化。
 * (注意别在这段里写出星号加斜杠,那会把块注释提前结束掉。)
 */
static int find_at_port(char *out, size_t n, int baud, int timeout_ms)
{
	char cand[512];
	char *uci = ouci_get("openfi_modem", "modem", "at_port");

	if (uci && *uci && strcmp(uci, "auto") != 0) {
		snprintf(out, n, "%s", uci);
		free(uci);
		return 0;
	}
	free(uci);

	/* 先看常见候选 */
	static const char *common[] = {
		"/dev/ttyUSB2", "/dev/ttyUSB3", "/dev/ttyUSB1", "/dev/ttyUSB0",
		"/dev/ttyACM0",
	};
	for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
		if (!osys_exists(common[i]))
			continue;
		if (probe_port(common[i], baud, timeout_ms)) {
			snprintf(out, n, "%s", common[i]);
			return 0;
		}
	}
	/* 再全扫一遍 */
	if (at_list_ports(cand, sizeof(cand)) > 0) {
		for (char *p = strtok(cand, ","); p; p = strtok(NULL, ",")) {
			if (probe_port(p, baud, timeout_ms)) {
				snprintf(out, n, "%s", p);
				return 0;
			}
		}
	}
	/* 兜底:哪怕探不通也报默认口,方便界面上提示"没找到模组" */
	snprintf(out, n, "%s", DEFAULT_PORT);
	return -1;
}

/* ---------------- 采样 ---------------- */

struct md_cfg {
	int baud;
	int timeout_ms;
	int period;
	int reset_gpio;
};

static void cfg_load(struct md_cfg *c)
{
	c->baud = (int)ouci_get_int("openfi_modem", "modem", "baud", 115200);
	c->timeout_ms = (int)ouci_get_int("openfi_modem", "modem", "timeout_ms", 1200);
	c->period = (int)ouci_get_int("openfi_modem", "modem", "period", 15);
	if (c->period < 5)
		c->period = 5;
	if (c->period > 300)
		c->period = 300;
	c->reset_gpio = ouci_get_bool("openfi_modem", "modem", "reset_gpio", 1);
}

/* 发一条命令并取响应;成功返回 0 */
static int ask(struct at_port *p, const char *cmd, int timeout_ms, struct at_resp *r)
{
	if (at_cmd(p, cmd, timeout_ms, r) != 0)
		return -1;
	return 0;
}

/*
 * 问一遍模组,填满状态。
 * 每条命令都独立判成败 —— 某一条不支持(比如固件裁掉了 +QENG)不能让整轮采集失败。
 */
static void collect_state(struct at_port *p, const struct md_cfg *cfg, struct md_state *s)
{
	struct at_resp r;
	const char *l;

	md_state_init(s);
	copy_str(s->at_port, sizeof(s->at_port), p->path);

	/* SIM 状态 */
	if (ask(p, "AT+CPIN?", cfg->timeout_ms, &r) == 0)
		s->sim = md_parse_cpin(at_find(&r, "+CPIN:") ? at_find(&r, "+CPIN:") :
				       (r.result == AT_RES_CME ? "CME ERROR: 10" : ""));

	/* 标识 */
	if (ask(p, "AT+CGSN", cfg->timeout_ms, &r) == 0 && r.n > 0 &&
	    md_parse_imei(r.lines[0], s->imei, sizeof(s->imei)) == 0)
		s->ok = 1;
	if (ask(p, "AT+CIMI", cfg->timeout_ms, &r) == 0 && r.n > 0)
		md_parse_imsi(r.lines[0], s->imsi, sizeof(s->imsi));
	if (ask(p, "AT+QCCID", cfg->timeout_ms, &r) == 0 && (l = at_find(&r, "+QCCID:")))
		md_parse_iccid(l, s->iccid, sizeof(s->iccid));
	else if (ask(p, "AT+CCID", cfg->timeout_ms, &r) == 0 && r.n > 0)
		md_parse_iccid(r.lines[0], s->iccid, sizeof(s->iccid));
	if (ask(p, "AT+CGMM", cfg->timeout_ms, &r) == 0 && r.n > 0) {
		md_parse_text(r.lines[0], s->model, sizeof(s->model));
		s->ok = 1;
	}
	if (ask(p, "AT+CGMR", cfg->timeout_ms, &r) == 0 && r.n > 0)
		md_parse_text(r.lines[0], s->firmware, sizeof(s->firmware));

	/* 信号 / 注网 */
	if (ask(p, "AT+CSQ", cfg->timeout_ms, &r) == 0) {
		l = at_find(&r, "+CSQ:");
		if (l && md_parse_csq(l, &s->rssi) == 0)
			s->bars = md_rssi_to_bars(s->rssi);
	}
	if (ask(p, "AT+COPS?", cfg->timeout_ms, &r) == 0) {
		l = at_find(&r, "+COPS:");
		if (l)
			md_parse_cops(l, s->operator, sizeof(s->operator), NULL, 0);
	}
	/* ★ 这块模组的 +COPS? 返回 +COPS: 0,0,"",11 —— 运营商名是空的。
	   用 +QNWINFO 里的 MCC/MNC(如 46000)兜底,映射成名字。 */
	if (!s->operator[0] && ask(p, "AT+QNWINFO", cfg->timeout_ms, &r) == 0) {
		l = at_find(&r, "+QNWINFO:");
		if (l) {
			char fields[8][64];
			int n = at_csv(l, fields, 8);

			if (n >= 2) {
				const char *op = md_operator_from_plmn(fields[1]);

				if (op[0])
					copy_str(s->operator, sizeof(s->operator), op);
			}
		}
	}
	if (ask(p, "AT+CEREG?", cfg->timeout_ms, &r) == 0) {
		l = at_find(&r, "+CEREG:");
		if (!l)
			l = at_find(&r, "+CREG:");
		if (l)
			s->reg = md_parse_creg(l);
	} else if (ask(p, "AT+CREG?", cfg->timeout_ms, &r) == 0) {
		l = at_find(&r, "+CREG:");
		if (l)
			s->reg = md_parse_creg(l);
	}

	/* 服务小区(RSRP/RSRQ/SINR + 制式/频段)
	 *
	 * ★ 优先用 +QCAINFO:它的字段布局固定。
	 *   +QENG="servingcell" 的字段数【随固件版本变】—— 真机上实测比我们
	 *   注释里记的多一个,导致 RSRP/RSRQ/SINR 整体错位,解析出 RSRP=+100
	 *   这种不可能的值(RSRP 一定是负数)。所以 QENG 只当兜底。
	 */
	if (ask(p, "AT+QCAINFO", cfg->timeout_ms, &r) == 0) {
		struct md_cell c;

		l = at_find(&r, "+QCAINFO:");
		if (l && md_parse_qcainfo(l, &c) == 0) {
			snprintf(s->band, sizeof(s->band), "%s", c.band);
			s->rsrp = c.rsrp;
			s->rsrq = c.rsrq;
			s->sinr = c.sinr;
		}
	}
	if (ask(p, "AT+QENG=\"servingcell\"", cfg->timeout_ms * 2, &r) == 0) {
		struct md_cell c;

		l = at_find(&r, "+QENG:");
		if (l && md_parse_qeng_serving(l, &c) == 0) {
			/* 制式以 QENG 为准(它一定给)*/
			snprintf(s->network, sizeof(s->network), "%s", c.rat);
			if (!s->band[0])
				snprintf(s->band, sizeof(s->band), "%s", c.band);
			/* 信号只有 QCAINFO 没给上时才用 QENG 的 */
			if (s->rsrp == -1000)
				s->rsrp = c.rsrp;
			if (s->rsrq == -1000)
				s->rsrq = c.rsrq;
			if (s->sinr == -1000)
				s->sinr = c.sinr;

			/* ★ 注网状态:QENG 第 2 个字段(CONNECT/DISCONNECT/NOCONN)
			   比 CREG/CEREG 的 stat 可靠 —— 真机上 CREG 一直是 0(未注册),
			   但模组明明连着、能上网。 */
			if (md_cell_state_online(l))
				s->reg = MD_REG_HOME;
		}
	}
	/* 有些固件靠 +QNWINFO 报制式 */
	if (!s->network[0] && ask(p, "AT+QNWINFO", cfg->timeout_ms, &r) == 0) {
		l = at_find(&r, "+QNWINFO:");
		if (l) {
			char fields[AT_MAX_FIELDS][64];
			int n = at_csv(l, fields, AT_MAX_FIELDS);

			if (n >= 1)
				copy_str(s->network, sizeof(s->network), fields[0]);
			if (n >= 3)
				copy_str(s->band, sizeof(s->band), fields[2]);
		}
	}

	/* 温度(多行:一行一个传感器)*/
	if (ask(p, "AT+QTEMP", cfg->timeout_ms, &r) == 0) {
		for (int i = 0; i < r.n; i++) {
			if (strncmp(r.lines[i], "+QTEMP:", 7) == 0)
				md_parse_qtemp(r.lines[i], &s->temp_soc, &s->temp_pa);
		}
	}

	/* ★ 数据链路(网卡名/IP/网关/DNS/收发字节/在线时长)
	   这些模组不知道 —— 是主机侧的事,从 /proc 和 sysfs 读。
	   之前这几个字段在 JSON 里一直是空的,就是因为压根没人填。 */
	md_fill_datalink(s);
}

/* ---------------- 输出 ---------------- */

/* 把状态渲染成 JSON(不带缓存文件的元信息)*/
static struct ojson *state_json(const struct md_state *s)
{
	struct ojson *o = oj_new();

	oj_bool(o, "ok", s->ok);
	oj_str(o, "imei", s->imei);
	oj_str(o, "imsi", s->imsi);
	oj_str(o, "iccid", s->iccid);
	oj_str(o, "model", s->model);
	oj_str(o, "firmware", s->firmware);
	oj_str(o, "at_port", s->at_port);

	oj_int(o, "sim", s->sim);
	oj_int(o, "sim_slot", s->sim_slot);
	oj_int(o, "sim_slots", s->sim_slots);

	oj_str(o, "operator", s->operator);
	oj_int(o, "reg", s->reg);
	oj_str(o, "network", s->network);
	oj_str(o, "band", s->band);
	oj_int(o, "rssi", s->rssi);
	oj_int(o, "rsrp", s->rsrp);
	oj_int(o, "rsrq", s->rsrq);
	oj_int(o, "sinr", s->sinr);
	oj_int(o, "bars", s->bars);

	oj_int(o, "temp_soc", s->temp_soc);
	oj_int(o, "temp_pa", s->temp_pa);

	oj_str(o, "ifname", s->ifname);
	oj_str(o, "ipaddr", s->ipaddr);
	oj_str(o, "gateway", s->gateway);
	oj_str(o, "dns1", s->dns1);
	oj_int(o, "rx_bytes", s->rx_bytes);
	oj_int(o, "tx_bytes", s->tx_bytes);
	oj_int(o, "uptime", s->uptime);
	return o;
}

static void write_cache(const struct md_state *s)
{
	struct ojson *o = state_json(s);
	char *js = oj_render(o);

	if (js) {
		char tmp[256], fin[256];
		FILE *f;

		osys_path(tmp, sizeof(tmp), STATE_FILE ".tmp");
		osys_path(fin, sizeof(fin), STATE_FILE);
		f = fopen(tmp, "w");
		if (f) {
			fprintf(f, "%s\n", js);
			if (fclose(f) == 0)
				rename(tmp, fin);
		}
		free(js);
	}
	oj_free(o);

	/* ★ 风扇要的模组温度:单独写一个小文件,省得它自己开串口。
	   温度读不到时删掉,让风扇知道"这路没有"。 */
	long t = s->temp_soc;

	if (t <= -100 || t >= 200)
		t = s->temp_pa;
	if (t > -100 && t < 200) {
		char b[16];

		snprintf(b, sizeof(b), "%ld", t);
		osys_write(TEMP_FILE, b);
	} else {
		char full[256];

		osys_path(full, sizeof(full), TEMP_FILE);
		unlink(full);
	}
}

/* ---------------- 硬复位 ---------------- */

static int modem_reset(void)
{
	/* 原厂 op_reset_modem.sh:拉 lte_reset 一下。gpio-export 出来的名字。 */
	const char *pins[] = { "/sys/class/gpio/lte_reset/value", NULL };
	int did = 0;

	for (int i = 0; pins[i]; i++) {
		if (!osys_exists(pins[i]))
			continue;
		osys_write(pins[i], "1");
		usleep(300 * 1000);
		osys_write(pins[i], "0");
		did++;
	}
	if (!did)
		return oj_fail("找不到模组复位引脚(/sys/class/gpio/lte_reset 不存在)");
	olog_info("模组已复位");
	return 0;
}

/* ---------------- main ---------------- */

static void usage(void)
{
	fprintf(stderr,
		"用法: openfi-modem <命令>\n"
		"  status        综合状态 JSON(读缓存,零副作用)\n"
		"  refresh       主动查一遍模组并更新缓存\n"
		"  collect       后台采样守护进程(推荐由 procd 拉起)\n"
		"  at <命令>     透传一条 AT 命令\n"
		"  at-port       打印探测到的 AT 口\n"
		"  reset         硬复位模组\n"
		"  selftest      自检\n");
}

static int cmd_refresh(const struct md_cfg *cfg)
{
	char port[128];
	struct at_port p;
	struct md_state s;

	if (find_at_port(port, sizeof(port), cfg->baud, 600) != 0)
		olog_warn("没有口回应 AT,按默认口 %s 试", port);

	if (at_open(&p, port, cfg->baud) != 0)
		return oj_fail("打开 %s 失败: %s", port, strerror(errno));

	collect_state(&p, cfg, &s);
	at_close(&p);
	write_cache(&s);

	struct ojson *o = state_json(&s);

	if (o) {
		char *js = oj_render(o);

		if (js) {
			puts(js);
			free(js);
		}
		oj_free(o);
	}
	return s.ok ? 0 : 1;
}

static int cmd_status(void)
{
	char buf[8192];
	FILE *f;
	char full[256];
	size_t n;

	osys_path(full, sizeof(full), STATE_FILE);
	f = fopen(full, "r");
	if (!f)
		return oj_fail("还没有缓存(采样进程没在跑?)");
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	fputs(buf, stdout);
	return 0;
}

static int cmd_at(const struct md_cfg *cfg, int argc, char **argv)
{
	char port[128];
	struct at_port p;
	struct at_resp r;

	if (argc < 1)
		return oj_fail("at 需要一个命令参数");
	if (find_at_port(port, sizeof(port), cfg->baud, 600) != 0)
		olog_warn("没有口回应 AT,按默认口 %s 试", port);
	if (at_open(&p, port, cfg->baud) != 0)
		return oj_fail("打开 %s 失败", port);

	/* 多条命令用 ';' 分隔,依次发 */
	char *cmds = strdup(argv[0]);

	for (char *c = strtok(cmds, ";"); c; c = strtok(NULL, ";")) {
		char *cmd = c;

		while (*cmd == ' ')
			cmd++;
		if (!*cmd)
			continue;
		if (at_cmd(&p, cmd, 3000, &r) != 0) {
			printf("(串口读写出错)\n");
			break;
		}
		for (int i = 0; i < r.n; i++)
			printf("%s\n", r.lines[i]);
		printf("%s\n", at_result_str(r.result));
	}
	free(cmds);
	at_close(&p);
	return 0;
}

static int cmd_selftest(const struct md_cfg *cfg)
{
	char port[128];
	int bad = 0;

	printf("openfi-modem 自检\n");
	printf("  配置: 波特率 %d  超时 %dms  周期 %ds\n",
	       cfg->baud, cfg->timeout_ms, cfg->period);

	printf("  候选串口:\n");
	{
		char list[512];

		if (at_list_ports(list, sizeof(list)) > 0)
			printf("    %s\n", list);
		else {
			printf("    ❌ 一个都没有(/dev/ttyUSB* /dev/ttyACM* 都没有)\n");
			bad++;
		}
	}
	if (find_at_port(port, sizeof(port), cfg->baud, 800) == 0) {
		printf("  ✅ AT 口探测成功: %s\n", port);
		struct at_port p;

		if (at_open(&p, port, cfg->baud) == 0) {
			struct at_resp r;

			if (at_cmd(&p, "AT+CGMM", 1500, &r) == 0 && r.n > 0)
				printf("  ✅ 模组型号: %s\n", r.lines[0]);
			else
				printf("  ⚠️ 能开串口但读不到型号\n");
			at_close(&p);
		}
	} else {
		printf("  ❌ 没有口回应 AT\n");
		bad++;
	}
	{
		char full[256];

		osys_path(full, sizeof(full), STATE_FILE);
		printf("  缓存文件: %s\n", osys_exists(STATE_FILE) ? "有" : "没有(采样进程没跑)");
		printf("  复位引脚: %s\n",
		       osys_exists("/sys/class/gpio/lte_reset/value") ? "有" : "没有");
	}
	printf("\n  %s\n", bad ? "自检发现问题" : "自检通过");
	return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
	struct md_cfg cfg;

	olog_init("openfi-modem");
	/*
	 * 允许用环境变量换掉 sysfs / UCI 的根目录 —— 于是本机和虚拟机里
	 * 都能用一个假模组(PTY)把完整流程跑一遍,不需要真硬件。
	 * (风扇程序里也做了同样的事。)
	 */
	{
		const char *r = getenv("OPENFI_SYSFS_ROOT");

		if (r && *r)
			osys_set_root(r);
		r = getenv("OPENFI_UCI_ROOT");
		if (r && *r)
			ouci_set_root(r);
	}
	cfg_load(&cfg);

	if (argc < 2) {
		usage();
		return 1;
	}
	const char *act = argv[1];

	if (!strcmp(act, "status"))
		return cmd_status();
	if (!strcmp(act, "refresh"))
		return cmd_refresh(&cfg);
	if (!strcmp(act, "selftest"))
		return cmd_selftest(&cfg);
	if (!strcmp(act, "reset"))
		return modem_reset();
	if (!strcmp(act, "at-port")) {
		char port[128];

		find_at_port(port, sizeof(port), cfg.baud, 600);
		printf("%s\n", port);
		return 0;
	}
	if (!strcmp(act, "at"))
		return cmd_at(&cfg, argc - 2, argv + 2);
	if (!strcmp(act, "-h") || !strcmp(act, "--help")) {
		usage();
		return 0;
	}

	if (!strcmp(act, "collect")) {
		signal(SIGTERM, on_term);
		signal(SIGINT, on_term);
		signal(SIGHUP, on_term);
		olog_info("采样进程启动,周期 %ds", cfg.period);
		while (!g_stop) {
			char port[128];
			struct at_port p;
			struct md_state s;

			if (find_at_port(port, sizeof(port), cfg.baud, 600) == 0 &&
			    at_open(&p, port, cfg.baud) == 0) {
				collect_state(&p, &cfg, &s);
				at_close(&p);
				write_cache(&s);
				olog_info("采样完成:信号 %d 格,温度 %ld/%ld",
					  s.bars, s.temp_soc, s.temp_pa);
			} else {
				olog_warn("模组没响应,跳过这一轮");
			}
			for (int i = 0; i < cfg.period * 10 && !g_stop; i++)
				usleep(100 * 1000);
		}
		return 0;
	}

	usage();
	return 1;
}
