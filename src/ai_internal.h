/* ai_internal.h - internal sharing across ai_http.c / ai_sse.c / ai_api.c
   (2026-10-18 structural split from the legacy ai_api.c monolith, zero behavior change).
   Not for host/engine consumers: the public surface stays in ai_api.h. */
#ifndef AI_INTERNAL_H
#define AI_INTERNAL_H

#include "ai_api.h"
#include "utils.h"
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
#else
#error "This version only supports Windows"
#endif

/* ---- shared structs (verbatim from legacy ai_api.c) ---- */

typedef struct {
    char scheme[16];
    char host[256];
    int port;
    char path[1024];
} url_parts_t;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int oom;            /* realloc 失败丢过数据：不能把截断的半截响应当成功（P1-⑭） */
} http_buffer_t;

typedef struct {
    http_buffer_t pend;     /* 未凑成完整行的残余字节 */
    http_buffer_t content;  /* 累积的正文 */
    char *error;            /* 流内 error 事件的消息 */
    ai_stream_cb cb;
    void *ud;
    int done;
} sse_ctx_t;

/* ---- ai_http.c ---- */
void http_buf_init(http_buffer_t *buf);
void http_buf_append(http_buffer_t *buf, const char *data, size_t len);
void http_buf_free(http_buffer_t *buf);
char* http_request_ex(const char *method, const char *url, const char *headers[],
                     int header_count, const char *body, size_t body_len,
                     int timeout_sec, char **err_out,
                     void (*on_body)(const char *data, size_t len, void *ud), void *ud);
char* http_request(const char *method, const char *url, const char *headers[],
                  int header_count, const char *body, size_t body_len,
                  int timeout_sec, char **err_out);
char* resolve_chat_endpoint(const char *api_url);

/* ---- ai_sse.c ---- */
void sse_reset(sse_ctx_t *s);
void sse_on_chunk(const char *data, size_t len, void *ud);

/* ---- ai_api.c (abort channel, read by request code) ----
   取消通道上下文化（2026-10-19）：两个独立通道——g_abort_ai（AI 文本请求）
   与 g_abort_comfy（ComfyUI 图片请求）。产品语义「并行互不打断」（见
   comfy_api.c 头注释）由通道隔离保证：取消 AI 不掐 comfy、取消 comfy 不掐
   AI；关窗走 ai_abort_all() 两条都掐。
   在途请求登记成链表节点，节点自带「取消线程已代关」标记——句柄值可能被
   新请求复用，靠全局记句柄值会误杀他人连接（单槽 g_active_req/g_aborted_req
   时代的教训，已废除）。 */
typedef struct abort_ctx abort_ctx_t;
typedef struct ai_req_node ai_req_node_t;
extern abort_ctx_t *g_abort_ai;
extern abort_ctx_t *g_abort_comfy;
void abort_lock(void);
int ai_aborted(void);                       /* 读 g_abort_ai（AI 请求侧惯用） */
int abort_ctx_aborted(abort_ctx_t *ctx);
void abort_ctx_abort(abort_ctx_t *ctx);     /* 掐断该通道所有在途请求 */
void abort_ctx_reset(abort_ctx_t *ctx);
void abort_err(char **err_out);
void abortable_sleep(abort_ctx_t *ctx, DWORD ms);
ai_req_node_t *ai_req_register(abort_ctx_t *ctx, HINTERNET h);  /* NULL=OOM，调用方自己关句柄 */
int ai_req_unregister(ai_req_node_t *n);    /* 1=取消线程已代关，调用方不得再关 */
extern INIT_ONCE g_abort_once;
extern CRITICAL_SECTION g_abort_cs;

#endif /* AI_INTERNAL_H */

