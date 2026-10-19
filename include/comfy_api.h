#ifndef NOVELNOW_COMFY_API_H
#define NOVELNOW_COMFY_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * ComfyUI 本地生图后端（配图接口第一实现）
 *
 * 定位：给「剧情分支」等场景提供「下一节背景图」预生成能力。
 * 纯 HTTP 客户端，独立于 ai_http.c：**不共享 abort 句柄、不走 AI 限流**，
 * 所以可以和 AI 文本任务并行跑（图片生成不占忙闲位）。
 *
 * ComfyUI 侧约定（写进「配图工坊」使用须知）：
 *   1. 本地预装 ComfyUI 并启动，默认基地址 http://127.0.0.1:8188
 *      （--port 可改；跨机器时 ComfyUI 侧需 --listen 0.0.0.0 并放行 8188/TCP；
 *      本程序是纯客户端出站 HTTP，自己**不开放任何端口**）。
 *   2. 工作流用 ComfyUI 的「导出(API 格式)」得到 JSON；在正向提示词
 *      （如 CLIPTextEncode 的 text）里写占位符 {{PROMPT}}，生图时会把
 *      剧情摘要 JSON 转义后替换进去。
 *   3. 提交/轮询/取图只用到 /prompt、/history/{id}、/view 三个 HTTP 接口。
 * ============================================================ */

/* 连通测试：GET {base_url}/system_stats，200 即通。
 * 成功返回 0；失败返回 -1 并写 err（errlen 字节）。 */
int comfy_test(const char *base_url, char *err, size_t errlen);

/* 生成一张图：
 *   workflow_json : API 格式工作流文本，须含 {{PROMPT}} 占位符
 *   prompt        : 场景描述（JSON 转义后注入占位符）
 *   out_path      : PNG 落盘路径（目录需已存在）
 * 流程：注入提示词 -> POST /prompt -> 轮询 /history/{id} -> GET /view 存盘。
 * 成功返回 0；失败返回 -1 并写 err。总等待上限约 5 分钟。 */
int comfy_generate(const char *base_url, const char *workflow_json,
                   const char *prompt, const char *out_path,
                   char *err, size_t errlen);

/* 读取图片文件转 data URL（data:image/<ext>;base64,...），供前端当背景。
 * 调用方 free 返回值；失败返回 NULL。限 16MB 以内。 */
char* image_data_url(const char *path);

#ifdef __cplusplus
}
#endif

#endif
