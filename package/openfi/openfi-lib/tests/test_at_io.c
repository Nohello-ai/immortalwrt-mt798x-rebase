/* openfi_at_io 集成测试 —— 用 pty 模拟一个真的 5G 模组
 *
 * 这不是"mock 掉 I/O",而是走真实的 open/tcsetattr/write/read 路径:
 *   parent(被测试代码)  ← pty 主端
 *   child (假模组)       ← pty 从端
 * 于是 termios 配置、命令结尾的 \r、超时、终止行识别、解析 —— 全都被真正执行了。
 */
#define _GNU_SOURCE
#include "../src/openfi_at_io.h"
#include "t.h"

#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 假模组:认得几条命令,连"回显"和 URC 都照着真机学 */
static void fake_modem(int fd)
{
	char line[256];
	size_t used = 0;
	int sent_urc = 0;

	for (;;) {
		char c;
		ssize_t n = read(fd, &c, 1);

		if (n <= 0)
			break;
		if (c == '\r') {
			line[used] = '\0';
			used = 0;
			/* 回显(真机默认开着 ATE1) */
			dprintf(fd, "%s\r\r\n", line);

			if (!strcmp(line, "AT+CSQ")) {
				dprintf(fd, "+CSQ: 21,98\r\n\r\nOK\r\n");
			} else if (!strcmp(line, "AT+CPIN?")) {
				dprintf(fd, "+CPIN: READY\r\nOK\r\n");
			} else if (!strcmp(line, "AT+QCFG=\"band\"")) {
				dprintf(fd, "+CME ERROR: 4\r\n");
			} else if (!strcmp(line, "AT+QTEMP")) {
				dprintf(fd, "+QTEMP: \"soc-thermal\",54\r\n"
					    "+QTEMP: \"pa-thermal\",47\r\nOK\r\n");
			} else if (!strcmp(line, "AT+SILENT")) {
				/* 故意不回 —— 测超时 */
			} else if (!strcmp(line, "AT+URC")) {
				if (!sent_urc) {
					dprintf(fd, "+QIND: \"acT\",\"NR5G-SA\"\r\n");
					sent_urc = 1;
				}
				dprintf(fd, "+QENG: \"servingcell\",\"NOCONN\",\"NR5G-SA\"\r\nOK\r\n");
			} else {
				dprintf(fd, "ERROR\r\n");
			}
			continue;
		}
		if (c == '\n')
			continue;
		if (used < sizeof(line) - 1)
			line[used++] = c;
	}
	_exit(0);
}

int main(void)
{
	int master, slave;
	struct at_port port;
	struct at_resp r;
	char slave_name[128];
	pid_t child;

	printf("── 建立 pty + 启动假模组 ──\n");
	if (openpty(&master, &slave, slave_name, NULL, NULL) != 0) {
		printf("  (本机没有 pty,跳过集成测试)\n");
		printf("\n  通过 0 项,失败 0 项\n");
		return 0;
	}
	/*
	 * pty 的规矩(我第一版写反了,测试立刻抓到):
	 *   被模拟的那个"串口设备" = slave 端(/dev/pts/N)—— 应用该开它
	 *   假模组(线路另一端)     = master 端
	 * 写 master → slave 可读;写 slave → master 可读。
	 * 所以:子进程拿 master 当"模组",父进程按设备名开 slave。
	 */
	child = fork();
	if (child == 0) {
		close(slave);
		fake_modem(master);
		_exit(0);
	}
	close(master);

	printf("── 打开串口(真实 termios 配置)──\n");
	T_EQ_INT(at_open(&port, slave_name, 115200), 0);
	T_OK(port.fd >= 0);

	printf("── 普通命令:CSQ ──\n");
	T_EQ_INT(at_cmd(&port, "AT+CSQ", 2000, &r), 0);
	T_EQ_INT(r.result, AT_RES_OK);
	T_EQ_INT(at_csv_int(at_find(&r, "+CSQ:"), 0, -1), 21);   /* 回显没污染 */
	T_EQ_INT(at_csv_int(at_find(&r, "+CSQ:"), 1, -1), 98);

	printf("── 字符串应答 ──\n");
	T_EQ_INT(at_cmd(&port, "AT+CPIN?", 2000, &r), 0);
	T_EQ_STR(at_find(&r, "+CPIN:"), "+CPIN: READY");

	printf("── 模组不支持的命令 ──\n");
	T_EQ_INT(at_cmd(&port, "AT+QCFG=\"band\"", 2000, &r), 0);
	T_EQ_INT(r.result, AT_RES_CME);
	T_EQ_INT(r.err_code, 4);

	printf("── 多行应答 ──\n");
	T_EQ_INT(at_cmd(&port, "AT+QTEMP", 2000, &r), 0);
	T_EQ_INT(r.n, 2);
	T_EQ_INT(at_csv_int(at_find(&r, "+QTEMP:"), 1, -1), 54);

	printf("── URC 干扰 ──\n");
	T_EQ_INT(at_cmd(&port, "AT+URC", 2000, &r), 0);
	T_OK(at_find(&r, "+QENG:") != NULL);

	printf("── ★ 超时(模组不回话)──\n");
	{
		struct timespec t0, t1;
		double ms;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		T_EQ_INT(at_cmd(&port, "AT+SILENT", 800, &r), 0);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
		T_EQ_INT(r.result, AT_RES_TIMEOUT);
		T_OK(ms >= 700 && ms < 2500);      /* 超时真的生效了,不是秒回也不是卡死 */
		printf("     实际等了 %.0f ms(设定 800)\n", ms);
	}

	printf("── 关闭 + 收尾 ──\n");
	at_close(&port);
	T_EQ_INT(port.fd, -1);
	kill(child, SIGTERM);
	waitpid(child, NULL, 0);

	T_SUMMARY();
}
