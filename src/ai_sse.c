/* ai_sse.c - SSE stream parser (2026-10-18 split from legacy ai_api.c, zero behavior change) */
#include "ai_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ============== 流式聊天（SSE） ============== */


void sse_reset(sse_ctx_t *s)
{
    http_buf_free(&s->pend);
    http_buf_free(&s->content);
    if (s->error) { free(s->error); s->error = NULL; }
    s->cb = NULL; s->ud = NULL; s->done = 0;
    s->prompt_tokens = s->completion_tokens = s->total_tokens = 0;
}

void sse_on_chunk(const char *data, size_t len, void *ud)
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
                    /* 支线 C：usage（OpenAI 兼容响应体里的 usage 对象；流式下
                       DeepSeek 在 [DONE] 前最后一条带，choices 可能是空数组） */
                    {
                        cJSON *usage = cJSON_GetObjectItem(j, "usage");
                        if (usage && cJSON_IsObject(usage)) {
                            cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
                            cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
                            cJSON *tt = cJSON_GetObjectItem(usage, "total_tokens");
                            if (pt && cJSON_IsNumber(pt)) s->prompt_tokens = pt->valueint;
                            if (ct && cJSON_IsNumber(ct)) s->completion_tokens = ct->valueint;
                            if (tt && cJSON_IsNumber(tt)) s->total_tokens = tt->valueint;
                        }
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
