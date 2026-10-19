/* host_internal.h - shared types/macros/globals for host.c split (2026-10-18, zero behavior change) */
#ifndef HOST_INTERNAL_H
#define HOST_INTERNAL_H

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

/* from host.c:42-48 */
/* 本文件所有 snprintf 都写入固定缓冲，截断是安全且有意的行为，
   关闭 GCC 对应的推测性告警（它会为每个 %s 计算最坏长度）。 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#define NW_VERSION L"0.2.12"

/* from host.c:54-66 */
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

/* from host.c:86-102 */
/* 自动保存 */
#define IDT_AUTOSAVE 1

/* 自定义窗口消息 */
#define NWM_TASK_DONE   (WM_APP + 10)  /* wParam = task_done_t* */
#define NWM_ASK         (WM_APP + 11)  /* wParam = ask_t* */
#define NWM_PROGRESS    (WM_APP + 12)  /* wParam = prog_t* */
#define NWM_CHAPTER_DONE (WM_APP + 13) /* wParam = ch_done_t*（快照，主线程用完释放） */
#define NWM_POST        (WM_APP + 14)  /* wParam = WCHAR* 待投递报文（由本窗口释放）*/
#define NWM_CHAPTER_DELTA (WM_APP + 15) /* wParam = delta_t* 流式增量 */

/* WebView2Loader 导出函数 */
typedef HRESULT (STDAPICALLTYPE *PFN_CreateEnv)(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);
typedef HRESULT (STDAPICALLTYPE *PFN_GetVersion)(PCWSTR browserExecutableFolder, LPWSTR *versionInfo);

/* from host.c:674-681 */
typedef struct ask_s {
    int    id;
    HANDLE done;      /* 手动复位事件 */
    int    answer;    /* 1=是 2=否 3=取消 */
    volatile LONG refs;  /* 引用计数：工作线程持有 1 个 + 每条已投递消息持有 1 个 */
    struct ask_s *next;
    char   text[1];   /* 柔性数组（标题由前端统一渲染） */
} ask_t;

/* from host.c:790-795 */
typedef struct {
    char stage[128];
    int  current;
    int  total;
    char message[512];
} prog_t;

/* from host.c:814-817 */
typedef struct {
    int index;      /* 正在生成的章节索引 */
    char *text;     /* 本次增量（堆上，主线程用完释放） */
} delta_t;

/* 逐章生成完成载荷（P0-C 快照）：th_step_write 在工作线程把章节字段拷贝好
   再投递主线程，主线程绝不回读 g.project——用户答「重写本章」后工作线程
   会立刻改写同一章，主线程回读 proj->chapters 就是 UAF。
   title/content 堆上，主线程用完释放。 */
typedef struct {
    int   index;
    int   word_count;
    char *title;      /* 堆上 */
    char *content;    /* 堆上 */
} ch_done_t;

/* from host.c:849-860 */
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

/* from host.c:930-930 */
typedef struct { int iterations; int count; } gen_dir_task_t;

/* from host.c:967-967 */
typedef struct { int index; int words; } ch_task_t;

/* from host.c:983-983 */
typedef struct { int words; int force; } expand_all_task_t;

/* from host.c:1118-1118 */
typedef struct { int dirs; int plans; int chapters; int words; int start_ch; } auto_task_t;

/* from host.c:1177-1180 */
typedef struct {
    char *system_prompt;
    char *user_prompt;
} ai_call_task_t;

/* ComfyUI 配图任务（comfy_gen）：全部字段为快照，线程内不碰 g/config */
typedef struct {
    char *node;           /* 图键（大纲节点 id），回执带回前端 */
    char *url;            /* ComfyUI 基地址 */
    char *workflow_json;  /* 工作流全文（已注入 {{PROMPT}} 前的原文） */
    char *prompt;         /* 场景描述 */
    char *out_path;       /* PNG 落盘路径 */
} comfy_gen_task_t;

