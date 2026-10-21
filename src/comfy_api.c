/* ============================================================
 * comfy_api.c — ComfyUI 本地生图后端（配图接口第一实现）
 *
 * 纯 WinInet HTTP 客户端，自包含请求逻辑（不复用 ai_http.c 的限流）。
 * 取消走自己的通道 g_abort_comfy（2026-10-19 取消通道上下文化）：图片
 * 生成可以和 AI 文本任务**并行**跑，取消 AI 不掐 comfy、取消 comfy 不掐
 * AI（互不打断是产品语义）；关窗 ai_abort_all() 两条通道都掐。
 *
 * ComfyUI HTTP 协议只用三个接口：
 *   POST /prompt            提交工作流（body: {"prompt":<工作流>,"client_id":...}）
 *   GET  /history/{id}      轮询产出（outputs.<node>.images[]）
 *   GET  /view?filename=..  取 PNG 字节流
 * ============================================================ */
#include "comfy_api.h"
#include "utils.h"
#include "cJSON.h"
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_internal.h"   /* 取消通道 g_abort_comfy + 在途句柄登记 */

#pragma comment(lib, "wininet.lib")

#define COMFY_POLL_MAX     300   /* 次数双保险；真正的封顶是 comfy_generate 里的墙钟 deadline（5 分钟）——
                                     每轮 GET 带 30s 超时，只数次数最坏 300×31s≈2.5h，别再拿「300*1s≈5 分钟」骗人 */
#define COMFY_RESP_CAP     (64u * 1024u * 1024u)   /* 响应体上限 64MB */
#define COMFY_IMAGE_MAX    (16u * 1024u * 1024u)   /* data URL 图片上限 16MB */

/* ---------- 可增长字节缓冲（二进制安全，PNG 里有 0 字节） ---------- */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
    int oom;
} cbuf_t;

static void cbuf_init(cbuf_t *b) { b->data = NULL; b->len = 0; b->cap = 0; b->oom = 0; }
static void cbuf_free(cbuf_t *b) { free(b->data); cbuf_init(b); }

static int cbuf_add(cbuf_t *b, const void *p, size_t n)
{
    if (b->oom) return 0;
    if (b->len + n + 1 > COMFY_RESP_CAP) { b->oom = 1; return 0; }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        {
            unsigned char *d = (unsigned char *)realloc(b->data, cap);
            if (!d) { b->oom = 1; return 0; }
            b->data = d;
            b->cap = cap;
        }
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';   /* 便于当 C 串用；二进制仍按 len 计 */
    return 1;
}

/* ---------- URL 拆解（只支持 http/https，风格照 ai_http.c 的 parse_url） ----------
   返回值口径与 parse_url 统一：0=成功、-1=失败（2026-10-19 前是 1=成功，
   两栈语义相反是 P1 技术债，已统一） */
typedef struct {
    char host[256];
    INTERNET_PORT port;
    char path[2048];
    int https;
} cparts_t;

static int cparse_url(const char *url, cparts_t *p)
{
    const char *s = url, *h, *e;
    size_t n;
    memset(p, 0, sizeof(*p));
    if (!url || !*url) return -1;
    if (_strnicmp(s, "https://", 8) == 0) { p->https = 1; p->port = 443; s += 8; }
    else if (_strnicmp(s, "http://", 7) == 0) { p->port = 80; s += 7; }
    else return -1;
    h = s;
    while (*s && *s != '/' && *s != ':') s++;
    n = (size_t)(s - h);
    if (n == 0 || n >= sizeof(p->host)) return -1;
    memcpy(p->host, h, n);
    p->host[n] = '\0';
    if (*s == ':') {
        int port = 0;
        int ndig = 0;
        s++;
        while (*s >= '0' && *s <= '9') {
            if (++ndig > 5) return -1;   /* 5 位封顶：防超长数字溢出绕过范围检查 */
            port = port * 10 + (*s - '0');
            s++;
        }
        /* 端口必须纯数字后紧跟路径或结尾："host:8080abc" 的尾巴混进 path
           会把请求打到错误的接口上（与 parse_url 的严格校验对齐） */
        if (port <= 0 || port > 65535) return -1;
        if (*s && *s != '/') return -1;
        p->port = (INTERNET_PORT)port;
    }
    if (!*s) { strcpy(p->path, "/"); return 0; }
    e = s + strlen(s);
    if ((size_t)(e - s) >= sizeof(p->path)) return -1;
    memcpy(p->path, s, (size_t)(e - s) + 1);
    return 0;
}

