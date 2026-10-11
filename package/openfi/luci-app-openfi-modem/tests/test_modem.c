/* 模组解析测试 —— 全部用【真机抓到的报文】,包括各种变体和坑 */
#define _GNU_SOURCE
#include "../src/openfi_modem.h"
#include "t.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
	struct md_cell c;
	long rssi, soc, pa;
	char buf[64], act[16];

	printf("── +CSQ(信号强度)──\n");
	T_EQ_INT(md_parse_csq("+CSQ: 20,99", &rssi), 0);   T_EQ_INT(rssi, 20);
	T_EQ_INT(md_parse_csq("+CSQ: 0,99", &rssi), 0);    T_EQ_INT(rssi, 0);
	T_EQ_INT(md_parse_csq("+CSQ: 31,99", &rssi), 0);   T_EQ_INT(rssi, 31);
	T_EQ_INT(md_parse_csq("+CSQ: 99,99", &rssi), 0);   T_EQ_INT(rssi, 99);  /* 未知 */
	T_EQ_INT(md_parse_csq("+CSQ:  7 , 99 ", &rssi), 0); T_EQ_INT(rssi, 7);  /* 带空格 */
	T_EQ_INT(md_parse_csq("+CSQ: 32,99", &rssi), -1);  /* 越界 */
	T_EQ_INT(md_parse_csq("ERROR", &rssi), -1);
	T_EQ_INT(md_parse_csq(NULL, &rssi), -1);

	printf("── 信号格数 ──\n");
	T_EQ_INT(md_rssi_to_bars(0), 0);
	T_EQ_INT(md_rssi_to_bars(9), 0);
	T_EQ_INT(md_rssi_to_bars(10), 1);
	T_EQ_INT(md_rssi_to_bars(14), 1);
	T_EQ_INT(md_rssi_to_bars(15), 2);
	T_EQ_INT(md_rssi_to_bars(20), 3);
	T_EQ_INT(md_rssi_to_bars(25), 4);
	T_EQ_INT(md_rssi_to_bars(30), 5);
	T_EQ_INT(md_rssi_to_bars(31), 5);
	T_EQ_INT(md_rssi_to_bars(99), 0);   /* 未知按无信号 */
	T_EQ_INT(md_rssi_to_bars(-1), 0);

	printf("── +CPIN(SIM 状态)──\n");
	T_EQ_INT(md_parse_cpin("+CPIN: READY"), MD_SIM_READY);
	T_EQ_INT(md_parse_cpin("+CME ERROR: 10"), MD_SIM_ABSENT);  /* 没插卡 */
	T_EQ_INT(md_parse_cpin("+CME ERROR: 13"), MD_SIM_ERROR);
	T_EQ_INT(md_parse_cpin("+CPIN: SIM PIN"), MD_SIM_PIN);
	T_EQ_INT(md_parse_cpin("+CPIN: SIM PUK"), MD_SIM_PUK);
	T_EQ_INT(md_parse_cpin("ERROR"), MD_SIM_ERROR);
	T_EQ_INT(md_parse_cpin(NULL), MD_SIM_UNKNOWN);

	printf("── +COPS(运营商)──\n");
	T_EQ_INT(md_parse_cops("+COPS: 0,0,\"CHINA MOBILE\",7", buf, sizeof(buf), act, sizeof(act)), 0);
	T_EQ_STR(buf, "CHINA MOBILE"); T_EQ_STR(act, "7");
	T_EQ_INT(md_parse_cops("+COPS: 0,2,\"CHN-UNICOM\",7", buf, sizeof(buf), NULL, 0), 0);
	T_EQ_STR(buf, "CHN-UNICOM");
	/* 没注网时运营商字段是空的 */
	T_EQ_INT(md_parse_cops("+COPS: 0", buf, sizeof(buf), NULL, 0), -1);
	T_EQ_INT(md_parse_cops(NULL, buf, sizeof(buf), NULL, 0), -1);

	printf("── +CREG / +CEREG(注网状态)──\n");
	T_EQ_INT(md_parse_creg("+CREG: 0,1"), MD_REG_HOME);
	T_EQ_INT(md_parse_creg("+CEREG: 0,5"), MD_REG_ROAMING);
	T_EQ_INT(md_parse_creg("+CREG: 0,2"), MD_REG_SEARCHING);
	T_EQ_INT(md_parse_creg("+CREG: 0,3"), MD_REG_DENIED);
	T_EQ_INT(md_parse_creg("+CREG: 0,0"), MD_REG_NONE);
	T_EQ_INT(md_parse_creg("+CEREG: 1"), MD_REG_HOME);        /* 单字段变体 */
	T_EQ_INT(md_parse_creg("+CEREG: 0,1,\"1234\",\"0A1B2C3\",7"), MD_REG_HOME);
	T_EQ_INT(md_parse_creg("ERROR"), MD_REG_UNKNOWN);
	T_EQ_INT(md_parse_creg(NULL), MD_REG_UNKNOWN);

	printf("── +QENG 服务小区:NR5G-SA(真机报文)──\n");
	{
		const char *l = "+QENG: \"servingcell\",\"NOCONN\",\"NR5G-SA\",\"TDD\",460,00,123456789,266,504990,41,5,-81,-10,-73,16,-";

		T_EQ_INT(md_parse_qeng_serving(l, &c), 0);
		T_EQ_STR(c.rat, "NR5G-SA");
		T_EQ_INT(c.arfcn, 504990);
		T_EQ_INT(c.pci, 266);
		T_EQ_STR(c.band, "41");
		T_EQ_INT(c.rsrp, -81);
		T_EQ_INT(c.rsrq, -10);
		T_EQ_INT(c.sinr, -73);
	}

	printf("── +QENG 服务小区:LTE(字段布局不同!偏移差 2)──\n");
	{
		const char *l = "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,00,1234567,266,1850,3,5,5,1234,-95,-11,-63,18,-";

		T_EQ_INT(md_parse_qeng_serving(l, &c), 0);
		T_EQ_STR(c.rat, "LTE");
		T_EQ_INT(c.arfcn, 1850);
		T_EQ_INT(c.pci, 266);
		T_EQ_STR(c.band, "3");
		T_EQ_INT(c.rsrp, -95);      /* LTE 的 RSRP 在下标 13 */
		T_EQ_INT(c.rsrq, -11);
		T_EQ_INT(c.sinr, 18);
	}

	printf("── +QENG 邻区 ──\n");
	T_EQ_INT(md_parse_qeng_neighbour("+QENG: \"neighbourcell intra\",\"LTE\",1850,266,-81,-10,-63,5", &c), 0);
	T_EQ_STR(c.rat, "LTE");
	T_EQ_INT(c.pci, 266);
	T_EQ_INT(c.rsrp, -81);

	printf("── +QENG 非服务小区行应当被拒 ──\n");
	T_EQ_INT(md_parse_qeng_serving("+QENG: \"neighbourcell intra\",\"LTE\",1,2,3", &c), -1);
	T_EQ_INT(md_parse_qeng_serving("+QENG: \"servingcell\"", &c), -1);
	T_EQ_INT(md_parse_qeng_serving(NULL, &c), -1);

	printf("── +QTEMP(真机是多行,一行一个传感器)──\n");
	T_EQ_INT(md_parse_qtemp("+QTEMP: \"soc-thermal\",54", &soc, &pa), 0); T_EQ_INT(soc, 54);
	T_EQ_INT(md_parse_qtemp("+QTEMP: \"pa-thermal\",47", &soc, &pa), 0);  T_EQ_INT(pa, 47);
	T_EQ_INT(md_parse_qtemp("+QTEMP: \"pa-thermal\",-3", &soc, &pa), 0);  T_EQ_INT(pa, -3);
	T_EQ_INT(md_parse_qtemp("+QTEMP: \"unknown\",5", &soc, &pa), -1);
	T_EQ_INT(md_parse_qtemp(NULL, &soc, &pa), -1);

	printf("── IMEI / IMSI / ICCID ──\n");
	T_EQ_INT(md_parse_imei("+CGSN: 012345678901234", buf, sizeof(buf)), 0);
	T_EQ_STR(buf, "012345678901234");
	T_EQ_INT(md_parse_imei("012345678901234", buf, sizeof(buf)), 0);   /* 有的直接回数字 */
	T_EQ_STR(buf, "012345678901234");
	T_EQ_INT(md_parse_imsi("+CIMI: 460001234567890", buf, sizeof(buf)), 0);
	T_EQ_STR(buf, "460001234567890");
	T_EQ_INT(md_parse_iccid("+QCCID: 89860112345678901234", buf, sizeof(buf)), 0);
	T_EQ_STR(buf, "89860112345678901234");
	T_EQ_INT(md_parse_iccid("+CCID: 8986011234567890123", buf, sizeof(buf)), 0);
	T_EQ_STR(buf, "8986011234567890123");
	T_EQ_INT(md_parse_imei("+CGSN: ERROR", buf, sizeof(buf)), -1);
	T_EQ_INT(md_parse_iccid("+QCCID: 123", buf, sizeof(buf)), -1);   /* 太短 */
	T_EQ_INT(md_parse_imei(NULL, buf, sizeof(buf)), -1);

	printf("── 型号 / 固件 ──\n");
	T_EQ_INT(md_parse_text("+CGMM: RM500U-CN", buf, sizeof(buf)), 0);
	T_EQ_STR(buf, "RM500U-CN");
	T_EQ_INT(md_parse_text("+CGMR: RM500UCNCBR01A03M4G", buf, sizeof(buf)), 0);
	T_EQ_STR(buf, "RM500UCNCBR01A03M4G");
	T_EQ_INT(md_parse_text("+CGMM: \"QUECTEL\"", buf, sizeof(buf)), 0);
	T_EQ_STR(buf, "QUECTEL");
	T_EQ_INT(md_parse_text("+CGMM:", buf, sizeof(buf)), -1);

	printf("── 从整段响应里找行(带 URC 干扰)──\n");
	{
		const char *resp =
			"AT+CSQ\r\r\n"
			"+QIND: \"acT\",\"NR5G-SA\"\r\n"
			"+CSQ: 21,98\r\n"
			"\r\nOK\r\n";

		T_EQ_STR(md_find_line(resp, "+CSQ:"), "+CSQ: 21,98");
		T_OK(md_find_line(resp, "+CREG:") == NULL);
		T_OK(md_find_line(NULL, "+CSQ:") == NULL);
	}

	printf("── 制式判断 ──\n");
	T_EQ_INT(md_is_5g("NR5G-SA"), 1);
	T_EQ_INT(md_is_5g("NR5G-NSA"), 1);
	T_EQ_INT(md_is_5g("LTE"), 0);
	T_EQ_INT(md_is_5g(""), 0);
	T_EQ_INT(md_is_5g(NULL), 0);

	printf("── 状态初始化 ──\n");
	{
		struct md_state s;

		md_state_init(&s);
		T_EQ_INT(s.ok, 0);
		T_EQ_INT(s.sim, MD_SIM_UNKNOWN);
		T_EQ_INT(s.reg, MD_REG_UNKNOWN);
		T_EQ_INT(s.rssi, 99);
		T_EQ_INT(s.temp_soc, -1000);
		T_EQ_INT(s.sim_slot, 1);
		md_state_init(NULL);
	}


	/* ---- +QCAINFO(真机报文,信号解析的主力)---- */
	printf("── +QCAINFO ──\n");
	{
		struct md_cell c;
		/* 真机原样抓下来的 */
		T_EQ_INT(md_parse_qcainfo("+QCAINFO: \"PCC\",723360,100,NR N79,1,875,-92,-3,-66,9", &c), 0);
		T_EQ_INT(c.rsrp, -92);
		T_EQ_INT(c.rsrq, -3);
		T_EQ_INT(c.sinr, 9);
		T_EQ_INT(c.pci, 875);
		T_EQ_STR(c.band, "N79");
		/* 非 PCC(比如 SCC 辅载波)不采用 */
		T_EQ_INT(md_parse_qcainfo("+QCAINFO: \"SCC\",723360,100,NR N79,1,875,-92,-3,-66,9", &c), -1);
		/* 字段不够就拒绝,不能瞎猜 */
		T_EQ_INT(md_parse_qcainfo("+QCAINFO: \"PCC\",723360", &c), -1);
	}

	/* ---- 连接状态(QENG 第 2 字段,比 CREG 可靠)---- */
	printf("── 在线状态 ──\n");
	T_EQ_INT(md_cell_state_online("+QENG: \"servingcell\",\"CONNECT\",\"NR5G-SA\"") != 0, 1);
	T_EQ_INT(md_cell_state_online("+QENG: \"servingcell\",\"NOCONN\",\"LTE\"") != 0, 0);
	T_EQ_INT(md_cell_state_online("+QENG: \"servingcell\",\"DISCONNECT\"") != 0, 0);
	T_EQ_INT(md_cell_state_online(NULL) != 0, 0);

	/* ---- 运营商 MCC/MNC 映射(COPS 给空串时的兜底)---- */
	printf("── 运营商映射 ──\n");
	T_EQ_STR(md_operator_from_plmn("46000"), "中国移动");
	T_EQ_STR(md_operator_from_plmn("46001"), "中国联通");
	T_EQ_STR(md_operator_from_plmn("46011"), "中国电信");
	T_EQ_STR(md_operator_from_plmn("46015"), "中国广电");
	T_EQ_STR(md_operator_from_plmn("99999"), "");
	T_EQ_STR(md_operator_from_plmn("460"), "");
	T_EQ_STR(md_operator_from_plmn(NULL), "");

	T_SUMMARY();
}
