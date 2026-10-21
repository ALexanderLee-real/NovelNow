/* ai_api.c - public API: config/response/retry/abort/chat/models (2026-10-18 split from legacy ai_api.c, zero behavior change) */
#include "ai_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ============== 限流与重试（免费/限流额度模型友好） ============== */

static INIT_ONCE g_throttle_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_throttle_cs;
static ULONGLONG g_last_request_tick = 0;

static BOOL CALLBACK throttle_init_fn(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_throttle_cs);
    return TRUE;
}

/* 请求发出前的节流闸：把相邻两次 AI 请求（含流式）的间隔拉到
   cfg->rate_limit_ms 以上。所有工作线程共用一把闸，全自动写作
   连发请求时也会被依次排开。 */
/* abortable_sleep 的声明见 ai_internal.h（comfy 轮询也要用） */

static void ai_throttle_wait(const ai_config_t *cfg)
{
    if (!cfg || cfg->rate_limit_ms <= 0) return;
    InitOnceExecuteOnce(&g_throttle_once, throttle_init_fn, NULL, NULL);
    ULONGLONG interval = (ULONGLONG)cfg->rate_limit_ms;
    ULONGLONG wait = 0;
    EnterCriticalSection(&g_throttle_cs);
    {
        ULONGLONG now = GetTickCount64();
        if (g_last_request_tick && now < g_last_request_tick + interval)
            wait = g_last_request_tick + interval - now;
        /* 先占位再睡：并发线程进来时拿到的是排在自己的位次之后的时刻 */
        g_last_request_tick = now + wait;
    }
    LeaveCriticalSection(&g_throttle_cs);
    if (wait) abortable_sleep(g_abort_ai, (DWORD)wait);
}

/* 判断错误是否值得重试：限流(429)、服务端 5xx、连接类失败。
   401/400 这类重试也不会好，不浪费次数。 */
static int retryable_error(const char *err)
{
    if (!err) return 0;
    if (strstr(err, "用户取消")) return 0;   /* 中止请求绝不重试 */
    if (strstr(err, "HTTP 429")) return 1;
    if (strstr(err, "HTTP 500") || strstr(err, "HTTP 502") ||
        strstr(err, "HTTP 503") || strstr(err, "HTTP 504")) return 1;
    if (strstr(err, "Connection lost")) return 1;
    if (strstr(err, "Request failed")) return 1;
    if (strstr(err, "rate limit") || strstr(err, "Rate limit") ||
        strstr(err, "rate_limit")) return 1;
    return 0;
}

/* ============== 请求中止（点取消立即掐断在途请求） ============== */

INIT_ONCE g_abort_once = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_abort_cs;   /* 一把锁护两个通道的在途链表，够用 */

/* 前置声明：两个结构互相引用 */
struct ai_req_node;

/* 取消通道上下文：flag=中止标记，reqs=该通道在途请求链表 */
struct abort_ctx {
    volatile LONG flag;
    struct ai_req_node *reqs;
};

/* 在途请求节点：句柄 + 「取消线程已代关」标记。
   标记必须挂在节点上而不是记全局句柄值——句柄值会被新请求复用，
   靠值比对会误杀他人连接（单槽 g_active_req/g_aborted_req 时代的教训）。 */
struct ai_req_node {
    HINTERNET h;
    volatile LONG aborted;
    struct abort_ctx *ctx;
    struct ai_req_node *next;
};

static struct abort_ctx g_ctx_ai = { 0, NULL };
static struct abort_ctx g_ctx_comfy = { 0, NULL };
abort_ctx_t *g_abort_ai = &g_ctx_ai;
abort_ctx_t *g_abort_comfy = &g_ctx_comfy;

static BOOL CALLBACK abort_init_fn(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_abort_cs);
    return TRUE;
}

void abort_lock(void)
{
    InitOnceExecuteOnce(&g_abort_once, abort_init_fn, NULL, NULL);
}

