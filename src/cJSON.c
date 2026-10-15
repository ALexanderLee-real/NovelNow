/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
  Lightweight version for novelnow project
*/

#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

static void *(*m_malloc)(size_t sz) = malloc;
static void (*m_free)(void *ptr) = free;

void cJSON_InitHooks(cJSON_Hooks* hooks)
{
    if (!hooks) { m_malloc = malloc; m_free = free; return; }
    m_malloc = (hooks->malloc_fn) ? hooks->malloc_fn : malloc;
    m_free = (hooks->free_fn) ? hooks->free_fn : free;
}

void cJSON_free(void *object) { if (object) m_free(object); }

static void* m_alloc(size_t size)
{
    void *ptr = m_malloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

static const char *skip(const char *in)
{
    while (in && *in && (unsigned char)*in <= 32) in++;
    return in;
}

char* cJSON_strdup(const char* string)
{
    size_t len; char *copy;
    if (!string) return NULL;
    len = strlen(string) + 1;
    copy = (char*)m_malloc(len);
    if (!copy) return NULL;
    memcpy(copy, string, len);
    return copy;
}

/* Parse number */
static const char *parse_number(cJSON *item, const char *num)
{
    double n = 0, sign = 1, scale = 0;
    int subscale = 0, signsubscale = 1;

    if (*num == '-') { sign = -1; num++; }
    if (*num == '0') num++;
    if (*num >= '1' && *num <= '9')
        do { n = (n * 10.0) + (*num++ - '0'); }
        while (*num >= '0' && *num <= '9');
    if (*num == '.' && num[1] >= '0' && num[1] <= '9')
    {
        num++;
        do { n = (n * 10.0) + (*num++ - '0'); scale--; }
        while (*num >= '0' && *num <= '9');
    }
    if (*num == 'e' || *num == 'E')
    {
        num++;
        if (*num == '+') num++;
        else if (*num == '-') { signsubscale = -1; num++; }
        while (*num >= '0' && *num <= '9')
            subscale = (subscale * 10) + (*num++ - '0');
    }
    n = sign * n * pow(10.0, (scale + subscale * signsubscale));
    item->valuedouble = n;
    item->valueint = (int)n;
    item->type = cJSON_Number;
    return num;
}

static int pow2gt(int x) { --x; x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16; return x + 1; }

typedef struct { char *buffer; int length; int offset; } printbuffer;

static char* ensure(printbuffer *p, int needed)
{
    char *newbuffer; int newsize;
    if (!p || !p->buffer) return 0;
    needed += p->offset;
    if (needed <= p->length) return p->buffer + p->offset;
    newsize = pow2gt(needed);
    newbuffer = (char*)m_malloc(newsize);
    if (!newbuffer) { m_free(p->buffer); p->length = 0; p->buffer = 0; return 0; }
    memcpy(newbuffer, p->buffer, p->length);
    m_free(p->buffer);
    p->length = newsize;
    p->buffer = newbuffer;
    return newbuffer + p->offset;
}

/* Parse string */

/* 读取 4 位十六进制；遇到非十六进制字符（含结尾 NUL）立即失败，不会越界读取 */
static int hex4(const char *p, unsigned int *out)
{
    unsigned int v = 0;
    int k;
    for (k = 0; k < 4; k++)
    {
        unsigned char c = (unsigned char)p[k];
        if (c >= '0' && c <= '9') c = (unsigned char)(c - '0');
        else if (c >= 'a' && c <= 'f') c = (unsigned char)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') c = (unsigned char)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | c;
    }
    *out = v;
    return 1;
}

static const char *parse_string(cJSON *item, const char *str)
{
    const char *ptr = str + 1;
    char *ptr2, *out;
    int len = 0;
    unsigned uc;
    if (*str != '\"') return 0;

    while (*ptr != '\"' && *ptr && ++len)
        if (*ptr++ == '\\') ptr++;
    if (*ptr != '\"') return 0;

    /* len 统计的是“转义单元”个数，一个 \uXXXX 最多展开成 3 字节（代理对 4 字节 / 2 个单元），
       所以 len*3+1 是解码后字节数的安全上界 */
    if (len > 715827882) return 0;   /* 防止 len*3 溢出 */
    out = (char*)m_alloc((size_t)len * 3 + 1);
    if (!out) return 0;
    ptr = str + 1;
    ptr2 = out;
    while (*ptr != '\"' && *ptr)
    {
        if (*ptr != '\\') *ptr2++ = *ptr++;
        else
        {
            ptr++;
            switch (*ptr)
            {
                case 'b': *ptr2++ = '\b'; break;
                case 'f': *ptr2++ = '\f'; break;
                case 'n': *ptr2++ = '\n'; break;
                case 'r': *ptr2++ = '\r'; break;
                case 't': *ptr2++ = '\t'; break;
                case '\"': *ptr2++ = '\"'; break;
                case '\\': *ptr2++ = '\\'; break;
                case '/': *ptr2++ = '/'; break;
                case 'u':
                {
                    unsigned int uc2;
                    if (!hex4(ptr + 1, &uc2))
                    {
                        /* 非法 \u 转义：按字面量 'u' 处理，不越过字符串结尾 */
                        *ptr2++ = 'u';
                        break;
                    }
                    uc = uc2;
                    ptr += 4;
                    /* UTF-16 代理对必须合并成一个码点，否则会写出落单代理项（非法 UTF-8） */
                    if (uc >= 0xd800 && uc <= 0xdbff && ptr[1] == '\\' && ptr[2] == 'u')
                    {
                        unsigned int lo;
                        if (hex4(ptr + 3, &lo) && lo >= 0xdc00 && lo <= 0xdfff)
                        {
                            uc = 0x10000 + (((uc - 0xd800) << 10) | (lo - 0xdc00));
                            ptr += 6;
                        }
                    }
                    if (uc >= 0xd800 && uc <= 0xdfff) uc = 0xfffd;
                    if (uc < 0x80) *ptr2++ = (char)uc;
                    else if (uc < 0x800)
                    {
                        *ptr2++ = (char)(0xc0 | (uc >> 6));
                        *ptr2++ = (char)(0x80 | (uc & 0x3f));
                    }
                    else if (uc < 0x10000)
                    {
                        *ptr2++ = (char)(0xe0 | (uc >> 12));
                        *ptr2++ = (char)(0x80 | ((uc >> 6) & 0x3f));
                        *ptr2++ = (char)(0x80 | (uc & 0x3f));
                    }
                    else
                    {
                        *ptr2++ = (char)(0xf0 | (uc >> 18));
                        *ptr2++ = (char)(0x80 | ((uc >> 12) & 0x3f));
                        *ptr2++ = (char)(0x80 | ((uc >> 6) & 0x3f));
                        *ptr2++ = (char)(0x80 | (uc & 0x3f));
                    }
                    break;
                }
                default: *ptr2++ = *ptr; break;
            }
            ptr++;
        }
    }
    *ptr2 = 0;
    item->valuestring = out;
    item->type = cJSON_String;
    return ptr + 1;
}

static char* print_string_ptr(const char *str, printbuffer *p)
{
    const char *ptr;
    char *ptr2, *out;
    int len = 0;
    unsigned char token;
    if (!str) { out = p ? ensure(p, 3) : (char*)m_alloc(3); if (out) strcpy(out, "\"\""); if (p) p->offset += 2; return out; }

    for (ptr = str; *ptr; ptr++)
    {
        unsigned char c = (unsigned char)*ptr;
        if (c > 31 && c != '\"' && c != '\\') len += 1;
        else if (c == '\\' || c == '\"' || c == '\b' || c == '\f' ||
                 c == '\n' || c == '\r' || c == '\t') len += 2;
        else len += 6;   /* 其余控制字符按 \uXXXX 输出，占 6 字节 */
    }
    if (p) out = ptr2 = ensure(p, len + 3);
    else out = ptr2 = (char*)m_alloc(len + 3);
    if (!out) return 0;

    ptr = str;
    *ptr2++ = '\"';
    while (*ptr)
    {
        if ((unsigned char)*ptr > 31 && *ptr != '\"' && *ptr != '\\')
            *ptr2++ = *ptr++;
        else
        {
            *ptr2++ = '\\';
            token = (unsigned char)*ptr++;
            switch (token)
            {
                case '\\': *ptr2++ = '\\'; break;
                case '\"': *ptr2++ = '\"'; break;
                case '\b': *ptr2++ = 'b'; break;
                case '\f': *ptr2++ = 'f'; break;
                case '\n': *ptr2++ = 'n'; break;
                case '\r': *ptr2++ = 'r'; break;
                case '\t': *ptr2++ = 't'; break;
                default: sprintf(ptr2, "u%04x", token); ptr2 += 5; break;
            }
        }
    }
    *ptr2++ = '\"';
    *ptr2++ = 0;
    if (p) p->offset = (int)(ptr2 - p->buffer);
    return out;
}

static const char *parse_value(cJSON *item, const char *value);

static const char *parse_array(cJSON *item, const char *value)
{
    cJSON *child;
    if (*value != '[') return 0;
    item->type = cJSON_Array;
    value = skip(value + 1);
    if (*value == ']') return value + 1;
    item->child = child = (cJSON*)m_alloc(sizeof(cJSON));
    if (!item->child) return 0;
    value = skip(parse_value(child, skip(value)));
    if (!value) return 0;
    while (*value == ',')
    {
        cJSON *new_item = (cJSON*)m_alloc(sizeof(cJSON));
        if (!new_item) return 0;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_value(child, skip(value + 1)));
        if (!value) return 0;
    }
    if (*value == ']') return value + 1;
    return 0;
}

