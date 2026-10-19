/* host_tasks.c - ask (cross-thread confirm) + progress/delta callbacks + worker threads (split from host.c 2026-10-18, zero behavior change) */
#include "host_internal.h"
#include "comfy_api.h"


/* ============================================================
 * 8. 跨线程「询问用户」
 *    工作线程阻塞在事件上，主线程把问题推给 JS，JS 回答后唤醒
 * ============================================================ */

/* 询问的最长等待。给得很宽（30 分钟）是刻意的：用户可能真的在逐字读一章，
   不该被催；它只是一道兜底，保证前端万一失灵时线程不会永久挂住。
   配合「取消任务」里主动唤醒待决询问，正常路径根本走不到这个上限。 */
#define ASK_WAIT_MAX_SEC 1800


/* 引用计数归零才真正释放：NWM_ASK 消息可能在工作线程超时退出之后才被窗口过程
   处理，此前直接 free(a) 会让窗口过程解引用已释放内存（P0 UAF） */
void ask_unref(ask_t *a)
{
    if (!a) return;
    if (InterlockedDecrement(&a->refs) == 0) {
        if (a->done) CloseHandle(a->done);
        free(a);
    }
}

CRITICAL_SECTION g_askCs;
int      g_askCsReady = 0;
ask_t   *g_asks      = NULL;
int      g_askSeq    = 0;

void ask_register(ask_t *a)
{
    if (!g_askCsReady) return;
    EnterCriticalSection(&g_askCs);
    a->next = g_asks;
    g_asks = a;
    LeaveCriticalSection(&g_askCs);
}

void ask_unregister(ask_t *a)
{
    ask_t **pp;
    if (!g_askCsReady) return;
    EnterCriticalSection(&g_askCs);
    pp = &g_asks;
    while (*pp) {
        if (*pp == a) { *pp = a->next; break; }
        pp = &(*pp)->next;
    }
    LeaveCriticalSection(&g_askCs);
}

/* 唤醒所有等待中的询问，全部按「取消」收场。
   两个调用点：窗口销毁（避免工作线程永久阻塞）、以及用户点「取消任务」
   （此时工作线程往往正卡在确认框上，引擎的取消标志根本轮不到被检查）。 */
void ask_release_all(void)
{
    ask_t *it;
    if (!g_askCsReady) return;
    EnterCriticalSection(&g_askCs);
    for (it = g_asks; it; it = it->next) { it->answer = 3; SetEvent(it->done); }
    LeaveCriticalSection(&g_askCs);
}

/* 返回 1=是 2=否 3=取消；非 UI 线程才允许调用 */
int bridge_ask(const char *text)
{
    size_t tl = text ? strlen(text) : 0;
    ask_t *a;
    if (GetCurrentThreadId() == g_uiThreadId) return 3;   /* UI 线程不能阻塞自身 */
    if (g_shuttingDown) return 3;

    a = (ask_t *)calloc(1, sizeof(ask_t) + tl);
    if (!a) return 3;
    a->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    a->answer = 3;
    a->refs = 1;   /* 工作线程持有 */
    if (tl) memcpy(a->text, text, tl);
    a->text[tl] = '\0';
    a->id = (int)InterlockedIncrement((LONG *)&g_askSeq);
    if (!a->done) { free(a); return 3; }

    ask_register(a);
    InterlockedIncrement(&a->refs);   /* 消息持有（投递成功与否都在 PostMessageW 之后判） */
    if (!PostMessageW(g_hwnd, NWM_ASK, (WPARAM)a, 0)) {
        ask_unregister(a);
        ask_unref(a);   /* 消息未投出，退还消息那份引用 */
        ask_unref(a);   /* 工作线程退出 */
        return 3;
    }
    /* 等 JS 回话。这里刻意不用 INFINITE：
       前端一旦把弹层弄丢（Esc/点遮罩关掉、或 JS 抛异常没接住），JS 就永远
       不会回 answer，工作线程会永久挂住——表现为「任务停在确认框上，
       连取消都没反应」，只能关窗口。改成分段等待：
         - 每秒醒一次，以便及时响应程序退出；
         - 超过 ASK_WAIT_MAX_SEC 仍未答复，按「取消」收场，保证线程一定能回来。 */
    {
        int waited = 0;
        for (;;) {
            DWORD w = WaitForSingleObject(a->done, 1000);
            if (w == WAIT_OBJECT_0) break;
            if (g_shuttingDown) { a->answer = 3; break; }
            if (++waited >= ASK_WAIT_MAX_SEC) {
                log_line("!! 询问 #%d 等待超过 %d 秒无答复，按「取消」收场", a->id, ASK_WAIT_MAX_SEC);
                a->answer = 3;
                break;
            }
        }
    }
    ask_unregister(a);
    {
        int ans = a->answer;
        ask_unref(a);   /* 工作线程退场；若窗口过程还在用，由消息那份引用兜底 */
        return ans;
    }
}