void abort_ctx_abort(abort_ctx_t *ctx)
{
    abort_lock();
    InterlockedExchange(&ctx->flag, 1);
    /* 从别的线程关闭句柄会让阻塞中的 InternetReadFile 立刻失败返回，
       这是 WinInet 官方认可的中断方式 */
    EnterCriticalSection(&g_abort_cs);
    for (struct ai_req_node *n = ctx->reqs; n; n = n->next) {
        n->aborted = 1;
        InternetCloseHandle(n->h);
    }
    LeaveCriticalSection(&g_abort_cs);
}

/* 对外惯用接口：掐 AI 通道（engine/host 的取消按钮走这里） */
void ai_abort_current(void)
{
    abort_ctx_abort(g_abort_ai);
}

/* 关窗等场景：两条通道都掐，AI 与 comfy 的阻塞读/轮询全部立刻醒 */
void ai_abort_all(void)
{
    abort_ctx_abort(g_abort_ai);
    abort_ctx_abort(g_abort_comfy);
}

int abort_ctx_aborted(abort_ctx_t *ctx)
{
    return InterlockedCompareExchange(&ctx->flag, 0, 0) != 0;
}

int ai_aborted(void)
{
    return abort_ctx_aborted(g_abort_ai);
}

/* 新任务开始时清掉上一次的中止标记（engine_clear_cancel 调用） */
void abort_ctx_reset(abort_ctx_t *ctx)
{
    abort_lock();
    InterlockedExchange(&ctx->flag, 0);
    /* 节点自带「已代关」标记随节点生灭，不再需要作废全局代关记录 */
}

void ai_abort_reset(void)
{
    abort_ctx_reset(g_abort_ai);
}

