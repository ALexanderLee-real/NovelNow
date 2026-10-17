/* engine_flow.c - engine flow/API implementation, split from engine.c (2026-10-18, zero behavior change) */
#include "engine_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

engine_t* get_engine(engine_state_t *state) {
    return (engine_t*)((char*)state - offsetof(engine_t, state));
}

int is_cancelled(engine_state_t *state)
{
    return state ? get_engine(state)->cancel : 0;
}
/* ============== 辅助函数 ============== */

void report_progress(engine_state_t *state, const char *stage,
                            int current, int total, const char *message)
{
    if (!state) return;
    strncpy(state->current_stage, stage, sizeof(state->current_stage) - 1);
    state->current_stage[sizeof(state->current_stage) - 1] = '\0';
    state->progress_current = current;
    state->progress_total = total;
    if (state->progress_cb) {
        state->progress_cb(stage, current, total, message, state->user_data);
    }
}

void set_error(engine_state_t *state, const char *error)
{
    if (!state || !error) return;
    strncpy(state->last_error, error, sizeof(state->last_error) - 1);
    state->last_error[sizeof(state->last_error) - 1] = '\0';
}

/* ============== 引擎创建/销毁 ============== */

engine_state_t* engine_create(app_config_t *cfg)
{
    engine_t *eng = (engine_t*)calloc(1, sizeof(engine_t));
    if (!eng) return NULL;
    eng->config = cfg;
    eng->state.is_running = 0;
    return &eng->state;
}

void engine_destroy(engine_state_t *state)
{
    if (!state) return;
    engine_t *eng = (engine_t*)((char*)state - offsetof(engine_t, state));
    free(eng);
}

void engine_set_progress_callback(engine_state_t *state, progress_callback_t cb, void *user_data)
{
    if (!state) return;
    state->progress_cb = cb;
    state->user_data = user_data;
}

void engine_set_chapter_delta_callback(engine_state_t *state, chapter_delta_cb_t cb, void *user_data)
{
    if (!state) return;
    state->chapter_delta_cb = cb;
    state->chapter_delta_user = user_data;
}

void engine_set_text_delta_callback(engine_state_t *state, text_delta_cb_t cb, void *user_data)
{
    if (!state) return;
    state->text_delta_cb = cb;
    state->text_delta_user = user_data;
}

/* 通用流式转发：ai_chat_single_stream 的回调直接转给 host */
static void text_delta_trampoline(const char *delta, void *ud)
{
    engine_state_t *st = (engine_state_t*)ud;
    if (st && st->text_delta_cb)
        st->text_delta_cb(delta, st->text_delta_user);
}

/* 注册了通用流式回调就走 SSE（全文仍会返回在 resp->content），否则保持原样 */
ai_response_t* engine_ai_call(engine_state_t *state, ai_config_t *ai,
                                     const char *system_prompt, const char *user_prompt)
{
    if (state && state->text_delta_cb)
        return ai_chat_single_stream(ai, system_prompt, user_prompt,
                                     text_delta_trampoline, state);
    return ai_chat_single(ai, system_prompt, user_prompt);
}

void engine_request_cancel(engine_state_t *state)
{
    if (!state) return;
    get_engine(state)->cancel = 1;
    /* 关键：只置标记的话，正在生成的一章要写完才会停（几分钟）。
       这里同时掐断在途的 AI 请求，点取消立刻生效。 */
    ai_abort_current();
}

void engine_clear_cancel(engine_state_t *state)
{
    if (!state) return;
    get_engine(state)->cancel = 0;
    ai_abort_reset();
}

/* ============== 阶段1：创意发散 ============== */

int engine_generate_directions(engine_state_t *state, novel_project_t *proj,
                               int iterations, int direction_count)
{
    if (!state || !proj || !proj->seed_idea) {
        set_error(state, "Invalid parameters");
        return -1;
    }
    
    state->is_running = 1;
    project_clear_directions(proj);
    
    char *current_idea = str_dup(proj->seed_idea);
    int total_steps = iterations * 2;
    int step = 0;
    
    for (int iter = 0; iter < iterations; iter++) {
        CANCEL_CHECK_CLEAN(state, free(current_idea));
        report_progress(state, "创意发散", step++, total_steps, 
                        "正在进行创意发散...");
        
        /* 构建提示词 */
        strbuf_t *prompt = strbuf_create(2048);
        strbuf_append(prompt, "基于以下小说创意想法，进行发散思考，提出");
        char num_str[16];
        sprintf(num_str, "%d", direction_count);
        strbuf_append(prompt, num_str);
        strbuf_append(prompt, "个不同的创作方向。\n\n");
        strbuf_append(prompt, "【当前创意】\n");
        strbuf_append(prompt, current_idea);
        strbuf_append(prompt, "\n\n");
        strbuf_append(prompt, "要求：\n");
        strbuf_append(prompt, "1. 每个方向要有独特的切入点和世界观设定\n");
        strbuf_append(prompt, "2. 方向之间要有明显的差异，涵盖不同的风格和题材\n");
        strbuf_append(prompt, "3. 每个方向包含：方向名称（简短有力）+ 详细描述（100字左右，说明核心设定、故事冲突和看点）\n");
        strbuf_append(prompt, "4. 用编号格式回复，例如：\n");
        strbuf_append(prompt, "1. 废土重生 -- 在末日后的废墟中，主角发现了远古文明的秘密...\n");
        strbuf_append(prompt, "2. 时间逆流 -- 时间开始倒流，主角必须在逆向的世界中找到真相...\n");
        
        ai_response_t *resp = engine_ai_call(state, get_engine(state)->config->ai, 
                                              "你是一位富有创意的小说策划师，擅长从简单的想法中发散出多种独特的创作方向。",
                                              strbuf_text(prompt));
        strbuf_destroy(prompt);
        
        if (!resp || !resp->success) {
            set_error(state, resp && resp->error ? resp->error : "AI request failed");
            if (resp) ai_response_destroy(resp);
            free(current_idea);
            state->is_running = 0;
            return -1;
        }
        
        report_progress(state, "创意发散", step++, total_steps, 
                        "正在解析创作方向...");
        
        /* 解析方向 */
        char *titles[16] = {0};
        char *descs[16] = {0};
        int count = parse_numbered_list(resp->content, titles, descs, 
                                        direction_count > 16 ? 16 : direction_count);
        
        /* 如果解析失败，整段作为一个方向 */
        if (count == 0) {
            project_add_direction(proj, "方向 1", resp->content);
        } else {
            for (int i = 0; i < count; i++) {
                project_add_direction(proj, titles[i], descs[i]);
                free(titles[i]);
                free(descs[i]);
            }
        }
        
        /* 更新当前创意（用于下一轮迭代：取最佳方向深化） */
        if (iter < iterations - 1 && proj->direction_count > 0) {
            free(current_idea);
            strbuf_t *buf = strbuf_create(1024);
            strbuf_append(buf, proj->directions[0].title);
            strbuf_append(buf, "\n");
            strbuf_append(buf, proj->directions[0].description);
            current_idea = strbuf_dup(buf);
            strbuf_destroy(buf);
            /* 清空上一轮的方向，为新一轮做准备 */
            if (iter < iterations - 1) {
                project_clear_directions(proj);
            }
        }
        
        ai_response_destroy(resp);
    }
    
    free(current_idea);
    
    if (proj->direction_count == 0) {
        set_error(state, "Failed to generate any directions");
        state->is_running = 0;
        return -1;
    }
    
    free(proj->current_stage); proj->current_stage = str_dup("direction");
    state->is_running = 0;
    return proj->direction_count;
}

