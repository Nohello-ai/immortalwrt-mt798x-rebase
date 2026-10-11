/*
 * openfi-fan —— OpenFi 6C 风扇温控守护进程
 *
 * 设计:决策全在 libopenfi 的 fan_state_* 里(那部分有 43 项单元测试覆盖),
 *       这个程序只做 I/O 粘合:读配置 → 读温度 → 问状态机 → 写 PWM。
 *
 * 用法:
 *   openfi-fan              守护进程(由 procd 拉起)
 *   openfi-fan --once       跑一轮就退出(调试 / CI 冒烟)
 *   openfi-fan --json       输出当前状态 JSON 给 LuCI
 *   openfi-fan --speed N    手动把转速设成 N(0..100),写完就退
 *   openfi-fan --selftest   上机自检:检查 PWM / 温度源 / 配置是否可用
 *
 * 关于 PWM(和原厂 op_fan.sh 一致):
 *   本板 25 kHz,反极性 —— duty_cycle 越小风扇越快,duty=0 是全速。
 *   两路都要写(pwm0 + pwm1),写同一个 duty。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "openfi_fanctl.h"
#include "openfi_json.h"
#include "openfi_log.h"
#include "openfi_switch.h"
#include "openfi_sysfs.h"
#include "openfi_temp.h"
#include "openfi_uci.h"

#define PWM_CHIP_DEFAULT "/sys/class/pwm/pwmchip0"
#define PERIOD_NS_DEFAULT 40000		/* 25 kHz */
#define STATE_FILE "/var/run/openfi-fan.json"
#define RUN_DIR "/var/run"

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_reload;

static void on_term(int sig) { (void)sig; g_stop = 1; }
static void on_hup(int sig)  { (void)sig; g_reload = 1; }

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- 配置 ---------------- */

struct fan_run_cfg {
	struct fan_cfg curve;
	int period;
	int silent_max_temp;
	int silent_when_hi;
	char pwm_chip[96];
	int period_ns;
	int channels[4];
	int nchan;
};

static void cfg_load(struct fan_run_cfg *rc)
{
	int min_speed;

	/* UCI 里没有就用默认 —— 不能因为配置缺项就不转风扇 */
	min_speed = (int)ouci_get_int("openfi_fan", "fan", "min_speed", 10);
	fan_cfg_default(&rc->curve, min_speed);

	rc->curve.t1 = (int)ouci_get_int("openfi_fan", "fan", "temp1", rc->curve.t1);
	rc->curve.t2 = (int)ouci_get_int("openfi_fan", "fan", "temp2", rc->curve.t2);
	rc->curve.t3 = (int)ouci_get_int("openfi_fan", "fan", "temp3", rc->curve.t3);
	rc->curve.t4 = (int)ouci_get_int("openfi_fan", "fan", "temp4", rc->curve.t4);
	rc->curve.s1 = (int)ouci_get_int("openfi_fan", "fan", "speed1", rc->curve.s1);
	rc->curve.s2 = (int)ouci_get_int("openfi_fan", "fan", "speed2", rc->curve.s2);
	rc->curve.s3 = (int)ouci_get_int("openfi_fan", "fan", "speed3", rc->curve.s3);
	rc->curve.s4 = (int)ouci_get_int("openfi_fan", "fan", "speed4", rc->curve.s4);
	rc->curve.emergency_temp =
		(int)ouci_get_int("openfi_fan", "fan", "emergency_temp",
				  rc->curve.emergency_temp);

	rc->period = (int)ouci_get_int("openfi_fan", "fan", "period", 5);
	if (rc->period < 1)
		rc->period = 1;
	if (rc->period > 60)
		rc->period = 60;

	rc->silent_max_temp =
		(int)ouci_get_int("openfi_fan", "fan", "silent_max_temp", 75);
	/* 开关哪一档算静默:默认高电平档(与原厂 "high: led off" 的注释一致) */
	rc->silent_when_hi = ouci_get_bool("openfi_fan", "fan", "silent_when_hi", 1);

	const char *chip = ouci_get_def("openfi_fan", "fan", "pwm_chip", PWM_CHIP_DEFAULT);

	snprintf(rc->pwm_chip, sizeof(rc->pwm_chip), "%s", chip);
	rc->period_ns = (int)ouci_get_int("openfi_fan", "fan", "period_ns",
					  PERIOD_NS_DEFAULT);
	/* 两路是板级事实,不做成配置项 —— 配错了风扇就不转 */
	rc->channels[0] = 0;
	rc->channels[1] = 1;
	rc->nchan = 2;

	if (!fan_cfg_valid(&rc->curve)) {
		olog_warn("配置的温度曲线不合法,退回默认曲线");
		fan_cfg_default(&rc->curve, min_speed);
	}
}

