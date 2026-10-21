/* host_project_ops.c - project/config JSON snapshots + autosave + project listing (split from host.c 2026-10-18, zero behavior change) */
#include "host_internal.h"


/* ============================================================
 * 6. 项目 -> JSON 快照
 *    注意：不含章节正文（可能上百 KB），正文按需用 get_chapter 取
 * ============================================================ */

cJSON *chapter_brief_json(novel_project_t *p, int i)
{
    chapter_t *c = &p->chapters[i];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "index", i);
    add_str(o, "title", c->title);
    add_str(o, "summary", c->summary);
    cJSON_AddNumberToObject(o, "wordCount", c->word_count);
    cJSON_AddNumberToObject(o, "status", c->status);
    cJSON_AddNumberToObject(o, "iteration", c->iteration_count);
    cJSON_AddBoolToObject(o, "hasContent", (c->content && *c->content) ? 1 : 0);
    return o;
}

cJSON *character_json(character_t *c)
{
    cJSON *o = cJSON_CreateObject();
    add_str(o, "name", c->name);
    add_str(o, "role", c->role);
    add_str(o, "description", c->description);
    add_str(o, "traits", c->traits);
    add_str(o, "background", c->background);
    return o;
}

cJSON *project_meta_json(novel_project_t *p)
{
    cJSON *o = cJSON_CreateObject();
    int i, sd = -1, sp = -1;
    cJSON *arr;

    if (!p) {
        cJSON_AddBoolToObject(o, "hasProject", 0);
        cJSON_AddNumberToObject(o, "selectedDirection", -1);
        cJSON_AddNumberToObject(o, "selectedPlan", -1);
        cJSON_AddItemToObject(o, "directions", cJSON_CreateArray());
        cJSON_AddItemToObject(o, "plans", cJSON_CreateArray());
        cJSON_AddItemToObject(o, "chapters", cJSON_CreateArray());
        cJSON_AddItemToObject(o, "characters", cJSON_CreateArray());
        cJSON_AddItemToObject(o, "checks", cJSON_CreateArray());
        cJSON_AddItemToObject(o, "world", cJSON_CreateObject());
        return o;
    }

    cJSON_AddBoolToObject(o, "hasProject", 1);
    add_str(o, "id", p->id);
    add_str(o, "name", p->name);
    add_str(o, "seedIdea", p->seed_idea);
    add_str(o, "createdAt", p->created_at);
    add_str(o, "updatedAt", p->updated_at);
    add_str(o, "stage", p->current_stage);
    cJSON_AddNumberToObject(o, "totalWords", p->total_words);
    cJSON_AddNumberToObject(o, "iterationRound", p->iteration_round);
    add_str(o, "finaleReport", p->finale_report);

    if (p->selected_direction && p->directions) sd = (int)(p->selected_direction - p->directions);
    if (p->selected_plan && p->plans)           sp = (int)(p->selected_plan - p->plans);
    cJSON_AddNumberToObject(o, "selectedDirection", sd);
    cJSON_AddNumberToObject(o, "selectedPlan", sp);

    arr = cJSON_AddItemToObject(o, "directions", cJSON_CreateArray());
    for (i = 0; i < p->direction_count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "index", i);
        add_str(d, "title", p->directions[i].title);
        add_str(d, "description", p->directions[i].description);
        cJSON_AddBoolToObject(d, "selected", p->directions[i].selected ? 1 : 0);
        cJSON_AddItemToArray(arr, d);
    }

    arr = cJSON_AddItemToObject(o, "plans", cJSON_CreateArray());
    for (i = 0; i < p->plan_count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "index", i);
        add_str(d, "title", p->plans[i].title);
        add_str(d, "synopsis", p->plans[i].synopsis);
        add_str(d, "outline", p->plans[i].outline);
        cJSON_AddBoolToObject(d, "selected", p->plans[i].selected ? 1 : 0);
        cJSON_AddItemToArray(arr, d);
    }

    arr = cJSON_AddItemToObject(o, "chapters", cJSON_CreateArray());
    for (i = 0; i < p->chapter_count; i++)
        cJSON_AddItemToArray(arr, chapter_brief_json(p, i));

    arr = cJSON_AddItemToObject(o, "foreshadows", cJSON_CreateArray());
    for (i = 0; i < p->foreshadow_count; i++) {
        cJSON *f = cJSON_CreateObject();
        add_str(f, "title", p->foreshadows[i].title);
        add_str(f, "description", p->foreshadows[i].description);
        cJSON_AddNumberToObject(f, "planted", p->foreshadows[i].planted_chapter);
        cJSON_AddNumberToObject(f, "resolved", p->foreshadows[i].resolved_chapter);
        cJSON_AddItemToArray(arr, f);
    }

    {
        cJSON *w = cJSON_AddItemToObject(o, "world", cJSON_CreateObject());
        add_str(w, "worldType", p->world.world_type);
        add_str(w, "setting", p->world.setting);
        add_str(w, "powerSystem", p->world.power_system);
        add_str(w, "geography", p->world.geography);
        add_str(w, "history", p->world.history);
    }

    arr = cJSON_AddItemToObject(o, "characters", cJSON_CreateArray());
    for (i = 0; i < p->character_count; i++)
        cJSON_AddItemToArray(arr, character_json(&p->characters[i]));

    arr = cJSON_AddItemToObject(o, "checks", cJSON_CreateArray());
    for (i = 0; i < p->check_count; i++) {
        cJSON *d = cJSON_CreateObject();
        add_str(d, "timestamp", p->checks[i].timestamp);
        cJSON_AddNumberToObject(d, "chapterIndex", p->checks[i].chapter_index);
        cJSON_AddNumberToObject(d, "score", p->checks[i].score);
        add_str(d, "issues", p->checks[i].issues);
        add_str(d, "suggestions", p->checks[i].suggestions);
        cJSON_AddItemToArray(arr, d);
    }

    cJSON_AddNumberToObject(o, "selectedChapter", g.selected_chapter);
    return o;
}