/* ============================================================
 * 9. 进度回调（工作线程 -> 主线程 -> JS）
 * ============================================================ */


void on_progress(const char *stage, int current, int total,
                        const char *message, void *user_data)
{
    prog_t *p;
    (void)user_data;
    if (g_shuttingDown || !g_hwnd) return;
    p = (prog_t *)calloc(1, sizeof(prog_t));
    if (!p) return;
    if (stage)   snprintf(p->stage, sizeof(p->stage), "%s", stage);
    if (message) snprintf(p->message, sizeof(p->message), "%s", message);
    p->current = current;
    p->total = total;
    /* 进度回调可能很密集：投递失败就直接丢弃，不能阻塞引擎 */
    if (!PostMessageW(g_hwnd, NWM_PROGRESS, (WPARAM)p, 0)) free(p);
}

/* ---- 章节正文流式增量（工作线程 -> 主线程 -> 前端） ---- */

void on_chapter_delta(int chapter_index, const char *delta, void *user_data)
{
    delta_t *d;
    (void)user_data;
    if (g_shuttingDown || !g_hwnd || !delta || !*delta) return;
    d = (delta_t*)calloc(1, sizeof(delta_t));
    if (!d) return;
    d->index = chapter_index;
    d->text = str_dup(delta);
    if (!d->text) { free(d); return; }
    /* SSE 增量很密集：投递失败直接丢弃，绝不能拖慢读流循环 */
    if (!PostMessageW(g_hwnd, NWM_CHAPTER_DELTA, (WPARAM)d, 0)) {
        free(d->text);
        free(d);
    }
}

/* 通用流式增量（方向/方案/大纲/一致性检查等）：复用 delta_t 与 NWM_CHAPTER_DELTA，
   index = -1，前端 push_event 时按 index 分流为 text_delta */

void on_text_delta(const char *delta, void *user_data)
{
    (void)user_data;
    on_chapter_delta(-1, delta, NULL);
}

/* ============================================================
 * 12. 任务线程实现
 * ============================================================ */

