#include "ai_api.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
#else
#error "This version only supports Windows"
#endif

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
    if (wait) Sleep((DWORD)wait);
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

static INIT_ONCE g_abort_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_abort_cs;
static volatile LONG g_abort_flag = 0;
static HINTERNET g_active_req = NULL;   /* 当前在途请求的 WinInet 句柄 */

static BOOL CALLBACK abort_init_fn(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_abort_cs);
    return TRUE;
}

static void abort_lock(void)
{
    InitOnceExecuteOnce(&g_abort_once, abort_init_fn, NULL, NULL);
}

void ai_abort_current(void)
{
    abort_lock();
    InterlockedExchange(&g_abort_flag, 1);
    /* 从别的线程关闭句柄会让阻塞中的 InternetReadFile 立刻失败返回，
       这是 WinInet 官方认可的中断方式 */
    EnterCriticalSection(&g_abort_cs);
    if (g_active_req) {
        InternetCloseHandle(g_active_req);
        g_active_req = NULL;   /* 请求线程稍后清理时会再关一次，重复关闭安全 */
    }
    LeaveCriticalSection(&g_abort_cs);
}

static int ai_aborted(void)
{
    return InterlockedCompareExchange(&g_abort_flag, 0, 0) != 0;
}

/* 新任务开始时清掉上一次的中止标记（engine_clear_cancel 调用） */
void ai_abort_reset(void)
{
    abort_lock();
    InterlockedExchange(&g_abort_flag, 0);
}

static void abort_err(char **err_out)
{
    if (err_out) {
        free(*err_out);
        *err_out = str_dup("操作已被用户取消");
    }
}

/* 指数退避：2s, 4s, 8s ... 封顶 30s */
static void retry_backoff_sleep(int attempt)
{
    DWORD ms = 2000u << (attempt > 4 ? 4 : attempt);
    if (ms > 30000) ms = 30000;
    Sleep(ms);
}

/* ============== 配置管理 ============== */

ai_config_t* ai_config_create(void)
{
    ai_config_t *cfg = (ai_config_t*)calloc(1, sizeof(ai_config_t));
    if (!cfg) return NULL;
    cfg->api_url = str_dup("https://api.deepseek.com");
    cfg->api_key = str_dup("");
    cfg->model = str_dup("deepseek-flash");
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

/* ============== URL 解析 ============== */

typedef struct {
    char scheme[16];
    char host[256];
    int port;
    char path[1024];
} url_parts_t;

static int parse_url(const char *url, url_parts_t *parts)
{
    if (!url || !parts) return -1;
    memset(parts, 0, sizeof(*parts));
    
    const char *p = url;
    
    /* scheme */
    const char *scheme_end = strstr(p, "://");
    if (scheme_end) {
        size_t slen = scheme_end - p;
        if (slen >= sizeof(parts->scheme)) return -1;
        memcpy(parts->scheme, p, slen);
        parts->scheme[slen] = '\0';
        p = scheme_end + 3;
    } else {
        strcpy(parts->scheme, "https");
    }
    
    /* host：端口冒号在路径之前时主机名必须止于冒号，
       否则 host 会带上端口（如 "127.0.0.1:8765"），InternetConnectA
       会拼出 "127.0.0.1:8765:8765" 并报 12005 无效 URL */
    const char *path_start = strchr(p, '/');
    const char *port_start = strchr(p, ':');
    const char *host_end;
    if (port_start && (!path_start || port_start < path_start)) {
        host_end = port_start;
    } else if (path_start) {
        host_end = path_start;
    } else {
        host_end = p + strlen(p);
    }
    size_t hlen = host_end - p;
    if (hlen >= sizeof(parts->host)) return -1;
    memcpy(parts->host, p, hlen);
    parts->host[hlen] = '\0';
    
    /* port */
    if (port_start && (!path_start || port_start < path_start)) {
        parts->port = atoi(port_start + 1);
    } else {
        if (strcmp(parts->scheme, "https") == 0) parts->port = 443;
        else parts->port = 80;
    }
    
    /* path */
    if (path_start) {
        strncpy(parts->path, path_start, sizeof(parts->path) - 1);
        parts->path[sizeof(parts->path) - 1] = '\0';
    } else {
        strcpy(parts->path, "/");
    }
    
    return 0;
}

/* ============== HTTP POST ============== */

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} http_buffer_t;

static void http_buf_init(http_buffer_t *buf)
{
    buf->capacity = 4096;
    buf->data = (char*)malloc(buf->capacity);
    buf->length = 0;
    if (buf->data) buf->data[0] = '\0';
}

static void http_buf_append(http_buffer_t *buf, const char *data, size_t len)
{
    /* data 可能为 NULL（初始 malloc 失败）：此时必须先尝试分配，
       否则 memcpy(NULL+len, ...) 直接崩溃 */
    if (!buf->data || buf->length + len + 1 > buf->capacity) {
        size_t cap = buf->capacity ? buf->capacity : 4096;
        while (cap < buf->length + len + 1) cap *= 2;
        char *new_data = (char*)realloc(buf->data, cap);
        if (!new_data) return;
        buf->data = new_data;
        buf->capacity = cap;
    }
    memcpy(buf->data + buf->length, data, len);
    buf->length += len;
    buf->data[buf->length] = '\0';
}

