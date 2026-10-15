#include "config.h"
#include "utils.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

app_config_t* config_create_default(const char *projects_dir)
{
    app_config_t *cfg = (app_config_t*)calloc(1, sizeof(app_config_t));
    if (!cfg) return NULL;
    
    cfg->ai = ai_config_create();
    if (!cfg->ai) { free(cfg); return NULL; }
    
    if (projects_dir) {
        cfg->projects_dir = str_dup(projects_dir);
    } else {
        char *exe_dir = get_exe_dir();
        cfg->projects_dir = path_join(exe_dir, "projects");
        free(exe_dir);
    }
    
    cfg->language = str_dup("zh-CN");
    cfg->auto_save_interval = 30;
    cfg->enable_consistency_check = 0;  /* 默认关闭：自动写作阶段5跳过一致性检查以节省 token */
    
    ensure_dir(cfg->projects_dir);
    return cfg;
}

void config_destroy(app_config_t *cfg)
{
    if (!cfg) return;
    if (cfg->ai) ai_config_destroy(cfg->ai);
    if (cfg->projects_dir) free(cfg->projects_dir);
    if (cfg->export_dir) free(cfg->export_dir);
    if (cfg->language) free(cfg->language);
    free(cfg);
}

char* config_get_default_path(void)
{
    char *exe_dir = get_exe_dir();
    char *path = path_join(exe_dir, "config.json");
    free(exe_dir);
    return path;
}

/* ============================================================
 * 服务商预设
 *
 * 内置常见 OpenAI 兼容服务商的端点与常用模型名，供前端一键切换。
 * 只预置「端点 + 模型名」，**绝不预置任何 Key**。
 *
 * 这些端点都走 OpenAI 兼容协议（POST /chat/completions + Bearer 认证），
 * ai_api.c 的 resolve_chat_endpoint() 能正确推导出完整地址：
 *   https://api.deepseek.com                          -> .../chat/completions
 *   https://api.openai.com/v1                         -> .../v1/chat/completions
 *   https://open.bigmodel.cn/api/paas/v4              -> .../v4/chat/completions
 *   https://ark.cn-beijing.volces.com/api/v3          -> .../v3/chat/completions
 *   https://api.xiaomimimo.com/v1                     -> .../v1/chat/completions
 *   https://dashscope.aliyuncs.com/compatible-mode/v1 -> .../v1/chat/completions
 *
 * models 段以 '|' 分隔，**第一项为该服务商的默认模型**。
 * 模型名只是「建议值」，用户可手改；前端另有「获取」按钮可从端点拉真实列表。
 *
 * 注意：模型名会随各家版本迭代变化，这里的取值以 2026-10 各厂商公开文档为准。
 * ============================================================ */

typedef struct {
    const char *id;
    const char *name;
    const char *url;
    const char *models;
} preset_t;

static const preset_t g_presets[] = {
    { "deepseek", "DeepSeek",
      "https://api.deepseek.com",
      "deepseek-flash|deepseek-v4-pro|deepseek-v4-flash" },

    { "gpt", "OpenAI · GPT",
      "https://api.openai.com/v1",
      "gpt-5.6|gpt-5.2|gpt-5.1|gpt-5|gpt-5-mini|gpt-4.1|gpt-4o-mini" },

    { "glm", "智谱 · GLM",
      "https://open.bigmodel.cn/api/paas/v4",
      "glm-5|glm-4.7|glm-4.6|glm-4.5-air|glm-4.7-flashx" },

    { "seed", "字节 · 豆包 Seed",
      "https://ark.cn-beijing.volces.com/api/v3",
      "doubao-seed-1.6|doubao-seed-1.6-thinking|doubao-seed-2-1-pro-260628" },

    { "mimo", "小米 · MiMo",
      "https://api.xiaomimimo.com/v1",
      "mimo-v2-flash|mimo-v2-pro|mimo-v2-omni" },

    { "qwen", "阿里 · 通义千问",
      "https://dashscope.aliyuncs.com/compatible-mode/v1",
      "qwen3.7-plus|qwen3.8-max|qwen-plus|qwen3.5-plus|qwen3.7-flash|qwen-flash" },

    /* 免费但限流：模型名后缀 :free，建议配合「限流间隔」使用 */
    { "openrouter_free", "OpenRouter · 免费限流",
      "https://openrouter.ai/api/v1",
      "deepseek/deepseek-chat-v3.1:free|z-ai/glm-4.5-air:free|qwen/qwen3-coder:free|meta-llama/llama-3.3-70b-instruct:free" },
};

