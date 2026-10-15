/* ============================================================
 * novelnow WebUI —— 宿主与桥接层
 *
 * 设计原则
 *  1. 业务层（engine / ai_api / project / config / utils / cJSON）**零改动**，
 *     本文件只做「窗口 + WebView2 + JSON 命令分发 + 线程编组」。
 *  2. 所有 UI 交互走 JSON：
 *       JS -> C : {"cmd":"...", "id":N, "params":{...}}
 *       C -> JS : {"type":"result","id":N,"cmd":"...","ok":true,"data":{...}}
 *                 {"type":"event","name":"progress","data":{...}}
 *  3. project 结构体**绝不跨线程传递**，只在 UI 线程读写；工作线程通过
 *     PostMessage 回主线程后由主线程序列化成 JSON 快照推给 JS。
 *     这是相对旧版 WM_USER+9 直接传 novel_project_t* 的关键修正。
 *  4. 所有 C->JS 推送都在 UI 线程发生（WebView2 的线程要求）。
 *  5. 工作线程需要用户确认时（逐章生成），阻塞在事件上；主线程把问题
 *     推给 JS，JS 回答后唤醒——UI 全程不阻塞。
 *
 * 编译：见 build.bat（MinGW gcc，纯 C）
 * ============================================================ */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <WebView2.h>

#include "cJSON.h"
#include "utils.h"
#include "ai_api.h"
#include "config.h"
#include "project.h"
#include "engine.h"

/* 本文件所有 snprintf 都写入固定缓冲，截断是安全且有意的行为，
   关闭 GCC 对应的推测性告警（它会为每个 %s 计算最坏长度）。 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#define NW_VERSION L"1.0.0"

/* ============================================================
 * 0. 全局状态
 * ============================================================ */

typedef struct {
    HWND                 hwnd;
    app_config_t        *config;
    engine_state_t      *engine;
    novel_project_t     *project;
    int                  selected_chapter;   /* -1 表示未选 */
    volatile LONG        busy;               /* 后台任务占用中 */
    int                  dirty;              /* 项目有未保存改动 */
    int                  webview_ready;      /* 页面已加载完毕 */
    char                 export_dir[1024];
    char                 last_doc[2048];
    char                 last_txt[2048];
} app_t;

static app_t g;

static WCHAR            g_exeDir[MAX_PATH];
static HWND             g_hwnd           = NULL;
static ICoreWebView2Controller *g_ctrl   = NULL;
static ICoreWebView2          *g_wv      = NULL;
static HMODULE          g_hLoader        = NULL;
static DWORD            g_uiThreadId     = 0;
static int              g_shuttingDown   = 0;

/* 命令行开关（全局，供各回调读取） */
static int              g_shotMode       = 0;   /* --shot:  就绪后自动截图 */
static int              g_useVHost       = 0;   /* --vhost: 改用虚拟主机映射（实验） */
static int              g_holdMode       = 0;   /* --hold:  截图后不退出（人工验收） */
static int              g_useFileUrl     = 0;   /* --file:  用 file:// 直载 ui/index.html */
static IStream         *g_shotStream     = NULL;/* CapturePreview 的输出流 */
static int              g_visTries       = 0;   /* 可见性自愈重试计数 */

/* 自动保存 */
#define IDT_AUTOSAVE 1

/* 自定义窗口消息 */
#define NWM_TASK_DONE   (WM_APP + 10)  /* wParam = task_done_t* */
#define NWM_ASK         (WM_APP + 11)  /* wParam = ask_t* */
#define NWM_PROGRESS    (WM_APP + 12)  /* wParam = prog_t* */
#define NWM_CHAPTER_DONE (WM_APP + 13) /* wParam = 章节索引 */
#define NWM_POST        (WM_APP + 14)  /* wParam = WCHAR* 待投递报文（由本窗口释放）*/
#define NWM_CHAPTER_DELTA (WM_APP + 15) /* wParam = delta_t* 流式增量 */

/* WebView2Loader 导出函数 */
typedef HRESULT (STDAPICALLTYPE *PFN_CreateEnv)(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);
typedef HRESULT (STDAPICALLTYPE *PFN_GetVersion)(PCWSTR browserExecutableFolder, LPWSTR *versionInfo);

static PFN_CreateEnv  g_pCreateEnv  = NULL;
static PFN_GetVersion g_pGetVersion = NULL;

/* ============================================================
 * 1. 编码工具
 * ============================================================ */

static WCHAR *u8_to_w(const char *s)
{
    int n;
    WCHAR *out;
    if (!s) s = "";
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    out = (WCHAR *)malloc((size_t)n * sizeof(WCHAR));
    if (!out) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    return out;
}

static char *w_to_u8_alloc(const WCHAR *s)
{
    int n;
    char *out;
    if (!s) s = L"";
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    out = (char *)malloc((size_t)n);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
    return out;
}

static void w_to_u8(const WCHAR *s, char *out, int n)
{
    if (!out || n <= 0) return;
    out[0] = '\0';
    if (!s) return;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
    out[n - 1] = '\0';
}

/* ============================================================
 * 2. 日志
 * ============================================================ */

static void log_line(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    char path[2048];
    char msg[1024];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    snprintf(path, sizeof(path), "%ls\\webui.log", g_exeDir);
    f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%8lu] %s\r\n", (unsigned long)GetTickCount(), msg);
    fclose(f);
}

/* ============================================================
 * 3. JSON 取值小工具（全部做 NULL 保护）
 * ============================================================ */

static const char *j_str(cJSON *o, const char *k, const char *def)
{
    cJSON *v;
    if (!o) return def;
    v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!v || !cJSON_IsString(v) || !v->valuestring) return def;
    return v->valuestring;
}

static int j_int(cJSON *o, const char *k, int def)
{
    cJSON *v;
    if (!o) return def;
    v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!v || !cJSON_IsNumber(v)) return def;
    return v->valueint;
}

/* 布尔取值：兼容 true / 1 / "true" / "1" 四种写法 */
static int j_bool(cJSON *o, const char *k, int def)
{
    cJSON *v;
    if (!o) return def;
    v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!v) return def;
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v) ? 1 : 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0 ? 1 : 0;
    if (cJSON_IsString(v) && v->valuestring)
        return (strcmp(v->valuestring, "1") == 0 || strcmp(v->valuestring, "true") == 0);
    return def;
}

/* 参数在 params 子对象里；没有 params 时退回根对象，方便简写 */
static cJSON *j_params(cJSON *root) { return cJSON_GetObjectItemCaseSensitive(root, "params"); }

static const char *p_str(cJSON *root, const char *k, const char *def)
{
    cJSON *p = j_params(root);
    return j_str(p ? p : root, k, def);
}
static int p_int(cJSON *root, const char *k, int def)
{
    cJSON *p = j_params(root);
    return j_int(p ? p : root, k, def);
}
static int p_bool(cJSON *root, const char *k, int def)
{
    cJSON *p = j_params(root);
    return j_bool(p ? p : root, k, def);
}

/* 加字符串但容忍 NULL */
static void add_str(cJSON *o, const char *k, const char *v)
{
    cJSON_AddStringToObject(o, k, v ? v : "");
}

/* ============================================================
 * 4. C -> JS 推送
 *    ★ 必须在 UI 线程调用（WebView2 的 PostWebMessageAsJson 要求）
 * ============================================================ */

/* ---- 出站重投队列 ----
   本机 WebView2（Runtime 154）存在这个状态：controller 创建时宿主窗口若还没
   show，IsVisible 会被锁死在 FALSE。此后 PostWebMessageAsJson 一律返回
   ERROR_INVALID_STATE(0x8007139F) 并把报文整条丢掉——JS 侧既不 resolve 也不
   reject，表现就是「界面起来了但所有按钮没反应」。
   实测同一份文件、同一个 exe，闭环成功率只有 50% 左右，高下全看这个竞态；
   而 cmd_hello 的回执正好撞在这个窗口里，所以启动握手首当其冲。
   对策：投失败的报文不丢，进队列，等可见性自愈后按原顺序补发。 */
#define POST_RETRY_MAX  64
#define IDT_POST_RETRY  95

static WCHAR *g_postRetry[POST_RETRY_MAX];
static int    g_postRetryN = 0;
static int    g_postRetryTries = 0;

/* 在消息循环里补发；顺带把卡住的可见性推一把 */
static void post_retry_pump(HWND hwnd)
{
    int i, keep = 0;
    BOOL vis = FALSE;

    if (g_ctrl) {
        g_ctrl->lpVtbl->get_IsVisible(g_ctrl, &vis);
        if (!vis) {
            g_ctrl->lpVtbl->put_IsVisible(g_ctrl, TRUE);
            g_ctrl->lpVtbl->get_IsVisible(g_ctrl, &vis);
        }
    }

    for (i = 0; i < g_postRetryN; i++) {
        WCHAR *w = g_postRetry[i];
        if (g_wv && !g_shuttingDown &&
            SUCCEEDED(g_wv->lpVtbl->PostWebMessageAsJson(g_wv, w))) {
            free(w);
        } else {
            g_postRetry[keep++] = w;    /* 还没就绪，按原顺序留在队里 */
        }
    }
    g_postRetryN = keep;

    if (g_postRetryN == 0) {
        KillTimer(hwnd, IDT_POST_RETRY);
        log_line("重投队列已清空（vis=%d，累计重试 %d 轮）", vis ? 1 : 0, g_postRetryTries);
        g_postRetryTries = 0;
    } else if (++g_postRetryTries >= 40) {
        log_line("!! 重试 %d 轮仍失败，丢弃 %d 条推送（vis=%d）",
                 g_postRetryTries, g_postRetryN, vis ? 1 : 0);
        for (i = 0; i < g_postRetryN; i++) free(g_postRetry[i]);
        g_postRetryN = 0;
        g_postRetryTries = 0;
        KillTimer(hwnd, IDT_POST_RETRY);
    }
}

static void post_json_root(cJSON *root)
{
    char *txt;
    WCHAR *wide;
    if (!root) return;
    if (!g_wv || g_shuttingDown) { cJSON_Delete(root); return; }
    /* 投递前记一条「类型 + 序号」。
       ★ 绝不打印报文正文：hello 回执里含 API Key，写进日志等于泄露。 */
    {
        static int n = 0;
        if (n < 12) {
            const char *ty = j_str(root, "type", "?");
            const char *nm = j_str(root, "name", NULL);
            if (!nm) nm = j_str(root, "cmd", NULL);
            n++;
            log_line("--> post #%d %s %s", n, ty, nm ? nm : "");
        }
    }

    txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) { log_line("!! post: cJSON_PrintUnformatted 返回 NULL"); return; }
    wide = u8_to_w(txt);
    free(txt);
    if (!wide) { log_line("!! post: u8_to_w 返回 NULL"); return; }

    /* 不在这里直接调 PostWebMessageAsJson，改投给本窗口的消息循环延后一拍。
       原因：多数推送都发生在 WebView2 自己的回调里（尤其是 MsgCB_Invoke 里回
       握手包）或工作线程转发过来的位置上。在回调内部同步回投 WebView2 会静默
       丢包——返回 S_OK，JS 侧却什么都收不到，表现为「界面起来了但按钮全没反应」。
       实测同一份文件同一个 exe，闭环成功率只有 50% 左右，且与文档内容无关。
       交给消息循环后，投递发生在回调返回之后，实测 6/6 稳定。
       报文所有权转交给 NWM_POST 的处理分支。 */
    if (!PostMessageW(g_hwnd, NWM_POST, (WPARAM)wide, 0)) {
        log_line("!! PostMessageW(NWM_POST) 失败，丢弃一条推送");
        free(wide);
    }
}

static void push_event(const char *name, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { if (data) cJSON_Delete(data); return; }
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "name", name);
    if (data) cJSON_AddItemToObject(root, "data", data);
    else      cJSON_AddItemToObject(root, "data", cJSON_CreateObject());
    post_json_root(root);
}

static void reply_ok(int id, const char *cmd, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { if (data) cJSON_Delete(data); return; }
    cJSON_AddStringToObject(root, "type", "result");
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "cmd", cmd ? cmd : "");
    cJSON_AddBoolToObject(root, "ok", 1);
    if (data) cJSON_AddItemToObject(root, "data", data);
    else      cJSON_AddItemToObject(root, "data", cJSON_CreateObject());
    post_json_root(root);
}

static void reply_err(int id, const char *cmd, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "result");
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "cmd", cmd ? cmd : "");
    cJSON_AddBoolToObject(root, "ok", 0);
    cJSON_AddStringToObject(root, "error", msg ? msg : "未知错误");
    post_json_root(root);
}

static void push_status(const char *text)
{
    cJSON *d = cJSON_CreateObject();
    add_str(d, "text", text);
    push_event("status", d);
}

static void push_toast(const char *kind, const char *text)
{
    cJSON *d = cJSON_CreateObject();
    add_str(d, "kind", kind);   /* info / success / error / warn */
    add_str(d, "text", text);
    push_event("toast", d);
}

/* ============================================================
 * 5. 忙闲控制（与旧版 require_idle / try_begin_task 等价）
 * ============================================================ */

static int is_busy(void) { return InterlockedCompareExchange(&g.busy, 0, 0) != 0; }

static int try_begin_task(void)
{
    if (InterlockedCompareExchange(&g.busy, 1, 0) != 0) return 0;
    return 1;
}

static void end_task(void)
{
    InterlockedExchange(&g.busy, 0);
    /* 通知前端任务态结束 */
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "busy", 0);
    push_event("busy", d);
}

static void push_busy(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "busy", is_busy() ? 1 : 0);
    push_event("busy", d);
}

/* ============================================================
 * 6. 项目 -> JSON 快照
 *    注意：不含章节正文（可能上百 KB），正文按需用 get_chapter 取
 * ============================================================ */

static cJSON *chapter_brief_json(novel_project_t *p, int i)
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