static void http_buf_free(http_buffer_t *buf)
{
    if (buf->data) free(buf->data);
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

/* POST 与 GET 共用同一套连接/收发/错误处理流程，仅动词与请求体不同。
   on_body 非空时（流式模式）：每读到一段响应体就回调一次，调用方在回调里
   做增量处理；resp_buf 仍然照常累积，保证错误路径能拿到完整报文。 */
static char* http_request_ex(const char *method, const char *url, const char *headers[],
                             int header_count, const char *body, size_t body_len,
                             int timeout_sec, char **err_out,
                             void (*on_body)(const char *data, size_t len, void *ud), void *ud)
{
    url_parts_t parts;
    if (parse_url(url, &parts) != 0) {
        if (err_out) *err_out = str_dup("Failed to parse URL");
        return NULL;
    }

    HINTERNET hInet = InternetOpenA("novelnow/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInet) {
        if (err_out) *err_out = str_dup("Failed to initialize WinInet");
        return NULL;
    }

    /* 使用配置里的超时（timeout 秒），不再写死 120 秒 */
    DWORD timeout_ms;
    if (timeout_sec < 5) timeout_sec = 5;
    if (timeout_sec > 3600) timeout_sec = 3600;
    timeout_ms = (DWORD)timeout_sec * 1000;
    InternetSetOptionA(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    InternetSetOptionA(hInet, INTERNET_OPTION_SEND_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    InternetSetOptionA(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));

    DWORD flags = (strcmp(parts.scheme, "https") == 0) ? INTERNET_FLAG_SECURE : 0;
    HINTERNET hConnect = InternetConnectA(hInet, parts.host, (INTERNET_PORT)parts.port,
                                          NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) {
        if (err_out) *err_out = str_dup("Failed to connect to server");
        InternetCloseHandle(hInet);
        return NULL;
    }

    HINTERNET hReq = HttpOpenRequestA(hConnect, method, parts.path,
                                      NULL, NULL, NULL, flags, 0);
    if (!hReq) {
        if (err_out) *err_out = str_dup("Failed to create request");
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInet);
        return NULL;
    }

    /* Add headers */
    for (int i = 0; i < header_count; i++) {
        HttpAddRequestHeadersA(hReq, headers[i], -1, HTTP_ADDREQ_FLAG_ADD);
    }

    /* Send request（GET 时 body 为 NULL / 0） */
    BOOL sent = HttpSendRequestA(hReq, NULL, 0, (LPVOID)body, (DWORD)body_len);
    if (!sent) {
        DWORD err = GetLastError();
        InternetCloseHandle(hReq);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInet);
        if (ai_aborted()) {
            abort_err(err_out);
            return NULL;
        }
        char err_buf[512];
        snprintf(err_buf, sizeof(err_buf),
                 "Request failed with error %lu (%s %s://%s:%d%s)",
                 err, method, parts.scheme, parts.host, parts.port, parts.path);
        if (err_out) *err_out = str_dup(err_buf);
        return NULL;
    }

    /* 登记在途句柄：取消线程会关闭它来立即中断阻塞读 */
    abort_lock();
    EnterCriticalSection(&g_abort_cs);
    g_active_req = hReq;
    LeaveCriticalSection(&g_abort_cs);

    /* Read response */
    http_buffer_t resp_buf;
    http_buf_init(&resp_buf);
    char chunk[4096];
    DWORD bytes_read;
    BOOL read_ok = TRUE;
    for (;;) {
        if (ai_aborted()) {
            read_ok = FALSE;
            break;
        }
        if (!InternetReadFile(hReq, chunk, sizeof(chunk) - 1, &bytes_read)) {
            read_ok = FALSE;   /* 必须与 bytes_read==0 区分：连接中断时返回
                                  半截 JSON 会被当成成功响应 */
            break;
        }
        if (bytes_read == 0) break;
        http_buf_append(&resp_buf, chunk, bytes_read);
        if (on_body) on_body(chunk, bytes_read, ud);
    }
    /* 注销在途句柄（真正的关闭统一放请求线程，重复关闭安全） */
    EnterCriticalSection(&g_abort_cs);
    g_active_req = NULL;
    LeaveCriticalSection(&g_abort_cs);

    if (ai_aborted()) {
        http_buf_free(&resp_buf);
        InternetCloseHandle(hReq);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInet);
        abort_err(err_out);
        return NULL;
    }
    if (!read_ok || !resp_buf.data) {
        if (err_out) *err_out = str_dup(read_ok ? "响应读取内存不足"
                                                : "Connection lost while reading response");
        http_buf_free(&resp_buf);
        InternetCloseHandle(hReq);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInet);
        return NULL;
    }

    /* Check status code */
    char status_code[32] = {0};
    DWORD status_len = sizeof(status_code);
    HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE, status_code, &status_len, NULL);
    int code = atoi(status_code);
    if (code < 200 || code >= 300) {
        if (err_out) {
            char *err_msg = (char*)malloc(512 + resp_buf.length);
            if (err_msg) {
                snprintf(err_msg, 512 + resp_buf.length, "HTTP %d: %.*s", code,
                         (int)(resp_buf.length > 512 ? 512 : resp_buf.length),
                         resp_buf.data ? resp_buf.data : "");
                *err_out = err_msg;
            }
        }
        http_buf_free(&resp_buf);
        InternetCloseHandle(hReq);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInet);
        return NULL;
    }

    InternetCloseHandle(hReq);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInet);

    char *result = resp_buf.data;
    resp_buf.data = NULL;
    http_buf_free(&resp_buf);
    return result;
}