/* ---------------- PWM ---------------- */

static int pwm_path(char *out, size_t n, const struct fan_run_cfg *rc,
		    int ch, const char *leaf)
{
	if (ch < 0)
		return snprintf(out, n, "%s/%s", rc->pwm_chip, leaf);
	return snprintf(out, n, "%s/pwm%d/%s", rc->pwm_chip, ch, leaf);
}

static int pwm_export(const struct fan_run_cfg *rc, int ch)
{
	char p[256], v[16];

	pwm_path(p, sizeof(p), rc, ch, "");
	/* 目录已经在 = 已经 export 过 */
	if (osys_exists(p))
		return 0;
	pwm_path(p, sizeof(p), rc, -1, "export");
	snprintf(v, sizeof(v), "%d", ch);
	return osys_write(p, v);
}

static int pwm_apply(const struct fan_run_cfg *rc, int speed)
{
	long duty = fan_duty_for(speed, rc->period_ns);
	char p[256], v[32];
	int failed = 0;

	for (int i = 0; i < rc->nchan; i++) {
		int ch = rc->channels[i];

		if (pwm_export(rc, ch) != 0) {
			olog_err("pwm%d export 失败", ch);
			failed = 1;
			continue;
		}
		/* 新 export 的通道 period=0,Linux 会拒绝一切 duty 写入 ——
		   所以必须先写 period */
		pwm_path(p, sizeof(p), rc, ch, "period");
		snprintf(v, sizeof(v), "%d", rc->period_ns);
		if (osys_write(p, v) != 0) {
			olog_err("pwm%d period 写入失败", ch);
			failed = 1;
			continue;
		}
		pwm_path(p, sizeof(p), rc, ch, "duty_cycle");
		snprintf(v, sizeof(v), "%ld", duty);
		if (osys_write(p, v) != 0) {
			olog_err("pwm%d duty 写入失败", ch);
			failed = 1;
			continue;
		}
		pwm_path(p, sizeof(p), rc, ch, "enable");
		if (osys_write(p, "1") != 0) {
			olog_err("pwm%d enable 失败", ch);
			failed = 1;
		}
	}
	return failed ? -1 : 0;
}

/* ---------------- 状态输出(给 LuCI) ---------------- */

static void write_state(const struct fan_run_cfg *rc, const struct fan_state *st,
			const struct temp_cfg *tc, const int temps[3], int unified,
			int applied)
{
	struct ojson *o = oj_new();
	char buf[32];

	oj_bool(o, "ok", 1);
	oj_int(o, "output", applied);		/* 实际下发成功的转速 */
	oj_int(o, "target", st->last_applied);	/* 状态机算出来的目标转速 */
	oj_str(o, "reason", fan_reason_str(st->last_reason));
	oj_bool(o, "silent", st->silent);
	oj_str(o, "switch", sw_level_str(sw_read_level(NULL, "func")));
	if (unified == TEMP_INVALID)
		oj_null(o, "temp");
	else
		oj_int(o, "temp", unified);

#define SRC(k, i) do {						\
	if (temps[i] == TEMP_INVALID) oj_null(o, k);		\
	else oj_int(o, k, temps[i]);				\
} while (0)
	SRC("cpu", TEMP_CPU);
	SRC("wifi", TEMP_WIFI);
	SRC("modem", TEMP_MODEM);
#undef SRC

	oj_int(o, "period", rc->period);
	oj_int(o, "t1", rc->curve.t1); oj_int(o, "t2", rc->curve.t2);
	oj_int(o, "t3", rc->curve.t3); oj_int(o, "t4", rc->curve.t4);
	oj_int(o, "s1", rc->curve.s1); oj_int(o, "s2", rc->curve.s2);
	oj_int(o, "s3", rc->curve.s3); oj_int(o, "s4", rc->curve.s4);
	oj_int(o, "min_speed", rc->curve.min_speed);
	oj_int(o, "emergency_temp", rc->curve.emergency_temp);
	oj_int(o, "silent_max_temp", rc->silent_max_temp);
	snprintf(buf, sizeof(buf), "%s", tc->wifi_iface);
	oj_str(o, "wifi_iface", buf);

	/*
	 * 写文件 + 同时打到 stdout(前者给 LuCI 轮询,后者给 --json / 调试)。
	 * 【注意】这里的路径必须走 osys_path —— 否则 root 注入只对"读"生效、
	 * "写"还是落在写死的绝对路径上,本地冒烟测试就会出现"写得进去读不出来"。
	 */
	/*
	 * 先渲染成字符串,自己写文件,再打到 stdout。
	 * 这样一份数据两处用,不用为了写文件再构造一次对象。
	 */
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
				rename(tmp, fin);  /* 原子替换,LuCI 不会读到半截 JSON */
		}
		puts(js);
		free(js);
	}
	oj_free(o);
}

