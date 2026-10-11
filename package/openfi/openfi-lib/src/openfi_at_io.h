/* OpenFi 后端公共库 —— AT 串口通道(termios)
 *
 * 【用 C 的一个额外好处】
 *   以前 shell 版必须靠外部的 stty + timeout 才能配串口 —— 而那块板子的
 *   busybox 里这两个 applet 都缺,只能拖 coreutils 进来。
 *   现在 termios 是 C 标准设施,直接配,零外部依赖。
 *
 * 【可测性】
 *   这一层靠 openpty(3) 做真集成测试:测试里 fork 一个"假模组"接在 pty 另一端,
 *   应答 AT 命令,然后走真实的 open/write/read 路径。见 tests/test_at_io.c。
 */
#ifndef OPENFI_AT_IO_H
#define OPENFI_AT_IO_H

#include "openfi_at.h"

#include <stddef.h>

struct at_port {
	int fd;
	int lock_fd;		/* 串口锁;拿不到时为 -1(退化成无锁) */
	char path[128];
};

/*
 * 串口锁。
 *
 * 【为什么需要】
 *   AT 口是独占资源,但系统里有【三方】会用它:
 *     · openfi-modem 的采样进程(每隔几秒问一次模组)
 *     · openfi-sms 的收发
 *     · files/usr/sbin/openfi-usbmode(开机探测 USB 数据模式)
 *   谁也不锁的话,两个进程的 AT 命令会互相插话 ——
 *   一方把另一方的响应读走,两边都超时失败,而且现象很随机。
 *
 * 【和 shell 侧对齐】
 *   shell 那边(at.sh / openfi-usbmode)用:
 *       锁文件 /var/run/openfi-at.lock
 *       最长等 12 秒
 *   这里用完全相同的文件和超时,所以 C 程序和 shell 脚本能互斥。
 *
 * 【覆盖】
 *   OPENFI_AT_LOCK 可以改锁文件路径(测试用,指到 /tmp)。
 */
#define AT_LOCK_DEFAULT_PATH "/var/run/openfi-at.lock"
#define AT_LOCK_WAIT_MS      12000

/*
 * 打开串口并配成 raw 模式。
 * baud 支持 9600/19200/38400/57600/115200/230400/460800/921600。
 * 成功 0,失败 -1。
 */
int at_open(struct at_port *p, const char *path, int baud);

void at_close(struct at_port *p);

/*
 * 发一条命令并把响应读全(直到 OK / ERROR / CME / CMS 或超时)。
 *   cmd     不带结尾换行的命令,如 "AT+CSQ"
 *   timeout_ms 整体超时
 * 返回 0 表示"读到了响应"(成败看 r->result),-1 表示串口 I/O 出错。
 */
int at_cmd(struct at_port *p, const char *cmd, int timeout_ms, struct at_resp *r);

/*
 * 发原始数据(不带回车),用于短信的 ">" 提示后发 PDU、以及 Ctrl-Z 结束。
 */
int at_write_raw(struct at_port *p, const char *data, size_t len);

/*
 * 等模组吐出 ">" 输入提示(短信 PDU 要用)。
 * ★ 发短信必须走这条路:AT+CMGS=<len> 之后模组不会回 OK,而是回 "> ",
 *   等我们把 PDU 写完再用 Ctrl-Z(0x1A)结束。以前直接用 at_cmd 发 PDU,
 *   既等不到 ">"、又用 CR 当结束符 —— 模组一直等剩余数据,发送必然失败。
 * 成功 0,超时 -1。
 */
int at_wait_prompt(struct at_port *p, int timeout_ms);

/* 列出系统里像"模组的 AT 口"的设备:返回逗号分隔的 /dev/ttyUSB* /dev/ttyACM* */
int at_list_ports(char *out, size_t outlen);

#endif