int config_presets_count(void)
{
    return (int)(sizeof(g_presets) / sizeof(g_presets[0]));
}

cJSON* config_presets_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    int i;
    if (!arr) return NULL;

    for (i = 0; i < config_presets_count(); i++) {
        cJSON *o  = cJSON_CreateObject();
        cJSON *ms = cJSON_CreateArray();
        const char *seg = g_presets[i].models;

        cJSON_AddStringToObject(o, "id",   g_presets[i].id);
        cJSON_AddStringToObject(o, "name", g_presets[i].name);
        cJSON_AddStringToObject(o, "url",  g_presets[i].url);

        while (seg && *seg) {
            const char *bar = strchr(seg, '|');
            size_t n = bar ? (size_t)(bar - seg) : strlen(seg);
            char buf[128];
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, seg, n);
            buf[n] = '\0';
            cJSON_AddItemToArray(ms, cJSON_CreateString(buf));
            if (!bar) break;
            seg = bar + 1;
        }
        cJSON_AddItemToObject(o, "models", ms);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

app_config_t* config_load(const char *path)
{
    if (!path || !file_exists(path)) {
        return config_create_default(NULL);
    }
    
    char *json_str = read_file(path);
    if (!json_str) return config_create_default(NULL);
    
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) return config_create_default(NULL);
    
    app_config_t *cfg = (app_config_t*)calloc(1, sizeof(app_config_t));
    if (!cfg) { cJSON_Delete(root); return NULL; }
    
    cfg->ai = ai_config_create();
    
    cJSON *ai_obj = cJSON_GetObjectItem(root, "ai");
    if (ai_obj) {
        cJSON *item;
        if ((item = cJSON_GetObjectItem(ai_obj, "api_url")) && cJSON_IsString(item)) {
            free(cfg->ai->api_url);
            cfg->ai->api_url = str_dup(item->valuestring);
        }
        if ((item = cJSON_GetObjectItem(ai_obj, "api_key")) && cJSON_IsString(item)) {
            free(cfg->ai->api_key);
            cfg->ai->api_key = str_dup(item->valuestring);
        }
        if ((item = cJSON_GetObjectItem(ai_obj, "model")) && cJSON_IsString(item)) {
            free(cfg->ai->model);
            cfg->ai->model = str_dup(item->valuestring);
        }
        if ((item = cJSON_GetObjectItem(ai_obj, "temperature")) && cJSON_IsNumber(item)) {
            cfg->ai->temperature = item->valuedouble;
        }
        if ((item = cJSON_GetObjectItem(ai_obj, "max_tokens")) && cJSON_IsNumber(item)) {
            /* 口径统一到 ai_api.h 的 AI_MAX_TOKENS_*。
               规则：用户的显式设置一律尊重，只在越界时夹取；
               只有「旧版本写死的 2048」和「缺失/非正数」才回落默认值。
               早前这里是「<4096 抬到 8192、>8192 压回 8192」，等价于只承认
               [4096, 8192]，而 UI 明明允许填到 200000 —— 用户填 16384 存盘后
               重启就被改回 8192，属于静默篡改用户配置，已修正。 */
            int m = item->valueint;
            if (m <= 0 || m == AI_MAX_TOKENS_LEGACY_DEFAULT) {
                m = AI_MAX_TOKENS_DEFAULT;
            } else if (m < AI_MAX_TOKENS_MIN) {
                m = AI_MAX_TOKENS_MIN;
            } else if (m > AI_MAX_TOKENS_MAX) {
                m = AI_MAX_TOKENS_MAX;
            }
            cfg->ai->max_tokens = m;
        }
        if ((item = cJSON_GetObjectItem(ai_obj, "timeout")) && cJSON_IsNumber(item)) {
            cfg->ai->timeout = item->valueint;
        }
        if ((item = cJSON_GetObjectItem(ai_obj, "rate_limit_ms")) && cJSON_IsNumber(item)) {
            int r = item->valueint;
            if (r < 0) r = 0;
            if (r > 600000) r = 600000;
            cfg->ai->rate_limit_ms = r;
        }
        if ((item = cJSON_GetObjectItem(ai_obj, "max_retries")) && cJSON_IsNumber(item)) {
            int r = item->valueint;
            if (r < 0) r = 0;
            if (r > 10) r = 10;
            cfg->ai->max_retries = r;
        }
    }
    
    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "projects_dir")) && cJSON_IsString(item)) {
        cfg->projects_dir = str_dup(item->valuestring);
    } else {
        char *exe_dir = get_exe_dir();
        cfg->projects_dir = path_join(exe_dir, "projects");
        free(exe_dir);
    }
    
    /* 导出目录：可选字段，读不到保持 NULL（前端 outDir 回退到 projectsDir） */
    if ((item = cJSON_GetObjectItem(root, "export_dir")) && cJSON_IsString(item)) {
        cfg->export_dir = str_dup(item->valuestring);
    }
    
    if ((item = cJSON_GetObjectItem(root, "language")) && cJSON_IsString(item)) {
        cfg->language = str_dup(item->valuestring);
    } else {
        cfg->language = str_dup("zh-CN");
    }
    
    if ((item = cJSON_GetObjectItem(root, "auto_save_interval")) && cJSON_IsNumber(item)) {
        cfg->auto_save_interval = item->valueint;
    } else {
        cfg->auto_save_interval = 30;
    }

    if ((item = cJSON_GetObjectItem(root, "enable_consistency_check")) && cJSON_IsNumber(item)) {
        cfg->enable_consistency_check = item->valueint ? 1 : 0;
    } else {
        cfg->enable_consistency_check = 0;
    }

    /* 自愈失效的 projects_dir：config.json 里的绝对路径在目录被重命名/移动后会
       指向不存在的目录，导致保存项目失败。若不存在或为空，回退到 exe 目录下的
       projects（首次运行/移动程序后都能自动恢复正确路径） */
    if (!cfg->projects_dir || !*cfg->projects_dir || !file_exists(cfg->projects_dir)) {
        free(cfg->projects_dir);
        char *exe_dir = get_exe_dir();
        cfg->projects_dir = path_join(exe_dir, "projects");
        free(exe_dir);
    }

    cJSON_Delete(root);
    ensure_dir(cfg->projects_dir);
    return cfg;
}