ai_req_node_t *ai_req_register(abort_ctx_t *ctx, HINTERNET h)
{
    struct ai_req_node *n;
    abort_lock();
    n = (struct ai_req_node *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->h = h;
    n->ctx = ctx;
    EnterCriticalSection(&g_abort_cs);
    n->next = ctx->reqs;
    ctx->reqs = n;
    LeaveCriticalSection(&g_abort_cs);
    return n;
}

/* 摘除并释放节点。返回 1 = 取消线程已代关句柄，调用方不得再关
   （句柄值可能已被新请求复用，二次关闭会误杀他人连接） */
int ai_req_unregister(ai_req_node_t *n)
{
    int closed_by_abort;
    if (!n) return 0;
    EnterCriticalSection(&g_abort_cs);
    for (struct ai_req_node **pp = &n->ctx->reqs; *pp; pp = &(*pp)->next) {
        if (*pp == n) { *pp = n->next; break; }
    }
    closed_by_abort = n->aborted ? 1 : 0;
    LeaveCriticalSection(&g_abort_cs);
    free(n);
    return closed_by_abort;
}

void abort_err(char **err_out)
{
    if (err_out) {
        free(*err_out);
        *err_out = str_dup("操作已被用户取消");
    }
}

/* 可打断睡眠：按 100ms 切片，abort/退出时立刻醒。
   没有它，WM_DESTROY 等工作线程收场（3 秒预算）就是空等——线程可能
   正卡在 30 秒退避或限流睡眠里，而关窗不能陪它睡（P1）。 */
void abortable_sleep(abort_ctx_t *ctx, DWORD ms)
{
    DWORD waited = 0;
    while (waited < ms) {
        if (abort_ctx_aborted(ctx)) return;
        Sleep(100);
        waited += 100;
    }
}

/* 指数退避：2s, 4s, 8s ... 封顶 30s */
static void retry_backoff_sleep(int attempt)
{
    DWORD ms = 2000u << (attempt > 4 ? 4 : attempt);
    if (ms > 30000) ms = 30000;
    abortable_sleep(g_abort_ai, ms);
}

/* ============== 配置管理 ============== */

ai_config_t* ai_config_create(void)
{
    ai_config_t *cfg = (ai_config_t*)calloc(1, sizeof(ai_config_t));
    if (!cfg) return NULL;
    cfg->api_url = str_dup("https://api.deepseek.com");
    cfg->api_key = str_dup("");
    cfg->model = str_dup("deepseek-flash");
    /* str_dup 失败（OOM）必须当场报：后面 cJSON_AddStringToObject 会对 NULL 字段
       调 strlen，直接崩（P0-⑨） */
    if (!cfg->api_url || !cfg->api_key || !cfg->model) {
        ai_config_destroy(cfg);
        return NULL;
    }
    cfg->temperature = 0.7;
    /* 2048 会被思考型模型的推理阶段耗尽导致空正文；DEFAULT 是 deepseek 全系的安全输出上限 */
    cfg->max_tokens = AI_MAX_TOKENS_DEFAULT;
    cfg->timeout = 120;
    cfg->rate_limit_ms = 0;   /* 默认不限流：付费端点没必要 */
    cfg->max_retries = 2;     /* 默认重试 2 次：限流端点白嫖党的基本尊严 */
    return cfg;
}

void ai_config_destroy(ai_config_t *cfg)
{
    if (!cfg) return;
    if (cfg->api_url) free(cfg->api_url);
    if (cfg->api_key) free(cfg->api_key);
    if (cfg->model) free(cfg->model);
    free(cfg);
}

/* ============== 响应管理 ============== */

ai_response_t* ai_response_create(void)
{
    ai_response_t *resp = (ai_response_t*)calloc(1, sizeof(ai_response_t));
    return resp;
}

void ai_response_destroy(ai_response_t *resp)
{
    if (!resp) return;
    if (resp->content) free(resp->content);
    if (resp->error) free(resp->error);
    free(resp);
}

ai_response_t* ai_chat(ai_config_t *cfg, chat_message_t *messages, int msg_count)
{
    ai_response_t *resp = ai_response_create();
    if (!resp) return NULL;
    
    if (!cfg || !cfg->api_url || !cfg->api_key || !*cfg->api_key) {
        resp->error = str_dup("API key not configured");
        return resp;
    }
    
    char *endpoint = resolve_chat_endpoint(cfg->api_url);
    if (!endpoint) {
        resp->error = str_dup("Invalid API URL");
        return resp;
    }
    
    /* Build request JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", cfg->model);
    cJSON_AddNumberToObject(root, "temperature", cfg->temperature);
    if (cfg->max_tokens > 0) {
        cJSON_AddNumberToObject(root, "max_tokens", cfg->max_tokens);
    }
    
    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; i < msg_count; i++) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", messages[i].role);
        cJSON_AddStringToObject(msg, "content", messages[i].content);
        cJSON_AddItemToArray(msgs, msg);
    }
    cJSON_AddItemToObject(root, "messages", msgs);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        free(endpoint);
        resp->error = str_dup("Failed to build request JSON");
        return resp;
    }

    /* 请求体整体必须是合法 UTF-8：任何漏网的非法字节（编码混杂、解析切字）
       都会让服务端解析失败并返回 400 invalid unicode code point */
    {
        char *json_safe = utf8_sanitize(json_str);
        if (json_safe) {
            free(json_str);
            json_str = json_safe;
        }
    }
    
    /* Prepare headers */
    char auth_header[4096];
    int ah_len = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", cfg->api_key);
    if (ah_len < 0 || (size_t)ah_len >= sizeof(auth_header)) {
        /* 截断的鉴权头只会换来莫名其妙的 401，不如直接说清楚（P1-⑯） */
        resp->error = str_dup("API Key 过长，无法构造鉴权头");
        free(json_str);
        return resp;
    }
    const char *headers[] = {
        "Content-Type: application/json",
        auth_header
    };
    
    /* Send request + parse（限流/5xx/连接错误自动重试，指数退避） */
    int attempts = (cfg->max_retries > 0 ? cfg->max_retries : 0) + 1;
    cJSON *resp_json = NULL;
    char *http_err = NULL;
    int retry;
    for (int attempt = 0; ; attempt++) {
        char *response_body;
        retry = 0;
        ai_throttle_wait(cfg);
        response_body = http_request("POST", endpoint, headers, 2, json_str, strlen(json_str),
                                     cfg->timeout, &http_err);
        if (!response_body) {
            if (http_err && retryable_error(http_err) && attempt + 1 < attempts) {
                free(http_err); http_err = NULL;
                retry = 1;
            } else {
                resp->error = http_err ? http_err : str_dup("Unknown HTTP error");
                http_err = NULL;
            }
        } else {
            resp_json = cJSON_Parse(response_body);
            free(response_body);
            if (!resp_json) {
                /* 网关偶发吐 HTML/半截报文，重试常有救；重试耗尽才报错 */
                if (attempt + 1 < attempts) retry = 1;
                else resp->error = str_dup("Failed to parse response JSON");
            }
        }

        if (!retry && resp_json) {
            /* Extract error if present */
            cJSON *err_obj = cJSON_GetObjectItem(resp_json, "error");
            if (err_obj) {
                cJSON *err_msg = cJSON_GetObjectItem(err_obj, "message");
                if (err_msg && cJSON_IsString(err_msg)) {
                    resp->error = str_dup(err_msg->valuestring);
                }
            }

            if (!resp->error) {
                /* Extract choices */
                cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
                if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                    cJSON *choice = cJSON_GetArrayItem(choices, 0);
                    cJSON *msg_obj = cJSON_GetObjectItem(choice, "message");
                    if (msg_obj) {
                        cJSON *content = cJSON_GetObjectItem(msg_obj, "content");
                        if (content && cJSON_IsString(content)) {
                            /* 空正文必须算失败：推理模型思考耗尽 token 时 content 是空串，
                               若照常返回成功，上层会把空字符串写进章节，产生"只有标题没有正文"的空章 */
                            const char *p = content->valuestring;
                            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                            if (*p) {
                                resp->content = str_dup(content->valuestring);
                                resp->success = 1;
                            } else {
                                cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
                                char errbuf[512];
                                snprintf(errbuf, sizeof(errbuf),
                                    "AI 返回了空正文（finish_reason=%s）。"
                                    "若使用推理模型，思考阶段可能耗尽了输出额度；"
                                    "请在设置中调大\"最大输出Tokens\"后重试",
                                    (finish && cJSON_IsString(finish)) ? finish->valuestring : "unknown");
                                resp->error = str_dup(errbuf);
                            }
                        }
                    }
                }

                /* Extract usage */
                cJSON *usage = cJSON_GetObjectItem(resp_json, "usage");
                if (usage) {
                    cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
                    cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
                    cJSON *tt = cJSON_GetObjectItem(usage, "total_tokens");
                    if (pt && cJSON_IsNumber(pt)) resp->prompt_tokens = pt->valueint;
                    if (ct && cJSON_IsNumber(ct)) resp->completion_tokens = ct->valueint;
                    if (tt && cJSON_IsNumber(tt)) resp->total_tokens = tt->valueint;
                }
            }
            cJSON_Delete(resp_json);
            resp_json = NULL;

            if (!resp->success && !resp->error) {
                resp->error = str_dup("No content in response");
            }
        }

        /* JSON 层报的错也过一遍重试判定（如正文里的 rate limit 文案） */
        if (!retry && !resp->success && resp->error &&
            attempt + 1 < attempts && retryable_error(resp->error)) {
            free(resp->error); resp->error = NULL;
            retry = 1;
        }

        if (retry) {
            retry_backoff_sleep(attempt);
            continue;
        }
        break;
    }

    free(http_err);
    free(json_str);
    free(endpoint);

    return resp;
}

