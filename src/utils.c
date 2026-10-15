#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>      /* _commit：rename 之前把临时文件真正落盘 */
#define mkdir(p) _mkdir(p)
#else
#include <unistd.h>
#include <libgen.h>
#endif

/* ============== 动态字符串 ============== */

strbuf_t* strbuf_create(size_t initial_capacity)
{
    strbuf_t *buf = (strbuf_t*)malloc(sizeof(strbuf_t));
    if (!buf) return NULL;
    if (initial_capacity < 16) initial_capacity = 16;
    buf->data = (char*)malloc(initial_capacity);
    if (!buf->data) { free(buf); return NULL; }
    buf->data[0] = '\0';
    buf->length = 0;
    buf->capacity = initial_capacity;
    return buf;
}

void strbuf_destroy(strbuf_t *buf)
{
    if (buf) {
        if (buf->data) free(buf->data);
        free(buf);
    }
}

static int strbuf_ensure(strbuf_t *buf, size_t needed)
{
    if (buf->length + needed + 1 <= buf->capacity) return 1;
    size_t new_cap = buf->capacity * 2;
    while (new_cap < buf->length + needed + 1) new_cap *= 2;
    char *new_data = (char*)realloc(buf->data, new_cap);
    /* 扩容失败必须报告，否则调用方会按新长度写入过小的旧缓冲区，造成堆溢出 */
    if (!new_data) return 0;
    buf->data = new_data;
    buf->capacity = new_cap;
    return 1;
}

void strbuf_append(strbuf_t *buf, const char *str)
{
    if (!buf || !str) return;
    size_t len = strlen(str);
    if (!strbuf_ensure(buf, len)) return;
    memcpy(buf->data + buf->length, str, len + 1);
    buf->length += len;
}

void strbuf_append_len(strbuf_t *buf, const char *str, size_t len)
{
    if (!buf || !str || len == 0) return;
    if (!strbuf_ensure(buf, len)) return;
    memcpy(buf->data + buf->length, str, len);
    buf->length += len;
    buf->data[buf->length] = '\0';
}

void strbuf_append_char(strbuf_t *buf, char c)
{
    if (!buf) return;
    if (!strbuf_ensure(buf, 1)) return;
    buf->data[buf->length++] = c;
    buf->data[buf->length] = '\0';
}

void strbuf_clear(strbuf_t *buf)
{
    if (!buf) return;
    buf->length = 0;
    if (buf->data) buf->data[0] = '\0';
}

char* strbuf_dup(strbuf_t *buf)
{
    if (!buf || !buf->data) return NULL;
    return _strdup(buf->data);
}

/* ============== 文件操作 ============== */

char* read_file(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    char *buf = (char*)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);
    /* 跳过 UTF-8 BOM：cJSON 不接受 EF BB BF 开头的 JSON，带 BOM 的
       config.json / project.json 会整体解析失败并静默回退默认配置 */
    if (read >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, read - 3 + 1);
    }
    return buf;
}

int write_file(const char *path, const char *content)
{
    if (!path || !content) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    return (written == len) ? 0 : -1;
}

int write_file_atomic(const char *path, const char *content)
{
    if (!path || !content) return -1;
    size_t len = strlen(path);
    if (len + 24 >= 1024) return -1;

    /* 临时名带上进程号：两个进程写同一目标时不会互踩同一个 .tmp */
    char tmp[1024];
#ifdef _WIN32
    snprintf(tmp, sizeof(tmp), "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
#else
    snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)getpid());
#endif

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;

    size_t content_len = strlen(content);
    int ok = (fwrite(content, 1, content_len, f) == content_len);
    if (ok && fflush(f) != 0) ok = 0;   /* 磁盘满/设备错误只有 flush 才暴露 */
#ifdef _WIN32
    /* fflush 只保证进了 OS 缓存，掉电前必须 _commit 落盘；
       否则 rename 成功也会得到"已替换但内容为空"的文件 */
    if (ok && _commit(_fileno(f)) != 0) ok = 0;