void push_project(void)
{
    push_event("project", project_meta_json(g.project));
}

/* ============================================================
 * 7. 配置 -> JSON
 * ============================================================ */

cJSON *config_json(void)
{
    cJSON *o = cJSON_CreateObject();
    char *cfg_path = config_get_default_path();
    /* 服务商预设随配置一起下发，前端据此渲染一键切换的按钮组。
       预设表本体在 config.c，属于配置层数据，与 Win32 壳无关。 */
    cJSON_AddItemToObject(o, "presets", config_presets_json());
    if (!g.config) {
        cJSON_AddBoolToObject(o, "ok", 0);
        free(cfg_path);
        return o;
    }
    cJSON_AddBoolToObject(o, "ok", 1);
    add_str(o, "apiUrl",  g.config->ai ? g.config->ai->api_url : "");
    add_str(o, "apiKey",  g.config->ai ? g.config->ai->api_key : "");
    add_str(o, "model",   g.config->ai ? g.config->ai->model   : "");
    cJSON_AddNumberToObject(o, "temperature", g.config->ai ? g.config->ai->temperature : 0.8);
    cJSON_AddNumberToObject(o, "maxTokens",   g.config->ai ? g.config->ai->max_tokens  : AI_MAX_TOKENS_DEFAULT);
    cJSON_AddNumberToObject(o, "timeout",     g.config->ai ? g.config->ai->timeout     : 120);
    cJSON_AddNumberToObject(o, "rateLimitMs", g.config->ai ? g.config->ai->rate_limit_ms : 0);
    cJSON_AddNumberToObject(o, "maxRetries",  g.config->ai ? g.config->ai->max_retries   : 2);
    add_str(o, "projectsDir", g.config->projects_dir);
    add_str(o, "language",    g.config->language);
    cJSON_AddNumberToObject(o, "autoSaveInterval", g.config->auto_save_interval);
    cJSON_AddBoolToObject(o, "enableConsistencyCheck", g.config->enable_consistency_check ? 1 : 0);
    add_str(o, "comfyUrl", g.config->comfy_url ? g.config->comfy_url : "");
    add_str(o, "comfyWorkflowPath", g.config->comfy_workflow_path ? g.config->comfy_workflow_path : "");
    cJSON_AddBoolToObject(o, "comfyAutoBg", g.config->comfy_auto_bg ? 1 : 0);
    add_str(o, "configPath", cfg_path);
    add_str(o, "exportDir", g.export_dir);
    add_str(o, "lastDoc", g.last_doc);
    add_str(o, "lastTxt", g.last_txt);
    add_str(o, "lastEpub", g.last_epub);   /* 支线 B */
    free(cfg_path);
    return o;
}

