/* OpenFi 后端公共库 —— UCI 配置访问
 *
 * 【为什么不用 libuci】
 *   UCI 配置文件就是三行语法:
 *       config <type> ['<name>']
 *       option <key> '<value>'
 *       list   <key> '<value>'
 *   自己解析 = 纯 C + 只用 stdio
 *     ① 100% 能在开发机上编译并【跑单元测试】(libuci 只有目标机才有)
 *     ② 少一个依赖,固件更小
 *     ③ 没有 ABI 版本问题
 *   写操作走 `uci` 命令(改动很少发生,不值得为它引入解析器的复杂度)
 *
 * 【测试怎么用】
 *   测试里调 ouci_set_root("/tmp/xxx"),自己造几个假配置文件,就能测了。
 */
#ifndef OPENFI_UCI_H
#define OPENFI_UCI_H

/* 覆盖配置根目录,默认 /etc/config。测试用;传 NULL 恢复默认 */
void ouci_set_root(const char *root);

/* 读一个 option。返回 malloc 的字符串,调用者 free;不存在返回 NULL */
char *ouci_get(const char *pkg, const char *sec, const char *opt);

/* 读一个 option,带默认值(永不返回 NULL)。
 * ⚠️ 缺项时返回的【就是你传进来的 def 指针】,所以【不要】把 def 传成
 *    你打算写入的目标缓冲区 —— 那会变成 snprintf(buf, n, "%s", buf),
 *    重叠拷贝是 UB。先放进临时变量。 */

const char *ouci_get_def(const char *pkg, const char *sec, const char *opt, const char *def);

/* 读整数,带默认值;非法值也返回默认值 */
long ouci_get_int(const char *pkg, const char *sec, const char *opt, long def);

/* 读布尔:on/1/true/yes/enabled → 1;off/0/false/no/disabled → 0;缺省用 def */
int ouci_get_bool(const char *pkg, const char *sec, const char *opt, int def);

/* 写一个 option 并 commit(走 uci 命令)。成功 0,失败 -1 */
int ouci_set(const char *pkg, const char *sec, const char *opt, const char *val);

/* 把配置读成一个 list(测 section 是否存在等)。返回逗号分隔的 option 名,调用者 free */
char *ouci_list_options(const char *pkg, const char *sec);

#endif
