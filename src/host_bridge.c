/* host_bridge.c - globals + encoding/log/json/push/reply/busy + task_done + launch_thread (split from host.c 2026-10-18, zero behavior change) */
#include "host_internal.h"

/* ---- global state definitions ---- */
app_t g;
WCHAR            g_exeDir[MAX_PATH];
HWND             g_hwnd           = NULL;
ICoreWebView2Controller *g_ctrl   = NULL;
ICoreWebView2          *g_wv      = NULL;
HMODULE          g_hLoader        = NULL;
DWORD            g_uiThreadId     = 0;
int              g_shuttingDown   = 0;
int              g_shotMode       = 0;   /* --shot:  就绪后自动截图 */
int              g_useVHost       = 0;   /* --vhost: 改用虚拟主机映射（实验） */
int              g_holdMode       = 0;   /* --hold:  截图后不退出（人工验收） */
int              g_useFileUrl     = 0;   /* --file:  用 file:// 直载 ui/index.html */
IStream         *g_shotStream     = NULL;/* CapturePreview 的输出流 */
int              g_visTries       = 0;   /* 可见性自愈重试计数 */
PFN_CreateEnv  g_pCreateEnv  = NULL;
PFN_GetVersion g_pGetVersion = NULL;

/* ============================================================
 * 1. 编码工具
 * ============================================================ */

WCHAR *u8_to_w(const char *s)
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

char *w_to_u8_alloc(const WCHAR *s)
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

void w_to_u8(const WCHAR *s, char *out, int n)
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

void log_line(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    char path[2048];
    char msg[1024];
    static SRWLOCK s_logLock = SRWLOCK_INIT;   /* 工作线程也打日志：并发 fopen/fwrite 同一文件会互相踩行（P1） */

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    snprintf(path, sizeof(path), "%ls\\webui.log", g_exeDir);
    AcquireSRWLockExclusive(&s_logLock);
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "[%8lu] %s\r\n", (unsigned long)GetTickCount(), msg);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&s_logLock);
}

/* ============================================================
 * 3. JSON 取值小工具（全部做 NULL 保护）
 * ============================================================ */

const char *j_str(cJSON *o, const char *k, const char *def)
{
    cJSON *v;
    if (!o) return def;
    v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!v || !cJSON_IsString(v) || !v->valuestring) return def;
    return v->valuestring;
}

int j_int(cJSON *o, const char *k, int def)
{
    cJSON *v;
    if (!o) return def;
    v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!v || !cJSON_IsNumber(v)) return def;
    return v->valueint;
}

/* 布尔取值：兼容 true / 1 / "true" / "1" 四种写法 */
int j_bool(cJSON *o, const char *k, int def)
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
cJSON *j_params(cJSON *root) { return cJSON_GetObjectItemCaseSensitive(root, "params"); }

const char *p_str(cJSON *root, const char *k, const char *def)
{
    cJSON *p = j_params(root);
    return j_str(p ? p : root, k, def);
}
int p_int(cJSON *root, const char *k, int def)
{
    cJSON *p = j_params(root);
    return j_int(p ? p : root, k, def);
}
int p_bool(cJSON *root, const char *k, int def)
{
    cJSON *p = j_params(root);
    return j_bool(p ? p : root, k, def);
}

/* 加字符串但容忍 NULL */
void add_str(cJSON *o, const char *k, const char *v)
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

WCHAR *g_postRetry[POST_RETRY_MAX];
int    g_postRetryN = 0;
int    g_postRetryTries = 0;

/* 在消息循环里补发；顺带把卡住的可见性推一把 */
void post_retry_pump(HWND hwnd)
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

void post_json_root(cJSON *root)
{
    char *txt;
    WCHAR *wide;
    if (!root) return;
    if (!g_wv || g_shuttingDown) { cJSON_Delete(root); return; }
    /* 投递前记一条「类型 + 序号」。
       ★ 绝不打印报文正文：hello 回执里含 API Key，写进日志等于泄露。 */
    {
        static volatile LONG n = 0;   /* 工作线程也走这里，静态 int++ 非原子（P1） */
        if (InterlockedIncrement(&n) <= 12) {
            const char *ty = j_str(root, "type", "?");
            const char *nm = j_str(root, "name", NULL);
            if (!nm) nm = j_str(root, "cmd", NULL);
            log_line("--> post #%ld %s %s", (long)n, ty, nm ? nm : "");
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

void push_event(const char *name, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { if (data) cJSON_Delete(data); return; }
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "name", name);
    if (data) cJSON_AddItemToObject(root, "data", data);
    else      cJSON_AddItemToObject(root, "data", cJSON_CreateObject());
    post_json_root(root);
}

void reply_ok(int id, const char *cmd, cJSON *data)
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

void reply_err(int id, const char *cmd, const char *msg)
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

void push_status(const char *text)
{
    cJSON *d = cJSON_CreateObject();
    add_str(d, "text", text);
    push_event("status", d);
}

void push_toast(const char *kind, const char *text)
{
    cJSON *d = cJSON_CreateObject();
    add_str(d, "kind", kind);   /* info / success / error / warn */
    add_str(d, "text", text);
    push_event("toast", d);
}

/* ============================================================
 * 5. 忙闲控制（与旧版 require_idle / try_begin_task 等价）
 * ============================================================ */

int is_busy(void) { return InterlockedCompareExchange(&g.busy, 0, 0) != 0; }

int try_begin_task(void)
{
    if (InterlockedCompareExchange(&g.busy, 1, 0) != 0) return 0;
    return 1;
}

void end_task(void)
{
    InterlockedExchange(&g.busy, 0);
    /* 通知前端任务态结束 */
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "busy", 0);
    push_event("busy", d);
}

void push_busy(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "busy", is_busy() ? 1 : 0);
    push_event("busy", d);
}

/* ============================================================
 * 10. 任务完成回执（工作线程 -> 主线程）
 * ============================================================ */


/* task_done_t 的统一释放口：payload 是堆指针，漏掉它会漏内存 */
void task_done_free(task_done_t *t)
{
    if (!t) return;
    free(t->payload);
    /* ptr 只有 load_project 在用（novel_project_t*）：主线程取走后置 NULL，
       走到这里还非空说明消息没送达（PostMessage 失败路径），销毁它防泄漏 */
    if (t->ptr) project_destroy((novel_project_t *)t->ptr);
    free(t);
}

void post_task_done(const char *cmd, int result, int ok,
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

int launch_thread(LPTHREAD_START_ROUTINE fn, LPVOID param)
{
    DWORD tid;
    HANDLE h = CreateThread(NULL, 0, fn, param, 0, &tid);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}

const char *engine_err(void)
{
    if (g.engine && g.engine->last_error[0]) return g.engine->last_error;
    return "未知错误";
}
