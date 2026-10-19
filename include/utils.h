#ifndef NOVEL_AI_UTILS_H
#define NOVEL_AI_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdio.h>

/* 动态字符串 */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} strbuf_t;

strbuf_t* strbuf_create(size_t initial_capacity);
void strbuf_destroy(strbuf_t *buf);
void strbuf_append(strbuf_t *buf, const char *str);
void strbuf_append_len(strbuf_t *buf, const char *str, size_t len);
void strbuf_append_char(strbuf_t *buf, char c);
void strbuf_clear(strbuf_t *buf);
char* strbuf_dup(strbuf_t *buf);
/* NULL 安全取文本：strbuf_create 失败（OOM）返回 NULL 时，直接解 buf->data 会崩。
   提示词场景下返回 "" 比崩掉好——AI 调用会因空提示词失败并走正常错误通道 */
static inline const char* strbuf_text(const strbuf_t *buf)
{
    return (buf && buf->data) ? buf->data : "";
}

/* 文件操作 */
char* read_file(const char *path);
int write_file(const char *path, const char *content);
/* 原子写：先写 path+".tmp"，校验 fwrite/fflush/fclose 全部成功后
   MoveFileEx 替换目标。任何一步失败都不碰原文件（返回 -1 并清理临时文件）。
   用于 project.json / config.json 这类"唯一副本"数据 */
int write_file_atomic(const char *path, const char *content);
int ensure_dir(const char *path);
int file_exists(const char *path);
/* UTF-8 路径安全 fopen：Windows 下走 _wfopen，「舞台图片」这类中文路径窄字符 fopen 会炸 */
FILE *fopen_utf8(const char *path, const char *mode);

/* 字符串工具 */
char* str_dup(const char *s);
/* 返回不超过 max_bytes 且不会切断 UTF-8 多字节序列的长度 */
size_t utf8_safe_len(const char *s, size_t max_bytes);
/* 返回一份合法 UTF-8 拷贝：非法序列、被截断的序列、落单代理项一律替换为 U+FFFD */
char* utf8_sanitize(const char *s);
/* 返回 UTF-8 字符数（码点数），用于中文正文字数统计 */
size_t utf8_char_count(const char *s);

/* 时间工具 */
char* get_timestamp_str(void);

/* 路径工具 */
char* path_join(const char *a, const char *b);
char* get_exe_dir(void);

#ifdef __cplusplus
}
#endif

#endif