static const char *parse_object(cJSON *item, const char *value)
{
    cJSON *child;
    if (*value != '{') return 0;
    item->type = cJSON_Object;
    value = skip(value + 1);
    if (*value == '}') return value + 1;
    item->child = child = (cJSON*)m_alloc(sizeof(cJSON));
    if (!item->child) return 0;
    value = skip(parse_string(child, skip(value)));
    if (!value) return 0;
    child->string = child->valuestring;
    child->valuestring = 0;
    if (*value != ':') return 0;
    value = skip(parse_value(child, skip(value + 1)));
    if (!value) return 0;
    while (*value == ',')
    {
        cJSON *new_item = (cJSON*)m_alloc(sizeof(cJSON));
        if (!new_item) return 0;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_string(child, skip(value + 1)));
        if (!value) return 0;
        child->string = child->valuestring;
        child->valuestring = 0;
        if (*value != ':') return 0;
        value = skip(parse_value(child, skip(value + 1)));
        if (!value) return 0;
    }
    if (*value == '}') return value + 1;
    return 0;
}

static const char *parse_value(cJSON *item, const char *value)
{
    if (!value) return 0;
    if (!strncmp(value, "null", 4)) { item->type = cJSON_NULL; return value + 4; }
    if (!strncmp(value, "false", 5)) { item->type = cJSON_False; return value + 5; }
    if (!strncmp(value, "true", 4)) { item->type = cJSON_True; item->valueint = 1; return value + 4; }
    if (*value == '\"') return parse_string(item, value);
    if (*value == '[') return parse_array(item, value);
    if (*value == '{') return parse_object(item, value);
    if (*value == '-' || (*value >= '0' && *value <= '9')) return parse_number(item, value);
    return 0;
}