/* ============== 阶段2：方案生成 ============== */

int engine_generate_plans(engine_state_t *state, novel_project_t *proj, int plan_count)
{
    if (!state || !proj || !proj->selected_direction) {
        set_error(state, "Please select a direction first");
        return -1;
    }
    
    state->is_running = 1;
    project_clear_plans(proj);
    CANCEL_CHECK(state);
    
    report_progress(state, "方案生成", 0, 2, "正在生成小说方案...");
    
    /* 构建背景提示词 */
    char *context = project_build_context_summary(proj);
    
    strbuf_t *prompt = strbuf_create(4096);
    strbuf_append(prompt, "基于以下创作方向，设计");
    char num_str[16];
    sprintf(num_str, "%d", plan_count);
    strbuf_append(prompt, num_str);
    strbuf_append(prompt, "篇不同的小说方案。\n\n");
    strbuf_append(prompt, context);
    strbuf_append(prompt, "\n\n");
    strbuf_append(prompt, "每篇方案包含以下内容：\n");
    strbuf_append(prompt, "1. 题目：吸引人的小说标题\n");
    strbuf_append(prompt, "2. 简介：200字左右的内容简介，涵盖核心冲突和故事主线\n");
    strbuf_append(prompt, "3. 大纲：详细的故事大纲，包含开端、发展、高潮、结局四个部分，列出主要情节节点\n");
    strbuf_append(prompt, "\n格式要求：用编号列出每篇方案，每篇方案内用 题目/简介/大纲 分段。\n");
    strbuf_append(prompt, "例如：\n");
    strbuf_append(prompt, "1. 题目：星辰彼岸\n简介：在遥远的星际时代...\n大纲：\n  开端：...\n  发展：...\n  高潮：...\n  结局：...\n");
    
    free(context);
    
    ai_response_t *resp = engine_ai_call(state, get_engine(state)->config->ai,
                                          "你是一位资深的小说策划编辑，擅长设计有吸引力的小说方案和完整大纲。",
                                          strbuf_text(prompt));
    strbuf_destroy(prompt);
    
    if (!resp || !resp->success) {
        set_error(state, resp && resp->error ? resp->error : "AI request failed");
        if (resp) ai_response_destroy(resp);
        state->is_running = 0;
        return -1;
    }
    
    report_progress(state, "方案生成", 1, 2, "正在解析方案...");
    
    /* 简化解析：按 "数字. 题目：" 分割方案 */
    const char *p = resp->content;
    int parsed = 0;
    
    while (*p && parsed < plan_count) {
        /* 查找下一个 "数字." 开头的方案 */
        const char *plan_start = NULL;
        const char *search = p;
        while (*search) {
            if (isdigit((unsigned char)*search) && 
                (search == resp->content || *(search-1) == '\n' || *(search-1) == '\r')) {
                const char *num_end = search;
                while (*num_end && isdigit((unsigned char)*num_end)) num_end++;
                if (*num_end == '.' || U8_EQ(num_end, "、")) {
                    plan_start = search;
                    break;
                }
            }
            search++;
        }
        
        if (!plan_start) break;
        
        /* 找到下一个方案的开始，作为当前方案的结束 */
        const char *next_start = NULL;
        const char *s2 = plan_start + 1;
        while (*s2) {
            if (isdigit((unsigned char)*s2) && (*(s2-1) == '\n' || *(s2-1) == '\r')) {
                const char *ne = s2;
                while (*ne && isdigit((unsigned char)*ne)) ne++;
                if (*ne == '.' || U8_EQ(ne, "、")) {
                    next_start = s2;
                    break;
                }
            }
            s2++;
        }
        
        const char *plan_end = next_start ? next_start : plan_start + strlen(plan_start);
        
        /* 提取题目 */
        const char *title_pos = strstr(plan_start, "题目");
        const char *title = "未命名";
        /* "未命名" 是 9 字节 UTF-8；写死 6 会把最后一个字切成半个，产生非法 UTF-8 */
        size_t title_len = strlen(title);
        if (title_pos && title_pos < plan_end) {
            const char *tp = u8_after(title_pos, "题目");
            if (*tp == ':') tp++;
            else if (U8_EQ(tp, "：")) tp += 3;
            while (*tp == ' ' || *tp == '\t') tp++;
            title = tp;
            const char *te = tp;
            while (*te && *te != '\n' && *te != '\r' && te < plan_end) te++;
            title_len = (size_t)(te - tp);
        }
        
        /* 提取简介 */
        const char *synopsis_pos = strstr(plan_start, "简介");
        char *synopsis = str_dup("");
        if (synopsis_pos && synopsis_pos < plan_end) {
            const char *sp = u8_after(synopsis_pos, "简介");
            if (*sp == ':') sp++;
            else if (U8_EQ(sp, "：")) sp += 3;
            while (*sp == ' ' || *sp == '\t' || *sp == '\n' || *sp == '\r') sp++;
            const char *se = sp;
            /* 简介到"大纲"之前结束 */
            const char *outline_pos = strstr(sp, "大纲");
            if (outline_pos && outline_pos < plan_end) {
                se = outline_pos;
            } else {
                se = plan_end;
            }
            while (se > sp && (*(se-1) == ' ' || *(se-1) == '\n' || *(se-1) == '\r')) se--;
            size_t slen = (size_t)(se - sp);
            if (slen > 0) {
                char *tmp = (char*)malloc(slen + 1);
                if (tmp) {   /* malloc 失败不能 memcpy(NULL) */
                    free(synopsis);
                    synopsis = tmp;
                    memcpy(synopsis, sp, slen);
                    synopsis[slen] = '\0';
                }
            }
        }
        
        /* 提取大纲 */
        const char *outline_pos = strstr(plan_start, "大纲");
        char *outline = str_dup("");
        if (outline_pos && outline_pos < plan_end) {
            const char *op = u8_after(outline_pos, "大纲");
            if (*op == ':') op++;
            else if (U8_EQ(op, "：")) op += 3;
            while (*op == ' ' || *op == '\t' || *op == '\n' || *op == '\r') op++;
            const char *oe = plan_end;
            while (oe > op && (*(oe-1) == ' ' || *(oe-1) == '\n' || *(oe-1) == '\r')) oe--;
            size_t olen = (size_t)(oe - op);
            if (olen > 0) {
                char *tmp = (char*)malloc(olen + 1);
                if (tmp) {
                    free(outline);
                    outline = tmp;
                    memcpy(outline, op, olen);
                    outline[olen] = '\0';
                }
            }
        }
        
        /* 标题截断 */
        char title_buf[256];
        if (title_len > 255) title_len = utf8_safe_len(title, 255);
        memcpy(title_buf, title, title_len);
        title_buf[title_len] = '\0';
        
        project_add_plan(proj, title_buf, synopsis, outline);
        free(synopsis);
        free(outline);
        
        parsed++;
        if (next_start) {
            p = next_start;
        } else {
            break;
        }
    }
    
    /* 如果解析失败，整篇作为一个方案 */
    if (parsed == 0) {
        project_add_plan(proj, "方案 1", resp->content, resp->content);
        parsed = 1;
    }
    
    ai_response_destroy(resp);
    
    free(proj->current_stage); proj->current_stage = str_dup("plan");
    state->is_running = 0;
    return parsed;
}