/* ---------- 一次 HTTP 请求：响应收集进 cbuf ---------- */
static int chttp_do(const char *method, const char *url,
                    const char *body, size_t body_len, int timeout_sec,
                    cbuf_t *resp, char *err, size_t errlen)
{
    cparts_t p;
    HINTERNET hInet = NULL, hConn = NULL, hReq = NULL;
    DWORD status = 0, slen = sizeof(status);
    int ok = 0;
    ai_req_node_t *req_node = NULL;   /* 在途句柄登记节点（out: 统一摘除） */

    if (cparse_url(url, &p) != 0) {
        snprintf(err, errlen, "地址格式不对：%s", url ? url : "(空)");
        return -1;
    }
    hInet = InternetOpenA("novelnow-comfy/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInet) { snprintf(err, errlen, "WinInet 初始化失败"); return -1; }
    if (timeout_sec > 0) {
        DWORD t = (DWORD)timeout_sec * 1000u;
        InternetSetOptionA(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
        InternetSetOptionA(hInet, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof(t));
        InternetSetOptionA(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    }
    hConn = InternetConnectA(hInet, p.host, p.port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) { snprintf(err, errlen, "连不上 %s:%u（ComfyUI 开着吗？）", p.host, (unsigned)p.port); goto out; }
    {
        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                      INTERNET_FLAG_KEEP_CONNECTION | (p.https ? INTERNET_FLAG_SECURE : 0);
        hReq = HttpOpenRequestA(hConn, method, p.path, NULL, NULL, NULL, flags, 0);
    }
    if (!hReq) { snprintf(err, errlen, "构造请求失败"); goto out; }
    if (!HttpSendRequestA(hReq,
                          body ? "Content-Type: application/json\r\n" : NULL, (DWORD)-1,
                          (LPVOID)body, (DWORD)body_len)) {
        snprintf(err, errlen, "发送请求失败（错误码 %lu）", GetLastError());
        goto out;
    }
    /* 登记在途句柄进 comfy 通道：取消/关窗能立刻掐断阻塞读 */
    req_node = ai_req_register(g_abort_comfy, hReq);
    if (!HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &slen, NULL)) {
        snprintf(err, errlen, "读取 HTTP 状态失败"); goto out;
    }
    for (;;) {
        char chunk[65536];
        DWORD got = 0;
        if (!InternetReadFile(hReq, chunk, sizeof(chunk), &got)) {
            snprintf(err, errlen, "读取响应失败（错误码 %lu）", GetLastError()); goto out;
        }
        if (got == 0) break;
        if (!cbuf_add(resp, chunk, got)) {
            snprintf(err, errlen, "响应太大或内存不足"); goto out;
        }
    }
    if (status != 200) {
        /* 非 200：带上响应体前 200 字符，ComfyUI 的报错全在里面 */
        char peek[201] = {0};
        if (resp->data) memcpy(peek, resp->data, resp->len < 200 ? resp->len : 200);
        snprintf(err, errlen, "HTTP %lu：%s", (unsigned long)status, peek);
        goto out;
    }
    ok = 1;
out:
    if (hReq) {
        /* 1=取消线程已代关，不许再关（句柄值可能已被新请求复用，会误杀他人） */
        if (!ai_req_unregister(req_node)) InternetCloseHandle(hReq);
    }
    if (hConn) InternetCloseHandle(hConn);
    if (hInet) InternetCloseHandle(hInet);
    if (!ok && abort_ctx_aborted(g_abort_comfy)) snprintf(err, errlen, "已取消");
    return ok ? 0 : -1;
}

/* ---------- JSON 字符串内容转义（注入 {{PROMPT}} 用） ---------- */
static char *json_escape_text(const char *s)
{
    size_t n = 0;
    const unsigned char *p;
    char *out, *w;
    if (!s) s = "";
    for (p = (const unsigned char *)s; *p; p++)
        n += (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t' || *p < 0x20) ? 2 : 1;
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    w = out;
    for (p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  *w++ = '\\'; *w++ = '"';  break;
        case '\\': *w++ = '\\'; *w++ = '\\'; break;
        case '\n': *w++ = '\\'; *w++ = 'n';  break;
        case '\r': *w++ = '\\'; *w++ = 'r';  break;
        case '\t': *w++ = '\\'; *w++ = 't';  break;
        default:
            if (*p < 0x20) { sprintf(w, "\\u%04x", *p); w += 6; }
            else *w++ = (char)*p;
        }
    }
    *w = '\0';
    return out;
}

/* 把 workflow 里所有 {{PROMPT}} 换成转义后的 prompt；返回新串（调用方 free） */
static char *inject_prompt(const char *workflow, const char *prompt)
{
    static const char PH[] = "{{PROMPT}}";
    char *esc, *out, *w;
    const char *r;
    size_t ph_len = sizeof(PH) - 1, count = 0, total;
    r = workflow;
    while ((r = strstr(r, PH)) != NULL) { count++; r += ph_len; }
    if (count == 0) return NULL;
    esc = json_escape_text(prompt);
    if (!esc) return NULL;
    total = strlen(workflow) + count * strlen(esc) + 1;
    out = (char *)malloc(total);
    if (!out) { free(esc); return NULL; }
    w = out;
    r = workflow;
    for (;;) {
        const char *hit = strstr(r, PH);
        if (!hit) break;
        memcpy(w, r, (size_t)(hit - r));
        w += (size_t)(hit - r);
        strcpy(w, esc);
        w += strlen(esc);
        r = hit + ph_len;
    }
    strcpy(w, r);
    free(esc);
    return out;
}

/* ---------- query 参数最小 URL 编码（/view 的 filename/subfolder 要过） ---------- */
static void url_encode_seg(const char *s, char *out, size_t outlen)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    for (; s && *s && w + 4 < outlen; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[w++] = (char)c;
        } else {
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 15];
        }
    }
    out[w] = '\0';
}

/* ============================================================
 * 对外接口
 * ============================================================ */

int comfy_test(const char *base_url, char *err, size_t errlen)
{
    char url[2300];
    cbuf_t resp;
    int rc;
    cbuf_init(&resp);
    snprintf(url, sizeof(url), "%s/system_stats", base_url);
    rc = chttp_do("GET", url, NULL, 0, 10, &resp, err, errlen);
    cbuf_free(&resp);
    return rc;
}

int comfy_generate(const char *base_url, const char *workflow_json,
                   const char *prompt, const char *out_path,
                   char *err, size_t errlen)
{
    char url[2300];
    char *wf = NULL, *body = NULL, *resp_text = NULL;
    cbuf_t resp;
    cJSON *root = NULL, *item, *outputs, *node, *images, *img;
    char prompt_id[128] = {0};
    char filename[512] = {0}, subfolder[512] = {0}, itype[64] = "output";
    int rc = -1, i;

    if (!workflow_json || !strstr(workflow_json, "{{PROMPT}}")) {
        snprintf(err, errlen, "工作流 JSON 里没找到 {{PROMPT}} 占位符（正向提示词文本里写一个）");
        return -1;
    }
    cbuf_init(&resp);

    /* 1. 注入提示词，包成提交体 */
    wf = inject_prompt(workflow_json, prompt ? prompt : "");
    if (!wf) { snprintf(err, errlen, "内存不足"); goto done; }
    { /* workflow 必须是 JSON 对象（ComfyUI 节点图）：不然拼出的提交体是非法的，
         对面报 400 还看不出为什么（P1-⑩） */
        cJSON *wf_chk = cJSON_Parse(wf);
        if (!wf_chk || !cJSON_IsObject(wf_chk)) {
            cJSON_Delete(wf_chk);
            snprintf(err, errlen, "工作流 JSON 不是 JSON 对象（ComfyUI 要求节点图，检查工作流文件内容）");
            goto done;
        }
        cJSON_Delete(wf_chk);
    }
    {
        size_t n = strlen(wf) + 64;
        body = (char *)malloc(n);
        if (!body) { snprintf(err, errlen, "内存不足"); goto done; }
        snprintf(body, n, "{\"prompt\":%s,\"client_id\":\"novelnow\"}", wf);
    }

    /* 2. POST /prompt */
    snprintf(url, sizeof(url), "%s/prompt", base_url);
    if (chttp_do("POST", url, body, strlen(body), 60, &resp, err, errlen) != 0) goto done;
    resp_text = (char *)resp.data;
    root = cJSON_Parse(resp_text ? resp_text : "");
    if (!root) { snprintf(err, errlen, "ComfyUI 返回的不是 JSON"); goto done; }
    item = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (item && !cJSON_IsNull(item)) {
        char *e = cJSON_PrintUnformatted(item);
        snprintf(err, errlen, "提交被拒：%s", e ? e : "?");
        free(e);
        goto done;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "prompt_id");
    if (!cJSON_IsString(item)) { snprintf(err, errlen, "返回里没有 prompt_id"); goto done; }
    snprintf(prompt_id, sizeof(prompt_id), "%s", item->valuestring);
    cJSON_Delete(root); root = NULL;

    /* 3. 轮询 /history/{id} 直到 outputs 里出图。
       墙钟 5 分钟 deadline 为主、COMFY_POLL_MAX 为次级保险：每轮 GET 带 30s 超时，
       只数次数最坏能磨 300×31s≈2.5 小时（P1-⑨，旧注释吹「≈5 分钟」是骗人的） */
    ULONGLONG poll_deadline = GetTickCount64() + 5ull * 60 * 1000;
    for (i = 0; i < COMFY_POLL_MAX && GetTickCount64() < poll_deadline; i++) {
        cJSON *entry, *status_obj, *ss;
        abortable_sleep(g_abort_comfy, 1000);   /* 可打断：取消/关窗不再傻睡满 1 秒 × 300 */
        if (abort_ctx_aborted(g_abort_comfy)) { snprintf(err, errlen, "已取消"); goto done; }
        cbuf_free(&resp);
        snprintf(url, sizeof(url), "%s/history/%s", base_url, prompt_id);
        if (chttp_do("GET", url, NULL, 0, 30, &resp, err, errlen) != 0) goto done;
        resp_text = (char *)resp.data;
        root = cJSON_Parse(resp_text ? resp_text : "");
        if (!root) { snprintf(err, errlen, "轮询返回的不是 JSON"); goto done; }
        entry = cJSON_GetObjectItemCaseSensitive(root, prompt_id);
        if (!entry || cJSON_IsNull(entry)) { cJSON_Delete(root); root = NULL; continue; }
        status_obj = cJSON_GetObjectItemCaseSensitive(entry, "status");
        ss = status_obj ? cJSON_GetObjectItemCaseSensitive(status_obj, "status_str") : NULL;
        if (ss && cJSON_IsString(ss) && strcmp(ss->valuestring, "error") == 0) {
            snprintf(err, errlen, "ComfyUI 生图失败（工作流内部报错，看 ComfyUI 窗口日志）");
            goto done;
        }
        outputs = cJSON_GetObjectItemCaseSensitive(entry, "outputs");
        if (!outputs || cJSON_IsNull(outputs)) { cJSON_Delete(root); root = NULL; continue; }
        /* 找第一个带 images 的节点 */
        img = NULL;
        for (node = outputs->child; node; node = node->next) {
            images = cJSON_GetObjectItemCaseSensitive(node, "images");
            if (images && cJSON_IsArray(images) && cJSON_GetArraySize(images) > 0) {
                img = cJSON_GetArrayItem(images, 0);
                break;
            }
        }
        if (!img) { cJSON_Delete(root); root = NULL; continue; }
        {
            cJSON *f = cJSON_GetObjectItemCaseSensitive(img, "filename");
            cJSON *sf = cJSON_GetObjectItemCaseSensitive(img, "subfolder");
            cJSON *ty = cJSON_GetObjectItemCaseSensitive(img, "type");
            if (cJSON_IsString(f)) snprintf(filename, sizeof(filename), "%s", f->valuestring);
            if (cJSON_IsString(sf)) snprintf(subfolder, sizeof(subfolder), "%s", sf->valuestring);
            if (cJSON_IsString(ty)) snprintf(itype, sizeof(itype), "%s", ty->valuestring);
        }
        break;
    }
    if (!filename[0]) {
        snprintf(err, errlen, "等待 5 分钟仍未出图（队列堵了或工作流太重）");
        goto done;
    }

    /* 4. GET /view 取图存盘 */
    {
        char ef[1024], es[1024], et[256];
        url_encode_seg(filename, ef, sizeof(ef));
        url_encode_seg(subfolder, es, sizeof(es));
        url_encode_seg(itype, et, sizeof(et));
        snprintf(url, sizeof(url), "%s/view?filename=%s&subfolder=%s&type=%s",
                 base_url, ef, es, et);
    }
    cbuf_free(&resp);
    if (chttp_do("GET", url, NULL, 0, 120, &resp, err, errlen) != 0) goto done;
    if (resp.len == 0) { snprintf(err, errlen, "取回的图片是空的"); goto done; }
    {
        FILE *fp = fopen_utf8(out_path, "wb");
        if (!fp) { snprintf(err, errlen, "无法写入 %s", out_path); goto done; }
        if (fwrite(resp.data, 1, resp.len, fp) != resp.len) {
            fclose(fp);
            snprintf(err, errlen, "写入 %s 时出错", out_path);
            goto done;
        }
        fclose(fp);
    }
    rc = 0;
done:
    cJSON_Delete(root);
    free(wf);
    free(body);
    cbuf_free(&resp);
    return rc;
}

