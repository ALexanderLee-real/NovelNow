#ifndef NOVEL_AI_API_H
#define NOVEL_AI_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cJSON.h"

/* 聊天消息结构 */
typedef struct {
    char *role;    /* system, user, assistant */
    char *content;
} chat_message_t;

/* max_tokens 的唯一权威口径。
 * UI 输入框、config.json 的读与写、engine 的临时抬升，全部引用这组常量。
 *
 * 背景：此前三处各写各的夹取区间 —— UI 允许 [256, 200000]、config.json 加载时
 * 却把 <4096 抬到 8192 且 >8192 压回 8192（实际只留下 [4096, 8192]）、engine 抬升
 * 上限又是另一个字面量 8192。结果是用户在界面上填 16384，存盘写入 16384，
 * 重启加载被静默改回 8192 —— 用户设置无声丢失，且日志里没有任何痕迹。 */
#define AI_MAX_TOKENS_MIN            256      /* 与 index.html 的 min 保持一致 */
#define AI_MAX_TOKENS_MAX            200000   /* 与 index.html 的 max 保持一致 */
#define AI_MAX_TOKENS_DEFAULT        8192     /* 思考型模型的推理阶段与正文共享额度，
                                                 额度不足时正文会返回空串 */
#define AI_MAX_TOKENS_LEGACY_DEFAULT 2048     /* 旧版本写死的默认值。
                                                 用户从未改过这项的话就是这个数，
                                                 加载时视为「未设置」并迁移到 DEFAULT，
                                                 否则旧存档会一直产出空正文 */
#define AI_MAX_TOKENS_LIFT_CAP       8192     /* engine 按目标字数临时抬升时的保守上限。
                                                 再高多数模型直接返回 400 */

/* API 配置 */
typedef struct {
    char *api_url;         /* API 端点，如 https://api.openai.com/v1/chat/completions */
    char *api_key;         /* API 密钥 */
    char *model;           /* 模型名称 */
    double temperature;    /* 温度参数 */
    int max_tokens;        /* 最大 token 数，取值受 AI_MAX_TOKENS_* 约束 */
    int timeout;           /* 超时时间（秒） */
    /* ---- 限流模式（免费/限流额度模型友好） ---- */
    int rate_limit_ms;     /* 相邻两次 AI 请求的最小间隔（毫秒），0 = 不限流 */
    int max_retries;       /* 遇到 429/5xx/连接错误时的自动重试次数，0 = 不重试 */
} ai_config_t;

/* 响应结果 */
typedef struct {
    int success;
    char *content;    /* AI 回复内容 */
    char *error;      /* 错误信息 */
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
} ai_response_t;

/* 初始化/销毁配置 */
ai_config_t* ai_config_create(void);
void ai_config_destroy(ai_config_t *cfg);

/* 初始化/销毁响应 */
ai_response_t* ai_response_create(void);
void ai_response_destroy(ai_response_t *resp);

/* 发送聊天请求（OpenAI 兼容格式） */
ai_response_t* ai_chat(ai_config_t *cfg, chat_message_t *messages, int msg_count);

/* 便捷函数：单轮对话 */
ai_response_t* ai_chat_single(ai_config_t *cfg, const char *system_prompt, const char *user_prompt);

/* ---- 流式（SSE, stream:true）----
 * 每收到一段增量正文就回调 ai_stream_cb(delta, user)，全文仍在返回的
 * resp->content 里（成功时），调用方无需自己拼接。 */
typedef void (*ai_stream_cb)(const char *delta, void *user);

ai_response_t* ai_chat_stream(ai_config_t *cfg, chat_message_t *messages, int msg_count,
                              ai_stream_cb cb, void *user);

ai_response_t* ai_chat_single_stream(ai_config_t *cfg, const char *system_prompt,
                                     const char *user_prompt, ai_stream_cb cb, void *user);

/* ---- 请求中止 ----
 * ai_abort_current：从任意线程调用，立即掐断在途的 AI 请求
 * （阻塞中的读取会立刻失败，错误信息为「操作已被用户取消」，不会触发重试；
 *   只掐 AI 通道，comfy 图片请求走自己的通道，互不打断）。
 * ai_abort_all：关窗等场景把 AI + comfy 两条通道都掐断。
 * ai_abort_reset：新任务开始时清除中止标记。 */
void ai_abort_current(void);
void ai_abort_all(void);
void ai_abort_reset(void);

/* ---- Token 用量统计（支线 C）----
 * ai_chat* 解析到 usage（OpenAI 兼容响应体的 usage 字段；流式下 DeepSeek 等
 * 在 [DONE] 前最后一条带，OpenAI 需要 stream_options.include_usage 才给——
 * 本项目不加该字段，兼容优先；拿不到就统计为 0，不猜不估）即自动记账。
 * ai_usage_take：原子领走「未领取」累计（读取即清零），调用方负责落盘持久化。
 * ai_usage_session：本次进程启动以来的累计（只读展示用）。 */
void ai_usage_take(long long *prompt, long long *completion, long long *total);
void ai_usage_session(long long *prompt, long long *completion, long long *total);

/* 获取可用模型列表（OpenAI 兼容 /v1/models 端点）
   cfg: 需已设置 api_url 与 api_key
   count: 输出模型数量
   返回动态分配的字符串数组（每个元素为模型 ID），失败返回 NULL
   注意：返回的数组需用 ai_list_models_free 释放 */
char** ai_list_models(ai_config_t *cfg, int *count);

/* 释放 ai_list_models 返回的数组 */
void ai_list_models_free(char **models, int count);

#ifdef __cplusplus
}
#endif

#endif