/* ============== 阶段3：大纲细化为章节 ============== */

int engine_expand_outline(engine_state_t *state, novel_project_t *proj, int chapter_count)
{
    if (!state || !proj || !proj->selected_plan) {
        set_error(state, "Please select a plan first");
        return -1;
    }
    
    state->is_running = 1;
    CANCEL_CHECK(state);
    report_progress(state, "大纲细化", 0, 3, "正在细化大纲为章节...");

    /* 必须先清空已有章节：engine_auto_write 或手动二次细化会再次进入本函数，
       若不清空则 project_add_chapter 会把新解析的章节追加在旧章节之后，
       导致章节数翻倍（用户要 15 章最终变成 30 章） */
    project_clear_chapters(proj);

    char *context = project_build_context_summary(proj);
    
    strbuf_t *prompt = strbuf_create(4096);
    strbuf_append(prompt, "根据以下小说方案，将大纲细化为具体的章节安排。\n\n");
    strbuf_append(prompt, context);
    strbuf_append(prompt, "\n\n");
    strbuf_append(prompt, "请设计大约 ");
    char num_str[16];
    sprintf(num_str, "%d", chapter_count);
    strbuf_append(prompt, num_str);
    strbuf_append(prompt, " 个章节。\n\n");
    strbuf_append(prompt, "每章包含：\n");
    strbuf_append(prompt, "- 章节标题\n");
    strbuf_append(prompt, "- 章节概要（50-100字，说明本章主要情节和人物发展）\n");
    strbuf_append(prompt, "\n格式要求：按编号列出每章，例如：\n");
    strbuf_append(prompt, "第1章 初入江湖 -- 主角李逍遥初入江湖，偶遇少女赵灵儿，开启了一段不平凡的旅程...\n");
    strbuf_append(prompt, "第2章 危机四伏 -- 两人遭到黑苗人追杀，被迫踏上逃亡之路...\n");
    
    free(context);
    
    ai_response_t *resp = engine_ai_call(state, get_engine(state)->config->ai,
                                          "你是一位专业的小说结构设计师，擅长将故事大纲细化为节奏合理的章节安排。",
                                          strbuf_text(prompt));
    strbuf_destroy(prompt);
    
    if (!resp || !resp->success) {
        set_error(state, resp && resp->error ? resp->error : "AI request failed");
        if (resp) ai_response_destroy(resp);
        state->is_running = 0;
        return -1;
    }
    
    report_progress(state, "大纲细化", 1, 3, "正在解析章节...");
    
    /* 解析章节列表。
       上限用 chapter_count（而非 *2）：AI 经常会比要求多给 1-3 章，
       写 *2 会让超出的全部被收下，造成章节数远超预期 */
    const char *p = resp->content;
    int count = 0;

    /* 先解析进临时数组，全部解析成功后才替换旧章节：
       以前在 AI 调用前就 project_clear_chapters，AI 失败/解析失败时
       用户已有的章节正文会被白白清空（P0 数据丢失） */
    char **tmp_titles = NULL;
    char **tmp_summaries = NULL;
    int tmp_count = 0;

    while (*p && count < chapter_count) {
        /* 匹配 "第X章" 或 "数字." 格式 */
        const char *ch_start = NULL;
        const char *s = p;
        while (*s) {
            if (U8_EQ(s, "第") && isdigit((unsigned char)*(s+3))) {
                const char *e = s + 3;
                while (*e && isdigit((unsigned char)*e)) e++;
                if (U8_EQ(e, "章") || U8_EQ(e, "节")) {
                    ch_start = s;
                    break;
                }
            }
            if (isdigit((unsigned char)*s) &&
                (s == resp->content || *(s-1) == '\n' || *(s-1) == '\r')) {
                const char *e = s;
                while (*e && isdigit((unsigned char)*e)) e++;
                if (*e == '.' || U8_EQ(e, "、")) {
                    ch_start = s;
                    break;
                }
            }
            s++;
        }
        
        if (!ch_start) break;
        
        /* 跳过编号 */
        const char *title_p = ch_start;
        while (*title_p) {
            if (isdigit((unsigned char)*title_p) || *title_p == '.') { title_p++; }
            else if (U8_EQ(title_p, "第") || U8_EQ(title_p, "章") ||
                     U8_EQ(title_p, "节") || U8_EQ(title_p, "、")) { title_p += 3; }
            else break;
        }
        while (*title_p == ' ' || *title_p == '\t') title_p++;
        
        /* 提取标题（到破折号或换行） */
        const char *title_end = title_p;
        const char *summary_start = NULL;
        const char *sep_start = NULL;
        while (*title_end && *title_end != '\n' && *title_end != '\r') {
            if (*title_end == '-' && *(title_end+1) == '-') { sep_start = title_end; title_end += 2; }
            else if (*title_end == ':') { sep_start = title_end; title_end++; }
            else if (U8_EQ(title_end, "：")) { sep_start = title_end; title_end += 3; }
            else { title_end++; continue; }
            while (*title_end == ' ' || *title_end == '\t') title_end++;
            summary_start = title_end;
            break;
        }

        /* 标题必须在分隔符之前结束。原来的 summary_start-2 假设分隔符是 "--"，
           遇到 ":"（1字节）会切进标题最后一个多字节字符、遇到 "："（3字节）会多带半个字符，
           两种情况都产生非法 UTF-8，随后发往 API 触发 400 */
        size_t title_len;
        if (sep_start) {
            title_len = (size_t)(sep_start - title_p);
        } else {
            title_len = (size_t)(title_end - title_p);
        }
        
        /* 去掉尾部空格 */
        while (title_len > 0 && (title_p[title_len-1] == ' ' || title_p[title_len-1] == '\t' ||
                                  title_p[title_len-1] == '-')) title_len--;
        
        char title_buf[256];
        if (title_len > 255) title_len = utf8_safe_len(title_p, 255);
        if (title_len == 0) {
            sprintf(title_buf, "第%d章", count + 1);
        } else {
            memcpy(title_buf, title_p, title_len);
            title_buf[title_len] = '\0';
        }
        
        /* 提取概要 */
        const char *summary_end = summary_start ? summary_start : title_end;
        while (*summary_end && *summary_end != '\n' && *summary_end != '\r') summary_end++;
        size_t sum_len = summary_start ? (size_t)(summary_end - summary_start) : 0;
        
        char *summary = NULL;
        if (summary_start && sum_len > 0) {
            if (sum_len > 2047) sum_len = utf8_safe_len(summary_start, 2047);
            summary = (char*)malloc(sum_len + 1);
            if (summary) {
                memcpy(summary, summary_start, sum_len);
                summary[sum_len] = '\0';
            }
        }
        if (!summary) summary = str_dup("");
        
        char *t_copy = str_dup(title_buf);
        if (t_copy) {
            char **nt = (char**)realloc(tmp_titles, sizeof(char*) * (size_t)(tmp_count + 1));
            char **ns = (char**)realloc(tmp_summaries, sizeof(char*) * (size_t)(tmp_count + 1));
            if (nt) tmp_titles = nt;
            if (ns) tmp_summaries = ns;
            if (nt && ns) {
                tmp_titles[tmp_count] = t_copy;
                tmp_summaries[tmp_count] = summary;   /* 所有权转入 */
                tmp_count++;
                summary = NULL;
            } else {
                free(t_copy);
            }
        }
        free(summary);
        count++;
        
        p = summary_end;
        if (*p == '\n') p++;
    }
    
    /* 提交：解析成功才替换旧章节；解析失败保留旧章节（旧数据不丢） */
    if (tmp_count > 0) {
        project_clear_chapters(proj);
        for (int i = 0; i < tmp_count; i++) {
            project_add_chapter(proj, tmp_titles[i], tmp_summaries[i]);
            free(tmp_titles[i]);
            free(tmp_summaries[i]);
        }
        count = tmp_count;
    } else {
        /* 解析失败：不要清空用户已有章节，只报错 */
        ai_response_destroy(resp);
        set_error(state, "大纲解析失败：AI 回复中没有找到章节列表，原章节已保留");
        state->is_running = 0;
        return -1;
    }
    free(tmp_titles);
    free(tmp_summaries);
    
    ai_response_destroy(resp);
    
    report_progress(state, "大纲细化", 2, 3, "正在生成人物设定...");
    CANCEL_CHECK(state);
    
    /* 额外：从方案中提取并生成角色 */
    if (proj->character_count == 0) {
        strbuf_t *char_prompt = strbuf_create(2048);
        strbuf_append(char_prompt, "根据以下小说方案，提炼出主要人物角色设定。\n\n");
        char *ctx = project_build_context_summary(proj);
        strbuf_append(char_prompt, ctx);
        free(ctx);
        strbuf_append(char_prompt, "\n\n请列出主要人物，格式为：\n");
        strbuf_append(char_prompt, "姓名：xxx\n定位：主角/配角/反派\n性格：xxx\n背景：xxx\n外貌：xxx\n\n");
        
        ai_response_t *char_resp = engine_ai_call(state, get_engine(state)->config->ai,
            "你是一位人物设定专家，擅长塑造丰满立体的小说角色。",
            strbuf_text(char_prompt));
        strbuf_destroy(char_prompt);
        
        if (char_resp && char_resp->success && strstr(char_resp->content, "姓名")) {
            /* 解析人物：复用文件后部的 parse_characters_from（按"姓名"分块，
               字段不会串人，比逐字段在 500 字节窗口内搜索更稳） */
            parse_characters_from(char_resp->content, proj);
        }
        if (char_resp) ai_response_destroy(char_resp);
    }
    
    free(proj->current_stage); proj->current_stage = str_dup("outline");
    report_progress(state, "大纲细化", 3, 3, "完成");
    state->is_running = 0;
    return count;
}