ai_response_t* ai_chat_single(ai_config_t *cfg, const char *system_prompt, const char *user_prompt)
{
    if (!cfg || !user_prompt) return NULL;

    chat_message_t msgs[2];
    int count = 0;

    if (system_prompt && *system_prompt) {
        msgs[count].role = "system";
        msgs[count].content = (char*)system_prompt;
        count++;
    }
    msgs[count].role = "user";
    msgs[count].content = (char*)user_prompt;
    count++;

    return ai_chat(cfg, msgs, count);
}

ai_response_t* ai_chat_stream(ai_config_t *cfg, chat_message_t *messages, int msg_count,
                              ai_stream_cb cb, void *user)
{
    ai_response_t *resp = ai_response_create();
    if (!resp) return NULL;

    if (!cfg || !cfg->api_url || !cfg->api_key || !*cfg->api_key) {
        resp->error = str_dup("API key not configured");
        return resp;
    }

    char *endpoint = resolve_chat_endpoint(cfg->api_url);
    if (!endpoint) {
        resp->error = str_dup("Invalid API URL");
        return resp;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", cfg->model);
    cJSON_AddNumberToObject(root, "temperature", cfg->temperature);
    if (cfg->max_tokens > 0) cJSON_AddNumberToObject(root, "max_tokens", cfg->max_tokens);
    cJSON_AddBoolToObject(root, "stream", 1);

    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; i < msg_count; i++) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", messages[i].role);
        cJSON_AddStringToObject(msg, "content", messages[i].content);
        cJSON_AddItemToArray(msgs, msg);
    }
    cJSON_AddItemToObject(root, "messages", msgs);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        free(endpoint);
        resp->error = str_dup("Failed to build request JSON");
        return resp;
    }
    {
        char *json_safe = utf8_sanitize(json_str);
        if (json_safe) { free(json_str); json_str = json_safe; }
    }

    char auth_header[4096];
    int ah_len = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", cfg->api_key);
    if (ah_len < 0 || (size_t)ah_len >= sizeof(auth_header)) {
        resp->error = str_dup("API Key 过长，无法构造鉴权头");
        free(json_str);
        return resp;
    }
    const char *headers[] = {
        "Content-Type: application/json",
        auth_header,
        "Accept: text/event-stream"
    };

    sse_ctx_t sse;
    memset(&sse, 0, sizeof(sse));

    /* 限流/5xx/连接错误自动重试。只在前置阶段失败（还没往回调里吐过任何
       增量）时才重试：正文吐到一半断流的话重发会把已有增量再吐一遍。 */
    int attempts = (cfg->max_retries > 0 ? cfg->max_retries : 0) + 1;
    char *http_err = NULL;
    char *response_body = NULL;
    const char *final_err = NULL;
    int retry;
    for (int attempt = 0; ; attempt++) {
        retry = 0;
        sse.cb = cb; sse.ud = user;
        http_buf_init(&sse.pend);
        http_buf_init(&sse.content);

        ai_throttle_wait(cfg);
        free(http_err); http_err = NULL;
        response_body = http_request_ex("POST", endpoint, headers, 3, json_str, strlen(json_str),
                                        cfg->timeout, &http_err, sse_on_chunk, &sse);
        final_err = sse.error ? sse.error : (http_err ? http_err : NULL);

        if (!response_body) {
            int retriable = final_err && retryable_error(final_err);
            int had_output = sse.content.length > 0;
            if (retriable && !had_output && attempt + 1 < attempts) {
                sse_reset(&sse);
                retry = 1;
            } else {
                resp->error = str_dup(final_err ? final_err : "Unknown HTTP error");
            }
        } else {
            free(response_body);
            if (sse.content.oom) {
                /* 中途 realloc 失败丢过 delta，正文已截断，不能当成功返回（P1-⑭） */
                resp->error = str_dup("响应读取内存不足，正文被截断");
            } else {
            const char *acc = sse.content.data ? sse.content.data : "";
            const char *p = acc;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (*p) {
                resp->content = str_dup(acc);
                resp->success = 1;
            } else if (sse.error) {
                resp->error = str_dup(sse.error);
            } else {
                resp->error = str_dup("AI 返回了空正文（流式）。若使用推理模型，"
                                      "请调大\"最大输出Tokens\"后重试");
            }
            }
        }

        if (!retry && !resp->success && resp->error &&
            attempt + 1 < attempts && retryable_error(resp->error) &&
            sse.content.length == 0) {
            free(resp->error); resp->error = NULL;
            sse_reset(&sse);
            retry = 1;
        }

        if (retry) {
            retry_backoff_sleep(attempt);
            continue;
        }
        break;
    }

    free(http_err);
    free(json_str);
    free(endpoint);
    sse_reset(&sse);
    return resp;
}

ai_response_t* ai_chat_single_stream(ai_config_t *cfg, const char *system_prompt,
                                     const char *user_prompt, ai_stream_cb cb, void *user)
{
    if (!cfg || !user_prompt) return NULL;

    chat_message_t msgs[2];
    int count = 0;

    if (system_prompt && *system_prompt) {
        msgs[count].role = "system";
        msgs[count].content = (char*)system_prompt;
        count++;
    }
    msgs[count].role = "user";
    msgs[count].content = (char*)user_prompt;
    count++;

    return ai_chat_stream(cfg, msgs, count, cb, user);
}

/* ============== 获取模型列表 ============== */

static char* build_models_url(const char *api_url)
{
    const char *pos = strstr(api_url, "chat/completions");
    if (pos) {
        size_t base_len = (size_t)(pos - api_url);
        char *url = (char*)malloc(base_len + 8);
        if (!url) return NULL;
        memcpy(url, api_url, base_len);
        strcpy(url + base_len, "models");
        return url;
    } else {
        size_t len = strlen(api_url);
        char *url = (char*)malloc(len + 8);
        if (!url) return NULL;
        strcpy(url, api_url);
        if (len > 0 && url[len - 1] != '/') strcat(url, "/");
        strcat(url, "models");
        return url;
    }
}