cJSON *cJSON_Parse(const char *value)
{
    const char *end;
    cJSON *c = (cJSON*)m_alloc(sizeof(cJSON));
    if (!c) return 0;
    end = parse_value(c, skip(value));
    if (!end) { cJSON_Delete(c); return 0; }
    return c;
}

static char *print_value(cJSON *item, int depth, int fmt, printbuffer *p);

static char *print_array(cJSON *item, int depth, int fmt, printbuffer *p)
{
    char **entries;
    char *out = 0, *ptr, *ret;
    int len = 5;
    cJSON *child = item->child;
    int numentries = 0, i = 0, fail = 0;
    size_t tmplen = 0;

    while (child) { numentries++; child = child->next; }
    entries = (char**)m_malloc(numentries * sizeof(char*));
    if (!entries) return 0;
    memset(entries, 0, numentries * sizeof(char*));

    child = item->child;
    while (child && !fail)
    {
        ret = print_value(child, depth + 1, fmt, NULL);
        entries[i++] = ret;
        if (ret) len += (int)strlen(ret) + 2;
        else fail = 1;
        child = child->next;
    }

    if (fail)
    {
        for (i = 0; i < numentries; i++) if (entries[i]) m_free(entries[i]);
        m_free(entries);
        return 0;
    }

    if (p) { out = ensure(p, len); if (!out) fail = 1; }
    else out = (char*)m_malloc(len);
    if (!out || fail)
    {
        for (i = 0; i < numentries; i++) if (entries[i]) m_free(entries[i]);
        m_free(entries);
        return 0;
    }

    ptr = out;
    *ptr++ = '[';
    for (i = 0; i < numentries; i++)
    {
        tmplen = strlen(entries[i]);
        memcpy(ptr, entries[i], tmplen); ptr += tmplen;
        if (i != numentries - 1) *ptr++ = ',';
        m_free(entries[i]);
    }
    m_free(entries);
    *ptr++ = ']';
    *ptr++ = 0;
    if (p) p->offset = (int)(ptr - p->buffer);
    return out;
}

