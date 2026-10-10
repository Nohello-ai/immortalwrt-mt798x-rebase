/* OpenFi 6C —— 短信 PDU 编解码(GSM 03.40 / 03.38)
 *
 * 【为什么这块最该写测试】
 *   PDU 是一串十六进制,错一位整条短信就乱码或者发不出去,而现象只有
 *   "对方收到的是一堆乱码" —— 没有报错、没有日志,靠上机试根本试不出来。
 *   所以这里把编解码做成纯函数,拿真机/运营商的实际 PDU 做单元测试。
 *
 * 支持范围:
 *   收:SMSC 地址、发送方号码(含 +86 国际格式)、时间戳、
 *       7bit(ASCII)/8bit/UCS2(中文)、UDH 长短信拼接
 *   发:7bit 与 UCS2 两种编码,带可选 UDH(长短信分段)
 */
#ifndef OPENFI_SMS_H
#define OPENFI_SMS_H

#include <stddef.h>

#define SMS_TEXT_MAX   1024
#define SMS_ADDR_MAX   32
#define SMS_HEX_MAX    2048

/* 编码方式 */
enum sms_dcs {
	SMS_DCS_7BIT = 0,	/* GSM 7 位默认字母表 */
	SMS_DCS_8BIT,		/* 二进制/数据 */
	SMS_DCS_UCS2,		/* UTF-16BE(中文走这个)*/
};

struct sms {
	/* 收发地址(号码) */
	char sca[SMS_ADDR_MAX];		/* 短信中心号码 */
	char addr[SMS_ADDR_MAX];	/* 对端号码 */

	/* 时间戳 "YYMMDDHHMMSS" + 时区(15 分钟为单位,带符号偏移) */
	char ts[24];

	enum sms_dcs dcs;
	char text[SMS_TEXT_MAX];	/* 统一转成 UTF-8 */

	/* 长短信(UDH 拼接)信息 */
	int  concat_total;		/* 0 = 不是长短信 */
	int  concat_seq;		/* 第几条(1 起)*/
	int  concat_ref;		/* 参考号 */

	int  valid;
};

void sms_init(struct sms *s);

/* ---- 底层工具(都有独立测试)---- */

/* 十六进制字符 → 字节数;返回写出的字节数,-1 表示有非法字符 */
int  sms_hex2bytes(const char *hex, unsigned char *out, size_t outlen);
/* 字节 → 小写十六进制字符串 */
void sms_bytes2hex(const unsigned char *in, size_t n, char *out, size_t outlen);

/* 半字节交换(PDU 里号码是"字节内高低位互换"存的)*/
void sms_swap_nibbles(char *s);

/* ---- 号码 ---- */

/*
 * 解析 PDU 里的地址字段(以"数字个数"开始,后面是半字节交换的 BCD)。
 *   len_bytes  该字段占几个字节(不含长度字节)
 * 输出成可读号码,如 "+8613800138000"。
 */
int sms_decode_addr(const unsigned char *p, int len_bytes, char *out, size_t outlen);

/* 把号码编码成 PDU 的地址字段(含长度字节)。返回写出的字节数,-1 失败 */
int sms_encode_addr(const char *num, unsigned char *out, size_t outlen, int with_len);

/* ---- 文本 ---- */

/*
 * 7bit 打包 / 解包。
 *
 * ★ 为什么要"起始 septet"参数:
 *   带 UDH(长短信头)时,UDH 占的字节数【未必落在 septet 边界上】——
 *   规范(GSM 03.40 §9.2.3.24)要求在其后补若干填充位,让正文从
 *   septet 边界开始。6 字节 UDH = 48 bit,48%7=6,所以要补 1 位,
 *   正文从第 7 个 septet 开始。
 *   如果按"字节对齐"去摆正文(把指针指到 UDH 后面那个字节),
 *   就会整体错 1 位 —— 收到的长英文短信全乱码,发出去的对方也读不了。
 *   这个偏移不是靠"自己编自己解"能发现的,必须对着规范向量测。
 */
int sms_unpack_7bit_at(const unsigned char *in, int start_sept, int septets,
		       char *out, size_t outlen);
int sms_pack_7bit_at(const char *text, int start_sept, unsigned char *out,
		     size_t outlen, int *septets_out);

/* 从第 0 个 septet 开始的简写(无 UDH 时用) */
int sms_unpack_7bit(const unsigned char *in, int septets, char *out, size_t outlen);
int sms_pack_7bit(const char *text, unsigned char *out, size_t outlen, int *septets_out);

/*
 * 算 UDH 占多少个 septet(含规范要求的填充位)。
 *   返回 (udh_len+1)*8 补到 septet 边界后除以 7。
 *   udh_len 传 UDHL 的值(UDH 数据字节数,不含 UDHL 自己)。
 */
int sms_udh_septets(int udh_len);

/* UCS2(UTF-16BE)解成 UTF-8 */
int sms_ucs2_to_utf8(const unsigned char *in, int nbytes, char *out, size_t outlen);
/* UTF-8 编成 UCS2。返回写出的字节数 */
int sms_utf8_to_ucs2(const char *utf8, unsigned char *out, size_t outlen);

/* ---- PDU ---- */

/*
 * 解一条收到的 PDU(SMS-DELIVER)。
 * hex 是十六进制字符串(允许中间有空白)。
 * 成功返回 0,fail 返回 -1(s->valid 也会被置位)。
 */
int sms_decode_pdu(const char *hex, struct sms *s);

/*
 * 编一条要发出去的 PDU(SMS-SUBMIT)。
 *   sca     短信中心号码(空字符串则用 00 表示"用默认中心")
 *   addr    对方号码
 *   text    UTF-8 文本
 *   dcs     想用的编码;传 -1 自动选(纯 ASCII 用 7bit,含非 ASCII 用 UCS2)
 *   concat_* 长短信参数(concat_total<=1 表示不加 UDH)
 * 返回 PDU 的十六进制字符串(写进 out),-1 失败。
 */
int sms_encode_pdu(const char *sca, const char *addr, const char *text, int dcs,
		   int concat_total, int concat_seq, int concat_ref,
		   char *out, size_t outlen);

/*
 * 估算一条短信要占多少"条"。
 *   use_ucs2 < 0 表示自动判断(试 7 位能否编码)
 * 注意:7 位编码时按【字符数(septet)】算,不能用 strlen ——
 * GSM 默认表里 ¤ ¡ Δ 这些是多字节 UTF-8,但各只占一个 septet,
 * 用字节数会高估段数(实测把 1 段的中文当成 3 段)。
 */
int sms_count_segments(const char *text, int use_ucs2);

/*
 * 算 PDU 里 TPDU 部分的字节数 —— AT+CMGS=<len> 要填的就是它。
 * ★ 这个长度【不含开头的短信中心字段】(长度字节 + 类型字节 + 数字字节)。
 *   以前我图省事写 "总字节数 - 1",那只在短信中心为空(单个 0x00)时才对;
 *   一旦配上短信中心号就多算 8 个字节,模组会一直等剩下的数据,发送必失败。
 */
int sms_tpdu_len(const char *sca, const char *pdu_hex);

#endif