/* 把 JSON 里的字段写回 config；缺字段则保留原值 */
void apply_config_json(cJSON *p)
{
    if (!g.config || !g.config->ai || !p) return;
#define SETS(field, key, def) do { \
        const char *v = j_str(p, key, NULL); \
        if (v) { free(g.config->ai->field); g.config->ai->field = str_dup(v); } \
        (void)(def); \
    } while (0)
    SETS(api_url, "apiUrl", "");
    SETS(api_key, "apiKey", "");
    SETS(model,   "model",  "");
#undef SETS

    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(p, "temperature");
        if (v && cJSON_IsNumber(v)) g.config->ai->temperature = v->valuedouble;
    }
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(p, "maxTokens");
        if (v && cJSON_IsNumber(v)) {
            int m = v->valueint;
            /* 与 config.c 的加载侧共用同一组边界，避免「UI 能存进去、
               下次加载读不回来」的区间打架 */
            if (m < AI_MAX_TOKENS_MIN) m = AI_MAX_TOKENS_MIN;
            if (m > AI_MAX_TOKENS_MAX) m = AI_MAX_TOKENS_MAX;
            g.config->ai->max_tokens = m;
        }
    }
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(p, "rateLimitMs");
        if (v && cJSON_IsNumber(v)) {
            int r = v->valueint;
            if (r < 0) r = 0;
            if (r > 600000) r = 600000; /* 10 分钟封顶 */
            g.config->ai->rate_limit_ms = r;
        }
    }
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(p, "maxRetries");
        if (v && cJSON_IsNumber(v)) {
            int r = v->valueint;
            if (r < 0) r = 0;
            if (r > 10) r = 10;
            g.config->ai->max_retries = r;
        }
    }
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(p, "timeout");
        if (v && cJSON_IsNumber(v)) {
            int t = v->valueint;
            if (t < 10) t = 10;
            if (t > 3600) t = 3600;
            g.config->ai->timeout = t;
        }
    }
    {
        const char *v = j_str(p, "projectsDir", NULL);
        if (v && *v) {
            free(g.config->projects_dir);
            g.config->projects_dir = str_dup(v);
            ensure_dir(g.config->projects_dir);
        }
    }
    {
        /* 导出目录：从前端回传的值写回 config 并同步运行时副本，实现持久化 */
        const char *v = j_str(p, "exportDir", NULL);
        if (v) {
            free(g.config->export_dir);
            g.config->export_dir = str_dup(v);
            snprintf(g.export_dir, sizeof(g.export_dir), "%s", v);
        }
    }
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(p, "autoSaveInterval");
        if (v && cJSON_IsNumber(v)) {
            int s = v->valueint;
            if (s < 0) s = 0;
            if (s > 3600) s = 3600;
            g.config->auto_save_interval = s;
        }
    }
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(p, "enableConsistencyCheck");
        if (v) g.config->enable_consistency_check = cJSON_IsTrue(v) ? 1 : 0;
    }
    {
        /* ComfyUI 配图接口：地址/工作流路径/自动生成背景开关 */
        const char *v = j_str(p, "comfyUrl", NULL);
        if (v) {
            free(g.config->comfy_url);
            g.config->comfy_url = str_dup(v);
        }
    }
    {
        const char *v = j_str(p, "comfyWorkflowPath", NULL);
        if (v) {
            free(g.config->comfy_workflow_path);
            g.config->comfy_workflow_path = *v ? str_dup(v) : NULL;
        }
    }
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(p, "comfyAutoBg");
        if (v) g.config->comfy_auto_bg = cJSON_IsTrue(v) ? 1 : 0;
    }
}