static char *print_object(cJSON *item, int depth, int fmt, printbuffer *p)
{
    char **entries = 0, **names = 0;
    char *out = 0, *ptr, *ret, *str;
    int len = 7;
    cJSON *child = item->child;
    int numentries = 0, i = 0, fail = 0;
    size_t tmplen = 0;

    while (child) { numentries++; child = child->next; }
    if (!numentries)
    {
        if (p) out = ensure(p, 3); else out = (char*)m_malloc(3);
        if (out) strcpy(out, "{}");
        if (p) p->offset += 2;
        return out;
    }
    entries = (char**)m_malloc(numentries * sizeof(char*));
    if (!entries) return 0;
    names = (char**)m_malloc(numentries * sizeof(char*));
    if (!names) { m_free(entries); return 0; }
    memset(entries, 0, numentries * sizeof(char*));
    memset(names, 0, numentries * sizeof(char*));

    child = item->child;
    depth++;
    while (child && !fail)
    {
        names[i] = str = print_string_ptr(child->string, 0);
        entries[i++] = ret = print_value(child, depth, fmt, NULL);
        if (str && ret) len += (int)strlen(ret) + (int)strlen(str) + 2;
        else fail = 1;
        child = child->next;
    }

    if (fail)
    {
        for (i = 0; i < numentries; i++) { if (entries[i]) m_free(entries[i]); if (names[i]) m_free(names[i]); }
        m_free(entries); m_free(names);
        return 0;
    }

    if (p) { out = ensure(p, len); if (!out) fail = 1; }
    else { out = (char*)m_malloc(len); if (!out) fail = 1; }
    if (fail)
    {
        for (i = 0; i < numentries; i++) { if (entries[i]) m_free(entries[i]); if (names[i]) m_free(names[i]); }
        m_free(entries); m_free(names);
        return 0;
    }

    ptr = out;
    *ptr++ = '{';
    for (i = 0; i < numentries; i++)
    {
        tmplen = strlen(names[i]); memcpy(ptr, names[i], tmplen); ptr += tmplen;
        *ptr++ = ':';
        tmplen = strlen(entries[i]); memcpy(ptr, entries[i], tmplen); ptr += tmplen;
        if (i != numentries - 1) *ptr++ = ',';
        m_free(entries[i]); m_free(names[i]);
    }
    m_free(entries); m_free(names);
    depth--;
    *ptr++ = '}';
    *ptr++ = 0;
    if (p) p->offset = (int)(ptr - p->buffer);
    return out;
}

