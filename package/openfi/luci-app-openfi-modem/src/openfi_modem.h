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

/* +QTEMP: "soc-thermal",54 → 按名字取值 */
int md_parse_qtemp(const char *line, long *soc, long *pa);

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
