/* engine_prompt.c - prompt/context builders, split from engine.c (2026-10-18, zero behavior change) */
#include "engine_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

const char* u8_after(const char *hit, const char *kw)
{
    return hit + strlen(kw);
}

/* ============== 阶段4：章节扩写 ============== */

/* 章节写完后的「记账」调用：让 AI 用极少的 token 顺带产出
   ① 本章概要（存入 summary，供后续章节做前文记忆）
   ② 本章埋下的伏笔
   ③ 本章回收的伏笔（与登记表标题匹配）
   失败不影响写作流程——概要缺失只是记忆变弱，不算错误 */
void record_chapter_memory(engine_state_t *state, novel_project_t *proj,
                                  int chapter_index)
{
    chapter_t *ch = project_get_chapter(proj, chapter_index);
    if (!ch || !ch->content || !*ch->content) return;

    /* 已记过账就不重复花一次调用（手动重写同章会走这里，别浪费配额）。
       注意：绝不能用 ch->summary 判断——大纲生成的章自带概要，
       以前 `if (ch->summary && *ch->summary) return;` 让本函数几乎永不执行，
       伏笔表恒空、前文记忆喂的是大纲计划而非实际剧情（P0） */
    if (ch->memory_recorded) return;

    strbuf_t *prompt = strbuf_create(2048);
    strbuf_append(prompt, "以下是刚写完的小说章节。请只输出一行 JSON（不要代码块、不要解释），格式：\n");
    strbuf_append(prompt, "{\"summary\":\"本章剧情梗概，60字以内\",\"planted\":[\"本章埋下的伏笔简称\"],\"resolved\":[\"本章回收的伏笔简称\"]}\n");
    strbuf_append(prompt, "planted/resolved 没有就给空数组。伏笔简称要短且稳定（如「骨灯的来历」），便于跨章追踪。\n\n");
    strbuf_append(prompt, "【章节标题】");
    strbuf_append(prompt, ch->title ? ch->title : "");
    strbuf_append(prompt, "\n\n【章节正文】\n");
    strbuf_append_len(prompt, ch->content, utf8_safe_len(ch->content, 6000));

    /* 短输出：登记不需要思考余量，压回小额度省时间。
       必须走非流式 ai_chat_single：engine_ai_call 在 host 注册 text_delta_cb 后
       会流式吐增量，这段记账 JSON 会被当正文流给前端（P1） */
    ai_config_t *ai = get_engine(state)->config->ai;
    int saved_max = ai->max_tokens;
    if (ai->max_tokens < 1024) ai->max_tokens = 1024;
    ai_response_t *resp = ai_chat_single(ai,
        "你是一个严谨的小说连载记录员，只输出 JSON。",
        strbuf_text(prompt));
    ai->max_tokens = saved_max;
    strbuf_destroy(prompt);

    if (!resp || !resp->success || !resp->content) {
        if (resp) ai_response_destroy(resp);
        return;
    }

    /* 容错解析：截取第一个 { 到最后一个 } */
    const char *s = strchr(resp->content, '{');
    const char *e = strrchr(resp->content, '}');
    if (s && e && e > s) {
        char *json_buf = (char*)malloc((size_t)(e - s) + 2);
        if (json_buf) {
            memcpy(json_buf, s, (size_t)(e - s) + 1);
            json_buf[e - s + 1] = '\0';
            cJSON *j = cJSON_Parse(json_buf);
            if (j) {
                const cJSON *sum = cJSON_GetObjectItem(j, "summary");
                if (cJSON_IsString(sum) && sum->valuestring && *sum->valuestring) {
                    free(ch->summary);
                    ch->summary = str_dup(sum->valuestring);
                }
                const cJSON *pl = cJSON_GetObjectItem(j, "planted");
                if (cJSON_IsArray(pl)) {
                    int n = cJSON_GetArraySize(pl);
                    for (int k = 0; k < n; k++) {
                        const cJSON *it = cJSON_GetArrayItem(pl, k);
                        if (cJSON_IsString(it) && it->valuestring)
                            project_add_foreshadow(proj, it->valuestring, "",
                                                   chapter_index);
                    }
                }
                const cJSON *rs = cJSON_GetObjectItem(j, "resolved");
                if (cJSON_IsArray(rs)) {
                    int n = cJSON_GetArraySize(rs);
                    for (int k = 0; k < n; k++) {
                        const cJSON *it = cJSON_GetArrayItem(rs, k);
                        if (cJSON_IsString(it) && it->valuestring)
                            project_resolve_foreshadow(proj, it->valuestring,
                                                       chapter_index);
                    }
                }
                cJSON_Delete(j);
                /* 账记上了才算数；解析失败不置位，下次重写该章时还有机会补记 */
                ch->memory_recorded = 1;
            }
            free(json_buf);
        }
    }
    ai_response_destroy(resp);
}