/* ============================================================
 * 11. 设置变更后重启自动保存定时器
 * ============================================================ */

void restart_autosave(void)
{
    KillTimer(g_hwnd, IDT_AUTOSAVE);
    if (g.config && g.config->auto_save_interval > 0) {
        /* 秒→毫秒要留在 32 位内：截到 86400 秒（1 天）上限，防 UINT 溢出绕回成高频定时器 */
        UINT sec = (UINT)(g.config->auto_save_interval > 86400 ? 86400 : g.config->auto_save_interval);
        SetTimer(g_hwnd, IDT_AUTOSAVE, sec * 1000u, NULL);
    }
}

int save_project_silently(void)
{
    if (!g.project || !g.config || !g.config->projects_dir) return 0;
    if (project_save(g.project, g.config->projects_dir) != 0) return 0;
    g.dirty = 0;
    return 1;
}

/* ============================================================
 * 15. 列出项目目录
 * ============================================================ */

/* 扫描 projects 目录，返回项目摘要数组（调用方持有） */
cJSON *build_project_list(void)
{
    cJSON *arr = cJSON_CreateArray();
    char *dir = NULL;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[MAX_PATH + 8];

    if (!arr) return NULL;
    if (g.config && g.config->projects_dir) dir = str_dup(g.config->projects_dir);
    if (dir) {
        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char sub[MAX_PATH + 8];
                char pj[MAX_PATH + 16];
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
                snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
                snprintf(pj, sizeof(pj), "%s\\project.json", sub);
                if (file_exists(pj)) {
                    cJSON *o = cJSON_CreateObject();
                    char *txt = read_file(pj);
                    cJSON_AddStringToObject(o, "id", fd.cFileName);
                    cJSON_AddStringToObject(o, "path", pj);
                    if (txt) {
                        cJSON *root = cJSON_Parse(txt);
                        if (root) {
                            cJSON_AddStringToObject(o, "name", j_str(root, "name", fd.cFileName));
                            cJSON_AddStringToObject(o, "updatedAt", j_str(root, "updated_at", ""));
                            cJSON_AddStringToObject(o, "stage", j_str(root, "current_stage", ""));
                            {
                                cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "chapters");
                                if (jc && cJSON_IsArray(jc))
                                    cJSON_AddNumberToObject(o, "chapterCount", cJSON_GetArraySize(jc));
                                else
                                    cJSON_AddNumberToObject(o, "chapterCount", 0);
                            }
                            cJSON_Delete(root);
                        } else {
                            cJSON_AddStringToObject(o, "name", fd.cFileName);
                            cJSON_AddStringToObject(o, "updatedAt", "");
                            cJSON_AddNumberToObject(o, "chapterCount", 0);
                        }
                        free(txt);
                    } else {
                        cJSON_AddStringToObject(o, "name", fd.cFileName);
                        cJSON_AddStringToObject(o, "updatedAt", "");
                        cJSON_AddNumberToObject(o, "chapterCount", 0);
                    }
                    cJSON_AddItemToArray(arr, o);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        free(dir);
    }
    return arr;
}

void reply_project_list(int id, const char *cmd)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "projects", build_project_list());
    reply_ok(id, cmd, d);
}

/* 任务结束后主动推送一份新的项目列表（幂等，前端直接覆盖） */
void push_project_list(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "projects", build_project_list());
    push_event("projects", d);
}