/* ---- 生成创作方向 ---- */
DWORD WINAPI th_gen_directions(LPVOID param)
{
    gen_dir_task_t *t = (gen_dir_task_t *)param;
    int r;
    if (!t) { post_task_done("gen_directions", -1, 0, "参数错误", -1); return 0; }
    r = engine_generate_directions(g.engine, g.project, t->iterations, t->count);
    free(t);
    if (r == ENGINE_CANCELLED) post_task_done("gen_directions", r, 0, "已取消", -1);
    else if (r > 0) post_task_done("gen_directions", r, 1, "创作方向已生成", -1);
    else post_task_done("gen_directions", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 生成方案 ---- */
DWORD WINAPI th_gen_plans(LPVOID param)
{
    int count = (int)(INT_PTR)param;
    int r = engine_generate_plans(g.engine, g.project, count);
    if (r == ENGINE_CANCELLED) post_task_done("gen_plans", r, 0, "已取消", -1);
    else if (r > 0) post_task_done("gen_plans", r, 1, "小说方案已生成", -1);
    else post_task_done("gen_plans", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 细化大纲 ---- */
DWORD WINAPI th_expand_outline(LPVOID param)
{
    int count = (int)(INT_PTR)param;
    int r = engine_expand_outline(g.engine, g.project, count);
    if (r == ENGINE_CANCELLED) post_task_done("expand_outline", r, 0, "已取消", -1);
    else if (r >= 0) post_task_done("expand_outline", r, 1, "大纲已细化", -1);
    else post_task_done("expand_outline", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 扩写单章 ---- */
DWORD WINAPI th_expand_chapter(LPVOID param)
{
    ch_task_t *t = (ch_task_t *)param;
    int r, idx;
    if (!t) { post_task_done("expand_chapter", -1, 0, "参数错误", -1); return 0; }
    idx = t->index;
    r = engine_expand_chapter(g.engine, g.project, idx, t->words);
    free(t);
    if (r == ENGINE_CANCELLED) post_task_done("expand_chapter", r, 0, "已取消", idx);
    else if (r == 0) post_task_done("expand_chapter", r, 1, "本章已生成", idx);
    else post_task_done("expand_chapter", r, 0, engine_err(), idx);
    return 0;
}

/* ---- 批量扩写 ---- */
DWORD WINAPI th_expand_all(LPVOID param)
{
    expand_all_task_t *t = (expand_all_task_t *)param;
    int r;
    if (!t) { post_task_done("expand_all", -1, 0, "参数错误", -1); return 0; }
    r = engine_expand_all_chapters(g.engine, g.project, t->words, t->force);
    free(t);
    if (r == ENGINE_CANCELLED) post_task_done("expand_all", r, 0, "已取消，已完成的章节已保留", -1);
    else if (r == 0) post_task_done("expand_all", r, 1, "全部扩写完成", -1);
    else post_task_done("expand_all", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 逐章生成（每章完成后询问是否满意） ---- */
DWORD WINAPI th_step_write(LPVOID param)
{
    ch_task_t *t = (ch_task_t *)param;
    novel_project_t *proj = g.project;
    int i, words;

    if (!t || !proj) {
        if (t) free(t);
        post_task_done("step_write", -1, 0, "没有可写入的项目", -1);
        return 0;
    }
    i = t->index;
    words = t->words;
    free(t);

    while (i >= 0 && i < proj->chapter_count) {
        chapter_t *ch;
        int r;

        r = engine_expand_chapter(g.engine, proj, i, words);
        if (r == ENGINE_CANCELLED) { post_task_done("step_write", r, 0, "已取消", -1); return 0; }
        if (r != 0) {
            char buf[1200];
            chapter_t *cc = &proj->chapters[i];
            snprintf(buf, sizeof(buf), "第%d章《%s》生成失败：%s",
                     i + 1, (cc->title && *cc->title) ? cc->title : "", engine_err());
            post_task_done("step_write", r, 0, buf, i);
            return 0;
        }

        /* 本章完成：让主线程刷新列表并把正文推给前端供审阅 */
        if (g_hwnd && !g_shuttingDown) PostMessageW(g_hwnd, NWM_CHAPTER_DONE, (WPARAM)i, 0);

        if (i + 1 >= proj->chapter_count) {
            post_task_done("step_write", 0, 1, "逐章生成完成：全部章节已写完", -1);
            return 0;
        }

        ch = &proj->chapters[i];
        {
            char buf[1400];
            int ans;
            snprintf(buf, sizeof(buf), "第%d章《%s》已生成（约 %d 字）",
                     i + 1, (ch->title && *ch->title) ? ch->title : "", ch->word_count);
            ans = bridge_ask(buf);
            if (ans == 1) i++;              /* 满意 -> 下一章 */
            else if (ans == 2) continue;    /* 不满意 -> 重写本章 */
            else { post_task_done("step_write", 0, 1, "逐章生成已停止，未完成章节可随时继续", i); return 0; }
        }
    }
    post_task_done("step_write", 0, 1, "逐章生成完成", -1);
    return 0;
}

/* ---- 一致性检查 ---- */
DWORD WINAPI th_check_consistency(LPVOID param)
{
    int idx = (int)(INT_PTR)param;
    double score;
    char buf[128];
    score = engine_check_consistency(g.engine, g.project, idx);
    if (score < 0) { post_task_done("check_consistency", -1, 0, engine_err(), idx); return 0; }
    snprintf(buf, sizeof(buf), "一致性评分：%.2f", score);
    post_task_done("check_consistency", 0, 1, buf, idx);
    return 0;
}

/* ---- 总结世界观 ---- */
DWORD WINAPI th_sum_world(LPVOID param)
{
    int r = engine_summarize_worldview(g.engine, g.project);
    (void)param;
    if (r == ENGINE_CANCELLED) post_task_done("summarize_worldview", r, 0, "已取消", -1);
    else if (r == 0) post_task_done("summarize_worldview", 0, 1, "世界观已总结", -1);
    else post_task_done("summarize_worldview", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 生成人物 ---- */
DWORD WINAPI th_gen_chars(LPVOID param)
{
    int r = engine_generate_characters(g.engine, g.project);
    char buf[128];
    (void)param;
    if (r == ENGINE_CANCELLED) post_task_done("generate_characters", r, 0, "已取消", -1);
    else if (r >= 0) {
        snprintf(buf, sizeof(buf), "已生成 %d 个人物", r);
        post_task_done("generate_characters", r, 1, buf, -1);
    } else post_task_done("generate_characters", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 伏笔回收校验（全书）---- */
DWORD WINAPI th_check_finale(LPVOID param)
{
    int r = engine_check_finale(g.engine, g.project);
    (void)param;
    if (r == ENGINE_CANCELLED) post_task_done("check_finale", r, 0, "已取消", -1);
    else if (r == 0) post_task_done("check_finale", 0, 1, "伏笔回收校验完成", -1);
    else post_task_done("check_finale", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 分析外部文本 ---- */
DWORD WINAPI th_analyze_text(LPVOID param)
{
    char *text = (char *)param;
    int r;
    char buf[160];
    r = engine_analyze_text(g.engine, g.project, text ? text : "");
    free(text);
    if (r == ENGINE_CANCELLED) post_task_done("analyze_text", r, 0, "已取消", -1);
    else if (r >= 0) {
        snprintf(buf, sizeof(buf), "文本分析完成，提取人物 %d 个", r);
        post_task_done("analyze_text", r, 1, buf, -1);
    } else post_task_done("analyze_text", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 全自动写作 ---- */
DWORD WINAPI th_auto_write(LPVOID param)
{
    auto_task_t *t = (auto_task_t *)param;
    int r;
    if (!t) { post_task_done("auto_write", -1, 0, "参数错误", -1); return 0; }
    r = engine_auto_write(g.engine, g.project, t->dirs, t->plans, t->chapters, t->words, t->start_ch);
    free(t);
    if (r == ENGINE_CANCELLED) post_task_done("auto_write", r, 0, "全自动写作已取消，已完成的部分已保留", -1);
    else if (r == 0) post_task_done("auto_write", r, 1, "全自动写作完成！", -1);
    else post_task_done("auto_write", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 获取模型列表 ---- */
DWORD WINAPI th_fetch_models(LPVOID param)
{
    int count = 0;
    char **models;
    (void)param;
    models = ai_list_models(g.config->ai, &count);
    if (!models) {
        post_task_done("fetch_models", -1, 0,
                       (g.config->ai && g.config->ai->api_key && *g.config->ai->api_key)
                           ? "获取模型列表失败，请检查 API 地址与 Key" : "请先填写 API Key", -1);
        return 0;
    }
    {
        /* 把结果直接推给主线程：改用事件携带 JSON 文本，避免全局数组跨线程 */
        cJSON *arr = cJSON_CreateArray();
        cJSON *d = cJSON_CreateObject();
        task_done_t *t;
        int i;
        char *txt;
        for (i = 0; i < count; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(models[i] ? models[i] : ""));
        cJSON_AddItemToObject(d, "models", arr);
        txt = cJSON_PrintUnformatted(d);
        cJSON_Delete(d);
        t = (task_done_t *)calloc(1, sizeof(task_done_t));
        if (t) {
            snprintf(t->cmd, sizeof(t->cmd), "fetch_models");
            t->ok = 1;
            t->chapter = -1;
            /* 模型列表 JSON 随模型数量线性增长（GLM/Qwen 动辄上百个模型，
               轻松超过 4KB）。此前塞进 message[1400] 被 snprintf 截断，
               主线程 cJSON_Parse 必然失败，于是 models 字段整块消失，
               用户看到「获取成功」却一个模型都没有。改走堆载荷 */
            snprintf(t->message, sizeof(t->message), "已获取 %d 个模型", count);
            t->payload = txt;        /* 所有权移交给 task_done_t */
            txt = NULL;
            if (!PostMessageW(g_hwnd, NWM_TASK_DONE, (WPARAM)t, 0)) task_done_free(t);
        }
        free(txt);
    }
    ai_list_models_free(models, count);
    return 0;
}

/* ---- 通用 AI 调用（galgame 分支页等多提示词管线用） ---- */

DWORD WINAPI th_ai_call(LPVOID param)
{
    ai_call_task_t *t = (ai_call_task_t *)param;
    ai_response_t *resp;
    task_done_t *d;
    if (!t) { post_task_done("ai_call", -1, 0, "参数错误", -1); return 0; }
    /* 流式：增量走 on_text_delta -> push_event("text_delta")，前端按当前模式路由 */
    resp = ai_chat_single_stream(g.config->ai, t->system_prompt, t->user_prompt,
                                 on_text_delta, NULL);
    free(t->system_prompt);
    free(t->user_prompt);
    free(t);
    if (!resp) { post_task_done("ai_call", -1, 0, "AI 调用失败（无响应）", -1); return 0; }
    d = (task_done_t *)calloc(1, sizeof(task_done_t));
    if (!d) {
        /* OOM 也必须回执：否则 handle_task_done 不会 end_task()，
           g.busy 永远停在忙态，后续所有任务都发不出去 */
        ai_response_destroy(resp);
        post_task_done("ai_call", -1, 0, "内存不足", -1);
        return 0;
    }
    snprintf(d->cmd, sizeof(d->cmd), "ai_call");
    d->result = resp->success ? 0 : -1;
    d->ok = resp->success ? 1 : 0;
    d->chapter = -1;
    if (resp->success && resp->content) {
        /* 全文走 payload（场景动辄上千字，message[1400] 装不下） */
        d->payload = str_dup(resp->content);
        if (d->payload) {
            snprintf(d->message, sizeof(d->message), "AI 回复完成");
        } else {
            /* strdup 失败时不能装作成功：前端会拿到 undefined 正文继续走剧情 */
            d->result = -1;
            d->ok = 0;
            snprintf(d->message, sizeof(d->message), "内存不足（AI 回复未能保存）");
        }
    } else {
        snprintf(d->message, sizeof(d->message), "%s",
                 (resp->error && *resp->error) ? resp->error : "AI 调用失败");
    }
    ai_response_destroy(resp);
    if (!PostMessageW(g_hwnd, NWM_TASK_DONE, (WPARAM)d, 0)) task_done_free(d);
    return 0;
}

/* ---- 加载项目 ---- */
DWORD WINAPI th_load_project(LPVOID param)
{
    char *path = (char *)param;
    novel_project_t *proj = project_load(path);
    char msg[1024];
    if (proj) snprintf(msg, sizeof(msg), "项目已加载：%s", proj->name ? proj->name : "");
    else      snprintf(msg, sizeof(msg), "项目加载失败：文件不存在或格式损坏");
    free(path);

    /* 把新 project 交给主线程替换（工作线程不碰全局） */
    {
        task_done_t *t = (task_done_t *)calloc(1, sizeof(task_done_t));
        if (!t) { if (proj) project_destroy(proj); return 0; }
        snprintf(t->cmd, sizeof(t->cmd), "load_project");
        t->ok = proj ? 1 : 0;
        t->chapter = -1;
        t->ptr = proj;                       /* 所有权移交给主线程 */
        snprintf(t->message, sizeof(t->message), "%s", msg);
        if (!PostMessageW(g_hwnd, NWM_TASK_DONE, (WPARAM)t, 0)) {
            if (proj) project_destroy(proj);
            task_done_free(t);
        }
    }
    return 0;
}

/* ============================================================
 * 13. ComfyUI 配图（不占忙闲位，可与 AI 任务并行）
 *     结果走 comfy_done 专用事件，不混进 task_done：
 *     task_done 收尾会 end_task()+setBusy(false)，会把正在跑的
 *     AI 任务的忙闲状态踩掉，所以 handle_task_done 里单独分叉。
 * ============================================================ */

/* comfy_test：GET /system_stats 探活，参数是已复制好的基地址 */
DWORD WINAPI th_comfy_test(LPVOID param)
{
    char *url = (char *)param;
    char err[512] = "";
    int ok = (url && comfy_test(url, err, sizeof(err)) == 0);
    task_done_t *d;
    free(url);
    d = (task_done_t *)calloc(1, sizeof(task_done_t));
    if (!d) return 0;
    snprintf(d->cmd, sizeof(d->cmd), "comfy_test");
    d->ok = ok ? 1 : 0;
    d->chapter = -1;
    snprintf(d->message, sizeof(d->message), "%s",
             ok ? "ComfyUI 连接成功" : (err[0] ? err : "ComfyUI 连接失败"));
    if (!PostMessageW(g_hwnd, NWM_TASK_DONE, (WPARAM)d, 0)) task_done_free(d);
    return 0;
}

/* comfy_gen：生成一张配图。payload 回 {"node":...,"path":...}，前端再用
   read_image 换 data URL 当背景。OOM/失败都必须回执，否则前端 pending 永挂。 */
DWORD WINAPI th_comfy_gen(LPVOID param)
{
    comfy_gen_task_t *t = (comfy_gen_task_t *)param;
    char err[512] = "";
    task_done_t *d;
    int ok;
    if (!t) return 0;
    ok = (comfy_generate(t->url, t->workflow_json, t->prompt, t->out_path,
                         err, sizeof(err)) == 0);
    d = (task_done_t *)calloc(1, sizeof(task_done_t));
    if (d) {
        cJSON *p = cJSON_CreateObject();
        snprintf(d->cmd, sizeof(d->cmd), "comfy_gen");
        d->ok = ok ? 1 : 0;
        d->chapter = -1;
        snprintf(d->message, sizeof(d->message), "%s",
                 ok ? "配图已生成" : (err[0] ? err : "配图生成失败"));
        if (p) {
            cJSON_AddStringToObject(p, "node", t->node ? t->node : "");
            if (ok) cJSON_AddStringToObject(p, "path", t->out_path ? t->out_path : "");
            d->payload = cJSON_PrintUnformatted(p);
            cJSON_Delete(p);
        }
    }
    free(t->node); free(t->url); free(t->workflow_json);
    free(t->prompt); free(t->out_path); free(t);
    if (d) {
        if (!PostMessageW(g_hwnd, NWM_TASK_DONE, (WPARAM)d, 0)) task_done_free(d);
    }
    return 0;
}
