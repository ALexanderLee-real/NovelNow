/* engine_parse.c - parsers for AI responses, split from engine.c (2026-10-18, zero behavior change) */
#include "engine_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>


/* ============== 解析 AI 返回的列表 ============== */

/* 简单解析：从 AI 回复中提取编号列表项（如 "1. 标题 - 描述" 格式） */
int parse_numbered_list(const char *text, char **titles, char **descriptions, int max_items)
{
    if (!text || !titles || !descriptions) return 0;
    
    int count = 0;
    const char *p = text;
    
    while (*p && count < max_items) {
        /* 查找 "数字." 或 "数字、" 开头的行 */
        const char *num_start = NULL;
        while (*p) {
            if (isdigit((unsigned char)*p) && (p == text || *(p-1) == '\n' || *(p-1) == '\r')) {
                num_start = p;
                break;
            }
            p++;
        }
        if (!num_start) break;
        
        /* 跳过数字和分隔符 */
        p = num_start;
        while (*p && isdigit((unsigned char)*p)) p++;
        if (*p == '.') p++;
        else if (U8_EQ(p, "、")) p += 3;
        while (*p == ' ' || *p == '\t') p++;
        
        /* 提取标题（到换行、冒号或破折号之前） */
        const char *title_start = p;
        const char *desc_start = NULL;
        while (*p && *p != '\n' && *p != '\r') {
            if (*p == '-' && *(p+1) == '-') { p += 2; }
            else if (U8_EQ(p, "——")) { p += 6; }   /* 中文双破折号 U+2014×2，模型爱用（P1） */
            else if (U8_EQ(p, "—")) { p += 3; }    /* 中文单破折号 U+2014（双的必须先判） */
            else if (*p == ':') { p++; }
            else if (U8_EQ(p, "：")) { p += 3; }
            else { p++; continue; }
            while (*p == ' ' || *p == '\t') p++;
            desc_start = p;
            break;
        }
        
        /* 提取标题 */
        size_t title_len = (desc_start ? (size_t)(desc_start - title_start) : (size_t)(p - title_start));
        /* 去掉尾部空格和破折号 */
        while (title_len > 0 && (title_start[title_len-1] == ' ' ||
               title_start[title_len-1] == '\t' || title_start[title_len-1] == '-')) {
            title_len--;
        }
        /* 尾部中文破折号也剥掉（「方向名——」没带描述的情况） */
        while (title_len >= 3 && memcmp(title_start + title_len - 3, "—", 3) == 0) {
            title_len -= 3;
        }
        /* 超长按 UTF-8 边界截断，而不是整条丢弃（丢弃会静默丢失内容） */
        if (title_len > 255) title_len = utf8_safe_len(title_start, 255);
        if (title_len > 0) {
            char *title = (char*)malloc(title_len + 1);
            if (title) {
                memcpy(title, title_start, title_len);
                title[title_len] = '\0';
            }
            titles[count] = title ? title : str_dup("未命名");
        } else {
            titles[count] = str_dup("未命名");
        }
        
        /* 提取描述 */
        const char *desc_end = p;
        while (*desc_end && *desc_end != '\n' && *desc_end != '\r') desc_end++;
        size_t desc_len = desc_start ? (size_t)(desc_end - desc_start) : 0;
        if (desc_start && desc_len > 0) {
            if (desc_len > 2047) desc_len = utf8_safe_len(desc_start, 2047);
            char *desc = (char*)malloc(desc_len + 1);
            if (desc) {
                memcpy(desc, desc_start, desc_len);
                desc[desc_len] = '\0';
            }
            descriptions[count] = desc ? desc : str_dup("");
        } else {
            descriptions[count] = str_dup("");
        }
        
        count++;
        if (*p == '\n') p++;
    }
    
    return count;
}

/* ============== 世界观与人物智能分析 ============== */

/* 在 [content, end) 内查找 "kw：" 并把该行的值拷入 out（UTF-8 安全截断）。
   end 为 NULL 表示不设上界。
   找不到时把 out 置为空串 —— 绝不能"保持原值不动"：AI 漏写某个字段是常态，
   之前调用方没初始化缓冲区，于是未初始化的栈内容会被当成字段值一路写进
   project.json 并拼进后续提示词。所有出口都必须以 '\0' 收尾。 */
void extract_field(const char *content, const char *end,
                          const char *kw, char *out, size_t max)
{
    const char *pos, *p, *e;
    size_t len;

    if (!out || max == 0) return;
    out[0] = '\0';
    if (!content || !kw) return;

    pos = strstr(content, kw);
    if (!pos || (end && pos >= end)) return;
    p = u8_after(pos, kw);
    if (*p == ':') p++;
    else if (U8_EQ(p, "：")) p += 3;
    while (*p == ' ' || *p == '\t') p++;

    e = p;
    while (*e && *e != '\n' && *e != '\r' && (!end || e < end)) e++;
    len = (size_t)(e - p);
    if (len > max - 1) len = utf8_safe_len(p, max - 1);
    memcpy(out, p, len);
    out[len] = '\0';
}

/* 解析 AI 输出中的人物块（姓名/定位/性格/背景/外貌），逐个追加到项目。
   每个块从一处 "姓名" 到下一处 "姓名" 之间，字段不会串到下一个人。
   返回解析出的人物数 */
int parse_characters_from(const char *content, novel_project_t *proj)
{
    const char *cp = content;
    int added = 0;

    if (!content || !proj) return 0;
    while (cp && *cp) {
        const char *name_pos = strstr(cp, "姓名");
        const char *next_pos;
        /* 必须初始化：extract_field 虽然现在会自行置空串，但这里显式写清楚
           意图，避免以后有人改动 extract_field 时又踩回未初始化栈内存。 */
        char name_buf[128] = {0};
        char role_buf[128] = {0};
        char trait_buf[512] = {0};
        char bg_buf[1024] = {0};
        char desc_buf[512] = {0};

        if (!name_pos) break;
        next_pos = strstr(u8_after(name_pos, "姓名"), "姓名");

        extract_field(name_pos, next_pos, "姓名", name_buf, sizeof(name_buf));
        extract_field(name_pos, next_pos, "定位", role_buf, sizeof(role_buf));
        extract_field(name_pos, next_pos, "性格", trait_buf, sizeof(trait_buf));
        extract_field(name_pos, next_pos, "背景", bg_buf, sizeof(bg_buf));
        extract_field(name_pos, next_pos, "外貌", desc_buf, sizeof(desc_buf));

        if (name_buf[0]) {
            project_add_character(proj, name_buf, role_buf, desc_buf, trait_buf, bg_buf);
            added++;
        }
        cp = next_pos;
    }
    return added;
}

/* 解析 AI 输出中的世界观三字段并写入项目（空字段保留原值） */
void parse_worldview_from(const char *content, novel_project_t *proj)
{
    char wt[128], setting[2048], power[1024];
    wt[0] = setting[0] = power[0] = '\0';

    extract_field(content, NULL, "世界类型", wt, sizeof(wt));
    extract_field(content, NULL, "背景设定", setting, sizeof(setting));
    extract_field(content, NULL, "力量体系", power, sizeof(power));

    project_set_world(proj,
                      wt[0] ? wt : NULL,
                      setting[0] ? setting : NULL,
                      power[0] ? power : NULL);
}
