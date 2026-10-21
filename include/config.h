#ifndef NOVEL_AI_CONFIG_H
#define NOVEL_AI_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ai_api.h"

/* 全局应用配置 */
typedef struct {
    ai_config_t *ai;           /* AI API 配置 */
    char *projects_dir;        /* 项目存储目录 */
    char *export_dir;          /* 导出目录（可为空，回退到 projects_dir） */
    char *language;            /* 语言设置 */
    int auto_save_interval;    /* 自动保存间隔（秒） */
    int enable_consistency_check; /* 一致性检查开关：默认关闭（每章一次 AI 调用，token 消耗大） */
    char *comfy_url;           /* ComfyUI 基地址（默认 http://127.0.0.1:8188），空=未配置 */
    char *comfy_workflow_path; /* API 格式工作流 JSON 文件路径（含 {{PROMPT}} 占位符） */
    int comfy_auto_bg;         /* 剧情分支自动生成下一节背景图开关 */
    /* 支线 C：累计 Token 用量（跨进程的终身计数）。
       host 在任务收尾 ai_usage_take() 领走增量后累加到这三项并 config_save。 */
    long long tokens_prompt;
    long long tokens_completion;
    long long tokens_total;
} app_config_t;

/* 加载/保存配置 */
app_config_t* config_load(const char *path);
int config_save(app_config_t *cfg, const char *path);

/* 创建默认配置 */
app_config_t* config_create_default(const char *projects_dir);

/* 销毁配置 */
void config_destroy(app_config_t *cfg);

/* 获取默认配置文件路径 */
char* config_get_default_path(void);

/* 服务商预设：返回 cJSON 数组（调用方负责 cJSON_Delete）
 *
 * 内置常见 OpenAI 兼容服务商的端点与常用模型名，供前端一键切换。
 * 只预置「端点 + 模型名」，**绝不预置任何 Key**。
 * 每项形如 {"id","name","url","models":[...]}，models[0] 为默认模型。
 * 端点均为 OpenAI 兼容协议，ai_api.c 的 resolve_chat_endpoint() 能正确推导完整地址。 */
cJSON* config_presets_json(void);

/* 预设条数（便于自检/日志） */
int config_presets_count(void);

#ifdef __cplusplus
}
#endif

#endif