#endif
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return -1; }

#ifdef _WIN32
    /* 原子替换：MOVEFILE_WRITE_THROUGH 保证 rename 本身也落盘；
       失败时目标文件保持原样 */
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp);
        return -1;
    }
#else
    remove(path);   /* POSIX rename 覆盖目标，失败则目标已丢失，故仅此分支非原子 */
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
#endif
    return 0;
}

int ensure_dir(const char *path)
{
    if (!path || !*path) return -1;
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '\\' || tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp);
            tmp[i] = path[i];
        }
    }
    mkdir(tmp);
    return 0;
}

int file_exists(const char *path)
{
    if (!path) return 0;
    struct stat st;
    return (stat(path, &st) == 0);
}

/* ============== 字符串工具 ============== */

char* str_dup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = (char*)malloc(len);
    if (!d) return NULL;
    memcpy(d, s, len);
    return d;
}

size_t utf8_safe_len(const char *s, size_t max_bytes)
{
    if (!s) return 0;
    size_t len = strlen(s);
    if (len <= max_bytes) return len;
    size_t i = max_bytes;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

char* utf8_sanitize(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    /* 最坏情况：每个输入字节都替换成 3 字节的 U+FFFD */
    char *out = (char*)malloc(len * 3 + 1);
    if (!out) return str_dup(s);
    size_t i = 0, o = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        unsigned int min_cp;
        if (c < 0x80) { out[o++] = (char)c; i++; continue; }
        if (c >= 0xC2 && c <= 0xDF)      { need = 2; min_cp = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 3; min_cp = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 4; min_cp = 0x10000; }
        else { need = 0; min_cp = 0; }
        if (need == 0 || i + need > len) {
            out[o++] = (char)0xEF; out[o++] = (char)0xBF; out[o++] = (char)0xBD;
            i++;
            continue;
        }
        unsigned int cp = (unsigned int)(c & (0xFF >> (need + 1)));
        size_t k;
        int valid = 1;
        for (k = 1; k < need; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { valid = 0; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        /* 拒绝过长编码、落单代理项、超界码点 */
        if (!valid || cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out[o++] = (char)0xEF; out[o++] = (char)0xBF; out[o++] = (char)0xBD;
            i++;
            continue;
        }
        for (k = 0; k < need; k++) out[o++] = s[i + k];
        i += need;
    }
    out[o] = '\0';
    return out;
}

size_t utf8_char_count(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (*s) {
        /* 统计首字节个数即码点个数；跳过续接字节，对残缺序列也能安全终止 */
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
        s++;
    }
    return n;
}

/* ============== 时间工具 ============== */

char* get_timestamp_str(void)
{
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char *buf = (char*)malloc(64);
    if (!buf) return NULL;
    strftime(buf, 64, "%Y%m%d_%H%M%S", tm_info);
    return buf;
}

/* ============== 路径工具 ============== */

char* path_join(const char *a, const char *b)
{
    if (!a && !b) return NULL;
    if (!a) return str_dup(b);
    if (!b) return str_dup(a);
    size_t la = strlen(a), lb = strlen(b);
    char *result = (char*)malloc(la + lb + 2);
    if (!result) return NULL;
    strcpy(result, a);
    if (la > 0 && result[la - 1] != '\\' && result[la - 1] != '/') {
        result[la++] = '\\';
        result[la] = '\0';
    }
    strcat(result, b);
    return result;
}

char* get_exe_dir(void)
{
#ifdef _WIN32
    char *buf = (char*)malloc(1024);
    if (!buf) return NULL;
    DWORD len = GetModuleFileNameA(NULL, buf, 1024);
    if (len == 0) { free(buf); return NULL; }
    /* 缓冲区不足时返回值等于 nSize，此时没有写入终止符，必须夹紧索引 */
    if (len >= 1024) len = 1023;
    buf[len] = '\0';
    char *last_slash = strrchr(buf, '\\');
    if (last_slash) *last_slash = '\0';
    return buf;
#else
    return str_dup(".");
#endif
}