/* ---- global state (definitions in host_bridge.c / host_tasks.c / host_webview.c) ---- */
extern app_t g;
extern WCHAR            g_exeDir[MAX_PATH];
extern HWND             g_hwnd;
extern ICoreWebView2Controller *g_ctrl;
extern ICoreWebView2          *g_wv;
extern HMODULE          g_hLoader;
extern DWORD            g_uiThreadId;
extern int              g_shuttingDown;
extern int              g_shotMode;
extern int              g_useVHost;
extern int              g_holdMode;
extern int              g_useFileUrl;
extern IStream         *g_shotStream;
extern int              g_visTries;
extern PFN_CreateEnv  g_pCreateEnv;
extern PFN_GetVersion g_pGetVersion;
/* 未就绪报文重投队列（post_retry_pump 在 bridge，WndProc 定时器在 webview） */
#define POST_RETRY_MAX  64
#define IDT_POST_RETRY  95
extern WCHAR *g_postRetry[POST_RETRY_MAX];
extern int    g_postRetryN;
extern int    g_postRetryTries;
extern CRITICAL_SECTION g_askCs;
extern int      g_askCsReady;
extern ask_t   *g_asks;
extern int      g_askSeq;
extern ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_CtrlCBVtbl;
extern ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_EnvCBVtbl;
extern ICoreWebView2WebMessageReceivedEventHandlerVtbl g_MsgCBVtbl;
extern ICoreWebView2CallDevToolsProtocolMethodCompletedHandlerVtbl g_CdpCBVtbl;
extern ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_EsCBVtbl;
extern ICoreWebView2CapturePreviewCompletedHandlerVtbl g_CapCBVtbl;
extern ICoreWebView2NavigationCompletedEventHandlerVtbl g_NavCBVtbl;

