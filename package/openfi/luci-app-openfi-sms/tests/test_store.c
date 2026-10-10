/* 短信存储层测试 —— 用临时目录,建/读/列/删/标记全测一遍 */
#define _GNU_SOURCE
#include "../src/openfi_sms_store.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DIR "/tmp/openfi-test-smsdir"

int main(void)
{
	struct sms_msg m, out[64];
	int n;

	system("rm -rf " DIR);
	sms_store_set_dir(DIR);
	T_EQ_STR(sms_store_dir(), DIR);
	T_EQ_INT(sms_store_init(), 0);
	{ struct stat st; T_OK(stat(DIR, &st) == 0); }

	printf("── 存一条 ──\n");
	memset(&m, 0, sizeof(m));
	snprintf(m.pdu.addr, sizeof(m.pdu.addr), "+8613800138000");
	snprintf(m.pdu.text, sizeof(m.pdu.text), "测试短信 hello");
	snprintf(m.pdu.ts, sizeof(m.pdu.ts), "2026-10-10 15:30:12");
	m.pdu.dcs = SMS_DCS_UCS2;
	T_EQ_INT(sms_store_save(&m), 0);
	T_OK(m.id[0] != '\0');       /* id 自动生成并回填 */
	T_EQ_INT(sms_store_count(), 1);

	printf("── 读回来 ──\n");
	{
		struct sms_msg r;

		T_EQ_INT(sms_store_get(m.id, &r), 0);
		T_EQ_STR(r.id, m.id);
		T_EQ_STR(r.pdu.addr, "+8613800138000");
		T_EQ_STR(r.pdu.text, "测试短信 hello");
		T_EQ_STR(r.pdu.ts, "2026-10-10 15:30:12");
		T_EQ_INT(r.pdu.dcs, SMS_DCS_UCS2);
		T_EQ_INT(r.read, 0);
		T_EQ_INT(r.outgoing, 0);
	}

	printf("── JSON 转义(正文里的引号/换行不能把文件写坏)──\n");
	{
		struct sms_msg q, r;

		memset(&q, 0, sizeof(q));
		snprintf(q.pdu.text, sizeof(q.pdu.text), "带\"引号\"和\\反斜杠\n换行");
		snprintf(q.pdu.addr, sizeof(q.pdu.addr), "10086");
		T_EQ_INT(sms_store_save(&q), 0);
		T_EQ_INT(sms_store_get(q.id, &r), 0);
		T_EQ_STR(r.pdu.text, "带\"引号\"和\\反斜杠\n换行");
		sms_store_delete(q.id);
	}

	printf("── 未读数 ──\n");
	{
		struct sms_msg a;

		memset(&a, 0, sizeof(a));
		snprintf(a.pdu.addr, sizeof(a.pdu.addr), "10086");
		snprintf(a.pdu.text, sizeof(a.pdu.text), "第二条");
		/* 故意设成"很久以后",验证排序真的按时间倒序 */
		a.received_at = 4102444800L;   /* 2100-01-01 */
		sms_store_save(&a);
	}
	T_EQ_INT(sms_store_count(), 2);
	T_EQ_INT(sms_store_unread(), 2);
	T_EQ_INT(sms_store_mark(m.id, 1), 0);
	T_EQ_INT(sms_store_unread(), 1);
	{
		struct sms_msg r;

		sms_store_get(m.id, &r);
		T_EQ_INT(r.read, 1);
	}

	printf("── 列表按时间倒序 ──\n");
	n = sms_store_list(out, 64);
	T_EQ_INT(n, 2);
	/* 第二条 received_at=2000 更新,应该排前面 */
	T_EQ_STR(out[0].pdu.text, "第二条");
	T_EQ_STR(out[1].pdu.text, "测试短信 hello");

	printf("── 分页(max 小于总数)──\n");
	n = sms_store_list(out, 1);
	/*
	 * ★ 返回值语义:【实际填入 out 的条数】,不是库存总数。
	 *   原先返回总数看着更"友好",但它逼着函数往调用方数组里塞
	 *   min(total, max) 条 —— 库存 520 条 + list 600 就直接越界写。
	 *   要总数请用 sms_store_count()(它独立数目录,不借道这里)。
	 */
	T_EQ_INT(n, 1);
	T_EQ_INT(sms_store_count(), 2);
	T_EQ_STR(out[0].pdu.text, "第二条");

	printf("── 删除 ──\n");
	T_EQ_INT(sms_store_delete(m.id), 0);
	T_EQ_INT(sms_store_count(), 1);
	T_EQ_INT(sms_store_delete("不存在的id"), -1);
	T_EQ_INT(sms_store_get(m.id, out), -1);

	printf("── 全删 ──\n");
	T_EQ_INT(sms_store_clear(), 1);
	T_EQ_INT(sms_store_count(), 0);
	T_EQ_INT(sms_store_unread(), 0);

	printf("── 目录建不出来时报错(不能静默失败)──\n");
	{
		sms_store_set_dir("/proc/不该能建/xyz");
		memset(&m, 0, sizeof(m));
		T_EQ_INT(sms_store_save(&m), -1);
		sms_store_set_dir(DIR);
	}

	printf("── 空目录也能列 ──\n");
	T_EQ_INT(sms_store_list(out, 8), 0);

	printf("── 空指针不崩 ──\n");
	T_EQ_INT(sms_store_save(NULL), -1);
	T_EQ_INT(sms_store_get(NULL, &m), -1);
	T_EQ_INT(sms_store_get("x", NULL), -1);
	sms_store_set_dir(NULL);
	T_EQ_STR(sms_store_dir(), "/etc/openfi-sms");

	system("rm -rf " DIR);
	T_SUMMARY();
}
