/* OpenFi 后端公共库 —— sysfs 小工具
 *
 * 【可测性】所有函数都接受一个 root 前缀,默认 "/"。
 * 测试时把 root 指到一个临时目录,造几个假文件就能完整测。
 * (读 LED / PWM / thermal 本质上就是读文件,所以这样测和真机行为一致。)
 */
#ifndef OPENFI_SYSFS_H
#define OPENFI_SYSFS_H

#include <stddef.h>

/* 设置 root 前缀(测试用);NULL 恢复 "/" */
void osys_set_root(const char *root);

/* 读一个 sysfs 文件的第一个"词"(去掉首尾空白)。
   返回 malloc 的字符串,调用者 free;读不到返回 NULL */
char *osys_read(const char *path);

/* 读成整数(毫度/百分比等)。读不到或非法返回 def */
long osys_read_int(const char *path, long def);

/* 写字符串进 sysfs 文件(自动补换行)。文件不存在会创建。成功 0,失败 -1 */
int osys_write(const char *path, const char *val);

/* 写整数 */
int osys_write_int(const char *path, long val);

/* 判断路径是否存在(文件或目录) */
int osys_exists(const char *path);

/* 列目录下匹配 prefix 的名字到 out(逗号分隔)。返回找到的个数 */
int osys_list_dir(const char *dir, const char *prefix, char *out, size_t outlen);

/* 把 root + path 拼起来(内部小工具,也给上层复用) */
void osys_path(char *out, size_t outlen, const char *path);

#endif