/* ---- functions (were static in host.c; split across translation units) ---- */
/* 单行函数体小工具（原 host.c:205/382，拆分脚本按块识别漏了单行定义） */
cJSON *j_params(cJSON *root);
int is_busy(void);
WCHAR *u8_to_w(const char *s);
char *w_to_u8_alloc(const WCHAR *s);
void w_to_u8(const WCHAR *s, char *out, int n);
void log_line(const char *fmt, ...);
const char *j_str(cJSON *o, const char *k, const char *def);
int j_int(cJSON *o, const char *k, int def);
int j_bool(cJSON *o, const char *k, int def);
const char *p_str(cJSON *root, const char *k, const char *def);
int p_int(cJSON *root, const char *k, int def);
int p_bool(cJSON *root, const char *k, int def);
void add_str(cJSON *o, const char *k, const char *v);
void post_retry_pump(HWND hwnd);
void post_json_root(cJSON *root);
void push_event(const char *name, cJSON *data);
void reply_ok(int id, const char *cmd, cJSON *data);
void reply_err(int id, const char *cmd, const char *msg);
void push_status(const char *text);
void push_toast(const char *kind, const char *text);
int try_begin_task(void);
void end_task(void);
void push_busy(void);
cJSON *chapter_brief_json(novel_project_t *p, int i);
cJSON *character_json(character_t *c);
cJSON *project_meta_json(novel_project_t *p);
void push_project(void);
cJSON *config_json(void);
void apply_config_json(cJSON *p);
void ask_unref(ask_t *a);
void ask_register(ask_t *a);
void ask_unregister(ask_t *a);
void ask_release_all(void);
int bridge_ask(const char *text);
void on_progress(const char *stage, int current, int total,                         const char *message, void *user_data);
void on_chapter_delta(int chapter_index, const char *delta, void *user_data);
void on_text_delta(const char *delta, void *user_data);
void task_done_free(task_done_t *t);
void post_task_done(const char *cmd, int result, int ok,                            const char *message, int chapter);
int launch_thread(LPTHREAD_START_ROUTINE fn, LPVOID param);
const char *engine_err(void);
void restart_autosave(void);
int save_project_silently(void);
DWORD WINAPI th_gen_directions(LPVOID param);
DWORD WINAPI th_gen_plans(LPVOID param);
DWORD WINAPI th_expand_outline(LPVOID param);
DWORD WINAPI th_expand_chapter(LPVOID param);
DWORD WINAPI th_expand_all(LPVOID param);
DWORD WINAPI th_step_write(LPVOID param);
DWORD WINAPI th_check_consistency(LPVOID param);
DWORD WINAPI th_sum_world(LPVOID param);
DWORD WINAPI th_gen_chars(LPVOID param);
DWORD WINAPI th_check_finale(LPVOID param);
DWORD WINAPI th_analyze_text(LPVOID param);
DWORD WINAPI th_auto_write(LPVOID param);
DWORD WINAPI th_fetch_models(LPVOID param);
DWORD WINAPI th_ai_call(LPVOID param);
DWORD WINAPI th_comfy_test(LPVOID param);
DWORD WINAPI th_comfy_gen(LPVOID param);
DWORD WINAPI th_load_project(LPVOID param);
int CALLBACK browse_cb(HWND h, UINT msg, LPARAM lp, LPARAM data);
char *pick_folder_dialog(const char *initial);
char *pick_project_file_dialog(const char *initial_dir);
char *pick_text_file_dialog(void);
int write_utf8_bom_file(const char *path, const char *content);
void rtf_append_text(strbuf_t *buf, const char *utf8);
int export_novel_txt(novel_project_t *proj, const char *path);
int export_novel_doc(novel_project_t *proj, const char *path);
int do_export(const char *dir_override);
void open_path(const char *path);
size_t utf8_trim_incomplete(const unsigned char *s, size_t len);
size_t gbk_trim_incomplete(const unsigned char *s, size_t len);
char *read_text_file_smart(const char *path, size_t max_bytes);
cJSON *build_project_list(void);
void reply_project_list(int id, const char *cmd);
void push_project_list(void);
int require_idle_reply(int id, const char *cmd, const char *what);
int need_discard_confirm(cJSON *root);
void reply_need_discard(int id, const char *cmd);
void cmd_hello(int id);
int check_api_key(int id, const char *cmd);
int check_project(int id, const char *cmd);
void cmd_dispatch(cJSON *root);
HRESULT STDMETHODCALLTYPE CtrlCB_QI(     ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, REFIID riid, void **ppv);
ULONG STDMETHODCALLTYPE CtrlCB_AddRef(     ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This);
ULONG STDMETHODCALLTYPE CtrlCB_Release(     ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This);
HRESULT STDMETHODCALLTYPE EnvCB_Invoke(     ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,     HRESULT errorCode, ICoreWebView2Environment *env);
HRESULT STDMETHODCALLTYPE MsgCB_Invoke(     ICoreWebView2WebMessageReceivedEventHandler *This,     ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args);
int b64_val(char c);
unsigned char *b64_decode(const char *in, int *outLen);
HRESULT STDMETHODCALLTYPE CdpCB_Invoke(     ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *This,     HRESULT errorCode, LPCWSTR jsonResult);
void write_png_bytes(const void *data, size_t len);
void request_screenshot_cdp(void);
HRESULT STDMETHODCALLTYPE EsCB_Invoke(     ICoreWebView2ExecuteScriptCompletedHandler *This,     HRESULT errorCode, LPCWSTR resultJson);
void test_execute_script(void);
HRESULT STDMETHODCALLTYPE CapCB_Invoke(     ICoreWebView2CapturePreviewCompletedHandler *This, HRESULT errorCode);
void request_screenshot(void);
HRESULT STDMETHODCALLTYPE NavCB_Invoke(     ICoreWebView2NavigationCompletedEventHandler *This,     ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args);
char *read_ui_file(const char *name);
void navigate_inline(void);
void navigate_file_url(void);
HRESULT STDMETHODCALLTYPE CtrlCB_Invoke(     ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,     HRESULT errorCode, ICoreWebView2Controller *controller);
void handle_task_done(task_done_t *t);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void enable_dark_titlebar(HWND hwnd);
void enable_dpi_awareness(void);
int load_loader(void);
void init_webview(void);
void load_app_state(void);
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow);

#endif /* HOST_INTERNAL_H */
