#ifndef NOVEL_AI_PROJECT_H
#define NOVEL_AI_PROJECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cJSON.h"

/* 方向/创意 */
typedef struct {
    char *title;       /* 方向标题 */
    char *description; /* 方向描述 */
    int selected;      /* 是否被选中 */
} direction_t;

/* 小说方案（题目+简介+大纲） */
typedef struct {
    char *title;          /* 小说题目 */
    char *synopsis;       /* 内容简介 */
    char *outline;        /* 详细大纲（全文本） */
    int selected;         /* 是否被选中 */
} novel_plan_t;

/* 章节 */
typedef struct {
    int index;            /* 章节序号 */
    char *title;          /* 章节标题 */
    char *summary;        /* 章节概要 */
    char *content;        /* 章节正文内容 */
    int word_count;       /* 字数 */
    int status;           /* 0=未写, 1=写作中, 2=已完成, 3=已审核 */
    int iteration_count;  /* 迭代次数 */
} chapter_t;

/* 人物角色 */
typedef struct {
    char *name;           /* 角色姓名 */
    char *role;           /* 角色定位（主角/配角/反派等） */
    char *description;    /* 角色描述（外貌、性格、背景等） */
    char *traits;         /* 性格特点 */
    char *background;     /* 背景故事 */
} character_t;

/* 世界观设定 */
typedef struct {
    char *world_type;     /* 世界类型（玄幻、科幻、都市等） */
    char *setting;        /* 背景设定 */
    char *power_system;   /* 力量体系 */
    char *geography;      /* 地理环境 */
    char *history;        /* 历史背景 */
} world_setting_t;

/* 伏笔记录（由 AI 在每章写完后登记） */
typedef struct {
    char *title;          /* 伏笔简称，如「第7页被撕」 */
    char *description;    /* 一句话说明埋了什么 */
    int planted_chapter;  /* 埋设章节序号（0起） */
    int resolved_chapter; /* 回收章节序号；-1=尚未回收 */
} foreshadow_t;

/* 主题一致性检查记录 */
typedef struct {
    char *timestamp;      /* 检查时间 */
    int chapter_index;    /* 章节索引 */
    double score;         /* 符合度评分 0-1 */
    char *issues;         /* 发现的问题 */
    char *suggestions;    /* 改进建议 */
} consistency_check_t;

/* 小说项目 */
typedef struct {
    char *id;             /* 项目ID */
    char *name;           /* 项目名称 */
    char *created_at;     /* 创建时间 */
    char *updated_at;     /* 最后更新时间 */
    
    /* 用户原始想法 */
    char *seed_idea;      /* 用户最初的想法 */
    
    /* 方向列表 */
    direction_t *directions;
    int direction_count;
    int direction_capacity;
    
    /* 选中的方向 */
    direction_t *selected_direction;
    
    /* 方案列表 */
    novel_plan_t *plans;
    int plan_count;
    int plan_capacity;
    
    /* 选中的方案 */
    novel_plan_t *selected_plan;
    
    /* 世界观设定 */
    world_setting_t world;
    
    /* 人物角色列表 */
    character_t *characters;
    int character_count;
    int character_capacity;
    
    /* 章节列表 */
    chapter_t *chapters;
    int chapter_count;
    int chapter_capacity;
    
    /* 一致性检查记录 */
    consistency_check_t *checks;
    int check_count;
    int check_capacity;

    /* 伏笔登记表 */
    foreshadow_t *foreshadows;
    int foreshadow_count;
    int foreshadow_capacity;
    
    /* 元数据 */
    int total_words;      /* 总字数 */
    int iteration_round;  /* 当前迭代轮次 */
    char *current_stage;  /* 当前阶段: idea, direction, plan, outline, writing, review */
    char *finale_report;  /* 伏笔回收校验报告（全书） */
    
} novel_project_t;

/* ============== 项目管理 ============== */

novel_project_t* project_create(const char *name, const char *seed_idea);
void project_destroy(novel_project_t *proj);

/* 保存/加载 */
int project_save(novel_project_t *proj, const char *dir);
novel_project_t* project_load(const char *path);

/* ============== 方向管理 ============== */

void project_add_direction(novel_project_t *proj, const char *title, const char *description);
void project_select_direction(novel_project_t *proj, int index);
void project_clear_directions(novel_project_t *proj);

/* ============== 方案管理 ============== */

void project_add_plan(novel_project_t *proj, const char *title, const char *synopsis, const char *outline);
void project_select_plan(novel_project_t *proj, int index);
void project_clear_plans(novel_project_t *proj);

/* ============== 角色管理 ============== */

void project_add_character(novel_project_t *proj, const char *name, const char *role,
                           const char *description, const char *traits, const char *background);
/* 清空全部人物（数组保留复用）；用于重新生成人物草稿前 */
void project_clear_characters(novel_project_t *proj);

/* ============== 世界观管理 ============== */

/* 整体写入世界观字段；某字段传 NULL 表示保留原值 */
void project_set_world(novel_project_t *proj, const char *world_type,
                       const char *setting, const char *power_system);

/* ============== 章节管理 ============== */

void project_add_chapter(novel_project_t *proj, const char *title, const char *summary);
void project_clear_chapters(novel_project_t *proj);
chapter_t* project_get_chapter(novel_project_t *proj, int index);
void project_update_chapter_content(novel_project_t *proj, int index, const char *content);

/* ============== 一致性检查 ============== */

void project_add_check(novel_project_t *proj, int chapter_index, double score,
                       const char *issues, const char *suggestions);

/* ============== 伏笔登记 ============== */

/* 登记一条伏笔；title/description 会被复制。按 title+planted_chapter 去重。 */
void project_add_foreshadow(novel_project_t *proj, const char *title,
                            const char *description, int planted_chapter);
/* 标记某伏笔已在某章回收（按标题匹配，取最早未回收的一条）；0=成功 */
int project_resolve_foreshadow(novel_project_t *proj, const char *title, int chapter);
/* 前文记忆：逐章概要 + 活跃伏笔清单；调用方负责 free。 */
char* project_build_memory_block(novel_project_t *proj, int upto_chapter);

/* ============== 辅助函数 ============== */

/* 生成完整的系统提示词（包含所有背景设定） */
char* project_build_system_prompt(novel_project_t *proj, const char *extra_context);

/* 计算总字数 */
void project_calc_total_words(novel_project_t *proj);

/* 生成项目背景摘要（用于一致性检查） */
char* project_build_context_summary(novel_project_t *proj);

/* 拼接全部章节正文（无正文回退概要/大纲），超长按 UTF-8 字符边界截断；
   供 AI 做世界观总结、人物提取等整体分析 */
char* project_build_full_text(novel_project_t *proj, size_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif
