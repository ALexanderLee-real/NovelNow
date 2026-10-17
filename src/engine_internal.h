/* engine_internal.h — engine 三文件（engine_prompt.c / engine_parse.c / engine_flow.c）的内部共享区
   2026-10-18 结构化拆分：原 engine.c 的内部结构、宏与跨文件静态工具集中到这里。
   只在 engine_*.c 之间使用，host 侧不要包含本头文件；对外语言面仍是 engine.h。 */
#ifndef ENGINE_INTERNAL_H
#define ENGINE_INTERNAL_H

#include "engine.h"
#include "utils.h"
#include <stddef.h>
#include <string.h>

/* '：' '、' '第' '章' '节' 都是多字节字符，写成单引号字符常量后无法与任何 char 相等，
   这里统一按 UTF-8 字节序列比较 */
#define U8_EQ(p, lit) (strncmp((p), (lit), sizeof(lit) - 1) == 0)

/* strstr 命中中文关键字后跳过关键字本体：必须按 strlen 跳。
   中文每字 3 字节，写死 +2 会把第一个字切成半个，产生 U+FFFD 乱码 */
const char* u8_after(const char *hit, const char *kw);

/* 引擎实例：state 通过 offsetof 反推回 engine_t（get_engine），历史包袱保持不动 */
struct engine_internal {
    app_config_t *config;
    engine_state_t state;
    volatile int cancel;   /* 用户请求取消：引擎循环在检查点轮询 */
};

typedef struct engine_internal engine_t;

/* 实现于 engine_flow.c（原 engine.c 静态函数，拆分后跨文件共用） */
engine_t* get_engine(engine_state_t *state);
int is_cancelled(engine_state_t *state);

/* 检查点宏：用于引擎各循环的边界，收到取消请求时立即退出并报告 */
#define CANCEL_CHECK(state) \
    do { \
        if (is_cancelled((state))) { \
            set_error((state), "任务已被用户取消"); \
            (state)->is_running = 0; \
            return ENGINE_CANCELLED; \
        } \
    } while (0)

/* 带清理的检查点：用于"已经分配了临时对象"之后的取消点，
   否则用户每点一次取消就泄漏一份 strbuf / 字符串 */
#define CANCEL_CHECK_CLEAN(state, cleanup_expr) \
    do { \
        if (is_cancelled((state))) { \
            cleanup_expr; \
            set_error((state), "任务已被用户取消"); \
            (state)->is_running = 0; \
            return ENGINE_CANCELLED; \
        } \
    } while (0)

/* ============== 跨文件共享的内部函数（原 engine.c 静态函数） ============== */

/* 进度/错误通道（engine_flow.c 实现） */
void report_progress(engine_state_t *state, const char *stage,
                     int current, int total, const char *message);
void set_error(engine_state_t *state, const char *error);

/* AI 调用（engine_flow.c 实现）：注册了通用流式回调就走 SSE，否则保持原样 */
ai_response_t* engine_ai_call(engine_state_t *state, ai_config_t *ai,
                              const char *system_prompt, const char *user_prompt);

/* 解析 AI 返回（engine_parse.c 实现） */
int parse_numbered_list(const char *text, char **titles, char **descriptions, int max_items);
void extract_field(const char *content, const char *end,
                   const char *kw, char *out, size_t max);
int parse_characters_from(const char *content, novel_project_t *proj);
void parse_worldview_from(const char *content, novel_project_t *proj);

/* 章节记忆记账（engine_prompt.c 实现） */
void record_chapter_memory(engine_state_t *state, novel_project_t *proj,
                           int chapter_index);

#endif /* ENGINE_INTERNAL_H */