static char *print_value(cJSON *item, int depth, int fmt, printbuffer *p)
{
    char *out = 0;
    if (!item) return 0;
    if (p)
    {
        switch ((item->type) & 255)
        {
            case cJSON_NULL: { out = ensure(p, 5); if (out) { strcpy(out, "null"); p->offset += 4; } } break;
            case cJSON_True: { out = ensure(p, 5); if (out) { strcpy(out, "true"); p->offset += 4; } } break;
            case cJSON_False: { out = ensure(p, 6); if (out) { strcpy(out, "false"); p->offset += 5; } } break;
            case cJSON_Number:
                out = ensure(p, 64);
                if (out)
                {
                    if ((item->valuedouble == (double)item->valueint) && item->valuedouble < 1e60 && item->valuedouble > -1e60)
                        sprintf(out, "%d", item->valueint);
                    else
                        sprintf(out, "%lf", item->valuedouble);
                    p->offset += (int)strlen(out);
                }
                break;
            case cJSON_String: out = print_string_ptr(item->valuestring, p); break;
            case cJSON_Array: out = print_array(item, depth, fmt, p); break;
            case cJSON_Object: out = print_object(item, depth, fmt, p); break;
        }
    }
    else
    {
        switch ((item->type) & 255)
        {
            case cJSON_NULL: out = cJSON_strdup("null"); break;
            case cJSON_True: out = cJSON_strdup("true"); break;
            case cJSON_False: out = cJSON_strdup("false"); break;
            case cJSON_Number:
                out = (char*)m_malloc(64);
                if (out)
                {
                    if ((item->valuedouble == (double)item->valueint) && item->valuedouble < 1e60 && item->valuedouble > -1e60)
                        sprintf(out, "%d", item->valueint);
                    else
                        sprintf(out, "%lf", item->valuedouble);
                }
                break;
            case cJSON_String: out = print_string_ptr(item->valuestring, 0); break;
            case cJSON_Array: out = print_array(item, depth, fmt, 0); break;
            case cJSON_Object: out = print_object(item, depth, fmt, 0); break;
        }
    }
    return out;
}

char *cJSON_Print(const cJSON *item) { return print_value((cJSON*)item, 0, 1, 0); }
char *cJSON_PrintUnformatted(const cJSON *item) { return print_value((cJSON*)item, 0, 0, 0); }

void cJSON_Delete(cJSON *c)
{
    cJSON *next;
    while (c)
    {
        next = c->next;
        if (!(c->type & cJSON_IsReference) && c->child) cJSON_Delete(c->child);
        if (!(c->type & cJSON_IsReference) && !(c->type & cJSON_StringIsConst) && c->valuestring) m_free(c->valuestring);
        if (!(c->type & cJSON_StringIsConst) && c->string) m_free(c->string);
        m_free(c);
        c = next;
    }
}

int cJSON_GetArraySize(const cJSON *array)
{
    cJSON *c; int i = 0;
    if (!array || array->type != cJSON_Array) return 0;
    c = array->child;
    while (c) { i++; c = c->next; }
    return i;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    cJSON *c;
    if (!array || array->type != cJSON_Array) return 0;
    c = array->child;
    while (c && index > 0) { index--; c = c->next; }
    return c;
}

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string)
{
    cJSON *c;
    if (!object || object->type != cJSON_Object || !string) return 0;
    c = object->child;
    while (c && c->string)
    {
        if (!strcmp(c->string, string)) return c;
        c = c->next;
    }
    return 0;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string);
}

int cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

static cJSON *cJSON_New_Item(void)
{
    return (cJSON*)m_alloc(sizeof(cJSON));
}

cJSON *cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item();
    if (item) item->type = cJSON_NULL;
    return item;
}