/* ai_stream_cb -> chapter_delta_cb 转发：把当前章节索引一起带给 host */
typedef struct { engine_state_t *st; int idx; } delta_ctx_t;

static void chapter_delta_trampoline(const char *delta, void *ud)
{
    delta_ctx_t *c = (delta_ctx_t*)ud;
    if (c && c->st && c->st->chapter_delta_cb)
        c->st->chapter_delta_cb(c->idx, delta, c->st->chapter_delta_user);
}

int engine_expand_chapter(engine_state_t *state, novel_project_t *proj,
                          int chapter_index, int target_words)
{
    if (!state || !proj) {
        set_error(state, "Invalid parameters");
        return -1;
    }
    
    /* 目标字数最终来自 UI 文本框，必须在使用前钳制：
       ① target_words*2 会整数溢出（UB）；
       ② 抬升后的 max_tokens 超过 AI_MAX_TOKENS_LIFT_CAP 会让 API 返回 400，
          整批扩写全军覆没 */
    if (target_words < 500) target_words = 500;
    if (target_words > 12000) target_words = 12000;
    
    chapter_t *ch = project_get_chapter(proj, chapter_index);
    if (!ch) {
        set_error(state, "Chapter not found");
        return -1;
    }
    
    state->is_running = 1;
    CANCEL_CHECK(state);
    report_progress(state, "章节扩写", 0, 2, ch->title);
    
    /* 构建系统提示词（包含所有背景） */
    char *system_prompt = project_build_system_prompt(proj, 
        "你正在写一部长篇小说，请严格按照故事大纲和人物设定进行创作。"
        "保持叙事节奏流畅，人物性格鲜明，情节有张力。"
        "注意埋下伏笔，为后续章节做铺垫。");
    
    /* 构建用户提示词 */
    strbuf_t *prompt = strbuf_create(2048);
    strbuf_append(prompt, "请扩写以下章节，目标字数约 ");
    char num_str[16];
    sprintf(num_str, "%d", target_words);
    strbuf_append(prompt, num_str);
    strbuf_append(prompt, " 字。\n\n");
    strbuf_append(prompt, "【章节标题】");
    strbuf_append(prompt, ch->title);
    strbuf_append(prompt, "\n\n");
    strbuf_append(prompt, "【本章概要】\n");
    strbuf_append(prompt, ch->summary ? ch->summary : "");
    strbuf_append(prompt, "\n\n");
    
    /* 添加上下文：前一章内容摘要（如果有） */
    if (chapter_index > 0) {
        chapter_t *prev = project_get_chapter(proj, chapter_index - 1);
        if (prev && prev->content && *prev->content) {
            strbuf_append(prompt, "【上一章回顾】\n");
            /* 只取前500字节作为上下文；必须落在 UTF-8 字符边界，否则请求体含非法字节会触发 400 */
            size_t prev_len = utf8_safe_len(prev->content, 500);
            strbuf_append_len(prompt, prev->content, prev_len);
            strbuf_append(prompt, "...\n\n");
        }
    }

    /* 前文记忆块：全书逐章概要 + 尚未回收的伏笔清单（长篇不断层的关键） */
    {
        char *mem = project_build_memory_block(proj, chapter_index);
        if (mem && *mem) {
            strbuf_append(prompt, mem);
            strbuf_append(prompt, "\n");
        }
        free(mem);
    }
    
    strbuf_append(prompt, "请直接写出本章正文内容，不要包含任何解释、标题或说明性文字。");
    
    /* 思考型模型的推理阶段与正文共享输出额度：配置值不够写完目标字数时，
       按目标字数临时抬升（中文约 2 token/字 + 2048 思考余量），请求后立即恢复 */
    {
        ai_config_t *ai = get_engine(state)->config->ai;
        int saved_max = ai->max_tokens;
        int needed = target_words * 2 + 2048;
        ai_response_t *resp;
        /* 抬升有上限，且只抬不降：用户若在设置里填了比 LIFT_CAP 更大的额度，
           那是他对自己模型上限的判断，这里绝不能把它压回去 */
        if (needed > AI_MAX_TOKENS_LIFT_CAP) needed = AI_MAX_TOKENS_LIFT_CAP;
        if (saved_max < needed) ai->max_tokens = needed;

        /* 流式：注册了增量回调就走 SSE，让前端实时看到正文逐段长出来 */
        if (state->chapter_delta_cb) {
            delta_ctx_t dctx = { state, chapter_index };
            resp = ai_chat_single_stream(ai, system_prompt, strbuf_text(prompt),
                                         chapter_delta_trampoline, &dctx);
        } else
            resp = ai_chat_single(ai, system_prompt, strbuf_text(prompt));
        ai->max_tokens = saved_max;
        strbuf_destroy(prompt);
        free(system_prompt);

        if (!resp || !resp->success) {
            set_error(state, resp && resp->error ? resp->error : "AI request failed");
            if (resp) ai_response_destroy(resp);
            state->is_running = 0;
            return -1;
        }

        /* 双保险：即使响应层漏检，也绝不把空内容写进章节 */
        {
            const char *p = resp->content ? resp->content : "";
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) {
                set_error(state, "AI 返回了空正文，本章未更新，请重试");
                ai_response_destroy(resp);
                state->is_running = 0;
                return -1;
            }
        }

        report_progress(state, "章节扩写", 1, 2, "保存内容...");

        project_update_chapter_content(proj, chapter_index, resp->content);
        ch->iteration_count++;

        ai_response_destroy(resp);
    }

    /* 记账：生成概要 + 登记伏笔（失败静默，不挡写作） */
    record_chapter_memory(state, proj, chapter_index);

    report_progress(state, "章节扩写", 2, 2, "完成");
    state->is_running = 0;
    return 0;
}