/* ---------- 图片文件 -> data URL（前端背景图用） ---------- */
static const char *mime_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "image/png";
    dot++;
    if (_stricmp(dot, "jpg") == 0 || _stricmp(dot, "jpeg") == 0) return "image/jpeg";
    if (_stricmp(dot, "webp") == 0) return "image/webp";
    if (_stricmp(dot, "gif") == 0) return "image/gif";
    return "image/png";
}

char* image_data_url(const char *path)
{
    FILE *fp;
    long fsz;
    unsigned char *raw = NULL;
    char *out = NULL, *w;
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, olen;

    if (!path || !*path) return NULL;
    fp = fopen_utf8(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsz <= 0 || (size_t)fsz > COMFY_IMAGE_MAX) { fclose(fp); return NULL; }
    raw = (unsigned char *)malloc((size_t)fsz);
    if (!raw) { fclose(fp); return NULL; }
    if (fread(raw, 1, (size_t)fsz, fp) != (size_t)fsz) { free(raw); fclose(fp); return NULL; }
    fclose(fp);

    olen = ((size_t)fsz + 2) / 3 * 4;
    out = (char *)malloc(olen + 64);
    if (!out) { free(raw); return NULL; }
    w = out;
    w += sprintf(w, "data:%s;base64,", mime_of(path));
    for (i = 0; i < (size_t)fsz; i += 3) {
        unsigned int v = raw[i] << 16;
        if (i + 1 < (size_t)fsz) v |= raw[i + 1] << 8;
        if (i + 2 < (size_t)fsz) v |= raw[i + 2];
        w[0] = b64[(v >> 18) & 63];
        w[1] = b64[(v >> 12) & 63];
        w[2] = (i + 1 < (size_t)fsz) ? b64[(v >> 6) & 63] : '=';
        w[3] = (i + 2 < (size_t)fsz) ? b64[v & 63] : '=';
        w += 4;
    }
    *w = '\0';
    free(raw);
    return out;
}