cJSON *cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item();
    if (item) { item->type = cJSON_True; item->valueint = 1; }
    return item;
}

cJSON *cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item();
    if (item) item->type = cJSON_False;
    return item;
}

cJSON *cJSON_CreateBool(int b) { return b ? cJSON_CreateTrue() : cJSON_CreateFalse(); }

cJSON *cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Number;
        item->valuedouble = num;
        item->valueint = (int)num;
    }
    return item;
}

cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item();
    if (!item) return 0;
    item->type = cJSON_String;
    item->valuestring = cJSON_strdup(string);
    return item;
}

cJSON *cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item();
    if (item) item->type = cJSON_Array;
    return item;
}

cJSON *cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item();
    if (item) item->type = cJSON_Object;
    return item;
}

static void suffix_object(cJSON *prev, cJSON *item)
{
    prev->next = item;
    item->prev = prev;
}

cJSON *cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    cJSON *c;
    if (!item) return 0;
    if (!array || array->type != cJSON_Array) { cJSON_Delete(item); return 0; }
    c = array->child;
    if (!c) { array->child = item; }
    else { while (c && c->next) c = c->next; suffix_object(c, item); }
    return item;
}

cJSON *cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    cJSON *c;
    if (!item) return 0;
    if (item->string) m_free(item->string);
    item->string = cJSON_strdup(string);
    if (!object || object->type != cJSON_Object) { cJSON_Delete(item); return 0; }
    c = object->child;
    if (!c) { object->child = item; }
    else { while (c && c->next) c = c->next; suffix_object(c, item); }
    return item;
}

cJSON *cJSON_AddNullToObject(cJSON * const object, const char * const name)
{
    cJSON *item = cJSON_CreateNull();
    if (item) cJSON_AddItemToObject(object, name, item);
    return item;
}

cJSON *cJSON_AddTrueToObject(cJSON * const object, const char * const name)
{
    cJSON *item = cJSON_CreateTrue();
    if (item) cJSON_AddItemToObject(object, name, item);
    return item;
}

cJSON *cJSON_AddFalseToObject(cJSON * const object, const char * const name)
{
    cJSON *item = cJSON_CreateFalse();
    if (item) cJSON_AddItemToObject(object, name, item);
    return item;
}

cJSON *cJSON_AddBoolToObject(cJSON * const object, const char * const name, int boolean)
{
    cJSON *item = cJSON_CreateBool(boolean);
    if (item) cJSON_AddItemToObject(object, name, item);
    return item;
}

cJSON *cJSON_AddNumberToObject(cJSON * const object, const char * const name, double number)
{
    cJSON *item = cJSON_CreateNumber(number);
    if (item) cJSON_AddItemToObject(object, name, item);
    return item;
}

cJSON *cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string)
{
    cJSON *item = cJSON_CreateString(string);
    if (item) cJSON_AddItemToObject(object, name, item);
    return item;
}

cJSON *cJSON_Duplicate(const cJSON *item, int recurse)
{
    cJSON *newitem, *cptr, *nptr = 0, *newchild;
    if (!item) return 0;
    newitem = cJSON_New_Item();
    if (!newitem) return 0;
    newitem->type = item->type & 255;
    newitem->valuedouble = item->valuedouble;
    newitem->valueint = item->valueint;
    if (item->valuestring) { newitem->valuestring = cJSON_strdup(item->valuestring); if (!newitem->valuestring) { m_free(newitem); return 0; } }
    if (item->string) { newitem->string = cJSON_strdup(item->string); if (!newitem->string) { cJSON_Delete(newitem); return 0; } }

    if (!recurse) return newitem;
    cptr = item->child;
    while (cptr)
    {
        newchild = cJSON_Duplicate(cptr, 1);
        if (!newchild) break;
        if (nptr) { nptr->next = newchild; newchild->prev = nptr; nptr = newchild; }
        else { newitem->child = newchild; nptr = newchild; }
        cptr = cptr->next;
    }
    return newitem;
}