int engine_expand_all_chapters(engine_state_t *state, novel_project_t *proj,
                               int target_words, int force)
{
    if (!state || !proj || proj->chapter_count == 0) return -1;
    
    int total = proj->chapter_count;
    int skipped = 0;
    for (int i = 0; i < total; i++) {
        CANCEL_CHECK(state);
        /* 已写完的章默认跳过：批量扩写不该把成品推倒重写（白烧 token 还毁稿）。
           force=1 时无条件覆盖（UI「全部重写」按钮）。字数按 3 字节/汉字估算。 */
        if (!force && proj->chapters[i].content && *proj->chapters[i].content) {
            size_t len = strlen(proj->chapters[i].content);
            if (target_words > 0 && len >= (size_t)target_words * 3 / 10 * 9) {
                skipped++;
                continue;
            }
        }
        report_progress(state, "批量扩写", i, total, proj->chapters[i].title);
        int ret = engine_expand_chapter(state, proj, i, target_words);
        if (ret != 0) return ret;
        /* 逐章落盘：中途断网/取消不丢已写内容 */
        if (project_save(proj, get_engine(state)->config->projects_dir) != 0) {
            set_error(state, "章节保存失败，已中止（防止进度丢失）");
            return -1;
        }
    }
    if (skipped > 0) {
        report_progress(state, "批量扩写", total, total, "完成（已完成章节跳过，强制重写可覆盖）");
    }
    return 0;
}

/* ============== 阶段5：一致性检查 ============== */

