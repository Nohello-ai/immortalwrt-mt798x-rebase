#define _GNU_SOURCE
#include "openfi_at_io.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

static speed_t baud_to_speed(int baud)
{
	switch (baud) {
	case 9600:   return B9600;
	case 19200:  return B19200;
	case 38400:  return B38400;
	case 57600:  return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
#ifdef B460800
	case 460800: return B460800;
#endif
#ifdef B921600
	case 921600: return B921600;
#endif
	default:     return B115200;
	}
}

int at_open(struct at_port *p, const char *path, int baud)
{
	struct termios tio;
	speed_t sp;

	if (!p || !path)
		return -1;
	memset(p, 0, sizeof(*p));
	p->fd = -1;
	snprintf(p->path, sizeof(p->path), "%s", path);

	/* O_NONBLOCK:不要让 open() 因为 modem control line 卡住;
	   开完再用 select 控制读超时。 */
	p->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (p->fd < 0)
		return -1;

	if (tcgetattr(p->fd, &tio) != 0) {
		close(p->fd);
		p->fd = -1;
		return -1;
	}

	sp = baud_to_speed(baud);
	cfsetospeed(&tio, sp);
	cfsetispeed(&tio, sp);

	/* raw:不做任何加工 —— 这正是以前要靠 stty raw -echo 做的事 */
	tio.c_cflag |= (CLOCAL | CREAD);
	tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
	tio.c_cflag |= CS8;
	tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
	tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR);
	tio.c_oflag &= ~OPOST;
	/* 读:至少 1 字节就返回,但不要无限等 —— 超时由 select 控制 */
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(p->fd, TCSANOW, &tio) != 0) {
		close(p->fd);
		p->fd = -1;
		return -1;
	}
	tcflush(p->fd, TCIOFLUSH);
	return 0;
}

void at_close(struct at_port *p)
{
	if (p && p->fd >= 0) {
		close(p->fd);
		p->fd = -1;
	}
}

int at_write_raw(struct at_port *p, const char *data, size_t len)
{
	size_t off = 0;

	if (!p || p->fd < 0 || !data)
		return -1;
	while (off < len) {
		ssize_t n = write(p->fd, data + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				continue;	/* 非阻塞,稍后重试即可 */
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

/* 读一大段,直到出现终止行或超时。返回 0 成功(可能只是超时), -1 I/O 错 */
static int read_until(struct at_port *p, int timeout_ms, char *out, size_t outlen)
{
	char buf[1024];
	size_t used = 0;
	long long remain = timeout_ms;

	if (outlen)
		out[0] = '\0';

	while (remain > 0) {
		fd_set rfds;
		struct timeval tv;
		int r;

		FD_ZERO(&rfds);
		FD_SET(p->fd, &rfds);
		tv.tv_sec = (time_t)(remain / 1000);
		tv.tv_usec = (suseconds_t)((remain % 1000) * 1000);

		r = select(p->fd + 1, &rfds, NULL, NULL, &tv);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			break;			/* 超时 */

		ssize_t n = read(p->fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		if (used + (size_t)n < outlen - 1) {
			memcpy(out + used, buf, (size_t)n);
			used += (size_t)n;
			out[used] = '\0';
		}
		/* 收到终止行就可以停了(不用等满超时) */
		if (strstr(out, "\r\nOK\r\n") || strstr(out, "\nOK\r\n") ||
		    strstr(out, "\r\nERROR\r\n") || strstr(out, "\nERROR\r\n") ||
		    strstr(out, "+CME ERROR:") || strstr(out, "+CMS ERROR:"))
			break;
		remain -= 50;			/* 粗略扣时;每轮最多等 50ms 的读 */
	}
	return 0;
}

int at_wait_prompt(struct at_port *p, int timeout_ms)
{
	long long remain = timeout_ms;

	if (!p || p->fd < 0)
		return -1;
	while (remain > 0) {
		fd_set rfds;
		struct timeval tv;
		int rc;

		FD_ZERO(&rfds);
		FD_SET(p->fd, &rfds);
		tv.tv_sec = (time_t)(remain / 1000);
		tv.tv_usec = (suseconds_t)((remain % 1000) * 1000);

		rc = select(p->fd + 1, &rfds, NULL, NULL, &tv);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (rc == 0)
			break;			/* 超时 */
		{
			char buf[256];
			ssize_t n = read(p->fd, buf, sizeof(buf));

			if (n < 0) {
				if (errno == EINTR || errno == EAGAIN)
					continue;
				return -1;
			}
			if (n == 0)
				break;
			for (ssize_t i = 0; i < n; i++)
				if (buf[i] == '>')
					return 0;
		}
		remain -= 50;
	}
	return -1;			/* 没等到提示 */
}

int at_cmd(struct at_port *p, const char *cmd, int timeout_ms, struct at_resp *r)
{
	char out[8192], line[AT_LINE_MAX + 4];

	if (!p || p->fd < 0 || !cmd || !r)
		return -1;
	tcflush(p->fd, TCIFLUSH);

	snprintf(line, sizeof(line), "%s\r", cmd);
	if (at_write_raw(p, line, strlen(line)) != 0) {
		memset(r, 0, sizeof(*r));
		r->result = AT_RES_IO;
		return -1;
	}
	if (read_until(p, timeout_ms, out, sizeof(out)) != 0) {
		memset(r, 0, sizeof(*r));
		r->result = AT_RES_IO;
		return -1;
	}
	return at_parse(out, r);
}

int at_list_ports(char *out, size_t outlen)
{
	static const char *prefixes[] = { "ttyUSB", "ttyACM" };
	int first = 1, n = 0;
	DIR *d;

	if (!out || !outlen)
		return 0;
	out[0] = '\0';
	d = opendir("/dev");
	if (!d)
		return 0;

	for (size_t pi = 0; pi < sizeof(prefixes) / sizeof(prefixes[0]); pi++) {
		struct dirent *de;

		rewinddir(d);
		while ((de = readdir(d))) {
			size_t pl = strlen(prefixes[pi]);

			if (strncmp(de->d_name, prefixes[pi], pl) != 0)
				continue;
			/* 只要纯数字后缀的 */
			int digits = 1;

			for (const char *c = de->d_name + pl; *c; c++)
				if (*c < '0' || *c > '9')
					digits = 0;
			if (!digits)
				continue;
			int w = snprintf(out + strlen(out), outlen - strlen(out),
					 "%s/dev/%s", first ? "" : ",", de->d_name);

			if (w < 0 || (size_t)w >= outlen - strlen(out))
				break;
			first = 0;
			n++;
		}
	}
	closedir(d);
	return n;
}
