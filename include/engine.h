#ifndef NOVEL_AI_ENGINE_H
#define NOVEL_AI_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ai_api.h"
#include "project.h"
#include "config.h"

/* 进度回调函数类型 */
typedef void (*progress_callback_t)(const char *stage, int current, int total, const char *message, void *user_data);

/* 章节正文流式增量回调（章节扩写期间持续触发；host 侧负责转发给前端） */
typedef void (*chapter_delta_cb_t)(int chapter_index, const char *delta, void *user_data);

/* 通用流式增量回调（方向/方案/大纲/一致性检查等阶段触发，不带章节索引） */
typedef void (*text_delta_cb_t)(const char *delta, void *user_data);

/* 引擎状态 */
typedef struct {
    int is_running;
    char current_stage[128];
    int progress_current;
    int progress_total;
    char last_error[1024];
    progress_callback_t progress_cb;
    void *user_data;
    chapter_delta_cb_t chapter_delta_cb;
    void *chapter_delta_user;
    text_delta_cb_t text_delta_cb;
    void *text_delta_user;
} engine_state_t;

/* ============== 引擎管理 ============== */

engine_state_t* engine_create(app_config_t *cfg);
void engine_destroy(engine_state_t *engine);
void engine_set_progress_callback(engine_state_t *engine, progress_callback_t cb, void *user_data);
void engine_set_chapter_delta_callback(engine_state_t *engine, chapter_delta_cb_t cb, void *user_data);
void engine_set_text_delta_callback(engine_state_t *engine, text_delta_cb_t cb, void *user_data);

/* ============== 任务取消（协作式） ============== */

/* 引擎函数返回此值表示任务被用户取消（区别于 -1 的执行失败） */
#define ENGINE_CANCELLED (-2)

/* 请求取消：主线程在用户点击"取消"时调用，正在运行的引擎循环将在下一个检查点停止 */
void engine_request_cancel(engine_state_t *state);

/* 清除取消标志：每次开始新任务前调用，避免上一次的取消请求误伤新任务 */
void engine_clear_cancel(engine_state_t *state);

/* ============== 阶段1：创意发散（想法 -> 多方向） ============== */

/*
   对用户的想法进行多轮迭代，生成多个创作方向
   iterations: 迭代轮次（建议2-3轮）
   direction_count: 每轮生成方向数量
*/
int engine_generate_directions(engine_state_t *engine, novel_project_t *proj,
                               int iterations, int direction_count);

/* ============== 阶段2：方案生成（方向 -> 题目/简介/大纲） ============== */

/*
   基于选定的方向，生成多篇小说方案（题目+简介+大纲）
   plan_count: 生成方案数量
*/
int engine_generate_plans(engine_state_t *engine, novel_project_t *proj, int plan_count);

/* ============== 阶段3：大纲细化（选定方案 -> 详细章节大纲） ============== */

/*
   将选定方案的大纲细化为具体章节
   chapter_count: 预期章节数
*/
int engine_expand_outline(engine_state_t *engine, novel_project_t *proj, int chapter_count);

/* ============== 阶段4：正文扩写（章节 -> 正文内容） ============== */

/*
   扩写单个章节
   chapter_index: 章节索引
   target_words: 目标字数
*/
int engine_expand_chapter(engine_state_t *engine, novel_project_t *proj,
                          int chapter_index, int target_words);

/*
   批量扩写所有章节
*/
int engine_expand_all_chapters(engine_state_t *engine, novel_project_t *proj, int target_words_per_chapter);

/* ============== 阶段5：一致性检查 ============== */

/*
   检查指定章节是否符合主题和设定
   返回符合度评分（0-1）
*/
double engine_check_consistency(engine_state_t *engine, novel_project_t *proj, int chapter_index);

/*
   检查所有章节的一致性
*/
int engine_check_all_consistency(engine_state_t *engine, novel_project_t *proj);

/* ============== 自动写作全流程 ============== */

/*
   全自动写作：从想法到完成正文的完整流程
   start_chapter > 0 时为「断点续写」：跳过阶段1-3（要求项目已有方向/方案/大纲），
   直接从该章节索引（0-based）开始扩写到最后一章。
*/
int engine_auto_write(engine_state_t *engine, novel_project_t *proj,
                      int direction_count, int plan_count, int chapter_count,
                      int words_per_chapter, int start_chapter);

/* ============== 世界观与人物智能分析 ============== */

/*
   从当前项目内容（章节正文，无则回退概要/大纲）生成主角团人物草稿，覆盖现有人物。
   返回生成的人物数；失败 -1；被取消 ENGINE_CANCELLED
*/
int engine_generate_characters(engine_state_t *engine, novel_project_t *proj);

/*
   通读当前项目全部内容，总结为世界观设定（世界类型/背景设定/力量体系）。
   成功返回 0；失败 -1；被取消 ENGINE_CANCELLED
*/
int engine_summarize_worldview(engine_state_t *engine, novel_project_t *proj);

/*
   全书伏笔回收校验：通读全部章节正文，检查伏笔埋设/回收情况与前后设定一致性，
   生成校验报告写入 proj->finale_report。
   成功返回 0；失败 -1；被取消 ENGINE_CANCELLED
*/
int engine_check_finale(engine_state_t *engine, novel_project_t *proj);

/*
   分析外部文本（如导入的 TXT）：一次调用同时生成世界观与主要人物并写入项目。
   text 必须是合法 UTF-8。返回解析出的人物数（>=0）；失败 -1；被取消 ENGINE_CANCELLED
*/
int engine_analyze_text(engine_state_t *engine, novel_project_t *proj, const char *text);

#ifdef __cplusplus
}
#endif

#endif