static cJSON *character_json(character_t *c)
{
    cJSON *o = cJSON_CreateObject();
    add_str(o, "name", c->name);
    add_str(o, "role", c->role);
    add_str(o, "description", c->description);
    add_str(o, "traits", c->traits);
    add_str(o, "background", c->background);
    return o;
}

static cJSON *project_meta_json(novel_project_t *p)
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

static void push_project(void)
{
    push_event("project", project_meta_json(g.project));
}

/* ============================================================
 * 7. 配置 -> JSON
 * ============================================================ */

static cJSON *config_json(void)
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
    add_str(o, "configPath", cfg_path);
    add_str(o, "exportDir", g.export_dir);
    add_str(o, "lastDoc", g.last_doc);
    add_str(o, "lastTxt", g.last_txt);
    free(cfg_path);
    return o;
}

/* 把 JSON 里的字段写回 config；缺字段则保留原值 */
static void apply_config_json(cJSON *p)
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
}

/* ============================================================
 * 8. 跨线程「询问用户」
 *    工作线程阻塞在事件上，主线程把问题推给 JS，JS 回答后唤醒
 * ============================================================ */

/* 询问的最长等待。给得很宽（30 分钟）是刻意的：用户可能真的在逐字读一章，
   不该被催；它只是一道兜底，保证前端万一失灵时线程不会永久挂住。
   配合「取消任务」里主动唤醒待决询问，正常路径根本走不到这个上限。 */
#define ASK_WAIT_MAX_SEC 1800

typedef struct ask_s {
    int    id;
    HANDLE done;      /* 手动复位事件 */
    int    answer;    /* 1=是 2=否 3=取消 */
    struct ask_s *next;
    char   text[1];   /* 柔性数组（标题由前端统一渲染） */
} ask_t;

static CRITICAL_SECTION g_askCs;
static int      g_askCsReady = 0;
static ask_t   *g_asks      = NULL;
static int      g_askSeq    = 0;

static void ask_register(ask_t *a)
{
    if (!g_askCsReady) return;
    EnterCriticalSection(&g_askCs);
    a->next = g_asks;
    g_asks = a;
    LeaveCriticalSection(&g_askCs);
}

static void ask_unregister(ask_t *a)
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
static void ask_release_all(void)
{
    ask_t *it;
    if (!g_askCsReady) return;
    EnterCriticalSection(&g_askCs);
    for (it = g_asks; it; it = it->next) { it->answer = 3; SetEvent(it->done); }
    LeaveCriticalSection(&g_askCs);
}

/* 返回 1=是 2=否 3=取消；非 UI 线程才允许调用 */
static int bridge_ask(const char *text)
{
    size_t tl = text ? strlen(text) : 0;
    ask_t *a;
    if (GetCurrentThreadId() == g_uiThreadId) return 3;   /* UI 线程不能阻塞自身 */
    if (g_shuttingDown) return 3;

    a = (ask_t *)calloc(1, sizeof(ask_t) + tl);
    if (!a) return 3;
    a->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    a->answer = 3;
    if (tl) memcpy(a->text, text, tl);
    a->text[tl] = '\0';
    a->id = (int)InterlockedIncrement((LONG *)&g_askSeq);
    if (!a->done) { free(a); return 3; }

    ask_register(a);
    if (!PostMessageW(g_hwnd, NWM_ASK, (WPARAM)a, 0)) {
        ask_unregister(a);
        CloseHandle(a->done);
        free(a);
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
        CloseHandle(a->done);
        free(a);
        return ans;
    }
}

/* ============================================================
 * 9. 进度回调（工作线程 -> 主线程 -> JS）
 * ============================================================ */

typedef struct {
    char stage[128];
    int  current;
    int  total;
    char message[512];
} prog_t;

static void on_progress(const char *stage, int current, int total,
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
typedef struct {
    int index;      /* 正在生成的章节索引 */
    char *text;     /* 本次增量（堆上，主线程用完释放） */
} delta_t;

static void on_chapter_delta(int chapter_index, const char *delta, void *user_data)
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

/* 通用流式增量（方向/方案/大纲/一致性检查等）：复用 delta_t，index = -1 */
#define NWM_TEXT_DELTA (WM_APP + 16)

static void on_text_delta(const char *delta, void *user_data)
{
    (void)user_data;
    on_chapter_delta(-1, delta, NULL);
}

/* ============================================================
 * 10. 任务完成回执（工作线程 -> 主线程）
 * ============================================================ */

typedef struct {
    char cmd[32];
    int  result;        /* 引擎原始返回值 */
    int  ok;            /* 业务成功与否 */
    int  chapter;       /* >=0 表示「本章已完成，刷新该章」 */
    void *ptr;          /* 需要把堆对象交给主线程时用（如 load_project 的 proj） */
    char message[1400]; /* 只放给人看的一句话；长度不定的大块数据走 payload */
    char *payload;      /* 可选：堆上的原始数据（如 fetch_models 的完整 JSON）。
                           属主是本结构，必须用 task_done_free() 释放。
                           绝不能再把大 JSON 往 message 里塞：抓 1400 字节截断后
                           cJSON_Parse 必然失败，模型列表就整块丢失了 */
} task_done_t;

/* task_done_t 的统一释放口：payload 是堆指针，漏掉它会漏内存 */
static void task_done_free(task_done_t *t)
{
    if (!t) return;
    free(t->payload);
    free(t);
}

static void post_task_done(const char *cmd, int result, int ok,
                           const char *message, int chapter)
{
    task_done_t *t;
    if (g_shuttingDown || !g_hwnd) return;
    t = (task_done_t *)calloc(1, sizeof(task_done_t));
    if (!t) return;
    snprintf(t->cmd, sizeof(t->cmd), "%s", cmd ? cmd : "");
    t->result = result;
    t->ok = ok;
    t->chapter = chapter;
    if (message) snprintf(t->message, sizeof(t->message), "%s", message);
    if (!PostMessageW(g_hwnd, NWM_TASK_DONE, (WPARAM)t, 0)) task_done_free(t);
}

static int launch_thread(LPTHREAD_START_ROUTINE fn, LPVOID param)
{
    DWORD tid;
    HANDLE h = CreateThread(NULL, 0, fn, param, 0, &tid);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}

static const char *engine_err(void)
{
    if (g.engine && g.engine->last_error[0]) return g.engine->last_error;
    return "未知错误";
}

/* ============================================================
 * 11. 设置变更后重启自动保存定时器
 * ============================================================ */

static void restart_autosave(void)
{
    KillTimer(g_hwnd, IDT_AUTOSAVE);
    if (g.config && g.config->auto_save_interval > 0)
        SetTimer(g_hwnd, IDT_AUTOSAVE, (UINT)g.config->auto_save_interval * 1000, NULL);
}

static int save_project_silently(void)
{
    if (!g.project || !g.config || !g.config->projects_dir) return 0;
    if (project_save(g.project, g.config->projects_dir) != 0) return 0;
    g.dirty = 0;
    return 1;
}

/* ============================================================
 * 12. 任务线程实现
 * ============================================================ */

/* ---- 生成创作方向 ---- */
typedef struct { int iterations; int count; } gen_dir_task_t;
static DWORD WINAPI th_gen_directions(LPVOID param)
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
static DWORD WINAPI th_gen_plans(LPVOID param)
{
    int count = (int)(INT_PTR)param;
    int r = engine_generate_plans(g.engine, g.project, count);
    if (r == ENGINE_CANCELLED) post_task_done("gen_plans", r, 0, "已取消", -1);
    else if (r > 0) post_task_done("gen_plans", r, 1, "小说方案已生成", -1);
    else post_task_done("gen_plans", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 细化大纲 ---- */
static DWORD WINAPI th_expand_outline(LPVOID param)
{
    int count = (int)(INT_PTR)param;
    int r = engine_expand_outline(g.engine, g.project, count);
    if (r == ENGINE_CANCELLED) post_task_done("expand_outline", r, 0, "已取消", -1);
    else if (r >= 0) post_task_done("expand_outline", r, 1, "大纲已细化", -1);
    else post_task_done("expand_outline", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 扩写单章 ---- */
typedef struct { int index; int words; } ch_task_t;
static DWORD WINAPI th_expand_chapter(LPVOID param)
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
static DWORD WINAPI th_expand_all(LPVOID param)
{
    int words = (int)(INT_PTR)param;
    int r = engine_expand_all_chapters(g.engine, g.project, words);
    if (r == ENGINE_CANCELLED) post_task_done("expand_all", r, 0, "已取消，已完成的章节已保留", -1);
    else if (r == 0) post_task_done("expand_all", r, 1, "全部扩写完成", -1);
    else post_task_done("expand_all", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 逐章生成（每章完成后询问是否满意） ---- */
static DWORD WINAPI th_step_write(LPVOID param)
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
static DWORD WINAPI th_check_consistency(LPVOID param)
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
static DWORD WINAPI th_sum_world(LPVOID param)
{
    int r = engine_summarize_worldview(g.engine, g.project);
    (void)param;
    if (r == ENGINE_CANCELLED) post_task_done("summarize_worldview", r, 0, "已取消", -1);
    else if (r == 0) post_task_done("summarize_worldview", 0, 1, "世界观已总结", -1);
    else post_task_done("summarize_worldview", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 生成人物 ---- */
static DWORD WINAPI th_gen_chars(LPVOID param)
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
static DWORD WINAPI th_check_finale(LPVOID param)
{
    int r = engine_check_finale(g.engine, g.project);
    (void)param;
    if (r == ENGINE_CANCELLED) post_task_done("check_finale", r, 0, "已取消", -1);
    else if (r == 0) post_task_done("check_finale", 0, 1, "伏笔回收校验完成", -1);
    else post_task_done("check_finale", r, 0, engine_err(), -1);
    return 0;
}

/* ---- 分析外部文本 ---- */
static DWORD WINAPI th_analyze_text(LPVOID param)
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
typedef struct { int dirs; int plans; int chapters; int words; int start_ch; } auto_task_t;
static DWORD WINAPI th_auto_write(LPVOID param)
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
static DWORD WINAPI th_fetch_models(LPVOID param)
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

/* ---- 加载项目 ---- */
static DWORD WINAPI th_load_project(LPVOID param)
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
 * 13. 文件对话框
 * ============================================================ */

static int CALLBACK browse_cb(HWND h, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED && data)
        SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

/* 弹出文件夹选择框；返回 UTF-8 路径（需 free），取消返回 NULL */
static char *pick_folder_dialog(const char *initial)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    WCHAR title[] = L"请选择输出文件夹";
    WCHAR initW[MAX_PATH] = L"";
    char *result = NULL;

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g.hwnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_cb;
    if (initial && *initial) {
        MultiByteToWideChar(CP_UTF8, 0, initial, -1, initW, MAX_PATH);
        bi.lParam = (LPARAM)initW;
    }

    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return NULL;
    if (SHGetPathFromIDListW(pidl, initW)) result = w_to_u8_alloc(initW);
    CoTaskMemFree(pidl);
    return result;
}

/* 选择 project.json；返回 UTF-8 路径（需 free），取消返回 NULL */
static char *pick_project_file_dialog(const char *initial_dir)
{
    OPENFILENAMEW ofn;
    WCHAR szFileW[1024] = L"";
    WCHAR filterW[256];
    WCHAR initDirW[MAX_PATH] = L"";
    const char *segs[4] = { "项目文件 (project.json)", "project.json", "所有文件 (*.*)", "*.*" };
    int si, k = 0;
    char *result = NULL;

    for (si = 0; si < 4; si++) {
        WCHAR *sg = u8_to_w(segs[si]);
        if (sg) {
            int n = lstrlenW(sg);
            if (k + n + 1 < 256) {
                memcpy(filterW + k, sg, (size_t)n * sizeof(WCHAR));
                k += n;
                filterW[k++] = L'\0';
            }
            free(sg);
        }
    }
    filterW[k] = L'\0';

    if (initial_dir && *initial_dir)
        MultiByteToWideChar(CP_UTF8, 0, initial_dir, -1, initDirW, MAX_PATH);

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFile = szFileW;
    ofn.nMaxFile = 1024;
    ofn.lpstrFilter = filterW;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = initDirW;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) result = w_to_u8_alloc(szFileW);
    return result;
}

/* 选择导入的 txt；返回 UTF-8 路径（需 free），取消返回 NULL */
static char *pick_text_file_dialog(void)
{
    OPENFILENAMEW ofn;
    WCHAR szFileW[1024] = L"";
    WCHAR filterW[] = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";
    char *result = NULL;

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFile = szFileW;
    ofn.nMaxFile = 1024;
    ofn.lpstrFilter = filterW;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) result = w_to_u8_alloc(szFileW);
    return result;
}

/* ============================================================
 * 14. 导出（DOC/RTF + TXT）—— 移植自旧版 main.c，逻辑未改
 * ============================================================ */

static int write_utf8_bom_file(const char *path, const char *content)
{
    static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fwrite(bom, 1, 3, fp);
    if (content) fwrite(content, 1, strlen(content), fp);
    fclose(fp);
    return 0;
}

static void rtf_append_text(strbuf_t *buf, const char *utf8)
{
    const unsigned char *p = (const unsigned char *)utf8;
    if (!utf8) return;
    while (*p) {
        if (*p < 0x80) {
            if (*p == '\\' || *p == '{' || *p == '}') strbuf_append_char(buf, '\\');
            strbuf_append_char(buf, (char)*p);
            p++;
        } else {
            unsigned int cp = 0;
            int len = 0, i;
            if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
            else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
            else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
            else { p++; continue; }
            for (i = 1; i < len; i++) {
                if (p[i] == '\0' || (p[i] & 0xC0) != 0x80) break;
                cp = (cp << 6) | (p[i] & 0x3F);
            }
            if (i < len) { p++; continue; }
            if (cp <= 0xFFFF) {
                char tmp[40];
                sprintf(tmp, "\\u%u?", cp);
                strbuf_append(buf, tmp);
            } else {
                unsigned int hi, lo;
                char tmp[48];
                cp -= 0x10000;
                hi = 0xD800 + (cp >> 10);
                lo = 0xDC00 + (cp & 0x3FF);
                sprintf(tmp, "\\u%u?\\u%u?", hi, lo);
                strbuf_append(buf, tmp);
            }
            p += len;
        }
    }
}

static int export_novel_txt(novel_project_t *proj, const char *path)
{
    strbuf_t *buf;
    int i, ret;
    if (!proj || !path) return -1;
    buf = strbuf_create(8192);
    if (!buf) return -1;
    if (proj->name) { strbuf_append(buf, proj->name); strbuf_append(buf, "\r\n\r\n"); }
    for (i = 0; i < proj->chapter_count; i++) {
        chapter_t *ch = &proj->chapters[i];
        strbuf_append(buf, ch->title ? ch->title : "");
        strbuf_append(buf, "\r\n\r\n");
        if (ch->content && *ch->content) {
            strbuf_append(buf, ch->content);
            strbuf_append(buf, "\r\n\r\n");
        }
    }
    ret = write_utf8_bom_file(path, buf->data);
    strbuf_destroy(buf);
    return ret;
}

static int export_novel_doc(novel_project_t *proj, const char *path)
{
    strbuf_t *buf;
    int i, ret;
    if (!proj || !path) return -1;
    buf = strbuf_create(8192);
    if (!buf) return -1;
    strbuf_append(buf, "{\\rtf1\\ansi\\ansicpg936\\deff0{\\fonttbl{\\f0\\fnil\\fcharset134 Microsoft YaHei;}}\\f0\\fs24\n");
    if (proj->name) {
        strbuf_append(buf, "\\b\\fs36 ");
        rtf_append_text(buf, proj->name);
        strbuf_append(buf, "\\b0\\fs24\\par\\par\n");
    }
    for (i = 0; i < proj->chapter_count; i++) {
        chapter_t *ch = &proj->chapters[i];
        strbuf_append(buf, "\\b\\fs28 ");
        rtf_append_text(buf, ch->title ? ch->title : "");
        strbuf_append(buf, "\\b0\\fs24\\par\n");
        if (ch->content && *ch->content) {
            rtf_append_text(buf, ch->content);
            strbuf_append(buf, "\\par\\par\n");
        }
    }
    strbuf_append(buf, "}\n");
    ret = write_file(path, buf->data);
    strbuf_destroy(buf);
    return ret;
}

/* 导出到指定目录；返回 0 成功 */
static int do_export(const char *dir_override)
{
    const char *dir;
    char doc_path[2048], txt_path[2048];
    char safe[256];
    const unsigned char *p;
    int k = 0;

    if (!g.project) return -1;
    dir = (dir_override && *dir_override) ? dir_override
        : (g.export_dir[0] ? g.export_dir
           : ((g.config && g.config->projects_dir && *g.config->projects_dir)
              ? g.config->projects_dir : "."));
    if (!dir || !*dir) dir = ".";

    p = (const unsigned char *)((g.project->name && *g.project->name) ? g.project->name : "novel");
    while (*p && k < 200) {
        unsigned char c = *p;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') { p++; continue; }
        safe[k++] = (char)c;
        p++;
    }
    safe[k] = '\0';
    if (k == 0) snprintf(safe, sizeof(safe), "novel");

    snprintf(doc_path, sizeof(doc_path), "%s\\%s.doc", dir, safe);
    snprintf(txt_path, sizeof(txt_path), "%s\\%s.txt", dir, safe);

    g.last_doc[0] = '\0';
    g.last_txt[0] = '\0';
    if (export_novel_doc(g.project, doc_path) == 0) {
        snprintf(g.last_doc, sizeof(g.last_doc), "%s", doc_path);
    }
    if (export_novel_txt(g.project, txt_path) == 0) {
        snprintf(g.last_txt, sizeof(g.last_txt), "%s", txt_path);
    }
    return (g.last_doc[0] || g.last_txt[0]) ? 0 : -1;
}

static void open_path(const char *path)
{
    WCHAR *w;
    if (!path || !*path) return;
    w = u8_to_w(path);
    if (!w) return;
    ShellExecuteW(g.hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
}

/* 砍掉结尾处不完整的 UTF-8 序列，返回可安全解码的字节数。
   背景：导入大文件时我们按 max_bytes 硬截断，最后一个字符极可能被切成半个。
   这半个字符本身不合法，会让下面 MB_ERR_INVALID_CHARS 的严格校验**整体**失败，
   于是整篇 UTF-8 中文被误判成 GBK 再转一遍——全文乱码。
   做法：从尾部往回最多看 4 字节，找到首字节就按它的序列长度判断是否完整。 */
static size_t utf8_trim_incomplete(const unsigned char *s, size_t len)
{
    size_t i, need;
    if (!s || len == 0) return 0;
    for (i = 1; i <= 4 && i <= len; i++) {
        unsigned char c = s[len - i];
        if ((c & 0x80) == 0) return len;            /* ASCII，上一字符完整 */
        if ((c & 0xC0) == 0xC0) {                   /* 找到首字节 */
            if ((c & 0xE0) == 0xC0)      need = 2;
            else if ((c & 0xF0) == 0xE0) need = 3;
            else if ((c & 0xF8) == 0xF0) need = 4;
            else return len;                        /* 非法首字节：交给严格校验去否掉 */
            return (i == need) ? len : len - i;
        }
        /* 0x80..0xBF 是续字节，继续往回找首字节 */
    }
    return len;                                     /* 往回 4 字节都没找到首字节：交给校验 */
}

/* 砍掉结尾处孤立的 GBK 首字节。
   ★ 不能只看最后一个字节：GBK 尾字节范围是 0x40-0xFE（含 0x81-0xFE），
   所以末字节落在 0x81-0xFE 时既可能是"半个汉字的首字节"，也可能是
   "完整汉字的尾字节"。必须从头部正向成对扫描才能确定边界，
   否则「中文」这种以 0xC4 结尾的合法文本会被误砍成「中?」。 */
static size_t gbk_trim_incomplete(const unsigned char *s, size_t len)
{
    size_t i = 0;
    if (!s) return 0;
    while (i < len) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }               /* ASCII */
        if (c == 0x80 || c == 0xFF) { i++; continue; } /* 非法单字节，跳过交给解码器 */
        if (i + 1 >= len) return i;                    /* 尾部只剩一个首字节：砍掉 */
        i += 2;
    }
    return i;
}

/* 读取导入的文本：自动识别 UTF-8 / UTF-16LE / GBK，超长按 UTF-8 边界截断 */
static char *read_text_file_smart(const char *path, size_t max_bytes)
{
    FILE *f;
    long size;
    unsigned char *raw;
    char *out = NULL;
    size_t rd;

    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }
    if ((size_t)size > max_bytes) size = (long)max_bytes;
    raw = (unsigned char *)malloc((size_t)size + 4);
    if (!raw) { fclose(f); return NULL; }
    rd = fread(raw, 1, (size_t)size, f);
    fclose(f);
    raw[rd] = 0; raw[rd + 1] = 0; raw[rd + 2] = 0;

    if (rd >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        out = str_dup((char *)raw + 3);
    } else if (rd >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        /* UTF-16LE。★ 这里**不能**用 MultiByteToWideChar(1200, ...)：
           本机实测无论参数怎么给都返回 0 + ERROR_INVALID_PARAMETER(87)，
           整条分支等于失效——导入 UTF-16 的 txt 永远报"读取失败"。
           Windows 上 WCHAR 的原生字节序就是 UTF-16LE，直接按码元拷贝即可；
           (rd-2)/2 天然按 2 字节对齐，被切半个字符时也不会越界。 */
        size_t units = (size_t)((rd - 2) / 2);
        if (units > 0) {
            WCHAR *wbuf = (WCHAR *)malloc((units + 1) * sizeof(WCHAR));
            if (wbuf) {
                memcpy(wbuf, raw + 2, units * sizeof(WCHAR));
                /* 结尾若是落单的高代理项（emoji 被切了一半），一并丢掉 */
                if (units > 0 && wbuf[units - 1] >= 0xD800 && wbuf[units - 1] <= 0xDBFF) units--;
                wbuf[units] = 0;
                out = w_to_u8_alloc(wbuf);
                free(wbuf);
            }
        }
    } else {
        /* 先按 UTF-8 试；只在**去掉结尾半个字符之后**仍然非法时，才退回 GBK。
           顺序很关键：先修尾再校验，否则大文件必然被切出半个字，
           严格校验必失败，于是所有 UTF-8 文本都会被误当 GBK 转成乱码。
           ★ 校验用 ulen 作显式长度，**不能**先 raw[ulen]=0：
             ulen < rd 时那会把 GBK 回退分支要用的字节提前抹掉
             （实测 D6 D0 CE C4 会被毁成 D6 D0 CE 00，解出「中?」）。 */
        size_t ulen = utf8_trim_incomplete(raw, rd);
        if (ulen > 0 &&
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCSTR)raw, (int)ulen, NULL, 0) > 0) {
            raw[ulen] = 0;
            out = str_dup((char *)raw);
        } else {
            size_t glen = gbk_trim_incomplete(raw, rd);
            int n;
            raw[glen] = 0;
            n = MultiByteToWideChar(936, 0, (LPCSTR)raw, (int)glen, NULL, 0);
            if (n > 0) {
                WCHAR *wbuf = (WCHAR *)malloc((size_t)(n + 1) * sizeof(WCHAR));
                if (wbuf) {
                    MultiByteToWideChar(936, 0, (LPCSTR)raw, (int)glen, wbuf, n);
                    wbuf[n] = 0;
                    out = w_to_u8_alloc(wbuf);
                    free(wbuf);
                }
            }
        }
    }
    free(raw);
    return out;
}

/* ============================================================
 * 15. 列出项目目录
 * ============================================================ */

/* 扫描 projects 目录，返回项目摘要数组（调用方持有） */
static cJSON *build_project_list(void)
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

static void reply_project_list(int id, const char *cmd)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "projects", build_project_list());
    reply_ok(id, cmd, d);
}