char** ai_list_models(ai_config_t *cfg, int *count)
{
    char *models_url, *body;
    char auth_header[4096];
    const char *headers[1];
    cJSON *root, *data;
    char **models;
    int n, cnt, i;

    if (count) *count = 0;
    if (!cfg || !cfg->api_url || !cfg->api_key || !*cfg->api_key) return NULL;

    models_url = build_models_url(cfg->api_url);
    if (!models_url) return NULL;

    int ah_len = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", cfg->api_key);
    if (ah_len < 0 || (size_t)ah_len >= sizeof(auth_header)) {
        free(models_url);
        return NULL;   /* 截断的鉴权头只会换来莫名其妙的 401（P1-⑯） */
    }
    headers[0] = auth_header;

    body = http_request("GET", models_url, headers, 1, NULL, 0, cfg->timeout, NULL);
    free(models_url);
    if (!body) return NULL;

    root = cJSON_Parse(body);
    free(body);
    if (!root) return NULL;

    data = cJSON_GetObjectItem(root, "data");
    if (!data || !cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return NULL;
    }

    n = cJSON_GetArraySize(data);
    models = (char**)calloc((size_t)n + 1, sizeof(char*));
    if (!models) {
        cJSON_Delete(root);
        return NULL;
    }

    cnt = 0;
    for (i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(data, i);
        cJSON *id = item ? cJSON_GetObjectItem(item, "id") : NULL;
        if (id && cJSON_IsString(id) && id->valuestring && *id->valuestring) {
            models[cnt++] = str_dup(id->valuestring);
        }
    }
    cJSON_Delete(root);

    if (count) *count = cnt;
    return models;
}

void ai_list_models_free(char **models, int count)
{
    int i;
    if (!models) return;
    for (i = 0; i < count; i++) free(models[i]);
    free(models);
}
