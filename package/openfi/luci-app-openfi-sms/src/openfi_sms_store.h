/* OpenFi 6C —— 短信本地存储
 *
 * 设计:一条短信一个文件(/etc/openfi-sms/<id>.json)。
 *   · 不需要数据库,不需要加锁 —— 删除就是 unlink,写入就是 rename(原子)
 *   · 掉电也不会写坏:先写 .tmp 再 rename
 *   · id 用"收到时间 + 序号",天然按时间排序
 *
 * 目录可以用环境变量 OPENFI_SMS_DIR 换掉 —— 于是存储逻辑在开发机上也能完整测。
 */
#ifndef OPENFI_SMS_STORE_H
#define OPENFI_SMS_STORE_H

#include "openfi_sms.h"

#include <time.h>

#define SMS_ID_MAX    48
#define SMS_DIR_MAX   200

/* 一条存下来的短信 */
struct sms_msg {
	char id[SMS_ID_MAX];
	struct sms pdu;			/* 解码后的内容 */
	int  read;			/* 0 = 未读 */
	long received_at;		/* unix 时间戳 */
	int  outgoing;			/* 1 = 自己发出去的 */
	int  status;			/* 发送状态(仅 outgoing 有意义)*/
};

/* 覆盖存储目录(测试用);NULL 恢复默认 /etc/openfi-sms */
/*
 * id 是否合法。
 * ★ id 会被直接拼进文件路径,不校验的话 `delete ../../tmp/x`
 *   就能删掉任意 *.json(`read` 也能把别的文件当短信读出来)。
 *   我们生成的 id 只含字母数字和短横线,按这个白名单卡住。
 */
int sms_id_ok(const char *id);

void sms_store_set_dir(const char *dir);
const char *sms_store_dir(void);

/* 确保目录存在。成功 0,失败 -1 */
int sms_store_init(void);

/* 存一条,id 为空则自动生成。成功 0(会把 id 回填到 msg->id) */
int sms_store_save(struct sms_msg *msg);

/* 按 id 读一条。成功 0,找不到 -1 */
int sms_store_get(const char *id, struct sms_msg *msg);

/* 删除一条。成功 0 */
int sms_store_delete(const char *id);

/* 全部删除。返回删掉的条数 */
int sms_store_clear(void);

/* 标记已读/未读 */
int sms_store_mark(const char *id, int read);

/*
 * 列出所有短信,按时间倒序(新的在前)。
 *   out  调用方给的数组
 *   max  最多填几条
 * 返回实际条数(可能大于 max,用于分页提示)
 */
/*
 * 列出短信,新的在前。
 * ★ 契约:out 必须至少有 SMS_STORE_MAX_LIST 项 —— 返回值是
 *   【实际填入的条数】,不会超过 min(max, 库存, SMS_STORE_MAX_LIST)。
 */
#define SMS_STORE_MAX_LIST 512
int sms_store_list(struct sms_msg *out, int max);

/* 未读条数 */
int sms_store_unread(void);

/* 总条数 */
int sms_store_count(void);

#endif