double engine_check_consistency(engine_state_t *state, novel_project_t *proj, int chapter_index)
{
    if (!state || !proj) return 0.0;
    
    chapter_t *ch = project_get_chapter(proj, chapter_index);
    if (!ch || !ch->content || !*ch->content) return 0.0;
    
    state->is_running = 1;
    report_progress(state, "一致性检查", 0, 2, ch->title);
    
    char *context = project_build_context_summary(proj);
    
    strbuf_t *prompt = strbuf_create(4096);
    strbuf_append(prompt, "请检查以下小说章节的内容是否与整体设定保持一致。\n\n");
    strbuf_append(prompt, "【小说整体设定】\n");
    strbuf_append(prompt, context);
    strbuf_append(prompt, "\n\n");
    strbuf_append(prompt, "【当前章节】");
    strbuf_append(prompt, ch->title);
    strbuf_append(prompt, "\n\n");
    strbuf_append(prompt, "【章节内容】\n");
    strbuf_append(prompt, ch->content);
    strbuf_append(prompt, "\n\n");
    strbuf_append(prompt, "请从以下方面进行检查：\n");
    strbuf_append(prompt, "1. 人物性格是否一致（有没有OOC的情况）\n");
    strbuf_append(prompt, "2. 世界观设定是否被违反\n");
    strbuf_append(prompt, "3. 情节逻辑是否合理\n");
    strbuf_append(prompt, "4. 时间线是否连贯\n");
    strbuf_append(prompt, "5. 前后细节是否矛盾\n\n");
    strbuf_append(prompt, "请按以下格式回复：\n");
    strbuf_append(prompt, "评分：X.X（0到1之间的小数，1表示完全一致）\n");
    strbuf_append(prompt, "问题：列出发现的不一致问题\n");
    strbuf_append(prompt, "建议：针对问题的修改建议\n");
    
    free(context);
    
    ai_response_t *resp = engine_ai_call(state, get_engine(state)->config->ai,
        "你是一位严谨的小说编辑，擅长发现作品中的设定矛盾和逻辑漏洞。请客观、严格地进行评审。",
        strbuf_text(prompt));
    strbuf_destroy(prompt);
    
    if (!resp || !resp->success) {
        set_error(state, resp && resp->error ? resp->error : "AI request failed");
        if (resp) ai_response_destroy(resp);
        state->is_running = 0;
        return -1.0;   /* 失败必须负数：0.0 会被当真实评分"完全不一致" */
    }
    
    /* 解析评分 */
    double score = 0.5;
    const char *score_pos = strstr(resp->content, "评分");
    if (score_pos) {
        const char *sp = u8_after(score_pos, "评分");
        while (*sp) {
            if (*sp == ':' || *sp == ' ' || *sp == '\t') { sp++; }
            else if (U8_EQ(sp, "：")) { sp += 3; }
            else break;
        }
        score = atof(sp);
        if (score < 0) score = 0;
        if (score > 1) score = 1;
    }
    
    /* 提取问题和建议 */
    char issues[2048] = "";
    char suggestions[2048] = "";
    
    const char *issue_pos = strstr(resp->content, "问题");
    if (issue_pos) {
        const char *ip = u8_after(issue_pos, "问题");
        if (*ip == ':') ip++;
        else if (U8_EQ(ip, "：")) ip += 3;
        const char *sug_pos = strstr(ip, "建议");
        const char *ie = sug_pos ? sug_pos : resp->content + strlen(resp->content);
        size_t ilen = (size_t)(ie - ip);
        if (ilen > 2047) ilen = utf8_safe_len(ip, 2047);
        memcpy(issues, ip, ilen);
        issues[ilen] = '\0';
        
        if (sug_pos) {
            const char *sp2 = u8_after(sug_pos, "建议");
            if (*sp2 == ':') sp2++;
            else if (U8_EQ(sp2, "：")) sp2 += 3;
            size_t slen = strlen(sp2);
            if (slen > 2047) slen = utf8_safe_len(sp2, 2047);
            memcpy(suggestions, sp2, slen);
            suggestions[slen] = '\0';
        }
    }
    
    project_add_check(proj, chapter_index, score, issues, suggestions);
    
    report_progress(state, "一致性检查", 1, 2, "完成");
    ai_response_destroy(resp);
    state->is_running = 0;
    
    return score;
}

int engine_check_all_consistency(engine_state_t *state, novel_project_t *proj)
{
    if (!state || !proj) return -1;
    int count = 0;
    for (int i = 0; i < proj->chapter_count; i++) {
        CANCEL_CHECK(state);
        if (proj->chapters[i].content && *proj->chapters[i].content) {
            int before = proj->check_count;
            engine_check_consistency(state, proj, i);
            /* AI 调用失败时不会写记录，不能把它算作"已检查" */
            if (proj->check_count > before) count++;
        }
    }
    return count;
}

/* ============== 自动写作全流程 ============== */