/*
 * 读整个文件(状态 JSON 有几百字节,不能用 osys_read ——
 * 那个是按"读 sysfs 的一个值"设计的,fgets 到 256 字节就截断了)。
 * 返回读到的字节数,失败返回 -1。
 */
static int read_whole_file(const char *path, char *out, size_t outlen)
{
	char full[256];
	FILE *f;
	size_t n;

	if (!path || !out || outlen < 2)
		return -1;
	osys_path(full, sizeof(full), path);
	f = fopen(full, "r");
	if (!f)
		return -1;
	n = fread(out, 1, outlen - 1, f);
	fclose(f);
	out[n] = '\0';
	return (int)n;
}

/* ---------------- 自检 ---------------- */

static int selftest(const struct fan_run_cfg *rc, const struct temp_cfg *tc)
{
	int bad = 0;

	printf("openfi-fan 自检\n");

	printf("  配置: 曲线 %d/%d/%d/%d → %d/%d/%d/%d  下限%d  紧急%d  周期%ds\n",
	       rc->curve.t1, rc->curve.t2, rc->curve.t3, rc->curve.t4,
	       rc->curve.s1, rc->curve.s2, rc->curve.s3, rc->curve.s4,
	       rc->curve.min_speed, rc->curve.emergency_temp, rc->period);
	printf("  配置曲线合法: %s\n", fan_cfg_valid(&rc->curve) ? "是" : "否(会退回默认)");

	printf("  PWM: %s  %d 路 (", rc->pwm_chip, rc->nchan);
	for (int i = 0; i < rc->nchan; i++)
		printf("%s%d", i ? "," : "", rc->channels[i]);
	printf(")  周期 %d ns\n", rc->period_ns);
	if (!osys_exists(rc->pwm_chip)) {
		printf("    ❌ %s 不存在 —— PWM 控制器没起来(pwm-fan 有没有抢?)\n",
		       rc->pwm_chip);
		bad++;
	}

	printf("  温度源:\n");
	for (int i = 0; i < TEMP_SRC_COUNT; i++) {
		static const char *nm[] = { "cpu", "wifi", "modem" };
		int t = temp_read((enum temp_src)i, tc);

		if (t == TEMP_INVALID)
			printf("    %-6s ❌ 读不到\n", nm[i]);
		else
			printf("    %-6s ✅ %d ℃\n", nm[i], t);
	}
	{
		int t3[3], u;

		if (temp_read_all(tc, t3, &u) == 0) {
			printf("  ❌ 三路温度全读不到 —— 曲线会一直用兜底值\n");
			bad++;
		} else {
			printf("  统一温度(取最高): %d ℃\n", u);
		}
	}
	printf("\n  %s\n", bad ? "自检发现问题(见上)" : "自检通过");
	return bad ? 1 : 0;
}

/* ---------------- main ---------------- */

static void usage(void)
{
	fprintf(stderr,
		"用法: openfi-fan [选项]\n"
		"  --status      输出上次的状态(只读,零副作用 —— 给 LuCI 轮询用)\n"
		"  --once        跑一轮就退出\n"
		"  --json        输出状态 JSON\n"
		"  --speed N     手动设转速 0..100\n"
		"  --selftest    上机自检\n"
		"  --speed N     固定转速 N%%(写进配置持久生效)\n"
		"  --speed auto  回到温度曲线\n"
		"  -h, --help    本帮助\n");
}

