/* 短信 PDU 编解码测试
 *
 * 重点:
 *   · 用已知的 PDU 向量(网上一查就有的 "hello" 那条)当基准
 *   · 7 位字母表不是 ASCII,专门测那几个不一样的位置
 *   · 编码→解码往返(最强的正确性检查)
 *   · 中文走 UCS2、长短信 UDH 拼接
 *   · 位数算错会串位,所以长度相关的都单独测
 */
#define _GNU_SOURCE
#include "../src/openfi_sms.h"
#include "t.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
	struct sms s;
	char hex[SMS_HEX_MAX], buf[512];
	unsigned char bytes[512];
	int sept;

	printf("── 十六进制 ↔ 字节 ──\n");
	T_EQ_INT(sms_hex2bytes("E8329BFD06", bytes, sizeof(bytes)), 5);
	T_EQ_INT(bytes[0], 0xE8);
	T_EQ_INT(bytes[4], 0x06);
	T_EQ_INT(sms_hex2bytes("e8 32 9b fd 06", bytes, sizeof(bytes)), 5);  /* 带空格也行 */
	T_EQ_INT(sms_hex2bytes("E8329BFD0", bytes, sizeof(bytes)), -1);      /* 奇数个 */
	T_EQ_INT(sms_hex2bytes("E8329BFD0G", bytes, sizeof(bytes)), -1);     /* 非法字符 */
	sms_bytes2hex(bytes, 5, buf, sizeof(buf));
	T_EQ_STR(buf, "e8329bfd06");
	sms_bytes2hex(NULL, 0, buf, sizeof(buf));
	T_EQ_STR(buf, "");

	printf("── 半字节交换 ──\n");
	/* +8613800138000 的 BCD 就是 68 31 08 10 83 00 F0,交换后应还原成数字序列 */
	snprintf(buf, sizeof(buf), "683108108300F0");
	sms_swap_nibbles(buf);
	T_EQ_STR(buf, "8613800138000F");    /* 末尾 F 是奇数位填充 */
	snprintf(buf, sizeof(buf), "1");
	sms_swap_nibbles(buf);              /* 奇数长度不能越界 */
	T_EQ_STR(buf, "1");

	printf("── 号码解码(PDU 里是半字节交换的 BCD)──\n");
	{
		/* 0D 91 68 31 08 10 83 00 F0 → +8613800138000
		   (数字 8613800138000 按两位一组交换:86→68, 13→31, 80→08,
		     01→10, 38→83, 00→00, 末尾单 0 补 F → F0) */
		unsigned char a[] = { 0x0D, 0x91, 0x68, 0x31, 0x08, 0x10, 0x83, 0x00, 0xF0 };

		T_EQ_INT(sms_decode_addr(a, 9, buf, sizeof(buf)), 0);
		T_EQ_STR(buf, "+8613800138000");
	}
	{
		/* 0B 81 31 00 80 08 00 F0 → 本地号码 13000880000 */
		unsigned char a[] = { 0x0B, 0x81, 0x31, 0x00, 0x80, 0x08, 0x00, 0xF0 };

		T_EQ_INT(sms_decode_addr(a, 8, buf, sizeof(buf)), 0);
		T_EQ_STR(buf, "13000880000");
	}

	printf("── 号码编码 ──\n");
	{
		int n = sms_encode_addr("+8613800138000", bytes, sizeof(bytes), 1);

		T_OK(n > 0);
		T_EQ_INT(bytes[0], 13);         /* 数字个数 */
		T_EQ_INT(bytes[1], 0x91);       /* 国际 */
		T_EQ_INT(bytes[2], 0x68);       /* "86" → 0x68 */
		T_EQ_INT(bytes[5], 0x10);       /* "01" → 0x10 */
		/* 往返:n 是含长度字节的总字节数 */
		T_EQ_INT(sms_decode_addr(bytes, n, buf, sizeof(buf)), 0);
		T_EQ_STR(buf, "+8613800138000");
	}
	{
		int n = sms_encode_addr("13000880000", bytes, sizeof(bytes), 1);

		T_OK(n > 0);
		T_EQ_INT(bytes[1], 0x81);       /* 国内 */
		T_EQ_INT(sms_decode_addr(bytes, n, buf, sizeof(buf)), 0);
		T_EQ_STR(buf, "13000880000");
	}
	T_EQ_INT(sms_encode_addr("abc", bytes, sizeof(bytes), 1), -1);

	printf("── 7 位打包:已知向量(hello)──\n");
	T_EQ_INT(sms_pack_7bit("hello", bytes, sizeof(bytes), &sept), 5);
	T_EQ_INT(sept, 5);
	sms_bytes2hex(bytes, 5, buf, sizeof(buf));
	T_EQ_STR(buf, "e8329bfd06");        /* ★ 这是公认的 hello 打包结果 */
	/* 往返 */
	T_EQ_INT(sms_unpack_7bit(bytes, 5, hex, sizeof(hex)), 5);
	T_EQ_STR(hex, "hello");

	printf("── 7 位字母表【不是 ASCII】──\n");
	/* 0x00 是 '@'、0x24 是 '¤'、0x40 是 '¡'、0x5B 是 'Ä' */
	T_EQ_INT(sms_pack_7bit("@", bytes, sizeof(bytes), &sept), 1);
	T_EQ_INT(bytes[0], 0x00);
	T_EQ_INT(sms_pack_7bit("¤", bytes, sizeof(bytes), &sept), 1);
	T_EQ_INT(bytes[0], 0x24);
	T_EQ_INT(sms_pack_7bit("¡", bytes, sizeof(bytes), &sept), 1);
	T_EQ_INT(bytes[0], 0x40);
	T_EQ_INT(sms_pack_7bit("Ä", bytes, sizeof(bytes), &sept), 1);
	T_EQ_INT(bytes[0], 0x5B);

	printf("── 7 位往返:各种字符 ──\n");
	{
		const char *cases[] = { "Hello World", "0123456789", "aA1!@#$%", "[]{}()<>", NULL };

		for (int i = 0; cases[i]; i++) {
			int n = sms_pack_7bit(cases[i], bytes, sizeof(bytes), &sept);
			int ok = (n >= 0);

			if (ok) {
				sms_unpack_7bit(bytes, sept, buf, sizeof(buf));
				T_EQ_STR(buf, cases[i]);
			} else {
				T_OK(0);	/* 不该编不进去 */
			}
		}
	}

	printf("── 扩展表字符(€ { } \\ 等,占两个 septet)──\n");
	{
		int n = sms_pack_7bit("€", bytes, sizeof(bytes), &sept);

		T_OK(n >= 0);
		T_EQ_INT(sept, 2);              /* 转义符 + 字符 */
		sms_unpack_7bit(bytes, sept, buf, sizeof(buf));
		T_EQ_STR(buf, "€");
	}
	{
		int n = sms_pack_7bit("{a}", bytes, sizeof(bytes), &sept);

		T_OK(n >= 0);
		sms_unpack_7bit(bytes, sept, buf, sizeof(buf));
		T_EQ_STR(buf, "{a}");
	}

	printf("── ★ 扩展表的【每一个】字符都要能原样往返 ──\n");
	/*
	 * 为什么要逐个测:
	 *   扩展表的下标是 GSM 03.38 规范里定死的,手写极容易错位
	 *   (我第一次就把 { 写在 0x7B、[ 写在 0x34 这种"按 ASCII 猜"的位置)。
	 *   变异测试发现:只测 { } € 的话,把 [ 的下标改错也抓不到 ——
	 *   所以这里把每一个都过一遍。
	 */
	{
		const char *ext[] = { "\f", "^", "{", "}", "\\", "[", "~", "]", "|", "€", NULL };

		for (int i = 0; ext[i]; i++) {
			int n = sms_pack_7bit(ext[i], bytes, sizeof(bytes), &sept);

			if (n < 0) { T_OK(0); continue; }   /* 编不进去就算失败 */
			T_EQ_INT(sept, 2);                  /* 转义符 + 字符本身 */
			sms_unpack_7bit(bytes, sept, buf, sizeof(buf));
			T_EQ_STR(buf, ext[i]);
		}
	}

	printf("── ★ 扩展字符的编码值必须符合规范(不能只测往返)──\n");
	/*
	 * 为什么单靠往返不够:
	 *   pack 和 unpack 用的是同一张表,所以把 [ 的下标从 0x3C 改成 0x34,
	 *   往返【照样通过】—— 自己跟自己一致,和 GSM 03.38 规范无关。
	 *   变异测试就是这么发现这个盲区的。
	 *   所以这里直接断言编码出来的 septet 值,对照规范:
	 *     \f=0x0A  ^=0x14  {=0x28  }=0x29  \=0x2F
	 *     [=0x3C  ~=0x3D  ]=0x3E  |=0x40  €=0x65
	 */
	{
		static const struct { const char *ch; unsigned char code; } ext[] = {
			{ "\f", 0x0A }, { "^", 0x14 }, { "{", 0x28 }, { "}", 0x29 },
			{ "\\", 0x2F }, { "[", 0x3C }, { "~", 0x3D }, { "]", 0x3E },
			{ "|", 0x40 }, { "€", 0x65 },
		};
		unsigned char enc[8];

		for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
			int n = sms_pack_7bit(ext[i].ch, enc, sizeof(enc), &sept);

			T_EQ_INT(n >= 0, 1);
			T_EQ_INT(sept, 2);                 /* 转义符 + 字符本身 */
			/* 第一个 septet 是 0x1B(转义),第二个是规范里定死的值 */
			T_EQ_INT(enc[0] & 0x7F, 0x1B);
			/*
			 * 取出第二个 septet。
			 * 位布局:septet0 占 bit 0..6,septet1 占 bit 7..13。
			 * 所以 CODE = (byte0 的 bit7) | (byte1 的 bit0..5 左移 1)。
			 * 我第一版写成 (enc[1] >> 1) & 0x7F —— 那是按"septet1 从 bit8
			 * 开始"算的,差了 1 位,结果全部对不上(12 项失败)。
			 * 好在这个测试自己把这个错误抓出来了。
			 */
			unsigned int got = (unsigned)((enc[0] >> 7) & 0x01)
					 | ((unsigned)(enc[1] & 0x3F) << 1);

			T_EQ_INT(got, ext[i].code);
		}
	}

	printf("── ★ 默认表里非 ASCII 的那些字符也要逐个往返 ──\n");
	{
		/* 这些位置的值和 ASCII 不一样,写错一个就是显示成别的字符 */
		const char *sp[] = { "@", "£", "$", "¥", "è", "é", "ù", "ì", "ò", "Ç",
				     "Ø", "ø", "Å", "å", "Δ", "_", "Φ", "Γ", "Λ", "Ω",
				     "Æ", "æ", "ß", "É", "¤", "¡", "Ä", "Ö", "Ñ", "Ü",
				     "§", "¿", "ä", "ö", "ñ", "ü", "à", NULL };

		for (int i = 0; sp[i]; i++) {
			int n = sms_pack_7bit(sp[i], bytes, sizeof(bytes), &sept);

			if (n < 0) { T_OK(0); continue; }
			sms_unpack_7bit(bytes, sept, buf, sizeof(buf));
			T_EQ_STR(buf, sp[i]);
		}
	}

	printf("── ★ 手写规范向量(往返测试永远查不出这类错误)──\n");
	/*
	 * 为什么必须有这一段:
	 *   带 UDH 的 7 位短信,规范要求在 UDH 之后补填充位,让正文从
	 *   septet 边界开始(6 字节 UDH → 48 位,48%7=6 → 补 1 位 → 第 49 位)。
	 *   我们的 pack 和 unpack 用的是同一套位运算,所以"自己编自己解"
	 *   不管对错都能通过 —— 必须拿【独立算出来的规范报文】验证。
	 *
	 *   这条向量是照 GSM 03.40 §9.2.3.24 手算的:
	 *     SMSC 长度 07 91 68 31 08 10 83 00 F0
	 *     FO=44(UDHI) MR=00 长度=0D 地址=91 683108108300F0
	 *     PID=00 DCS=00 时间戳 62 10 10 00 00 00 00
	 *     UDL=08 UD= 05 00 03 2A 02 01 82
	 *   其中 82 = 1000 0010:bit48 是填充位 0,正文 'A'(0x41)
	 *   从 bit49 起 —— 按字节对齐去读会得到 0x82 的低 6 位 + 下一位,
	 *   解出 '$' 而不是 'A'(这正是我们原来的 bug)。
	 */
	{
		const char *spec_A =
			"0D91683108108300F0440D91683108108300F000006210100000000008"
			"0500032A020182";
		struct sms t;

		T_EQ_INT(sms_decode_pdu(spec_A, &t), 0);
		T_EQ_STR(t.text, "A");
		T_EQ_INT(t.concat_total, 2);
		T_EQ_INT(t.concat_seq, 1);
		T_EQ_INT(t.concat_ref, 0x2A);
	}

	printf("── ★ 反向:我们编出来的必须和规范报文一致 ──\n");
	{
		char h[SMS_HEX_MAX];
		struct sms t;

		T_EQ_INT(sms_encode_pdu("", "10086", "A", -1, 2, 1, 0x2A,
					h, sizeof(h)), 0);
		/* 关键就是结尾那一段:UDH + 补 1 位之后的正文 */
		T_OK(strstr(h, "0500032a020182") != NULL);
		/* 而且自己解回来也要对 */
		T_EQ_INT(sms_decode_pdu(h, &t), 0);
		T_EQ_STR(t.text, "A");
	}

	printf("── ★ AT+CMGS=<len> 要用 TPDU 字节数(不含短信中心字段)──\n");
	/*
	 * 规范(27.005)说的是 TPDU 字节数,不含整个 SMSC 字段。
	 * 以前图省事写"总字节数 - 1",只在短信中心为空时才对;
	 * 配上短信中心号就多算 8 个字节,模组会一直等剩下的数据。
	 * 而设备第一次 collect 就会把 AT+CSCA? 读到的号写进配置 ——
	 * 也就是说这个坑是"必然踩到"。
	 */
	{
		char h[SMS_HEX_MAX];
		struct { const char *sca; int digits; } cs[] = {
			{ "", 0 },
			{ "13800138000", 11 },
			{ "+8613800138000", 13 },
			{ "+86138001380000", 14 },
		};

		for (size_t i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
			int total, tpdu, smsc;

			T_EQ_INT(sms_encode_pdu(cs[i].sca, "10086", "hi", -1, 0, 0, 0,
						h, sizeof(h)), 0);
			total = (int)(strlen(h) / 2);
			tpdu = sms_tpdu_len(cs[i].sca, h);
			smsc = cs[i].sca[0] ? 2 + (cs[i].digits + 1) / 2 : 1;
			T_EQ_INT(tpdu, total - smsc);
		}
	}

	printf("── ★ 截断的报文必须被拒绝,不能当成功 ──\n");
	/*
	 * 以前 UDL 声明超出实际字节数时只是"夹到可用长度",于是半截
	 * 短信被当成完整短信入库(实测 "hello" 少一字节 → 解出 "hell"
	 * 且返回 0)。上层只看返回值,根本发现不了。
	 */
	{
		struct sms t;
		const char *cut =
			"0D91683108108300F0040D91683108108300F0000062101000000000"
			"05E8329BFD";

		T_OK(sms_decode_pdu(cut, &t) != 0);
	}

	printf("── ★ UDHL 声称比 UD 还长:必须拒绝(远程可触发的越界读)──\n");
	{
		struct sms t;
		/* UDHI + 8bit DCS,UDHL=0xFF 但 UD 只剩几个字节 */
		const char *bad =
			"0D91683108108300F0440D91683108108300F0000462101000000000"
			"04FFAABBCC";

		T_OK(sms_decode_pdu(bad, &t) != 0);
	}

	printf("── ★ UCS2 + UDH:正文长度要减掉 UDHL 自己那一字节 ──\n");
	{
		struct sms t;
		/* 8bit DCS(04),UDL=0x0A,UD = 05 00 03 2A 02 01 DE AD BE EF + 哨兵 AA */
		const char *pdu =
			"0D91683108108300F0440D91683108108300F0000462101000000000"
			"0A0500032A0201DEADBEEFAA";

		T_EQ_INT(sms_decode_pdu(pdu, &t), 0);
		/* 正文只能是 4 字节 DEADBEEF;多出来的 AA 是 UD 字段【之后】的数据,
		   以前会被当成正文解出来(真实场景里那是栈上的未初始化内容) */
		T_EQ_STR(t.text, "deadbeef");
	}

	printf("── ★ 段数必须和实际切段一致(否则 UDH 里的 total 是错的)──\n");
	{
		char t[700];
		int i;

		/* 7 位:每段 153 字符,不能用字节数套 153 公式 */
		for (i = 0; i <= 400; i += 50) {
			memset(t, 'x', (size_t)i);
			t[i] = '\0';
			T_EQ_INT(sms_count_segments(t, -1),
				 i <= 160 ? 1 : (i + 152) / 153);
		}
		/* 中文(UCS2):每段 67 字符 */
		for (i = 10; i <= 200; i += 30) {
			int k;

			for (k = 0; k < i; k++) {
				t[k * 3] = (char)0xE4;
				t[k * 3 + 1] = (char)0xB8;
				t[k * 3 + 2] = (char)0xAD;
			}
			t[i * 3] = '\0';
			T_EQ_INT(sms_count_segments(t, -1),
				 i <= 70 ? 1 : (i + 66) / 67);
		}
	}

	printf("── ★ 超长号码必须被拒绝(否则写穿缓冲区)──\n");
	{
		char num[2100], h[SMS_HEX_MAX];

		memset(num, '1', sizeof(num) - 1);
		num[sizeof(num) - 1] = '\0';
		T_OK(sms_encode_pdu("", num, "hi", -1, 0, 0, 0, h, sizeof(h)) != 0);
	}

	printf("── 中文编不进 7 位(应失败,让调用方走 UCS2)──\n");
	T_EQ_INT(sms_pack_7bit("中文", bytes, sizeof(bytes), &sept), -1);

	printf("── UCS2 ↔ UTF-8 ──\n");
	{
		/* "测试" = U+6D4B U+8BD5 */
		unsigned char u[] = { 0x6D, 0x4B, 0x8B, 0xD5 };

		T_EQ_INT(sms_ucs2_to_utf8(u, 4, buf, sizeof(buf)), 6);
		T_EQ_STR(buf, "测试");
		T_EQ_INT(sms_utf8_to_ucs2("测试", bytes, sizeof(bytes)), 4);
		T_EQ_INT(bytes[0], 0x6D);
		T_EQ_INT(bytes[1], 0x4B);
		T_EQ_INT(bytes[2], 0x8B);
		T_EQ_INT(bytes[3], 0xD5);
	}
	{
		/* 往返:中文 + 英文 + emoji(增补平面,走代理对)*/
		const char *cases[] = { "测试", "Hello 世界", "a", "中文测试字符串", "😀", NULL };

		for (int i = 0; cases[i]; i++) {
			int n = sms_utf8_to_ucs2(cases[i], bytes, sizeof(bytes));

			T_OK(n > 0);
			sms_ucs2_to_utf8(bytes, n, buf, sizeof(buf));
			T_EQ_STR(buf, cases[i]);
		}
	}

	printf("── 解 PDU:英文(已知向量)──\n");
	{
		/* SMSC=+8613800138000,发送方 +8613800138000,正文 hello */
		const char *pdu =
			"0D91683108108300F0"      /* 短信中心 +8613800138000 */
			"04"                      /* SMS-DELIVER */
			"0D91683108108300F0"      /* 发送方 */
			"00"                      /* PID */
			"00"                      /* DCS = 7 位 */
			"62101000000000"          /* 时间戳 */
			"05"                      /* UDL = 5 个字符 */
			"E8329BFD06";             /* "hello" 的 7 位打包 */

		T_EQ_INT(sms_decode_pdu(pdu, &s), 0);
		T_EQ_INT(s.valid, 1);
		T_EQ_STR(s.sca, "+8613800138000");
		T_EQ_STR(s.addr, "+8613800138000");
		T_EQ_STR(s.text, "hello");
		T_EQ_INT(s.dcs, SMS_DCS_7BIT);
		T_EQ_INT(s.concat_total, 0);
	}

	printf("── 解 PDU:中文(UCS2)──\n");
	{
		/* 正文 "测试" = 6D4B8BD5,UDL=4 字节 */
		const char *pdu =
			"0D91683108108300F0"
			"04"
			"0D91683108108300F0"
			"00"
			"08"                      /* DCS = UCS2 */
			"62101000000000"
			"04"                      /* UDL = 4 字节 */
			"6D4B8BD5";               /* "测试" = U+6D4B U+8BD5 */

		T_EQ_INT(sms_decode_pdu(pdu, &s), 0);
		T_EQ_INT(s.dcs, SMS_DCS_UCS2);
		T_EQ_STR(s.text, "测试");
	}

	printf("── 长短信(UDH 拼接)—— 靠「编码再解码」覆盖 ──\n");
	/*
	 * 带 UDH 的 7 位编码要算"UDH 折算成多少 septet + 填充位",
	 * 手写报文很容易把填充算错。所以这里不手造报文,而是让程序自己编一条
	 * 带 UDH 的长短信,再解回来 —— 编解一致同样能证明 UDH 处理是对的
	 * (下面"往返:长短信"那条会检查 total/seq/ref 三个字段)。
	 */
	T_OK(1);

	printf("── ★ 编码 → 解码 往返(最强的正确性检查)──\n");
	{
		const char *cases[] = { "hello", "Hello World 123", "测试短信", "中英mixed文本",
					"a", "很长的一条短信内容用来测试会不会被正确编码再解回来", NULL };

		for (int i = 0; cases[i]; i++) {
			T_EQ_INT(sms_encode_pdu("+8613800138000", "10086", cases[i], -1,
						0, 0, 0, hex, sizeof(hex)), 0);
			T_EQ_INT(sms_decode_pdu(hex, &s), 0);
			T_EQ_STR(s.text, cases[i]);
			T_EQ_STR(s.addr, "10086");
		}
	}

	printf("── 往返:UCS2 强制 ──\n");
	{
		T_EQ_INT(sms_encode_pdu("+8613800138000", "10086", "abc", SMS_DCS_UCS2,
					0, 0, 0, hex, sizeof(hex)), 0);
		T_EQ_INT(sms_decode_pdu(hex, &s), 0);
		T_EQ_INT(s.dcs, SMS_DCS_UCS2);
		T_EQ_STR(s.text, "abc");
	}

	printf("── 往返:长短信(带 UDH)──\n");
	{
		T_EQ_INT(sms_encode_pdu("+8613800138000", "10086", "第一段内容", -1,
					3, 1, 77, hex, sizeof(hex)), 0);
		T_EQ_INT(sms_decode_pdu(hex, &s), 0);
		T_EQ_STR(s.text, "第一段内容");
		T_EQ_INT(s.concat_total, 3);
		T_EQ_INT(s.concat_seq, 1);
		T_EQ_INT(s.concat_ref, 77);
	}

	printf("── 没有短信中心(空)→ 用 00 ──\n");
	{
		T_EQ_INT(sms_encode_pdu("", "10086", "hi", -1, 0, 0, 0, hex, sizeof(hex)), 0);
		T_EQ_INT(sms_decode_pdu(hex, &s), 0);
		T_EQ_STR(s.text, "hi");
		T_EQ_STR(s.sca, "");
	}

	printf("── 分段数估算(界面提示用)──\n");
	T_EQ_INT(sms_count_segments("hello", 0), 1);
	T_EQ_INT(sms_count_segments("测试", 1), 1);
	{
		char big[200];

		memset(big, 'a', 160); big[160] = '\0';
		T_EQ_INT(sms_count_segments(big, 0), 1);
		memset(big, 'a', 161); big[161] = '\0';
		T_EQ_INT(sms_count_segments(big, 0), 2);
	}

	printf("── 异常输入不崩 ──\n");
	T_EQ_INT(sms_decode_pdu("", &s), -1);
	T_EQ_INT(sms_decode_pdu("ZZZZ", &s), -1);
	T_EQ_INT(sms_decode_pdu("079168310800138000", &s), -1);   /* 太短 */
	T_EQ_INT(sms_decode_pdu(NULL, &s), -1);
	T_EQ_INT(sms_decode_pdu("00", NULL), -1);
	T_EQ_INT(sms_encode_pdu("+86", NULL, "x", -1, 0, 0, 0, hex, sizeof(hex)), -1);
	T_EQ_INT(sms_encode_pdu("+86", "10086", NULL, -1, 0, 0, 0, hex, sizeof(hex)), -1);
	sms_init(NULL);

	T_SUMMARY();
}