int engine_auto_write(engine_state_t *state, novel_project_t *proj,
                      int direction_count, int plan_count, int chapter_count,
                      int words_per_chapter, int start_chapter)
{
    if (!state || !proj) return -1;
    
    state->is_running = 1;

    /* 断点续写：start_chapter > 0 时跳过阶段1-3，直接从指定章节扩写 */
    if (start_chapter > 0) {
        if (!proj->selected_direction || !proj->selected_plan || proj->chapter_count == 0) {
            set_error(state, "续写需要项目已有创作方向、方案和大纲，请改用完整流程");
            state->is_running = 0;
            return -1;
        }
        if (start_chapter >= proj->chapter_count) {
            set_error(state, "起始章节超出大纲范围");
            state->is_running = 0;
            return -1;
        }
        /* 起始章前先补齐前面缺正文的章，保证上下文连续 */
        for (int i = 0; i < start_chapter; i++) {
            chapter_t *ch = &proj->chapters[i];
            if (ch->content && *ch->content) continue;
            CANCEL_CHECK(state);
            report_progress(state, "续写补章", i, start_chapter, ch->title);
            int ret = engine_expand_chapter(state, proj, i, words_per_chapter);
            if (ret != 0) { state->is_running = 0; return ret; }
            if (project_save(proj, get_engine(state)->config->projects_dir) != 0) {
                set_error(state, "章节保存失败，已中止（防止进度丢失）");
                state->is_running = 0;
                return -1;
            }
        }
        report_progress(state, "自动写作", 4, 5, "阶段4：正文扩写（续写）");
        for (int i = start_chapter; i < proj->chapter_count; i++) {
            CANCEL_CHECK(state);
            report_progress(state, "续写扩写", i, proj->chapter_count, proj->chapters[i].title);
            int ret = engine_expand_chapter(state, proj, i, words_per_chapter);
            if (ret != 0) { state->is_running = 0; return ret; }
            if (project_save(proj, get_engine(state)->config->projects_dir) != 0) {
                set_error(state, "章节保存失败，已中止（防止进度丢失）");
                state->is_running = 0;
                return -1;
            }
        }
        goto consistency_stage;
    }
    
    /* 阶段1：创意发散 */
    report_progress(state, "自动写作", 1, 5, "阶段1：创意发散");
    int dcount = engine_generate_directions(state, proj, 2, direction_count);
    if (dcount == ENGINE_CANCELLED) { state->is_running = 0; return ENGINE_CANCELLED; }
    if (dcount <= 0) { state->is_running = 0; return -1; }
    
    /* 自动选择第一个方向 */
    project_select_direction(proj, 0);
    
    /* 阶段2：方案生成 */
    report_progress(state, "自动写作", 2, 5, "阶段2：方案生成");
    int pcount = engine_generate_plans(state, proj, plan_count);
    if (pcount == ENGINE_CANCELLED) { state->is_running = 0; return ENGINE_CANCELLED; }
    if (pcount <= 0) { state->is_running = 0; return -1; }
    
    /* 自动选择第一个方案 */
    project_select_plan(proj, 0);
    
    /* 阶段3：大纲细化 */
    report_progress(state, "自动写作", 3, 5, "阶段3：大纲细化");
    int ccount = engine_expand_outline(state, proj, chapter_count);
    if (ccount == ENGINE_CANCELLED) { state->is_running = 0; return ENGINE_CANCELLED; }
    if (ccount <= 0) { state->is_running = 0; return -1; }
    
    /* 大纲完成即落盘：后续扩写逐章保存。落盘失败继续会把风险放大到全部章节 */
    if (project_save(proj, get_engine(state)->config->projects_dir) != 0) {
        set_error(state, "项目保存失败，已中止（防止进度丢失）");
        state->is_running = 0;
        return -1;
    }
    
    /* 阶段4：正文扩写（expand_all 内部已逐章落盘） */
    report_progress(state, "自动写作", 4, 5, "阶段4：正文扩写");
    {
        int ret = engine_expand_all_chapters(state, proj, words_per_chapter, 1);
        /* 必须同时处理失败(-1)与取消(-2)：忽略 -1 会让部分章节没写完
           却照样返回成功，UI 随后导出"看似完成"的残缺文章 */
        if (ret != 0) { state->is_running = 0; return ret; }
    }
    
consistency_stage:
    /* 阶段5：一致性检查（可选，默认关闭以节省 token）。
       开启时每章都会调用一次 AI，N 章 = N 次额外请求，token 消耗显著上升 */
    if (get_engine(state)->config->enable_consistency_check) {
        report_progress(state, "自动写作", 5, 5, "阶段5：一致性检查");
        {
            int ret = engine_check_all_consistency(state, proj);
            if (ret == ENGINE_CANCELLED) { state->is_running = 0; return ENGINE_CANCELLED; }
        }
    }
    
    free(proj->current_stage); proj->current_stage = str_dup("writing");
    state->is_running = 0;
    return 0;
}

int engine_generate_characters(engine_state_t *state, novel_project_t *proj)
{
    engine_t *eng;
    char *text;
    strbuf_t *prompt;
    ai_response_t *resp;
    int added;

    if (!state || !proj) { set_error(state, "Invalid parameters"); return -1; }
    eng = get_engine(state);
    state->is_running = 1;

    report_progress(state, "人物生成", 1, 3, "正在整理小说内容...");
    text = project_build_full_text(proj, 60000);
    if (!text || !*text) {
        free(text);
        set_error(state, "项目里还没有可分析的内容，请先写作章节或导入TXT");
        state->is_running = 0;
        return -1;
    }

    prompt = strbuf_create(8192);
    strbuf_append(prompt, "根据以下小说内容，提炼出主角团（主要人物）的人物设定草稿。\n\n");
    strbuf_append(prompt, text);
    strbuf_append(prompt, "\n\n要求：\n");
    strbuf_append(prompt, "1. 提取 4~8 位主要人物，按重要性排序\n");
    strbuf_append(prompt, "2. 人物信息必须来自上文内容，不得凭空编造\n");
    strbuf_append(prompt, "3. 每个人物严格按以下格式输出，每个字段写在一行内：\n");
    strbuf_append(prompt, "姓名：xxx\n定位：主角/女主/反派/导师/配角\n性格：xxx\n背景：xxx\n外貌：xxx\n\n");
    strbuf_append(prompt, "人物之间用空行分隔，除人物列表外不要输出任何其他内容。\n");
    free(text);

    CANCEL_CHECK_CLEAN(state, strbuf_destroy(prompt));
    report_progress(state, "人物生成", 2, 3, "AI 正在分析人物...");
    resp = engine_ai_call(state, eng->config->ai,
        "你是一位人物设定专家，擅长从小说文本中提炼鲜活立体的角色档案。",
        strbuf_text(prompt));
    strbuf_destroy(prompt);

    if (!resp || !resp->success) {
        set_error(state, resp && resp->error ? resp->error : "AI request failed");
        if (resp) ai_response_destroy(resp);
        state->is_running = 0;
        return -1;
    }

    /* 先确认能解析出人物再覆盖，避免格式异常时清空现有设定 */
    if (!strstr(resp->content, "姓名")) {
        set_error(state, "未能从 AI 回复中解析出人物，请重试");
        ai_response_destroy(resp);
        state->is_running = 0;
        return -1;
    }

    report_progress(state, "人物生成", 3, 3, "正在写入人物草稿...");
    project_clear_characters(proj);
    added = parse_characters_from(resp->content, proj);
    ai_response_destroy(resp);

    state->is_running = 0;
    if (added == 0) {
        set_error(state, "人物解析结果为空，请重试");
        return -1;
    }
    return added;
}