int config_save(app_config_t *cfg, const char *path)
{
    if (!cfg || !path || !cfg->ai) return -1;
    
    cJSON *root = cJSON_CreateObject();
    
    cJSON *ai_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(ai_obj, "api_url", cfg->ai->api_url);
    cJSON_AddStringToObject(ai_obj, "api_key", cfg->ai->api_key);
    cJSON_AddStringToObject(ai_obj, "model", cfg->ai->model);
    cJSON_AddNumberToObject(ai_obj, "temperature", cfg->ai->temperature);
    cJSON_AddNumberToObject(ai_obj, "max_tokens", cfg->ai->max_tokens);
    cJSON_AddNumberToObject(ai_obj, "timeout", cfg->ai->timeout);
    cJSON_AddNumberToObject(ai_obj, "rate_limit_ms", cfg->ai->rate_limit_ms);
    cJSON_AddNumberToObject(ai_obj, "max_retries", cfg->ai->max_retries);
    cJSON_AddItemToObject(root, "ai", ai_obj);
    
    cJSON_AddStringToObject(root, "projects_dir", cfg->projects_dir);
    if (cfg->export_dir && *cfg->export_dir)
        cJSON_AddStringToObject(root, "export_dir", cfg->export_dir);
    cJSON_AddStringToObject(root, "language", cfg->language);
    cJSON_AddNumberToObject(root, "auto_save_interval", cfg->auto_save_interval);
    cJSON_AddNumberToObject(root, "enable_consistency_check", cfg->enable_consistency_check ? 1 : 0);
    
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    if (!json_str) return -1;
    int ret = write_file_atomic(path, json_str);
    free(json_str);
    return ret;
}