/* 非流式调用方（ai_chat / ai_list_models）的原接口保持不变 */
static char* http_request(const char *method, const char *url, const char *headers[],
                          int header_count, const char *body, size_t body_len,
                          int timeout_sec, char **err_out)
{
    return http_request_ex(method, url, headers, header_count, body, body_len,
                           timeout_sec, err_out, NULL, NULL);
}

/* ============== 聊天 API ============== */

/* 用户可能只填 base_url（如 https://api.deepseek.com），也可能填完整端点。
   这里统一推导出真正的 chat/completions 地址，避免请求打到站点根路径而返回 404。 */
static char* resolve_chat_endpoint(const char *api_url)
{
    if (!api_url || !*api_url) return NULL;

    /* 已经是完整端点 */
    if (strstr(api_url, "/chat/completions")) return str_dup(api_url);

    size_t len = strlen(api_url);
    while (len > 0 && api_url[len - 1] == '/') len--;

    const char *suffix;
    if (len >= 3 && memcmp(api_url + len - 3, "/v1", 3) == 0) {
        suffix = "/chat/completions";
    } else if (strstr(api_url, "openai.com")) {
        /* OpenAI 官方要求 /v1 前缀 */
        suffix = "/v1/chat/completions";
    } else {
        suffix = "/chat/completions";
    }

    char *url = (char*)malloc(len + strlen(suffix) + 1);
    if (!url) return NULL;
    memcpy(url, api_url, len);
    strcpy(url + len, suffix);
    return url;
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
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", cfg->api_key);
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

/* ============== 流式聊天（SSE） ============== */

typedef struct {
    http_buffer_t pend;     /* 未凑成完整行的残余字节 */
    http_buffer_t content;  /* 累积的正文 */
    char *error;            /* 流内 error 事件的消息 */
    ai_stream_cb cb;
    void *ud;
    int done;
} sse_ctx_t;

static void sse_reset(sse_ctx_t *s)
{
    http_buf_free(&s->pend);
    http_buf_free(&s->content);
    if (s->error) { free(s->error); s->error = NULL; }
    s->cb = NULL; s->ud = NULL; s->done = 0;
}

static void sse_on_chunk(const char *data, size_t len, void *ud)
{
    sse_ctx_t *s = (sse_ctx_t*)ud;
    http_buf_append(&s->pend, data, len);
    if (!s->pend.data) return;

    char *p = s->pend.data;
    for (;;) {
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        size_t llen = strlen(p);
        if (llen && p[llen - 1] == '\r') p[llen - 1] = '\0';

        if (strncmp(p, "data:", 5) == 0) {
            const char *d = p + 5;
            while (*d == ' ' || *d == '\t') d++;
            if (strcmp(d, "[DONE]") == 0) {
                s->done = 1;
            } else {
                cJSON *j = cJSON_Parse(d);
                if (j) {
                    cJSON *choices = cJSON_GetObjectItem(j, "choices");
                    const char *piece = NULL;
                    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                        cJSON *delta = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "delta");
                        cJSON *content = delta ? cJSON_GetObjectItem(delta, "content") : NULL;
                        if (content && cJSON_IsString(content) && content->valuestring)
                            piece = content->valuestring;
                    }
                    cJSON *err = cJSON_GetObjectItem(j, "error");
                    if (err) {
                        cJSON *msg = cJSON_GetObjectItem(err, "message");
                        if (msg && cJSON_IsString(msg) && !s->error)
                            s->error = str_dup(msg->valuestring);
                    }
                    if (piece && *piece) {
                        http_buf_append(&s->content, piece, strlen(piece));
                        if (s->cb) s->cb(piece, s->ud);
                    }
                    cJSON_Delete(j);
                }
                /* 解析失败的行静默忽略：keep-alive 注释行等非 data 行本就不该进来，
                   data 行里偶发的非 JSON（个别网关的心跳）也不值得中断整章生成 */
            }
        }
        p = nl + 1;
    }

    /* 把残余半行挪回缓冲区头部 */
    size_t rest = strlen(p);
    if (rest) memmove(s->pend.data, p, rest + 1);
    s->pend.length = rest;
    if (s->pend.data) s->pend.data[rest] = '\0';
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
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", cfg->api_key);
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

    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", cfg->api_key);
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
