/* ai_http.c - URL parsing and HTTP request execution (2026-10-18 split from legacy ai_api.c, zero behavior change) */
#include "ai_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ============== URL 解析 ============== */


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
        /* 端口必须是 1-65535 的纯数字：atoi 把 "abc" 变 0、把超长数截断，
           都会静默连到错误端口（P1-⑮） */
        {
            const char *pv = port_start + 1;
            /* 失败必须 return -1（parse_url 约定 0=成功）：早前这里 return 0
               把非法端口当解析成功（P1-⑮ 的修复没修好） */
            if (!*pv) return -1;
            for (const char *d = pv; *d; d++) if (*d < '0' || *d > '9') return -1;
            if (parts->port < 1 || parts->port > 65535) return -1;
        }
    } else {
        if (strcmp(parts->scheme, "https") == 0) parts->port = 443;
        else parts->port = 80;
    }
    
    /* path：超长直接拒绝，不静默截断——截断会把请求打到错误的接口上 */
    if (path_start) {
        size_t plen = strlen(path_start);
        if (plen >= sizeof(parts->path)) return -1;
        memcpy(parts->path, path_start, plen + 1);
    } else {
        strcpy(parts->path, "/");
    }
    
    return 0;
}

/* ============== HTTP POST ============== */


void http_buf_init(http_buffer_t *buf)
{
    buf->capacity = 4096;
    buf->data = (char*)malloc(buf->capacity);
    buf->length = 0;
    buf->oom = 0;
    if (buf->data) buf->data[0] = '\0';
}

void http_buf_append(http_buffer_t *buf, const char *data, size_t len)
{
    /* data 可能为 NULL（初始 malloc 失败）：此时必须先尝试分配，
       否则 memcpy(NULL+len, ...) 直接崩溃 */
    if (!buf->data || buf->length + len + 1 > buf->capacity) {
        size_t cap = buf->capacity ? buf->capacity : 4096;
        while (cap < buf->length + len + 1) {
            size_t doubled = cap * 2;
            if (doubled <= cap) { cap = buf->length + len + 1; break; }  /* 翻倍溢出钳制 */
            cap = doubled;
        }
        char *new_data = (char*)realloc(buf->data, cap);
        if (!new_data) { buf->oom = 1; return; }   /* 丢数据要留痕，不能静默截断 */
        buf->data = new_data;
        buf->capacity = cap;
    }
    memcpy(buf->data + buf->length, data, len);
    buf->length += len;
    buf->data[buf->length] = '\0';
}

void http_buf_free(http_buffer_t *buf)
{
    if (buf->data) free(buf->data);
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
    buf->oom = 0;
}

/* POST 与 GET 共用同一套连接/收发/错误处理流程，仅动词与请求体不同。
   on_body 非空时（流式模式）：每读到一段响应体就回调一次，调用方在回调里
   做增量处理；resp_buf 仍然照常累积，保证错误路径能拿到完整报文。 */
char* http_request_ex(const char *method, const char *url, const char *headers[],
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

    /* 登记在途句柄：取消线程会遍历注册表关闭它们，立即中断阻塞读 */
    ai_req_node_t *req_node = ai_req_register(g_abort_ai, hReq);

    /* 状态码先于读体查询：非 2xx 的错误体绝不能喂给 on_body——SSE 解析器
       会把它收进 sse.content，让「content 为空才重试」的重试判定失效（P1） */
    char status_code[32] = {0};
    DWORD status_len = sizeof(status_code);
    if (!HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE, status_code, &status_len, NULL)) {
        /* 不检查就会拿 0 当状态码，报出误导的 "HTTP 0"（P1-⑬） */
        snprintf(status_code, sizeof(status_code), "0");
    }
    int code = atoi(status_code);
    int ok_status = (code >= 200 && code < 300);

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
        if (on_body && ok_status) on_body(chunk, bytes_read, ud);
    }
    /* 注销登记：返回 1 = 取消线程已代关 hReq，之后所有出口都不许再关 */
    int closed_by_abort = ai_req_unregister(req_node);

    if (ai_aborted()) {
        http_buf_free(&resp_buf);
        /* abort 线程已代关 hReq 时不能再关：句柄值可能已被新请求复用，会误杀他人 */
        if (!closed_by_abort) InternetCloseHandle(hReq);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInet);
        abort_err(err_out);
        return NULL;
    }
    if (!read_ok || !resp_buf.data || resp_buf.oom) {
        if (err_out) *err_out = str_dup(read_ok ? "响应读取内存不足"
                                                : "Connection lost while reading response");
        http_buf_free(&resp_buf);
        if (!closed_by_abort) InternetCloseHandle(hReq);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInet);
        return NULL;
    }

    /* 状态码已在读体前取好（见上），这里直接判 */
    if (!ok_status) {
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
        if (!closed_by_abort) InternetCloseHandle(hReq);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInet);
        return NULL;
    }

    if (!closed_by_abort) InternetCloseHandle(hReq);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInet);

    char *result = resp_buf.data;
    resp_buf.data = NULL;
    http_buf_free(&resp_buf);
    return result;
}

/* 非流式调用方（ai_chat / ai_list_models）的原接口保持不变 */
char* http_request(const char *method, const char *url, const char *headers[],
                          int header_count, const char *body, size_t body_len,
                          int timeout_sec, char **err_out)
{
    return http_request_ex(method, url, headers, header_count, body, body_len,
                           timeout_sec, err_out, NULL, NULL);
}

/* ============== 聊天 API ============== */

/* 用户可能只填 base_url（如 https://api.deepseek.com），也可能填完整端点。
   这里统一推导出真正的 chat/completions 地址，避免请求打到站点根路径而返回 404。 */
char* resolve_chat_endpoint(const char *api_url)
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