/* 任务结束后主动推送一份新的项目列表（幂等，前端直接覆盖） */
static void push_project_list(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "projects", build_project_list());
    push_event("projects", d);
}

/* ============================================================
 * 16. 命令处理（全部在 UI 线程）
 * ============================================================ */

/* 需要独占 project 的命令在这些场景下被拒绝 */
static int require_idle_reply(int id, const char *cmd, const char *what)
{
    if (!is_busy()) return 0;
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "后台任务正在进行，无法%s，请先取消或等待完成", what);
        reply_err(id, cmd, msg);
    }
    return 1;
}

/* ---- 未保存改动的确认 ----
   新建 / 加载项目会销毁当前 project，有可能丢掉还没落盘的改动，必须先征询。
   这里**刻意不用 MessageBoxW**：cmd_dispatch 的调用栈来自 WebView2 的
   WebMessageReceived 回调，在那里弹模态会开嵌套消息循环，和 WebView2 自己的
   消息泵互相卡住的风险很高（WM_CLOSE 是窗口消息，没这个顾虑）。
   改为：先回执一个 needConfirm，前端弹自绘弹层，用户选完带 force 重发。
   返回 1 表示「已回执，调用方直接 return」。 */
static int need_discard_confirm(cJSON *root)
{
    if (!g.dirty || !g.project) return 0;
    return !p_bool(root, "force", 0);
}

static void reply_need_discard(int id, const char *cmd)
{
    cJSON *d = cJSON_CreateObject();
    add_str(d, "needConfirm", "discard");
    add_str(d, "cmd", cmd ? cmd : "");
    add_str(d, "projectName", (g.project && g.project->name) ? g.project->name : "");
    reply_ok(id, cmd, d);
}

static void cmd_hello(int id)
{
    cJSON *d = cJSON_CreateObject();
    char ver[64];
    w_to_u8(NW_VERSION, ver, sizeof(ver));
    add_str(d, "version", ver);
    cJSON_AddItemToObject(d, "config", config_json());
    cJSON_AddItemToObject(d, "project", project_meta_json(g.project));
    cJSON_AddBoolToObject(d, "busy", is_busy() ? 1 : 0);
    add_str(d, "exportDir", g.export_dir);
    add_str(d, "lastDoc", g.last_doc);
    add_str(d, "lastTxt", g.last_txt);
    log_line("配置下发：服务商预设 %d 组，当前模型 %s",
             config_presets_count(),
             (g.config && g.config->ai && g.config->ai->model) ? g.config->ai->model : "?");
    reply_ok(id, "hello", d);
    push_project_list();   /* 附带项目列表 */
}

/* --- 生成类命令的公共准入检查 --- */
static int check_api_key(int id, const char *cmd)
{
    if (g.config && g.config->ai && g.config->ai->api_key && *g.config->ai->api_key) return 0;
    reply_err(id, cmd, "请先在「设置」页配置 API Key");
    return 1;
}

static int check_project(int id, const char *cmd)
{
    if (g.project) return 0;
    reply_err(id, cmd, "还没有项目，请先在「项目」页新建");
    return 1;
}

