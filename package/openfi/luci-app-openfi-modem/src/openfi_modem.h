/* OpenFi 6C —— 5G 模组状态模型与解析
 *
 * 模组是移远 RM500U-CN(展锐 UDX710),走 USB 的 AT 口(2c7c:0900)。
 *
 * 设计:解析是纯函数 —— 把真机抓到的原始报文喂进去,在开发机上就能测透。
 * 真机报文的坑(都写进测试了):
 *   · 回显、URC 干扰行混在响应里
 *   · 同一语义在不同固件版本上字段数不一样(多一个尾巴字段/少一个)
 *   · 字段可能带引号、可能为空、可能是 "-" 或 "NOCONN"
 *   · 温度是 "soc-thermal",54 这种带名字的两段式
 */
#ifndef OPENFI_MODEM_H
#define OPENFI_MODEM_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * 截断式字符串拷贝(一定留 '\0')。
 *
 * 原来只在 main.c 里定义。现在 openfi_modem.c 也要用(填数据链路那几个字段),
 * 所以提到头文件里 —— 两边共用一份,避免各写各的慢慢走样。
 */
static inline void copy_str(char *dst, size_t n, const char *src)
{
	size_t i;

	if (!dst || n == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	/* 手写循环而不是 snprintf:截断时 snprintf 会触发
	   -Wformat-truncation(编译器不知道我们就是要截断)。 */
	for (i = 0; i + 1 < n && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

#define MD_STR      32
#define MD_MAX_CELLS 8

/* SIM 卡状态 */
enum md_sim {
	MD_SIM_UNKNOWN = 0,
	MD_SIM_READY,
	MD_SIM_ABSENT,
	MD_SIM_PIN,
	MD_SIM_PUK,
	MD_SIM_ERROR,
};

/* 注网状态(3GPP:0 未注册 / 1 已注册本地 / 2 搜索中 / 3 拒绝 / 4 未知 / 5 漫游) */
enum md_reg {
	MD_REG_UNKNOWN = -1,
	MD_REG_NONE = 0,
	MD_REG_HOME,
	MD_REG_SEARCHING,
	MD_REG_DENIED,
	MD_REG_UNKNOWN3,
	MD_REG_ROAMING,
};

/* 一个服务小区 */
struct md_cell {
	char rat[16];		/* LTE / NR5G-SA / NR5G-NSA ... */
	char band[16];		/* 频段号 */
	long arfcn;		/* 频点 */
	long pci;		/* 物理小区号 */
	long rsrp;		/* dBm */
	long rsrq;		/* dB */
	long sinr;		/* dB */
	long cells;		/* 同频小区数(-1 未知) */
};

struct md_state {
	/* 标识 */
	char imei[MD_STR];
	char imsi[MD_STR];
	char iccid[MD_STR];
	char model[MD_STR];
	char firmware[MD_STR];

	/* SIM */
	enum md_sim sim;
	int  sim_slot;		/* 当前卡槽(1 起) */
	int  sim_slots;		/* 卡槽总数 */

	/* 网络 */
	char operator[MD_STR];
	enum md_reg reg;	/* 数据域注网 */
	char network[16];	/* LTE / NR5G-SA ... */
	char band[16];
	long rssi;		/* 0..31(99 = 未知),来自 +CSQ */
	long rsrp, rsrq, sinr;	/* 服务小区 */
	int  bars;		/* 信号格数 0..5 */

	/* 温度 */
	long temp_soc;
	long temp_pa;

	/* 数据链路 */
	char ifname[16];
	char ipaddr[46];
	char gateway[46];
	char dns1[46];
	char dns2[46];
	long rx_bytes, tx_bytes;
	long uptime;		/* 拨号持续秒数 */

	/* 元信息 */
	char at_port[64];
	int  ok;		/* 是否成功从模组读到过东西 */
};

void md_state_init(struct md_state *s);

/* ---- 纯解析函数(全部本地可测)---- */

/* +CSQ: 20,99 → rssi=20;顺带按 3GPP 表算格数 */
int md_parse_csq(const char *line, long *rssi);
int md_rssi_to_bars(long rssi);

/* +CPIN: READY / +CME ERROR: 10(没插卡)*/
enum md_sim md_parse_cpin(const char *line);

/* +COPS: 0,0,"CHINA MOBILE",7 → 运营商 + 制式 */
int md_parse_cops(const char *line, char *oper, size_t n, char *act, size_t m);

/* +CREG: 0,1 / +CEREG: 0,1,"1234","0A1B2C3",7 → 注网状态 */
enum md_reg md_parse_creg(const char *line);

/* +QENG: "servingcell",... —— 真机格式见测试。
 * 返回 0 成功, -1 不是服务小区行 */
int md_parse_qeng_serving(const char *line, struct md_cell *c);

/* +QENG: "neighbourcell ..." 一行里可能给多个邻区 —— 只取第一个可用的 */
int md_parse_qeng_neighbour(const char *line, struct md_cell *c);

/* +QCAINFO: "PCC",723360,100,NR N79,1,875,-92,-3,-66,9 → 信号
 *
 * 【为什么优先用它而不是 +QENG】
 *   +QENG="servingcell" 的字段布局【随固件版本变】:同样是 NR5G-SA,
 *   真机实测字段比我们注释里记的多一个,导致 RSRP/RSRQ/SINR 整体错位 ——
 *   解析出来 RSRP 是 +100(不可能,RSRP 一定是负数)。
 *   +QCAINFO 的布局是固定的:
 *       "PCC",<ARFCN>,<带宽>,<频段名>,<?>,<PCI>,<RSRP>,<RSRQ>,<RSSI>,<SINR>
 *   实测稳定,数值也合理(-92 / -3 / -66 / 9)。
 */
int md_parse_qcainfo(const char *line, struct md_cell *c);

/* +QTEMP: "soc-thermal",54 → 按名字取值 */
int md_parse_qtemp(const char *line, long *soc, long *pa);

/* +QENG 的第 2 个字段是连接状态(CONNECT/DISCONNECT/NOCONN)。
 * 比 CREG/CEREG 的 stat 可靠 —— 真机上 CREG 一直是 0,但模组明明连着。 */
int md_cell_state_online(const char *qeng_line);

/* MCC/MNC(如 46000)→ 运营商名。认不出来就返回空串 */
const char *md_operator_from_plmn(const char *mccmnc);

/* 从系统里填数据链路信息(网卡名/IP/网关/DNS/收发字节/在线时长)。
 * 这些模组自己不知道 —— 是主机侧的事,所以从 /proc 和 sysfs 读。 */
void md_fill_datalink(struct md_state *s);

/* +CGSN / +GSN → IMEI(纯数字,15 位) */
int md_parse_imei(const char *line, char *out, size_t n);
/* +CIMI → IMSI */
int md_parse_imsi(const char *line, char *out, size_t n);
/* +QCCID / +CCID → ICCID(可能带 +CCID: 前缀) */
int md_parse_iccid(const char *line, char *out, size_t n);
/* +CGMM / +CGMI / +CGMR → 文本(去引号) */
int md_parse_text(const char *line, char *out, size_t n);

/* 从整段响应里挑出某前缀的行(复用 at_find 也行,这里给个不依赖 at 模块的版本) */
const char *md_find_line(const char *resp, const char *prefix);

/*
 * 制式字符串 → 是否 5G
 * "+QNWINFO: \"NR5G-SA\"..." / "NR5G-NSA" / "LTE" 等
 */
int md_is_5g(const char *network);

#endif