int engine_summarize_worldview(engine_state_t *state, novel_project_t *proj)
{
    engine_t *eng;
    char *text;
    strbuf_t *prompt;
    ai_response_t *resp;

    if (!state || !proj) { set_error(state, "Invalid parameters"); return -1; }
    eng = get_engine(state);
    state->is_running = 1;

    report_progress(state, "世界观总结", 1, 3, "正在整理小说内容...");
    text = project_build_full_text(proj, 60000);
    if (!text || !*text) {
        free(text);
        set_error(state, "项目里还没有可总结的内容，请先写作章节或导入TXT");
        state->is_running = 0;
        return -1;
    }

    prompt = strbuf_create(8192);
    strbuf_append(prompt, "请通读以下小说内容，总结出这个故事的世界观设定。\n\n");
    strbuf_append(prompt, text);
    strbuf_append(prompt, "\n\n严格按以下格式输出，每个字段写在一行内：\n");
    strbuf_append(prompt, "世界类型：xxx（如东方玄幻/科幻末世/都市异能/历史架空等）\n");
    strbuf_append(prompt, "背景设定：xxx（时代、地点、社会形态，100~200字）\n");
    strbuf_append(prompt, "力量体系：xxx（修炼/科技/魔法等规则与等级划分，100~200字）\n\n");
    strbuf_append(prompt, "除以上三行外不要输出任何其他内容。\n");
    free(text);

    CANCEL_CHECK_CLEAN(state, strbuf_destroy(prompt));
    report_progress(state, "世界观总结", 2, 3, "AI 正在总结世界观...");
    resp = engine_ai_call(state, eng->config->ai,
        "你是一位世界观架构师，擅长从故事文本中归纳出严谨自洽的世界设定。",
        strbuf_text(prompt));
    strbuf_destroy(prompt);

    if (!resp || !resp->success) {
        set_error(state, resp && resp->error ? resp->error : "AI request failed");
        if (resp) ai_response_destroy(resp);
        state->is_running = 0;
        return -1;
    }

    report_progress(state, "世界观总结", 3, 3, "正在写入世界观...");
    parse_worldview_from(resp->content, proj);
    ai_response_destroy(resp);

    state->is_running = 0;
    return 0;
}

int engine_check_finale(engine_state_t *state, novel_project_t *proj)
{
    engine_t *eng;
    char *text;
    strbuf_t *prompt;
    ai_response_t *resp;

    if (!state || !proj) { set_error(state, "Invalid parameters"); return -1; }
    eng = get_engine(state);
    state->is_running = 1;

    report_progress(state, "伏笔校验", 1, 3, "正在整理全书内容...");
    text = project_build_full_text(proj, 60000);
    if (!text || !*text) {
        free(text);
        set_error(state, "全书还没有可校验的内容，请先写作章节正文");
        state->is_running = 0;
        return -1;
    }

    prompt = strbuf_create(8192);
    strbuf_append(prompt, "请通读以下小说全文，做一次「伏笔回收校验」。\n\n");
    strbuf_append(prompt, text);
    strbuf_append(prompt, "\n\n请按以下结构输出校验报告：\n");
    strbuf_append(prompt, "一、已回收的伏笔：列出文中已经呼应、交代清楚的伏笔与悬念；\n");
    strbuf_append(prompt, "二、未回收的伏笔：列出埋下但尚未交代清楚的伏笔，并说明在哪一章埋下；\n");
    strbuf_append(prompt, "三、前后矛盾：指出设定、人物、时间线不一致的地方；\n");
    strbuf_append(prompt, "四、改进建议：针对未回收伏笔与矛盾给出具体回收建议。\n");
    strbuf_append(prompt, "请用清晰的中文分点输出，只依据上文内容，不得编造文中不存在的情节。\n");
    free(text);

    CANCEL_CHECK_CLEAN(state, strbuf_destroy(prompt));
    report_progress(state, "伏笔校验", 2, 3, "AI 正在校验伏笔回收...");
    resp = engine_ai_call(state, eng->config->ai,
        "你是一位资深小说编辑，擅长审查长篇小说的伏笔回收与设定一致性。",
        strbuf_text(prompt));
    strbuf_destroy(prompt);

    if (!resp || !resp->success) {
        set_error(state, resp && resp->error ? resp->error : "AI request failed");
        if (resp) ai_response_destroy(resp);
        state->is_running = 0;
        return -1;
    }

    report_progress(state, "伏笔校验", 3, 3, "正在写入校验报告...");
    if (proj->finale_report) free(proj->finale_report);
    proj->finale_report = str_dup(resp->content ? resp->content : "");
    ai_response_destroy(resp);

    state->is_running = 0;
    return 0;
}

int engine_analyze_text(engine_state_t *state, novel_project_t *proj, const char *text)
{
    engine_t *eng;
    strbuf_t *prompt;
    ai_response_t *resp;
    int added;
    size_t text_len;

    if (!state || !proj || !text || !*text) {
        set_error(state, "Invalid parameters");
        return -1;
    }
    eng = get_engine(state);
    state->is_running = 1;

    report_progress(state, "文本分析", 1, 3, "正在准备分析文本...");
    /* 与 engine_generate_characters 相同的 60KB 上限：更大文本会超出常见模型的上下文窗口 */
    text_len = strlen(text);
    if (text_len > 60000) text_len = utf8_safe_len(text, 60000);

    prompt = strbuf_create(8192);
    strbuf_append(prompt, "请通读以下文本，完成两项分析：一、总结世界观；二、提炼主要人物。\n\n");
    strbuf_append_len(prompt, text, text_len);
    strbuf_append(prompt, "\n\n严格按以下格式输出：\n");
    strbuf_append(prompt, "【世界观】\n");
    strbuf_append(prompt, "世界类型：xxx（如东方玄幻/科幻末世/都市异能/历史架空等）\n");
    strbuf_append(prompt, "背景设定：xxx（时代、地点、社会形态，100~200字）\n");
    strbuf_append(prompt, "力量体系：xxx（修炼/科技/魔法等规则与等级划分，100~200字）\n");
    strbuf_append(prompt, "【主要人物】\n");
    strbuf_append(prompt, "姓名：xxx\n定位：主角/女主/反派/导师/配角\n性格：xxx\n背景：xxx\n外貌：xxx\n\n");
    strbuf_append(prompt, "要求：人物信息必须来自文本内容，4~8 位按重要性排序；");
    strbuf_append(prompt, "每个字段写在一行内，人物之间用空行分隔，不要输出其他内容。\n");

    CANCEL_CHECK_CLEAN(state, strbuf_destroy(prompt));
    report_progress(state, "文本分析", 2, 3, "AI 正在分析文本...");
    resp = engine_ai_call(state, eng->config->ai,
        "你是一位资深小说编辑，擅长从文本中提炼世界观设定与人物档案。",
        strbuf_text(prompt));
    strbuf_destroy(prompt);

    if (!resp || !resp->success) {
        set_error(state, resp && resp->error ? resp->error : "AI request failed");
        if (resp) ai_response_destroy(resp);
        state->is_running = 0;
        return -1;
    }

    report_progress(state, "文本分析", 3, 3, "正在写入分析结果...");
    parse_worldview_from(resp->content, proj);

    added = 0;
    if (strstr(resp->content, "姓名")) {
        project_clear_characters(proj);
        added = parse_characters_from(resp->content, proj);
    }
    ai_response_destroy(resp);

    state->is_running = 0;
    return added;
}