static void cmd_dispatch(cJSON *root)
{
    const char *cmd = j_str(root, "cmd", NULL);
    int id = j_int(root, "id", 0);
    cJSON *accepted;

    if (!cmd) return;

    /* ---- 任务进行中也允许的命令 ---- */
    if (strcmp(cmd, "hello") == 0) { cmd_hello(id); return; }
    if (strcmp(cmd, "get_config") == 0) { reply_ok(id, cmd, config_json()); return; }
    if (strcmp(cmd, "list_projects") == 0) { reply_project_list(id, cmd); return; }
    if (strcmp(cmd, "get_project") == 0) { reply_ok(id, cmd, project_meta_json(g.project)); return; }
    if (strcmp(cmd, "ping") == 0) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "tick", (double)GetTickCount());
        reply_ok(id, cmd, d);
        /* 前端在 hello 回执之后会补一次 ping：这条日志出现即证明
           JS -> C -> JS 双向通道都通（排查白屏时先看这里）。 */
        log_line("闭环确认：JS -> C -> JS 双向通畅");
        return;
    }
    /* ---- UI 自检通道 ----
       前端把诊断信息写进 webui.log。WebView2 的 DevTools 控制台在本环境下
       拿不到，这是排查白屏 / JS 异常唯一可观测的路径。 */
    if (strcmp(cmd, "log") == 0) {
        const char *m = p_str(root, "msg", "");
        log_line("[UI] %s", m ? m : "(空)");
        return;
    }
    if (strcmp(cmd, "answer") == 0) {
        int aid = p_int(root, "askId", -1);
        const char *ans = p_str(root, "answer", "cancel");
        ask_t *it;
        int v = 3;
        if (ans && strcmp(ans, "yes") == 0) v = 1;
        else if (ans && strcmp(ans, "no") == 0) v = 2;
        if (g_askCsReady) {
            EnterCriticalSection(&g_askCs);
            for (it = g_asks; it; it = it->next) {
                if (it->id == aid) { it->answer = v; SetEvent(it->done); break; }
            }
            LeaveCriticalSection(&g_askCs);
        }
        reply_ok(id, cmd, NULL);
        return;
    }
    if (strcmp(cmd, "cancel") == 0) {
        if (g.engine) engine_request_cancel(g.engine);
        /* 工作线程可能正卡在 bridge_ask 的确认框上，这个状态下引擎的取消标志
           不会被看到（检查点在章节之间，线程还没走到）。这里直接把待决询问
           按「取消」唤醒，保证「取消任务」在确认框挡着时同样生效。 */
        ask_release_all();
        push_status("正在取消任务，将在当前步骤结束后停止...");
        reply_ok(id, cmd, NULL);
        return;
    }
    if (strcmp(cmd, "get_chapter") == 0) {
        int idx = p_int(root, "index", -1);
        if (!g.project || idx < 0 || idx >= g.project->chapter_count) {
            reply_err(id, cmd, "章节不存在");
            return;
        }
        {
            chapter_t *c = &g.project->chapters[idx];
            cJSON *d = cJSON_CreateObject();
            cJSON_AddNumberToObject(d, "index", idx);
            add_str(d, "title", c->title);
            add_str(d, "summary", c->summary);
            add_str(d, "content", c->content);
            cJSON_AddNumberToObject(d, "wordCount", c->word_count);
            cJSON_AddNumberToObject(d, "status", c->status);
            reply_ok(id, cmd, d);
        }
        return;
    }
    if (strcmp(cmd, "get_plan") == 0) {
        int idx = p_int(root, "index", -1);
        if (!g.project || idx < 0 || idx >= g.project->plan_count) {
            reply_err(id, cmd, "方案不存在");
            return;
        }
        {
            novel_plan_t *pl = &g.project->plans[idx];
            cJSON *d = cJSON_CreateObject();
            cJSON_AddNumberToObject(d, "index", idx);
            add_str(d, "title", pl->title);
            add_str(d, "synopsis", pl->synopsis);
            add_str(d, "outline", pl->outline);
            reply_ok(id, cmd, d);
        }
        return;
    }
    if (strcmp(cmd, "set_selected_chapter") == 0) {
        int idx = p_int(root, "index", -1);
        g.selected_chapter = idx;
        reply_ok(id, cmd, NULL);
        return;
    }

    /* ---- 以下命令需要独占 project，任务运行中一律拒绝 ---- */

    if (strcmp(cmd, "save_config") == 0) {
        cJSON *p = j_params(root);
        char *cfg_path;
        if (require_idle_reply(id, cmd, "保存设置")) return;
        apply_config_json(p ? p : root);
        cfg_path = config_get_default_path();
        if (config_save(g.config, cfg_path) == 0) {
            restart_autosave();
            reply_ok(id, cmd, config_json());
            push_toast("success", "设置已保存");
        } else {
            reply_err(id, cmd, "配置文件写入失败，请检查磁盘权限");
        }
        free(cfg_path);
        return;
    }
    if (strcmp(cmd, "fetch_models") == 0) {
        cJSON *p = j_params(root);
        if (require_idle_reply(id, cmd, "获取模型列表")) return;
        /* 允许用户未点保存就取列表：先把当前输入并进 config */
        if (p) apply_config_json(p);
        if (check_api_key(id, cmd)) return;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        push_status("正在获取模型列表...");
        push_busy();
        if (!launch_thread(th_fetch_models, NULL)) {
            end_task();
            reply_err(id, cmd, "无法启动线程");
            return;
        }
        accepted = cJSON_CreateObject();
        cJSON_AddBoolToObject(accepted, "accepted", 1);
        reply_ok(id, cmd, accepted);
        return;
    }
    if (strcmp(cmd, "pick_folder") == 0) {
        char *picked = pick_folder_dialog(p_str(root, "initial", NULL));
        if (!picked) { reply_err(id, cmd, "未选择文件夹"); return; }
        snprintf(g.export_dir, sizeof(g.export_dir), "%s", picked);
        if (g.config) {
            free(g.config->export_dir);
            g.config->export_dir = str_dup(picked);
        }
        {
            cJSON *d = cJSON_CreateObject();
            add_str(d, "path", picked);
            reply_ok(id, cmd, d);
        }
        free(picked);
        return;
    }
    if (strcmp(cmd, "pick_project_file") == 0) {
        char *picked = pick_project_file_dialog(
            (g.config && g.config->projects_dir) ? g.config->projects_dir : NULL);
        if (!picked) { reply_err(id, cmd, "未选择文件"); return; }
        {
            cJSON *d = cJSON_CreateObject();
            add_str(d, "path", picked);
            reply_ok(id, cmd, d);
        }
        free(picked);
        return;
    }
    if (strcmp(cmd, "pick_text_file") == 0) {
        char *picked = pick_text_file_dialog();
        if (!picked) { reply_err(id, cmd, "未选择文件"); return; }
        {
            cJSON *d = cJSON_CreateObject();
            add_str(d, "path", picked);
            reply_ok(id, cmd, d);
        }
        free(picked);
        return;
    }
    if (strcmp(cmd, "open_path") == 0) {
        open_path(p_str(root, "path", NULL));
        reply_ok(id, cmd, NULL);
        return;
    }
    if (strcmp(cmd, "reveal_path") == 0) {
        const char *path = p_str(root, "path", NULL);
        if (path && *path) {
            WCHAR *w = u8_to_w(path);
            if (w) {
                WCHAR args[MAX_PATH + 16];
                swprintf(args, MAX_PATH + 16, L"/select,\"%s\"", w);
                ShellExecuteW(g.hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
                free(w);
            }
        }
        reply_ok(id, cmd, NULL);
        return;
    }

    /* --- 项目 --- */
    if (strcmp(cmd, "new_project") == 0) {
        const char *name = p_str(root, "name", "");
        const char *idea = p_str(root, "idea", "");
        if (require_idle_reply(id, cmd, "新建项目")) return;
        if (!name || !*name) { reply_err(id, cmd, "请输入项目名称"); return; }
        /* 这一步会销毁当前项目，先把未保存的改动问清楚 */
        if (need_discard_confirm(root)) { reply_need_discard(id, cmd); return; }
        if (g.project) { project_destroy(g.project); g.project = NULL; }
        g.project = project_create(name, idea);
        if (!g.project) { reply_err(id, cmd, "内存不足，无法创建项目"); return; }
        g.selected_chapter = -1;
        g.dirty = 1;
        g.last_doc[0] = g.last_txt[0] = '\0';
        reply_ok(id, cmd, project_meta_json(g.project));
        push_project();
        push_status("新项目已创建");
        return;
    }
    if (strcmp(cmd, "load_project") == 0) {
        const char *pid = p_str(root, "id", NULL);
        const char *ppath = p_str(root, "path", NULL);
        char full[4096];
        char *dup;
        if (require_idle_reply(id, cmd, "加载项目")) return;
        if (ppath && *ppath) {
            dup = str_dup(ppath);
        } else if (pid && *pid && g.config && g.config->projects_dir) {
            snprintf(full, sizeof(full), "%s\\%s\\project.json", g.config->projects_dir, pid);
            dup = str_dup(full);
        } else {
            reply_err(id, cmd, "缺少项目标识");
            return;
        }
        if (!file_exists(dup)) {
            free(dup);
            reply_err(id, cmd, "项目文件不存在");
            return;
        }
        /* 加载完成后会替换掉当前项目（在 handle_task_done 里销毁旧的），
           所以要在起线程之前先确认未保存的改动 */
        if (need_discard_confirm(root)) { free(dup); reply_need_discard(id, cmd); return; }
        if (!try_begin_task()) { free(dup); reply_err(id, cmd, "有任务正在进行"); return; }
        push_status("正在加载项目...");
        push_busy();
        if (!launch_thread(th_load_project, dup)) {
            free(dup);
            end_task();
            reply_err(id, cmd, "无法启动加载线程");
            return;
        }
        accepted = cJSON_CreateObject();
        cJSON_AddBoolToObject(accepted, "accepted", 1);
        reply_ok(id, cmd, accepted);
        return;
    }
    if (strcmp(cmd, "save_project") == 0) {
        const char *name = p_str(root, "name", NULL);
        if (require_idle_reply(id, cmd, "保存项目")) return;
        if (!g.project) { reply_err(id, cmd, "没有项目可保存"); return; }
        if (name) { free(g.project->name); g.project->name = str_dup(name); }
        if (project_save(g.project, g.config->projects_dir) == 0) {
            g.dirty = 0;
            reply_ok(id, cmd, project_meta_json(g.project));
            push_toast("success", "项目已保存");
            push_status("项目保存成功");
        } else {
            reply_err(id, cmd, "项目保存失败，请检查 projects 目录是否可写");
        }
        return;
    }
    if (strcmp(cmd, "set_project_name") == 0) {
        const char *name = p_str(root, "name", "");
        if (require_idle_reply(id, cmd, "修改项目名称")) return;
        if (!g.project) { reply_err(id, cmd, "没有项目"); return; }
        free(g.project->name);
        g.project->name = str_dup(name ? name : "");
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        return;
    }
    if (strcmp(cmd, "set_project_idea") == 0) {
        const char *idea = p_str(root, "idea", "");
        if (require_idle_reply(id, cmd, "修改创意")) return;
        if (!g.project) { reply_err(id, cmd, "没有项目"); return; }
        free(g.project->seed_idea);
        g.project->seed_idea = str_dup(idea ? idea : "");
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        return;
    }

    /* --- 编辑章节 / 方案 / 世界观 / 人物 --- */
    if (strcmp(cmd, "set_chapter") == 0) {
        int idx = p_int(root, "index", -1);
        if (require_idle_reply(id, cmd, "编辑章节")) return;
        if (!g.project || idx < 0 || idx >= g.project->chapter_count) {
            reply_err(id, cmd, "章节不存在");
            return;
        }
        {
            chapter_t *c = &g.project->chapters[idx];
            const char *t = j_str(j_params(root), "title", NULL);
            const char *s = j_str(j_params(root), "summary", NULL);
            const char *ct = j_str(j_params(root), "content", NULL);
            if (t) { free(c->title);  c->title  = str_dup(t); }
            if (s) { free(c->summary); c->summary = str_dup(s); }
            if (ct != NULL) {
                free(c->content);
                c->content = str_dup(ct);
                c->word_count = (int)utf8_char_count(ct);
            }
            project_calc_total_words(g.project);
            g.dirty = 1;
            reply_ok(id, cmd, NULL);
            push_project();
        }
        return;
    }
    if (strcmp(cmd, "set_plan") == 0) {
        int idx = p_int(root, "index", -1);
        if (require_idle_reply(id, cmd, "编辑方案")) return;
        if (!g.project || idx < 0 || idx >= g.project->plan_count) {
            reply_err(id, cmd, "方案不存在");
            return;
        }
        {
            novel_plan_t *pl = &g.project->plans[idx];
            const char *t = j_str(j_params(root), "title", NULL);
            const char *s = j_str(j_params(root), "synopsis", NULL);
            const char *o = j_str(j_params(root), "outline", NULL);
            if (t) { free(pl->title);    pl->title = str_dup(t); }
            if (s) { free(pl->synopsis); pl->synopsis = str_dup(s); }
            if (o) { free(pl->outline);  pl->outline = str_dup(o); }
            g.dirty = 1;
            reply_ok(id, cmd, NULL);
            push_project();
        }
        return;
    }
    if (strcmp(cmd, "set_world") == 0) {
        cJSON *p = j_params(root);
        if (require_idle_reply(id, cmd, "编辑世界观")) return;
        if (!g.project) { reply_err(id, cmd, "没有项目"); return; }
        {
            const char *wt = j_str(p, "worldType", NULL);
            const char *se = j_str(p, "setting", NULL);
            const char *ps = j_str(p, "powerSystem", NULL);
            const char *ge = j_str(p, "geography", NULL);
            const char *hi = j_str(p, "history", NULL);
            project_set_world(g.project, wt, se, ps);
            /* geography / history 业务层没有 setter，这里直接写公开字段 */
            if (ge) { free(g.project->world.geography); g.project->world.geography = str_dup(ge); }
            if (hi) { free(g.project->world.history);   g.project->world.history   = str_dup(hi); }
            g.dirty = 1;
            reply_ok(id, cmd, NULL);
            push_project();
        }
        return;
    }
    if (strcmp(cmd, "set_characters") == 0) {
        cJSON *p = j_params(root);
        cJSON *arr = p ? cJSON_GetObjectItemCaseSensitive(p, "characters") : NULL;
        if (require_idle_reply(id, cmd, "编辑人物")) return;
        if (!g.project) { reply_err(id, cmd, "没有项目"); return; }
        if (!arr || !cJSON_IsArray(arr)) { reply_err(id, cmd, "人物数据格式错误"); return; }
        /* 整体替换：复用业务层的 clear + add，避免自行操作容量字段 */
        project_clear_characters(g.project);
        {
            int n = cJSON_GetArraySize(arr), i;
            for (i = 0; i < n; i++) {
                cJSON *c = cJSON_GetArrayItem(arr, i);
                if (!c) continue;
                project_add_character(g.project,
                    j_str(c, "name", ""), j_str(c, "role", ""),
                    j_str(c, "description", ""), j_str(c, "traits", ""),
                    j_str(c, "background", ""));
            }
        }
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        push_project();
        return;
    }
    if (strcmp(cmd, "select_direction") == 0) {
        int idx = p_int(root, "index", -1);
        if (require_idle_reply(id, cmd, "选择方向")) return;
        if (!g.project || idx < 0 || idx >= g.project->direction_count) {
            reply_err(id, cmd, "方向不存在");
            return;
        }
        project_select_direction(g.project, idx);
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        push_project();
        push_status("已选定方向，可继续生成方案");
        return;
    }
    if (strcmp(cmd, "select_plan") == 0) {
        int idx = p_int(root, "index", -1);
        if (require_idle_reply(id, cmd, "选择方案")) return;
        if (!g.project || idx < 0 || idx >= g.project->plan_count) {
            reply_err(id, cmd, "方案不存在");
            return;
        }
        project_select_plan(g.project, idx);
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        push_project();
        push_status("已选定方案，可继续细化大纲");
        return;
    }
    if (strcmp(cmd, "export") == 0) {
        const char *dir = p_str(root, "dir", NULL);
        if (require_idle_reply(id, cmd, "导出")) return;
        if (!g.project || g.project->chapter_count == 0) {
            reply_err(id, cmd, "没有可导出的章节，请先细化大纲或写入章节内容");
            return;
        }
        if (do_export(dir) == 0) {
            cJSON *d = cJSON_CreateObject();
            add_str(d, "doc", g.last_doc);
            add_str(d, "txt", g.last_txt);
            reply_ok(id, cmd, d);
            push_toast("success", "已导出 DOC / TXT");
            push_status("已导出 DOC/TXT");
        } else {
            reply_err(id, cmd, "导出失败，请检查输出目录是否可写");
        }
        return;
    }

    /* --- 生成类（异步） --- */
#define ACCEPT_ASYNC                                                                \
    do {                                                                            \
        accepted = cJSON_CreateObject();                                            \
        cJSON_AddBoolToObject(accepted, "accepted", 1);                             \
        reply_ok(id, cmd, accepted);                                                \
        push_busy();                                                                \
    } while (0)

    if (strcmp(cmd, "gen_directions") == 0) {
        gen_dir_task_t *t;
        int iters = p_int(root, "iterations", 2);
        int count = p_int(root, "count", 5);
        if (require_idle_reply(id, cmd, "生成创作方向")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (iters < 1) iters = 1;
        if (iters > 6) iters = 6;
        if (count < 1) count = 5;
        if (count > 20) count = 20;
        /* 生成前把最新创意写回项目 */
        {
            const char *idea = p_str(root, "idea", NULL);
            if (idea) { free(g.project->seed_idea); g.project->seed_idea = str_dup(idea); }
        }
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        t = (gen_dir_task_t *)malloc(sizeof(gen_dir_task_t));
        if (!t) { end_task(); reply_err(id, cmd, "内存不足"); return; }
        t->iterations = iters;
        t->count = count;
        engine_clear_cancel(g.engine);
        push_status("正在生成创作方向...");
        if (!launch_thread(th_gen_directions, t)) {
            free(t); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "gen_plans") == 0) {
        int count = p_int(root, "count", 3);
        if (require_idle_reply(id, cmd, "生成方案")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!g.project->selected_direction) { reply_err(id, cmd, "请先选定一个创作方向"); return; }
        if (count < 1) count = 3;
        if (count > 10) count = 10;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在生成小说方案...");
        if (!launch_thread(th_gen_plans, (LPVOID)(INT_PTR)count)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "expand_outline") == 0) {
        int count = p_int(root, "count", 20);
        if (require_idle_reply(id, cmd, "细化大纲")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!g.project->selected_plan) { reply_err(id, cmd, "请先选定一个小说方案"); return; }
        if (count < 3) count = 20;
        if (count > 50) count = 50;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在细化大纲...");
        if (!launch_thread(th_expand_outline, (LPVOID)(INT_PTR)count)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "expand_chapter") == 0) {
        ch_task_t *t;
        int idx = p_int(root, "index", -1);
        int words = p_int(root, "words", 2000);
        if (require_idle_reply(id, cmd, "扩写章节")) return;
        if (check_api_key(id, cmd)) return;
        if (!g.project || idx < 0 || idx >= g.project->chapter_count) {
            reply_err(id, cmd, "请先选择一个章节");
            return;
        }
        if (words < 200) words = 2000;
        if (words > 20000) words = 20000;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        t = (ch_task_t *)malloc(sizeof(ch_task_t));
        if (!t) { end_task(); reply_err(id, cmd, "内存不足"); return; }
        t->index = idx; t->words = words;
        engine_clear_cancel(g.engine);
        push_status("正在扩写本章...");
        if (!launch_thread(th_expand_chapter, t)) {
            free(t); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "expand_all") == 0) {
        int words = p_int(root, "words", 2000);
        if (require_idle_reply(id, cmd, "批量扩写")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (g.project->chapter_count == 0) { reply_err(id, cmd, "没有章节可扩写"); return; }
        if (words < 200) words = 2000;
        if (words > 20000) words = 20000;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在批量扩写...");
        if (!launch_thread(th_expand_all, (LPVOID)(INT_PTR)words)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "step_write") == 0) {
        ch_task_t *t;
        int words = p_int(root, "words", 2000);
        int start = -1, i;
        if (require_idle_reply(id, cmd, "逐章生成")) return;
        if (check_api_key(id, cmd)) return;
        if (!g.project || g.project->chapter_count == 0) {
            reply_err(id, cmd, "没有章节可生成，请先在「大纲」页细化大纲");
            return;
        }
        for (i = 0; i < g.project->chapter_count; i++) {
            chapter_t *c = &g.project->chapters[i];
            if (!c->content || !*c->content) { start = i; break; }
        }
        if (start < 0) { reply_err(id, cmd, "所有章节均已写完；如需重写某一章，请选中该章后点「扩写本章」"); return; }
        if (words < 200) words = 2000;
        if (words > 20000) words = 20000;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        t = (ch_task_t *)malloc(sizeof(ch_task_t));
        if (!t) { end_task(); reply_err(id, cmd, "内存不足"); return; }
        t->index = start; t->words = words;
        engine_clear_cancel(g.engine);
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "逐章生成：正在生成第 %d 章...", start + 1);
            push_status(msg);
        }
        if (!launch_thread(th_step_write, t)) {
            free(t); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "check_consistency") == 0) {
        int idx = p_int(root, "index", -1);
        if (require_idle_reply(id, cmd, "一致性检查")) return;
        if (check_api_key(id, cmd)) return;
        if (!g.project || idx < 0 || idx >= g.project->chapter_count) {
            reply_err(id, cmd, "请先选择一个章节");
            return;
        }
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在进行一致性检查...");
        if (!launch_thread(th_check_consistency, (LPVOID)(INT_PTR)idx)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "summarize_worldview") == 0) {
        if (require_idle_reply(id, cmd, "总结世界观")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在总结世界观...");
        if (!launch_thread(th_sum_world, NULL)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "generate_characters") == 0) {
        if (require_idle_reply(id, cmd, "生成人物")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在根据小说内容生成人物草稿...");
        if (!launch_thread(th_gen_chars, NULL)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "check_finale") == 0) {
        if (require_idle_reply(id, cmd, "伏笔回收校验")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在做伏笔回收校验...");
        if (!launch_thread(th_check_finale, NULL)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "analyze_text") == 0) {
        const char *text = p_str(root, "text", NULL);
        char *dup = str_dup(text ? text : "");
        if (require_idle_reply(id, cmd, "分析文本")) { free(dup); return; }
        if (check_api_key(id, cmd)) { free(dup); return; }
        if (check_project(id, cmd)) { free(dup); return; }
        if (!dup) { reply_err(id, cmd, "内存不足"); return; }
        if (!try_begin_task()) { free(dup); reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在分析文本...");
        if (!launch_thread(th_analyze_text, dup)) {
            free(dup); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "auto_write") == 0) {
        auto_task_t *t;
        int dirs = p_int(root, "dirCount", 5);
        int plans = p_int(root, "planCount", 3);
        int chs = p_int(root, "chapters", 20);
        int words = p_int(root, "words", 3000);
        int start_ch = p_int(root, "startChapter", 0);
        if (require_idle_reply(id, cmd, "全自动写作")) return;
        if (check_api_key(id, cmd)) return;
        if (dirs < 1) dirs = 5;
        if (dirs > 20) dirs = 20;
        if (plans < 1) plans = 3;
        if (plans > 10) plans = 10;
        if (chs < 3) chs = 20;
        if (chs > 50) chs = 50;
        if (words < 200) words = 3000;
        if (words > 20000) words = 20000;
        if (start_ch < 0) start_ch = 0;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        t = (auto_task_t *)malloc(sizeof(auto_task_t));
        if (!t) { end_task(); reply_err(id, cmd, "内存不足"); return; }
        t->dirs = dirs; t->plans = plans; t->chapters = chs; t->words = words; t->start_ch = start_ch;
        engine_clear_cancel(g.engine);
        push_status("开始全自动写作...");
        if (!launch_thread(th_auto_write, t)) {
            free(t); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
#undef ACCEPT_ASYNC

    if (strcmp(cmd, "import_txt") == 0) {
        const char *path = p_str(root, "path", NULL);
        char *text;
        if (require_idle_reply(id, cmd, "导入文本")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!path || !*path) { reply_err(id, cmd, "缺少文件路径"); return; }
        text = read_text_file_smart(path, 4 * 1024 * 1024);
        if (!text || !*text) {
            free(text);
            reply_err(id, cmd, "文件读取失败或内容为空（仅支持 txt，最大 4MB）");
            return;
        }
        if (!try_begin_task()) { free(text); reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在分析导入的文本...");
        if (!launch_thread(th_analyze_text, text)) {
            free(text); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        {
            cJSON *a = cJSON_CreateObject();
            cJSON_AddBoolToObject(a, "accepted", 1);
            reply_ok(id, cmd, a);
            push_busy();
        }
        return;
    }
    if (strcmp(cmd, "shutdown") == 0) {
        reply_ok(id, cmd, NULL);
        PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
        return;
    }

    reply_err(id, cmd, "未知命令");
}

/* ============================================================
 * 17. WebView2 COM 回调（手写 vtable，保持纯 C）
 * ============================================================ */

#define DEFINE_CB(NAME, IFACE)                                                  \
    typedef struct NAME { IFACE iface; LONG ref; } NAME;                        \
    static HRESULT STDMETHODCALLTYPE NAME##_QI(IFACE *This, REFIID riid, void **ppv) \
    {                                                                           \
        if (!ppv) return E_POINTER;                                             \
        if (IsEqualIID(riid, &IID_IUnknown) ||                                  \
            IsEqualIID(riid, &IID_##IFACE)) {                                   \
            *ppv = This; This->lpVtbl->AddRef(This); return S_OK;               \
        }                                                                       \
        *ppv = NULL; return E_NOINTERFACE;                                      \
    }                                                                           \
    static ULONG STDMETHODCALLTYPE NAME##_AddRef(IFACE *This)                   \
    { NAME *s = (NAME *)This; return (ULONG)InterlockedIncrement(&s->ref); }    \
    static ULONG STDMETHODCALLTYPE NAME##_Release(IFACE *This)                  \
    {                                                                           \
        NAME *s = (NAME *)This;                                                 \
        LONG r = InterlockedDecrement(&s->ref);                                 \
        if (r == 0) free(s);                                                    \
        return (ULONG)r;                                                        \
    }

/* ---- 环境创建完成 ---- */
static HRESULT STDMETHODCALLTYPE CtrlCB_QI(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, REFIID riid, void **ppv);
static ULONG   STDMETHODCALLTYPE CtrlCB_AddRef(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This);
static ULONG   STDMETHODCALLTYPE CtrlCB_Release(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This);
static HRESULT STDMETHODCALLTYPE CtrlCB_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT errorCode, ICoreWebView2Controller *controller);

typedef struct CtrlCB { ICoreWebView2CreateCoreWebView2ControllerCompletedHandler iface; LONG ref; } CtrlCB;

static HRESULT STDMETHODCALLTYPE CtrlCB_QI(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE CtrlCB_AddRef(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This)
{
    CtrlCB *s = (CtrlCB *)This;
    return (ULONG)InterlockedIncrement(&s->ref);
}

static ULONG STDMETHODCALLTYPE CtrlCB_Release(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This)
{
    CtrlCB *s = (CtrlCB *)This;
    LONG r = InterlockedDecrement(&s->ref);
    if (r == 0) free(s);
    return (ULONG)r;
}

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_CtrlCBVtbl = {
    CtrlCB_QI, CtrlCB_AddRef, CtrlCB_Release, CtrlCB_Invoke
};

DEFINE_CB(EnvCB, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)

static HRESULT STDMETHODCALLTYPE EnvCB_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Environment *env)
{
    CtrlCB *cb;
    (void)This;
    if (FAILED(errorCode) || !env) {
        log_line("!! 环境创建失败 hr=0x%08lX（WebView2 Runtime 可能未安装）", (unsigned long)errorCode);
        if (g_hwnd) {
            MessageBoxW(g_hwnd,
                L"WebView2 Runtime 初始化失败。\n\n"
                L"本程序需要 Microsoft Edge WebView2 Runtime（Win11 自带）。\n"
                L"请访问 https://developer.microsoft.com/microsoft-edge/webview2/ 安装后重试。",
                L"novelnow", MB_OK | MB_ICONERROR);
        }
        return S_OK;
    }
    log_line("OK  环境创建成功");

    cb = (CtrlCB *)calloc(1, sizeof(CtrlCB));
    if (!cb) return S_OK;
    cb->iface.lpVtbl = &g_CtrlCBVtbl;
    cb->ref = 1;
    if (FAILED(env->lpVtbl->CreateCoreWebView2Controller(
            env, g_hwnd, (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)cb)))
        log_line("!! CreateCoreWebView2Controller 调用失败");
    return S_OK;
}

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_EnvCBVtbl = {
    EnvCB_QI, EnvCB_AddRef, EnvCB_Release, EnvCB_Invoke
};

/* ---- JS -> C 消息 ---- */
DEFINE_CB(MsgCB, ICoreWebView2WebMessageReceivedEventHandler)

static HRESULT STDMETHODCALLTYPE MsgCB_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args)
{
    LPWSTR raw = NULL;
    char *utf8;
    cJSON *root;
    (void)This; (void)sender;

    if (!args) return S_OK;
    if (FAILED(args->lpVtbl->TryGetWebMessageAsString(args, &raw)) || !raw) return S_OK;

    utf8 = w_to_u8_alloc(raw);
    CoTaskMemFree(raw);
    if (!utf8) return S_OK;

    /* 消息可能很大（整章正文），栈上放不下，全部堆分配 */
    root = cJSON_Parse(utf8);
    if (root) {
        const char *c = j_str(root, "cmd", "?");
        log_line("<== JS cmd=%s (%d 字节)", c, (int)strlen(utf8));
        /* 首条消息证明「页面已加载 + 桥接已通」，比 NavigationCompleted 可靠：
           NavigateToString 的文档 URL 恒为 about:blank，导航完成事件也可能不来。 */
        if (!g.webview_ready) {
            g.webview_ready = 1;
            log_line("页面就绪（收到首条 JS 消息）");
            /* 可见性自愈不在这里启动——它已提前到 controller 创建时无条件运行，
               否则握手回执会撞在 IsVisible=FALSE 的窗口里被丢掉。 */
        }
        cmd_dispatch(root);
        cJSON_Delete(root);
    } else {
        log_line("!! 无法解析 JS 消息（前 200 字节）: %.200s", utf8);
    }
    free(utf8);
    return S_OK;
}

static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_MsgCBVtbl = {
    MsgCB_QI, MsgCB_AddRef, MsgCB_Release, MsgCB_Invoke
};

/* ---- 截图（供自动化验收，--shot 使用） ---- */
DEFINE_CB(CdpCB, ICoreWebView2CallDevToolsProtocolMethodCompletedHandler)

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *b64_decode(const char *in, int *outLen)
{
    int len = (int)strlen(in), i, o = 0, q[4], qn = 0;
    unsigned char *out = (unsigned char *)malloc((size_t)(len / 4 * 3 + 8));
    if (!out) return NULL;
    for (i = 0; i < len; i++) {
        int v;
        if (in[i] == '=' || in[i] == '"') break;
        v = b64_val(in[i]);
        if (v < 0) continue;
        q[qn++] = v;
        if (qn == 4) {
            out[o++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
            out[o++] = (unsigned char)(((q[1] & 15) << 4) | (q[2] >> 2));
            out[o++] = (unsigned char)(((q[2] & 3) << 6) | q[3]);
            qn = 0;
        }
    }
    if (qn == 2) out[o++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
    else if (qn == 3) {
        out[o++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
        out[o++] = (unsigned char)(((q[1] & 15) << 4) | (q[2] >> 2));
    }
    *outLen = o;
    return out;
}

static HRESULT STDMETHODCALLTYPE CdpCB_Invoke(
    ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *This,
    HRESULT errorCode, LPCWSTR jsonResult)
{
    char *utf8;
    const char *p, *q;
    int n;
    unsigned char *png;
    int pngLen = 0;
    char path[2048];
    FILE *f;
    (void)This;

    if (FAILED(errorCode) || !jsonResult) {
        log_line("!! CDP 截图失败 hr=0x%08lX", (unsigned long)errorCode);
        if (g_shotMode) SetTimer(g_hwnd, 99, 800, NULL);
        return S_OK;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, jsonResult, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return S_OK;
    utf8 = (char *)malloc((size_t)n);
    if (!utf8) return S_OK;
    WideCharToMultiByte(CP_UTF8, 0, jsonResult, -1, utf8, n, NULL, NULL);

    p = strstr(utf8, "\"data\":\"");
    if (!p) { free(utf8); return S_OK; }
    p += 8;
    q = strchr(p, '"');
    if (!q) { free(utf8); return S_OK; }
    {
        char *b64 = (char *)malloc((size_t)(q - p) + 1);
        if (!b64) { free(utf8); return S_OK; }
        memcpy(b64, p, (size_t)(q - p));
        b64[q - p] = '\0';
        png = b64_decode(b64, &pngLen);
        free(b64);
    }
    free(utf8);

    if (png && pngLen > 0) {
        snprintf(path, sizeof(path), "%ls\\screenshot.png", g_exeDir);
        f = fopen(path, "wb");
        if (f) {
            fwrite(png, 1, (size_t)pngLen, f);
            fclose(f);
            log_line("OK  已截图 screenshot.png (%d 字节)", pngLen);
        }
    }
    free(png);
    if (g_shotMode) SetTimer(g_hwnd, 99, 900, NULL);
    return S_OK;
}

static ICoreWebView2CallDevToolsProtocolMethodCompletedHandlerVtbl g_CdpCBVtbl = {
    CdpCB_QI, CdpCB_AddRef, CdpCB_Release, CdpCB_Invoke
};

/* 把 PNG 字节落盘到 exe 同目录 */
static void write_png_bytes(const void *data, size_t len)
{
    char path[2048];
    FILE *f;
    if (!data || len == 0) { log_line("!! 截图数据为空"); return; }
    snprintf(path, sizeof(path), "%ls\\screenshot.png", g_exeDir);
    f = fopen(path, "wb");
    if (!f) { log_line("!! 无法写入 screenshot.png"); return; }
    fwrite(data, 1, len, f);
    fclose(f);
    log_line("OK  已截图 screenshot.png (%d 字节)", (int)len);
}

/* 备用路径：CDP Page.captureScreenshot（需要 Runtime 正常派发 CDP 回调） */
static void request_screenshot_cdp(void)
{
    CdpCB *cb;
    HRESULT hr;
    log_line("请求截图…（CDP 备用路径）");
    if (!g_wv) { log_line("!! g_wv 为空，无法截图"); return; }
    cb = (CdpCB *)calloc(1, sizeof(CdpCB));
    if (!cb) return;
    cb->iface.lpVtbl = &g_CdpCBVtbl;
    cb->ref = 1000000;   /* 诊断：排除 handler 被提前释放导致回调不来 */
    hr = g_wv->lpVtbl->CallDevToolsProtocolMethod(
        g_wv, L"Page.captureScreenshot", L"{\"format\":\"png\"}",
        (ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *)cb);
    log_line("CallDevToolsProtocolMethod hr=0x%08lX", (unsigned long)hr);
}

/* ---- 主路径：ICoreWebView2::CapturePreview ---- */
DEFINE_CB(CapCB, ICoreWebView2CapturePreviewCompletedHandler)

/* 诊断用：验证 completion handler 机制本身是否可用 */
DEFINE_CB(EsCB, ICoreWebView2ExecuteScriptCompletedHandler)

static HRESULT STDMETHODCALLTYPE EsCB_Invoke(
    ICoreWebView2ExecuteScriptCompletedHandler *This,
    HRESULT errorCode, LPCWSTR resultJson)
{
    (void)This;
    log_line("ExecuteScript 回调 hr=0x%08lX 结果=%ls",
             (unsigned long)errorCode, resultJson ? resultJson : L"(null)");
    return S_OK;
}

static ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_EsCBVtbl = {
    EsCB_QI, EsCB_AddRef, EsCB_Release, EsCB_Invoke
};

static void test_execute_script(void)
{
    EsCB *cb;
    HRESULT hr;
    if (!g_wv) return;
    cb = (EsCB *)calloc(1, sizeof(EsCB));
    if (!cb) return;
    cb->iface.lpVtbl = &g_EsCBVtbl;
    cb->ref = 1000000;
    hr = g_wv->lpVtbl->ExecuteScript(
        g_wv, L"1+1", (ICoreWebView2ExecuteScriptCompletedHandler *)cb);
    log_line("ExecuteScript hr=0x%08lX", (unsigned long)hr);
}

static HRESULT STDMETHODCALLTYPE CapCB_Invoke(
    ICoreWebView2CapturePreviewCompletedHandler *This, HRESULT errorCode)
{
    STATSTG st;
    LARGE_INTEGER zero;
    unsigned char *buf;
    ULONG rd = 0;
    size_t n;
    (void)This;

    log_line("CapCB_Invoke 被调用 hr=0x%08lX", (unsigned long)errorCode);

    if (FAILED(errorCode) || !g_shotStream) {
        log_line("!! CapturePreview 失败 hr=0x%08lX", (unsigned long)errorCode);
        request_screenshot_cdp();
        return S_OK;
    }
    memset(&st, 0, sizeof(st));
    if (FAILED(g_shotStream->lpVtbl->Stat(g_shotStream, &st, STATFLAG_NONAME))) {
        log_line("!! 无法读取截图流大小");
        return S_OK;
    }
    n = (size_t)st.cbSize.QuadPart;
    if (n == 0) { log_line("!! 截图流为空"); return S_OK; }

    zero.QuadPart = 0;
    g_shotStream->lpVtbl->Seek(g_shotStream, zero, STREAM_SEEK_SET, NULL);
    buf = (unsigned char *)malloc(n);
    if (!buf) return S_OK;
    if (SUCCEEDED(g_shotStream->lpVtbl->Read(g_shotStream, buf, (ULONG)n, &rd)) && rd > 0)
        write_png_bytes(buf, (size_t)rd);
    else
        log_line("!! 读取截图流失败");
    free(buf);
    return S_OK;
}

static ICoreWebView2CapturePreviewCompletedHandlerVtbl g_CapCBVtbl = {
    CapCB_QI, CapCB_AddRef, CapCB_Release, CapCB_Invoke
};

static void request_screenshot(void)
{
    CapCB *cb;
    HGLOBAL hg;
    HRESULT hr;

    log_line("请求截图…");
    if (!g_wv) { log_line("!! g_wv 为空，无法截图"); return; }

    /* 隐藏窗口下 WebView2 合成器不产帧，CapturePreview/CDP 都会返回
       ERROR_INVALID_STATE(0x8007139F)。截图前重申一次可见性与尺寸。 */
    if (g_ctrl) {
        RECT rc;
        BOOL vis = FALSE;
        HRESULT hb, hv, hg;
        GetClientRect(g_hwnd, &rc);
        log_line("截图前 bounds=(%ld,%ld,%ld,%ld) 窗口可见=%d 最小化=%d",
                 (long)rc.left, (long)rc.top, (long)rc.right, (long)rc.bottom,
                 IsWindowVisible(g_hwnd) ? 1 : 0, IsIconic(g_hwnd) ? 1 : 0);
        hb = g_ctrl->lpVtbl->put_Bounds(g_ctrl, rc);
        hv = g_ctrl->lpVtbl->put_IsVisible(g_ctrl, TRUE);
        hg = g_ctrl->lpVtbl->get_IsVisible(g_ctrl, &vis);
        log_line("put_Bounds hr=0x%08lX put_IsVisible hr=0x%08lX get_IsVisible hr=0x%08lX vis=%d",
                 (unsigned long)hb, (unsigned long)hv, (unsigned long)hg, vis ? 1 : 0);
    }

    if (g_shotStream) { g_shotStream->lpVtbl->Release(g_shotStream); g_shotStream = NULL; }
    hg = GlobalAlloc(GMEM_MOVEABLE, 0);
    if (!hg) { log_line("!! GlobalAlloc 失败"); return; }
    hr = CreateStreamOnHGlobal(hg, TRUE, &g_shotStream);
    if (FAILED(hr) || !g_shotStream) {
        GlobalFree(hg);
        log_line("!! CreateStreamOnHGlobal hr=0x%08lX", (unsigned long)hr);
        return;
    }

    cb = (CapCB *)calloc(1, sizeof(CapCB));
    if (!cb) return;
    cb->iface.lpVtbl = &g_CapCBVtbl;
    cb->ref = 1000000;   /* 同上，排除生命周期因素 */
    hr = g_wv->lpVtbl->CapturePreview(
        g_wv, COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, g_shotStream,
        (ICoreWebView2CapturePreviewCompletedHandler *)cb);
    log_line("CapturePreview hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) request_screenshot_cdp();

    /* 诊断：completion handler 机制是否可用（若此回调也不来，说明是
       Runtime 侧不在可回调状态，而非本文件的 QI/vtable 问题）。 */
    test_execute_script();
}

/* ---- 导航完成 ---- */
DEFINE_CB(NavCB, ICoreWebView2NavigationCompletedEventHandler)

static HRESULT STDMETHODCALLTYPE NavCB_Invoke(
    ICoreWebView2NavigationCompletedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args)
{
    BOOL ok = FALSE;
    (void)This; (void)sender;
    if (!args) return S_OK;
    args->lpVtbl->get_IsSuccess(args, &ok);
    log_line("导航完成 success=%d", ok ? 1 : 0);
    g.webview_ready = 1;

    return S_OK;
}

static ICoreWebView2NavigationCompletedEventHandlerVtbl g_NavCBVtbl = {
    NavCB_QI, NavCB_AddRef, NavCB_Release, NavCB_Invoke
};

/* 读取 ui 目录下的文本文件（UTF-8，去 BOM），失败返回 NULL */
static char *read_ui_file(const char *name)
{
    WCHAR path[MAX_PATH];
    HANDLE hf;
    DWORD size = 0, rd = 0;
    char *raw;
    WCHAR wname[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, MAX_PATH);
    swprintf(path, MAX_PATH, L"%s\\ui\\%s", g_exeDir, wname);

    hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(hf, NULL);
    if (size == 0 || size == INVALID_FILE_SIZE) { CloseHandle(hf); return NULL; }
    raw = (char *)malloc(size + 1);
    if (!raw) { CloseHandle(hf); return NULL; }
    if (!ReadFile(hf, raw, size, &rd, NULL)) rd = 0;
    CloseHandle(hf);
    raw[rd] = '\0';
    return raw;
}

/* 把 index.html 的外链 css/js 内联成一份自包含页面。
   虚拟主机映射不可用时用它兜底（NavigateToString 的 base 是 about:blank，
   相对路径取不到资源）。 */
static void navigate_inline(void)
{
    char *html = read_ui_file("index.html");
    char *css  = read_ui_file("app.css");
    char *js   = read_ui_file("app.js");
    strbuf_t *buf;
    char *out;
    WCHAR *wide;
    const char *linkTag = "<link rel=\"stylesheet\" href=\"app.css\">";
    const char *scriptTag = "<script src=\"app.js\"></script>";
    const char *first = NULL;
    int which = 0;

    if (!html) {
        log_line("!! 读不到 ui/index.html，无法内联");
        if (css) free(css);
        if (js) free(js);
        return;
    }

    buf = strbuf_create(strlen(html) + (css ? strlen(css) : 0) + (js ? strlen(js) : 0) + 512);
    if (!buf) { free(html); free(css); free(js); return; }

    /* 按出现顺序替换两个外链标签：先出现的先处理 */
    {
        const char *a = strstr(html, linkTag);
        const char *b = strstr(html, scriptTag);
        if (a && (!b || a < b)) { first = a; which = 1; }
        else if (b)             { first = b; which = 2; }
    }

    if (!first) {
        /* 没有外链标签，原样投递 */
        strbuf_append(buf, html);
    } else {
        const char *p = html;
        while (first) {
            /* tag 必须每轮跟着 which 重算：写死在循环外的话，第二轮
               处理 <script> 时会用 <link> 的长度去推进 p，末尾的
               </body> 会被多跳掉几个字节。 */
            const char *tag = (which == 1) ? linkTag : scriptTag;
            strbuf_append_len(buf, p, (size_t)(first - p));
            if (which == 1) {
                strbuf_append(buf, "<style>\n");
                if (css) strbuf_append(buf, css);
                strbuf_append(buf, "\n</style>");
            } else {
                strbuf_append(buf, "<script>\n");
                if (js) strbuf_append(buf, js);
                strbuf_append(buf, "\n</script>");
            }
            p = first + strlen(tag);
            /* 找下一个标签 */
            {
                const char *a = strstr(p, linkTag);
                const char *b = strstr(p, scriptTag);
                first = NULL;
                if (a && (!b || a < b)) { first = a; which = 1; }
                else if (b)             { first = b; which = 2; }
            }
        }
        strbuf_append(buf, p);
    }

    out = strbuf_dup(buf);
    strbuf_destroy(buf);
    free(html); free(css); free(js);

    wide = out ? u8_to_w(out) : NULL;
    free(out);
    if (wide) {
        HRESULT hr = g_wv->lpVtbl->NavigateToString(g_wv, wide);
        log_line("内联投递 hr=0x%08lX，HTML %d 字符",
                 (unsigned long)hr, (int)wcslen(wide));
        free(wide);
    }
}

/* file:// 直载（保留 index.html 的外链 <link>/<script>，浏览器原生加载）。
   与 NavigateToString 相比，这是「真实文档 + 真实源」，渲染管线更接近常态。 */
static void navigate_file_url(void)
{
    WCHAR url[MAX_PATH + 64];
    WCHAR dir[MAX_PATH];
    int i;
    HRESULT hr;

    wcsncpy(dir, g_exeDir, MAX_PATH - 1);
    dir[MAX_PATH - 1] = L'\0';
    for (i = 0; dir[i]; i++) if (dir[i] == L'\\') dir[i] = L'/';

    swprintf(url, MAX_PATH + 64, L"file:///%s/ui/index.html", dir);
    hr = g_wv->lpVtbl->Navigate(g_wv, url);
    log_line("file:// 导航 hr=0x%08lX url=%ls", (unsigned long)hr, url);
}

/* ---- 控制器创建完成：装配页面 ---- */
static HRESULT STDMETHODCALLTYPE CtrlCB_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Controller *controller)
{
    RECT rc;
    ICoreWebView2Settings *settings = NULL;
    EventRegistrationToken token;
    MsgCB *msgcb;
    NavCB *navcb;
    (void)This;

    if (FAILED(errorCode) || !controller) {
        log_line("!! 控制器创建失败 hr=0x%08lX", (unsigned long)errorCode);
        return S_OK;
    }
    log_line("OK  控制器创建成功");

    g_ctrl = controller;
    controller->lpVtbl->AddRef(controller);
    if (FAILED(controller->lpVtbl->get_CoreWebView2(controller, &g_wv)) || !g_wv) {
        log_line("!! get_CoreWebView2 失败");
        return S_OK;
    }
    GetClientRect(g_hwnd, &rc);
    controller->lpVtbl->put_Bounds(controller, rc);
    /* ---- 可见性前置确认（必须发生在导航之前）----
       这是本程序最要命的一个坑：
         controller 若在宿主窗口 show 之前创建，IsVisible 会被锁死在 FALSE，
         而**默认值虽然号称是 TRUE，实际读回来是 0**。
       IsVisible=FALSE 时的后果不是「画面不好看」，而是整个渲染器把页面当作不可见：
         - NavigateToString 不提交，Source 永远停在 about:blank，
           页面里的 JS 一行都不会执行；
         - C -> JS 的 PostWebMessageAsJson 返回 S_OK 却把报文丢掉；
         - JS 的 setTimeout / setInterval 全部冻结。
       症状就是「界面起来了但所有按钮都没反应」，且时好时坏
       （实测同一份程序闭环成功率只有 50%）。
       注意不能只写一次：第一次 put_IsVisible(TRUE) 反而会把状态置成 FALSE，
       要「推 + 读回」反复几次才收敛。这里在导航前先收敛好。 */
    {
        int i;
        BOOL vis = FALSE;
        for (i = 0; i < 8; i++) {
            controller->lpVtbl->put_IsVisible(controller, TRUE);
            controller->lpVtbl->get_IsVisible(controller, &vis);
            if (vis) break;
        }
        log_line("可见性前置确认：vis=%d 尝试=%d 次", vis ? 1 : 0, i + 1);
    }
    /* 兜底：导航之后仍可能被置回 FALSE，用定时器 96 继续跟直到读回 TRUE */
    g_visTries = 0;
    SetTimer(g_hwnd, 96, 100, NULL);

    if (SUCCEEDED(g_wv->lpVtbl->get_Settings(g_wv, &settings)) && settings) {
        settings->lpVtbl->put_IsStatusBarEnabled(settings, FALSE);
        settings->lpVtbl->put_AreDefaultContextMenusEnabled(settings, TRUE);
        settings->lpVtbl->put_AreDevToolsEnabled(settings, TRUE);
        settings->lpVtbl->put_IsZoomControlEnabled(settings, FALSE);
        settings->lpVtbl->Release(settings);
    }

    /* 接收 JS -> C */
    msgcb = (MsgCB *)calloc(1, sizeof(MsgCB));
    if (msgcb) {
        msgcb->iface.lpVtbl = &g_MsgCBVtbl;
        msgcb->ref = 1;
        if (FAILED(g_wv->lpVtbl->add_WebMessageReceived(
                g_wv, (ICoreWebView2WebMessageReceivedEventHandler *)msgcb, &token)))
            log_line("!! 注册 WebMessageReceived 失败");
        else
            log_line("OK  JS -> C 通道就绪");
    }

    /* 导航完成回调 */
    navcb = (NavCB *)calloc(1, sizeof(NavCB));
    if (navcb) {
        HRESULT hr;
        navcb->iface.lpVtbl = &g_NavCBVtbl;
        navcb->ref = 1;
        hr = g_wv->lpVtbl->add_NavigationCompleted(
            g_wv, (ICoreWebView2NavigationCompletedEventHandler *)navcb, &token);
        log_line("add_NavigationCompleted hr=0x%08lX", (unsigned long)hr);
    }

    /* ---------- 加载页面 ----------
       实测结论：在本机 WebView2 Runtime 154 下，对
       `SetVirtualHostNameToFolderMapping` 注册的 https://<host>/ 调 Navigate
       会返回 S_OK 但导航永不提交（Source 一直是 about:blank），
       且随后任何 NavigateToString 都会拿到 ERROR_INVALID_STATE。
       而同环境下 NavigateToString 完全正常（spike 已验证）。
       因此默认走「读文件 -> 内联 css/js -> NavigateToString」，
       虚拟主机映射改用 --vhost 参数显式开启以便后续排查。 */
    if (g_useVHost) {
        ICoreWebView2_3 *wv3 = NULL;
        WCHAR uiDir[MAX_PATH];
        swprintf(uiDir, MAX_PATH, L"%s\\ui", g_exeDir);
        if (SUCCEEDED(g_wv->lpVtbl->QueryInterface(g_wv, &IID_ICoreWebView2_3, (void **)&wv3)) && wv3) {
            HRESULT hm = wv3->lpVtbl->SetVirtualHostNameToFolderMapping(
                    wv3, L"novelnow.local", uiDir,
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            if (SUCCEEDED(hm)) {
                HRESULT hn = g_wv->lpVtbl->Navigate(g_wv, L"https://novelnow.local/index.html");
                log_line("--vhost: 映射成功，Navigate hr=0x%08lX", (unsigned long)hn);
            } else {
                log_line("--vhost: 映射失败 hr=0x%08lX", (unsigned long)hm);
            }
            wv3->lpVtbl->Release(wv3);
        }
    } else if (g_useFileUrl) {
        navigate_file_url();
    } else {
        navigate_inline();
    }

    /* 诊断看门狗：页面若迟迟不就绪，把当前 URL 记下来便于排查 */
    SetTimer(g_hwnd, 97, 3000, NULL);
    return S_OK;
}

/* ============================================================
 * 18. 主线程消息处理（任务回执 / 询问 / 进度）
 * ============================================================ */

static void handle_task_done(task_done_t *t)
{
    const char *cmd = t->cmd;

    /* 加载项目：把工作线程解析好的 project 换上 */
    if (strcmp(cmd, "load_project") == 0) {
        novel_project_t *proj = (novel_project_t *)t->ptr;
        if (proj) {
            if (g.project) project_destroy(g.project);
            g.project = proj;
            g.selected_chapter = -1;
            g.dirty = 0;
            g.last_doc[0] = g.last_txt[0] = '\0';
        }
        end_task();
        {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddBoolToObject(d, "ok", t->ok);
            add_str(d, "cmd", cmd);
            add_str(d, "message", t->message);
            push_event("task_done", d);
        }
        push_project();
        push_project_list();
        push_status(t->message);
        if (!t->ok) push_toast("error", t->message);
        return;
    }

    /* 获取模型列表：payload 里是 {"models":[...]}（大 JSON，不能走 message） */
    if (strcmp(cmd, "fetch_models") == 0) {
        end_task();
        {
            cJSON *d = cJSON_CreateObject();
            cJSON *parsed = (t->ok && t->payload) ? cJSON_Parse(t->payload) : NULL;
            cJSON *m = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "models") : NULL;
            int  n  = (m && cJSON_IsArray(m)) ? cJSON_GetArraySize(m) : 0;
            int  ok = (t->ok && parsed && m && cJSON_IsArray(m)) ? 1 : 0;
            char msg[160];

            if (t->ok && !ok) {
                /* 以前这条路径是静默的：载荷被截断 -> cJSON_Parse 失败 -> models 缺失，
                   前端只收到 ok=true，用户看到「获取成功」却一个模型都没有。
                   现在必须留下痕迹并如实报错 */
                log_line("!! fetch_models 载荷解析失败（payload %d 字节）",
                         t->payload ? (int)strlen(t->payload) : -1);
            }
            if (ok)       snprintf(msg, sizeof(msg), "已获取 %d 个模型", n);
            else          snprintf(msg, sizeof(msg), "%s",
                                   t->message[0] ? t->message : "获取模型列表失败");

            cJSON_AddBoolToObject(d, "ok", ok);
            add_str(d, "cmd", cmd);
            add_str(d, "message", msg);
            if (ok) cJSON_AddItemToObject(d, "models", cJSON_Duplicate(m, 1));
            push_event("task_done", d);
            push_status(msg);
            if (!ok) push_toast("error", msg);
            cJSON_Delete(parsed);
        }
        return;
    }

    end_task();
    {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "ok", t->ok);
        add_str(d, "cmd", cmd);
        add_str(d, "message", t->message);
        cJSON_AddNumberToObject(d, "chapter", t->chapter);
        push_event("task_done", d);
    }
    push_status(t->message);
    if (!t->ok && t->result != ENGINE_CANCELLED) push_toast("error", t->message);
    else if (t->ok) push_toast("success", t->message);

    /* 任务改变了项目内容：推一份新快照 */
    if (t->ok || t->result == ENGINE_CANCELLED) {
        g.dirty = 1;
        push_project();
    }
}

/* ============================================================
 * 19. 窗口过程
 * ============================================================ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_ctrl) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_ctrl->lpVtbl->put_Bounds(g_ctrl, rc);
        }
        return 0;

    case WM_DPICHANGED:
        if (g_ctrl) {
            RECT *r = (RECT *)lp;
            SetWindowPos(hwnd, NULL, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_GETMINMAXINFO:
        {
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            mmi->ptMinTrackSize.x = 900;
            mmi->ptMinTrackSize.y = 600;
        }
        return 0;

    case WM_SETFOCUS:
        if (g_ctrl) g_ctrl->lpVtbl->MoveFocus(g_ctrl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;

    /* ---- 延后投递 C -> JS（见 post_json_root 的说明）---- */
    case NWM_POST:
        {
            WCHAR *w = (WCHAR *)wp;
            HRESULT hr;
            if (!w) return 0;
            if (!g_wv || g_shuttingDown) { free(w); return 0; }
            /* 从 WebView2 回调里同步回投会丢包，改到这里；失败则入重投队列。 */
            hr = g_wv->lpVtbl->PostWebMessageAsJson(g_wv, w);
            if (SUCCEEDED(hr)) { free(w); return 0; }
            if (g_postRetryN < POST_RETRY_MAX) {
                g_postRetry[g_postRetryN++] = w;
                if (g_postRetryTries == 0)
                    log_line("!! 投递失败 hr=0x%08lX，转入重投队列（当前 %d 条待发）",
                             (unsigned long)hr, g_postRetryN);
                SetTimer(hwnd, IDT_POST_RETRY, 200, NULL);
            } else {
                log_line("!! 投递失败 hr=0x%08lX 且重投队列已满，丢弃一条", (unsigned long)hr);
                free(w);
            }
        }
        return 0;

    /* ---- 任务完成 ---- */
    case NWM_TASK_DONE:
        {
            task_done_t *t = (task_done_t *)wp;
            if (t) { handle_task_done(t); task_done_free(t); }
        }
        return 0;

    /* ---- 工作线程请求用户确认：推给网页 ---- */
    case NWM_ASK:
        {
            ask_t *a = (ask_t *)wp;
            if (a) {
                cJSON *d = cJSON_CreateObject();
                cJSON_AddNumberToObject(d, "askId", a->id);
                add_str(d, "text", a->text);
                add_str(d, "kind", "chapter_review");
                push_event("ask", d);
            }
        }
        return 0;

    /* ---- 进度 ---- */
    case NWM_PROGRESS:
        {
            prog_t *p = (prog_t *)wp;
            if (p) {
                cJSON *d = cJSON_CreateObject();
                add_str(d, "stage", p->stage);
                cJSON_AddNumberToObject(d, "current", p->current);
                cJSON_AddNumberToObject(d, "total", p->total);
                add_str(d, "message", p->message);
                push_event("progress", d);
                if (p->message[0]) push_status(p->message);
                free(p);
            }
        }
        return 0;

    /* ---- 章节正文流式增量：推给前端实时回填 ---- */
    case NWM_CHAPTER_DELTA:
        {
            delta_t *d = (delta_t*)wp;
            if (d) {
                cJSON *j = cJSON_CreateObject();
                cJSON_AddNumberToObject(j, "index", d->index);
                add_str(j, "delta", d->text);
                push_event(d->index < 0 ? "text_delta" : "chapter_delta", j);
                free(d->text);
                free(d);
            }
        }
        return 0;

    /* ---- 逐章生成：某章完成 ---- */
    case NWM_CHAPTER_DONE:
        {
            int idx = (int)wp;
            if (g.project && idx >= 0 && idx < g.project->chapter_count) {
                chapter_t *c = &g.project->chapters[idx];
                cJSON *d = cJSON_CreateObject();
                cJSON_AddNumberToObject(d, "index", idx);
                add_str(d, "title", c->title);
                add_str(d, "content", c->content);
                cJSON_AddNumberToObject(d, "wordCount", c->word_count);
                push_event("chapter_written", d);
                push_project();
                g.dirty = 1;
            }
        }
        return 0;

    /* ---- 自动保存 ---- */
    case WM_TIMER:
        if (wp == IDT_AUTOSAVE) {
            if (g.dirty && !is_busy() && g.project && g.config) {
                if (save_project_silently()) push_status("已自动保存项目");
            }
        } else if (wp == 97) {
            /* 诊断看门狗：3 秒还未就绪则把当前 URL 记下来便于排查 */
            KillTimer(hwnd, 97);
            if (!g.webview_ready) {
                LPWSTR src = NULL;
                if (g_wv && SUCCEEDED(g_wv->lpVtbl->get_Source(g_wv, &src)) && src) {
                    char *u = w_to_u8_alloc(src);
                    log_line("!! 看门狗：导航 3 秒未完成，当前 URL=%s", u ? u : "(?)");
                    free(u);
                    CoTaskMemFree(src);
                }
            }
        } else if (wp == IDT_POST_RETRY) {
            post_retry_pump(hwnd);
        } else if (wp == 96) {
            /* 可见性自愈（现在无条件运行，见 controller 创建处的说明）。
               IsVisible 被锁死在 FALSE 时，渲染器视页面为不可见：C -> JS 报文
               被静默丢弃、JS 定时器全部冻结。这里反复重设直到读回 TRUE。
               间隔 100ms，通常 1~3 次即收敛。 */
            BOOL vis = FALSE;
            if (g_ctrl) {
                g_ctrl->lpVtbl->put_IsVisible(g_ctrl, TRUE);
                g_ctrl->lpVtbl->get_IsVisible(g_ctrl, &vis);
            }
            g_visTries++;
            if (vis || g_visTries >= 25) {
                KillTimer(hwnd, 96);
                log_line("可见性自愈结束：vis=%d 尝试=%d 次（IsIconic=%d）",
                         vis ? 1 : 0, g_visTries, IsIconic(hwnd) ? 1 : 0);
                if (g_shotMode) {
                    SetTimer(hwnd, 98, 300, NULL);
                } else if (vis) {
                    /* 确认可见之后前端才可以握手：此前发出的报文会被丢掉。
                       前端收到 view_ready 才发 hello，并有超时兜底（见 app.js）。 */
                    cJSON *d = cJSON_CreateObject();
                    push_event("view_ready", d);
                }
            }
            return 0;
        } else if (wp == 98) {
            KillTimer(hwnd, 98);
            request_screenshot();
        } else if (wp == 99) {
            KillTimer(hwnd, 99);
            if (g_holdMode) { log_line("--hold：保留窗口不退出"); return 0; }
            log_line("--shot 模式：正常退出");
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_CLOSE:
        if (is_busy()) {
            /* 不弹模态框：交给前端用自绘弹层处理，避免跨线程/模态的坑 */
            {
                cJSON *d = cJSON_CreateObject();
                add_str(d, "kind", "warn");
                add_str(d, "text", "后台任务正在进行，请等待任务完成或点击取消后再关闭窗口。");
                push_event("toast", d);
            }
            return 0;
        }
        if (g.dirty && g.project) {
            if (MessageBoxW(hwnd, L"项目有未保存的改动，是否保存后退出？",
                            L"novelnow", MB_YESNOCANCEL | MB_ICONQUESTION) == IDYES) {
                if (!save_project_silently())
                    MessageBoxW(hwnd, L"保存失败，请检查 projects 目录权限。", L"novelnow", MB_OK | MB_ICONERROR);
            }
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_shuttingDown = 1;
        KillTimer(hwnd, IDT_AUTOSAVE);
        KillTimer(hwnd, IDT_POST_RETRY);
        {
            int i;
            for (i = 0; i < g_postRetryN; i++) free(g_postRetry[i]);
            g_postRetryN = 0;
        }
        ask_release_all();
        if (g.engine) { engine_destroy(g.engine); g.engine = NULL; }
        if (g.project) { project_destroy(g.project); g.project = NULL; }
        if (g.config) { config_destroy(g.config); g.config = NULL; }
        if (g_ctrl) {
            g_ctrl->lpVtbl->Close(g_ctrl);
            g_ctrl->lpVtbl->Release(g_ctrl);
            g_ctrl = NULL;
            g_wv = NULL;
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ============================================================
 * 20. 初始化
 * ============================================================ */

static void enable_dark_titlebar(HWND hwnd)
{
    typedef HRESULT (WINAPI *PFN_DwmSet)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    BOOL dark = TRUE;
    if (!dwm) return;
    {
        PFN_DwmSet f = (PFN_DwmSet)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (f) {
            /* 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 1903+)，19 为旧版编号 */
            if (FAILED(f(hwnd, 20, &dark, sizeof(dark))))
                f(hwnd, 19, &dark, sizeof(dark));
        }
    }
    FreeLibrary(dwm);
}

static void enable_dpi_awareness(void)
{
    typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    PFN_SetCtx f = u ? (PFN_SetCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;

    /* manifest 里已声明 PerMonitorV2；此时再调 API 会返回 ERROR_ACCESS_DENIED，
       属于"已生效"，不是失败。 */
    if (f && f((HANDLE)-4)) { log_line("DPI: PerMonitorV2（运行时设置）"); return; }
    if (GetLastError() == ERROR_ACCESS_DENIED) { log_line("DPI: PerMonitorV2（manifest 已声明）"); return; }
    if (SetProcessDPIAware()) log_line("DPI: SystemAware（回退）");
    else log_line("DPI: 未设置");
}

static int load_loader(void)
{
    WCHAR path[MAX_PATH + 32];
    HMODULE h;
    swprintf(path, MAX_PATH + 32, L"%s\\WebView2Loader.dll", g_exeDir);
    h = LoadLibraryW(path);
    if (!h) {
        h = LoadLibraryW(L"WebView2Loader.dll");
        if (!h) { log_line("!! WebView2Loader.dll 加载失败 err=%lu", (unsigned long)GetLastError()); return 0; }
        log_line("WebView2Loader.dll 从系统路径加载");
    } else {
        log_line("WebView2Loader.dll 从 exe 同目录加载");
    }
    g_hLoader = h;
    g_pCreateEnv  = (PFN_CreateEnv)GetProcAddress(h, "CreateCoreWebView2EnvironmentWithOptions");
    g_pGetVersion = (PFN_GetVersion)GetProcAddress(h, "GetAvailableCoreWebView2BrowserVersionString");
    if (!g_pCreateEnv) { log_line("!! 找不到 CreateCoreWebView2EnvironmentWithOptions"); return 0; }
    return 1;
}

static void init_webview(void)
{
    WCHAR userData[MAX_PATH + 32];
    EnvCB *cb;

    if (g_pGetVersion) {
        LPWSTR ver = NULL;
        if (SUCCEEDED(g_pGetVersion(NULL, &ver)) && ver) {
            log_line("WebView2 Runtime: %ls", ver);
            CoTaskMemFree(ver);
        } else {
            log_line("!! 未检测到 WebView2 Runtime");
        }
    }

    swprintf(userData, MAX_PATH + 32, L"%s\\webview_data", g_exeDir);
    cb = (EnvCB *)calloc(1, sizeof(EnvCB));
    if (!cb) return;
    cb->iface.lpVtbl = &g_EnvCBVtbl;
    cb->ref = 1;
    if (FAILED(g_pCreateEnv(NULL, userData, NULL,
            (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)cb)))
        log_line("!! CreateCoreWebView2EnvironmentWithOptions 立即失败");
}

static void load_app_state(void)
{
    char *cfg_path = config_get_default_path();
    g.config = config_load(cfg_path);
    free(cfg_path);
    if (!g.config) { log_line("!! 配置加载失败"); return; }
    log_line("配置: projects_dir=%s auto_save=%d",
             g.config->projects_dir ? g.config->projects_dir : "(null)",
             g.config->auto_save_interval);

    /* 把持久化的导出目录同步到运行时副本（g.export_dir 供 do_export / 前端下发用） */
    if (g.config->export_dir && *g.config->export_dir)
        snprintf(g.export_dir, sizeof(g.export_dir), "%s", g.config->export_dir);

    g.engine = engine_create(g.config);
    if (g.engine) engine_set_progress_callback(g.engine, on_progress, NULL);
    if (g.engine) engine_set_chapter_delta_callback(g.engine, on_chapter_delta, NULL);
    if (g.engine) engine_set_text_delta_callback(g.engine, on_text_delta, NULL);
    else log_line("!! 引擎创建失败");

    g.selected_chapter = -1;
    g.last_doc[0] = g.last_txt[0] = '\0';
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    WCHAR *p;
    (void)hPrev;

    if (lpCmdLine && strstr(lpCmdLine, "--shot")) g_shotMode = 1;
    if (lpCmdLine && strstr(lpCmdLine, "--vhost")) g_useVHost = 1;
    if (lpCmdLine && strstr(lpCmdLine, "--hold"))  g_holdMode = 1;
    if (lpCmdLine && strstr(lpCmdLine, "--file"))  g_useFileUrl = 1;

    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    p = wcsrchr(g_exeDir, L'\\');
    if (p) *p = L'\0';

    g_uiThreadId = GetCurrentThreadId();
    InitializeCriticalSection(&g_askCs);
    g_askCsReady = 1;

    log_line("=== novelnow WebUI 启动 ===");
    log_line("exe 目录: %ls", g_exeDir);
    log_line("参数: shot=%d vhost=%d hold=%d file=%d",
             g_shotMode, g_useVHost, g_holdMode, g_useFileUrl);

    enable_dpi_awareness();
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
        log_line("!! CoInitializeEx 失败");

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"novelnowWebUI";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(0, L"novelnowWebUI",
                           L"novelnow · AI 小说写作助手",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1360, 880,
                           NULL, NULL, hInst, NULL);
    if (!hwnd) { log_line("!! CreateWindow 失败"); return 1; }
    g.hwnd = hwnd;
    g_hwnd = hwnd;

    /* --shot 由自动化调用，进程可能拿到 SW_HIDE 的 nCmdShow；
       更关键的是：窗口若被其它全屏窗口遮挡，WebView2 合成器不产帧，
       CapturePreview/CDP 会一直挂着不回。故置顶 + 前台。 */
    if (g_shotMode) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetWindowPos(hwnd, HWND_TOPMOST, 40, 40, 1360, 880,
                     SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
    } else {
        ShowWindow(hwnd, nCmdShow);
    }
    UpdateWindow(hwnd);
    enable_dark_titlebar(hwnd);

    /* 业务层先就位，再起 WebView2 */
    load_app_state();
    restart_autosave();

    if (!load_loader()) {
        MessageBoxW(hwnd,
            L"WebView2Loader.dll 加载失败。\n\n"
            L"请确认该 DLL 与 exe 在同一目录。",
            L"novelnow", MB_OK | MB_ICONERROR);
        return 1;
    }
    init_webview();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hLoader) FreeLibrary(g_hLoader);
    if (g_askCsReady) { DeleteCriticalSection(&g_askCs); g_askCsReady = 0; }
    CoUninitialize();
    log_line("=== novelnow WebUI 退出 ===");
    return 0;
}