int main(int argc, char **argv)
{
	static struct option opts[] = {
		{ "once",     no_argument,       NULL, '1' },
		{ "status",   no_argument,       NULL, 'S' },
		{ "json",     no_argument,       NULL, 'j' },
		{ "speed",    required_argument, NULL, 's' },
		{ "selftest", no_argument,       NULL, 't' },
		{ "help",     no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int opt_once = 0, opt_json = 0, opt_selftest = 0, opt_speed = -1;
	int opt_speed_auto = 0;
	int opt_status = 0;
	struct fan_run_cfg rc;
	struct temp_cfg tc;
	struct fan_state st;
	int temps[TEMP_SRC_COUNT], unified;
	int applied = -1;
	int rc_code = 0;

	while (1) {
		int c = getopt_long(argc, argv, "h", opts, NULL);

		if (c == -1)
			break;
		switch (c) {
		case '1': opt_once = 1; break;
		case 'S': opt_status = 1; opt_once = 1; break;
		case 'j': opt_json = 1; opt_once = 1; break;
		case 't': opt_selftest = 1; opt_once = 1; break;
		case 's':
			/* --speed auto 表示"回到温度曲线" */
			if (!strcmp(optarg, "auto")) {
				opt_speed_auto = 1;
			} else {
				opt_speed = atoi(optarg);
				if (opt_speed < 0)
					opt_speed = 0;
				if (opt_speed > 100)
					opt_speed = 100;
			}
			opt_once = 1;
			break;
		case 'h': default: usage(); return (c == 'h') ? 0 : 1;
		}
	}

	olog_init("openfi-fan");
	/*
	 * 【可测性】允许用环境变量把 sysfs / UCI 的根换掉。
	 * 于是开发机上可以造一棵假的 pwmchip + 假的配置文件,
	 * 把整个守护进程(含 PWM 写入、JSON 输出)真跑一遍 ——
	 * 而不是只能上机看风扇转不转。
	 */
	{
		const char *r = getenv("OPENFI_SYSFS_ROOT");

		if (r && *r)
			osys_set_root(r);
		r = getenv("OPENFI_UCI_ROOT");
		if (r && *r)
			ouci_set_root(r);
	}
	cfg_load(&rc);
	temp_cfg_default(&tc);
	/* UCI 里可以覆盖温度来源(方便别的板子/调试) */
	{
		/*
		 * ★ 注意:ouci_get_def 在配置项缺失时【返回传进去的默认值指针】。
		 * 如果默认值就是目标缓冲区本身,再 snprintf 回去就是"自己拷自己"
		 * ——重叠拷贝属于未定义行为,实测会把缓冲区内容搅坏
		 * (JSON 里冒出控制字符就是这个引起的)。
		 * 所以先读进临时缓冲,再赋值。
		 */
		char tmp[sizeof(tc.wifi_iface)];
		const char *p = ouci_get_def("openfi_fan", "fan", "wifi_iface", "ra0");

		snprintf(tmp, sizeof(tmp), "%s", p);
		memcpy(tc.wifi_iface, tmp, sizeof(tmp));
		tc.wifi_enabled = ouci_get_bool("openfi_fan", "fan", "wifi_temp", 1);
	}

	/* --status:只把上次写的状态文件读出来打印,不读温度、不碰 PWM ——
	   LuCI 每几秒轮询一次,绝不能有副作用。 */
	if (opt_status) {
		char *buf = malloc(4096);

		if (buf && read_whole_file(STATE_FILE, buf, 4096) > 0) {
			fputs(buf, stdout);
			fputc('\n', stdout);
			free(buf);
			return 0;
		}
		free(buf);
		return oj_fail("还没有状态文件(守护进程没起来?)");
	}

	if (opt_selftest)
		return selftest(&rc, &tc);

	if (fan_state_init(&st, &rc.curve, rc.silent_max_temp) != 0) {
		olog_warn("状态机初始化失败(配置非法),用默认曲线重来");
		fan_cfg_default(&rc.curve, 10);
		if (fan_state_init(&st, &rc.curve, rc.silent_max_temp) != 0) {
			olog_err("还是失败,退出");
			return 1;
		}
	}
	/*
	 * 静默档:直接读物理开关电平。
	 * 【不再依赖 /var/run/openfi-silent 标志文件】——
	 * 那个文件只在按键事件发生时更新,重启后若没人拨过开关就是空的,
	 * 而实际开关可能正停在静默档。每轮读电平则天然正确。
	 */
	{
		int sl = sw_is_silent(sw_read_level(NULL, "func"), rc.silent_when_hi);

		if (sl >= 0)
			st.silent = sl;
		olog_info("启动时的开关状态:%s", st.silent ? "静默档" : "正常档");
	}

	if (opt_speed_auto) {
		/*
		 * 回到温度曲线。清掉 manual_speed 并把 mode 改回 auto ——
		 * 只改 mode 也行,但把速度一并清掉,免得界面上的旧值造成误解。
		 */
		if (ouci_set("openfi_fan", "fan", "mode", "auto") != 0)
			return oj_fail("写配置失败");
		olog_info("已回到温度曲线");
		return 0;
	}

	if (opt_speed >= 0) {
		/*
		 * ★ 以前这里只是"写一次 PWM 然后退出"。
		 *   问题是守护进程每个周期都会按状态机重新计算并覆盖 PWM ——
		 *   温度一动(几秒到几十秒)用户的设置就没了,而且状态页显示的
		 *   还是守护进程缓存的值,看起来"设了没反应"。
		 *   现在把选择写进配置:守护进程按 mode=manual + manual_speed 走,
		 *   一直到用户显式改回 auto 为止;顺手也立刻写一次,
		 *   不用等到下个周期。
		 */
		char buf[16];

		/*
		 * ★ 顺序很重要:先写 PWM(立刻见效),再持久化配置。
		 *   反过来的话,配置写不进去(比如配置文件所在的分区只读、
		 *   或者跑在没有配置根目录的测试环境里)就直接返回了,
		 *   PWM 根本没动 —— 用户看到的现象是"点了没反应"。
		 *   配置只是"下次重启后还记得",写失败不该拦住当前动作。
		 */
		if (pwm_apply(&rc, opt_speed) != 0)
			return oj_fail("PWM 写入失败");

		snprintf(buf, sizeof(buf), "%d", opt_speed);
		if (ouci_set("openfi_fan", "fan", "mode", "manual") != 0 ||
		    ouci_set("openfi_fan", "fan", "manual_speed", buf) != 0)
			olog_warn("转速已生效,但写配置失败 —— 重启后会回到温度曲线");
		else
			olog_info("已固定转速 %d%%(写进配置,改回请用 --speed auto)", opt_speed);
		return 0;
	}

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGHUP, on_hup);
	signal(SIGPIPE, SIG_IGN);

	olog_info("启动:曲线 %d/%d/%d/%d → %d/%d/%d/%d,周期 %ds,静默=%d",
		  rc.curve.t1, rc.curve.t2, rc.curve.t3, rc.curve.t4,
		  rc.curve.s1, rc.curve.s2, rc.curve.s3, rc.curve.s4,
		  rc.period, st.silent);

	for (;;) {
		int got = temp_read_all(&tc, temps, &unified);
		int want;

		/* 每轮再看一眼物理开关(一次小文件读,代价可忽略)。
		   这样开关拨动后最多等一个周期就生效 —— 即使没收到按键事件
		   (比如事件被别的程序抢消费掉了)也不会卡在旧状态。 */
		{
			int sl = sw_is_silent(sw_read_level(NULL, "func"), rc.silent_when_hi);

			if (sl >= 0)
				fan_state_set_silent(&st, sl);
		}

		if (opt_once && opt_json)
			applied = st.last_applied;
		want = fan_state_step(&st, unified, now_ms());
		if (want >= 0 && want != applied) {
			if (pwm_apply(&rc, want) != 0) {
				olog_err("PWM 下发失败(转速 %d)", want);
				rc_code = 1;
			} else {
				applied = want;
				olog_info("转速 → %d%%(原因 %s,温度 %d℃,读到 %d 路)",
					  want, fan_reason_str(st.last_reason),
					  unified == TEMP_INVALID ? -1 : unified, got);
			}
		}

		write_state(&rc, &st, &tc, temps, unified, applied);
		if (opt_once)
			break;

		/* 分片睡眠:好让信号能及时进来(SIGHUP 要立刻重判) */
		for (int i = 0; i < rc.period * 10 && !g_stop && !g_reload; i++)
			usleep(100 * 1000);

		if (g_reload) {
			g_reload = 0;
			olog_info("收到 SIGHUP:重新读配置");
			cfg_load(&rc);
			fan_state_init(&st, &rc.curve, rc.silent_max_temp);
			applied = -1;
		}
		if (g_stop)
			break;
	}

	if (!opt_once) {
		/* 退出前把风扇开到【全速】——守护进程挂了也不能让板子过热 */
		olog_info("收到停止信号:退出前把风扇开到全速以保证散热");
		pwm_apply(&rc, 100);
	}
	return rc_code;
}
