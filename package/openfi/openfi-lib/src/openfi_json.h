/* OpenFi 后端公共库 —— 极简 JSON 输出(自带,不依赖 json-c)
 *
 * 【为什么不用 json-c】
 *   我们只需要"构造一个对象然后打印出来",不需要解析 —— 自己写几十行就够。
 *   去掉这个依赖带来三个好处:
 *     ① 固件更小、包少一个依赖
 *     ② 交叉编译变简单 —— 于是可以用 aarch64-linux-gnu-gcc -static
 *        编出二进制,放进真机/虚拟机里【真跑一遍】
 *        (json-c 的 aarch64 版本本地没有,留着它就做不了这件事)
 *     ③ 本机就能用 qemu-aarch64 跑同一份二进制,提前发现架构/ABI 问题
 *
 * 能力范围:平面对象(键值对)+ 一个 oj_raw 用于嵌入已经拼好的片段。
 * 我们的状态输出都是平面的,够用。
 */
#ifndef OPENFI_JSON_H
#define OPENFI_JSON_H

struct ojson;

/* 新建一个对象 */
struct ojson *oj_new(void);

/* 加字段。值是 NULL 时写 null(省得调用方到处判空) */
void oj_str(struct ojson *o, const char *k, const char *v);
void oj_int(struct ojson *o, const char *k, long v);
void oj_bool(struct ojson *o, const char *k, int v);
void oj_null(struct ojson *o, const char *k);

/*
 * 嵌入一段已经拼好的 JSON(用于嵌套对象/数组)。
 * raw 必须本身是合法 JSON 片段 —— 不做校验,调用方负责。
 * raw 为 NULL 时写 null。
 */
void oj_raw(struct ojson *o, const char *k, const char *raw);

/* 打成一行 JSON 输出到 stdout,并释放对象 */
void oj_emit(struct ojson *o);

/* 渲染成字符串(malloc,调用者 free),对象保留 —— 需要一处数据多处用时 */
char *oj_render(struct ojson *o);

/* 释放对象(不输出) */
void oj_free(struct ojson *o);

/* 统一的错误输出:{"ok":false,"error":"..."},返回 1(方便直接 return) */
int oj_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
