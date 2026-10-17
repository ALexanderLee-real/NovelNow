#include "project.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============== 辅助：安全字符串拷贝 ============== */

static char* safe_dup(const char *s)
{
    return s ? str_dup(s) : str_dup("");
}

/* ============== 项目创建/销毁 ============== */

static char* make_safe_id(void);   /* 定义在 project_save 前，创建时就要用 */

novel_project_t* project_create(const char *name, const char *seed_idea)
{
    novel_project_t *proj = (novel_project_t*)calloc(1, sizeof(novel_project_t));
    if (!proj) return NULL;
    
    char *ts = get_timestamp_str();
    /* id 必须过 make_safe_id（时间戳+序号）：裸时间戳同一秒建两个项目
       id 相同 → 同目录互相覆盖，用户项目被抹掉 */
    proj->id = make_safe_id();
    proj->name = safe_dup(name);
    proj->created_at = str_dup(ts ? ts : "");
    proj->updated_at = str_dup(ts ? ts : "");
    proj->seed_idea = safe_dup(seed_idea);
    proj->current_stage = str_dup("idea");
    proj->iteration_round = 0;
    proj->total_words = 0;
    proj->finale_report = str_dup("");
    proj->gal_json = str_dup("{}");
    free(ts);
    
    /* 初始化世界观 */
    proj->world.world_type = str_dup("");
    proj->world.setting = str_dup("");
    proj->world.power_system = str_dup("");
    proj->world.geography = str_dup("");
    proj->world.history = str_dup("");
    
    return proj;
}

static void foreshadow_clear(foreshadow_t *f);

void project_destroy(novel_project_t *proj)
{
    if (!proj) return;
    
    if (proj->id) free(proj->id);
    if (proj->name) free(proj->name);
    if (proj->created_at) free(proj->created_at);
    if (proj->updated_at) free(proj->updated_at);
    if (proj->seed_idea) free(proj->seed_idea);
    if (proj->current_stage) free(proj->current_stage);
    if (proj->finale_report) free(proj->finale_report);
    if (proj->gal_json) free(proj->gal_json);
    
    /* 方向 */
    for (int i = 0; i < proj->direction_count; i++) {
        if (proj->directions[i].title) free(proj->directions[i].title);
        if (proj->directions[i].description) free(proj->directions[i].description);
    }
    if (proj->directions) free(proj->directions);
    
    /* 方案 */
    for (int i = 0; i < proj->plan_count; i++) {
        if (proj->plans[i].title) free(proj->plans[i].title);
        if (proj->plans[i].synopsis) free(proj->plans[i].synopsis);
        if (proj->plans[i].outline) free(proj->plans[i].outline);
    }
    if (proj->plans) free(proj->plans);
    
    /* 世界观 */
    if (proj->world.world_type) free(proj->world.world_type);
    if (proj->world.setting) free(proj->world.setting);
    if (proj->world.power_system) free(proj->world.power_system);
    if (proj->world.geography) free(proj->world.geography);
    if (proj->world.history) free(proj->world.history);
    
    /* 角色 */
    for (int i = 0; i < proj->character_count; i++) {
        if (proj->characters[i].name) free(proj->characters[i].name);
        if (proj->characters[i].role) free(proj->characters[i].role);
        if (proj->characters[i].description) free(proj->characters[i].description);
        if (proj->characters[i].traits) free(proj->characters[i].traits);
        if (proj->characters[i].background) free(proj->characters[i].background);
    }
    if (proj->characters) free(proj->characters);
    
    /* 章节 */
    for (int i = 0; i < proj->chapter_count; i++) {
        if (proj->chapters[i].title) free(proj->chapters[i].title);
        if (proj->chapters[i].summary) free(proj->chapters[i].summary);
        if (proj->chapters[i].content) free(proj->chapters[i].content);
    }
    if (proj->chapters) free(proj->chapters);

    /* 伏笔 */
    for (int i = 0; i < proj->foreshadow_count; i++)
        foreshadow_clear(&proj->foreshadows[i]);
    free(proj->foreshadows);

    /* 检查记录 */
    for (int i = 0; i < proj->check_count; i++) {
        if (proj->checks[i].timestamp) free(proj->checks[i].timestamp);
        if (proj->checks[i].issues) free(proj->checks[i].issues);
        if (proj->checks[i].suggestions) free(proj->checks[i].suggestions);
    }
    if (proj->checks) free(proj->checks);
    
    free(proj);
}

/* ============== 方向管理 ============== */

void project_add_direction(novel_project_t *proj, const char *title, const char *description)
{
    if (!proj) return;
    if (proj->direction_count >= proj->direction_capacity) {
        int new_cap = proj->direction_capacity == 0 ? 8 : proj->direction_capacity * 2;
        direction_t *new_arr = (direction_t*)realloc(proj->directions, new_cap * sizeof(direction_t));
        if (!new_arr) return;
        memset(new_arr + proj->direction_count, 0, 
               (new_cap - proj->direction_count) * sizeof(direction_t));
        proj->directions = new_arr;
        proj->direction_capacity = new_cap;
    }
    proj->directions[proj->direction_count].title = safe_dup(title);
    proj->directions[proj->direction_count].description = safe_dup(description);
    proj->directions[proj->direction_count].selected = 0;
    proj->direction_count++;
}

void project_select_direction(novel_project_t *proj, int index)
{
    if (!proj || index < 0 || index >= proj->direction_count) return;
    for (int i = 0; i < proj->direction_count; i++) {
        proj->directions[i].selected = (i == index) ? 1 : 0;
    }
    proj->selected_direction = &proj->directions[index];
    if (proj->current_stage) free(proj->current_stage);
    proj->current_stage = str_dup("direction");
}

void project_clear_directions(novel_project_t *proj)
{
    if (!proj) return;
    for (int i = 0; i < proj->direction_count; i++) {
        if (proj->directions[i].title) free(proj->directions[i].title);
        if (proj->directions[i].description) free(proj->directions[i].description);
    }
    proj->direction_count = 0;
    proj->selected_direction = NULL;
}

/* ============== 方案管理 ============== */

void project_add_plan(novel_project_t *proj, const char *title, const char *synopsis, const char *outline)
{
    if (!proj) return;
    if (proj->plan_count >= proj->plan_capacity) {
        int new_cap = proj->plan_capacity == 0 ? 8 : proj->plan_capacity * 2;
        novel_plan_t *new_arr = (novel_plan_t*)realloc(proj->plans, new_cap * sizeof(novel_plan_t));
        if (!new_arr) return;
        memset(new_arr + proj->plan_count, 0,
               (new_cap - proj->plan_count) * sizeof(novel_plan_t));
        proj->plans = new_arr;
        proj->plan_capacity = new_cap;
    }
    proj->plans[proj->plan_count].title = safe_dup(title);
    proj->plans[proj->plan_count].synopsis = safe_dup(synopsis);
    proj->plans[proj->plan_count].outline = safe_dup(outline);
    proj->plans[proj->plan_count].selected = 0;
    proj->plan_count++;
}

void project_select_plan(novel_project_t *proj, int index)
{
    if (!proj || index < 0 || index >= proj->plan_count) return;
    for (int i = 0; i < proj->plan_count; i++) {
        proj->plans[i].selected = (i == index) ? 1 : 0;
    }
    proj->selected_plan = &proj->plans[index];
    if (proj->current_stage) free(proj->current_stage);
    proj->current_stage = str_dup("plan");
}

void project_clear_plans(novel_project_t *proj)
{
    if (!proj) return;
    for (int i = 0; i < proj->plan_count; i++) {
        if (proj->plans[i].title) free(proj->plans[i].title);
        if (proj->plans[i].synopsis) free(proj->plans[i].synopsis);
        if (proj->plans[i].outline) free(proj->plans[i].outline);
    }
    proj->plan_count = 0;
    proj->selected_plan = NULL;
}

/* ============== 角色管理 ============== */

void project_add_character(novel_project_t *proj, const char *name, const char *role,
                           const char *description, const char *traits, const char *background)
{
    if (!proj) return;
    if (proj->character_count >= proj->character_capacity) {
        int new_cap = proj->character_capacity == 0 ? 16 : proj->character_capacity * 2;
        character_t *new_arr = (character_t*)realloc(proj->characters, new_cap * sizeof(character_t));
        if (!new_arr) return;
        memset(new_arr + proj->character_count, 0,
               (new_cap - proj->character_count) * sizeof(character_t));
        proj->characters = new_arr;
        proj->character_capacity = new_cap;
    }
    character_t *c = &proj->characters[proj->character_count];
    c->name = safe_dup(name);
    c->role = safe_dup(role);
    c->description = safe_dup(description);
    c->traits = safe_dup(traits);
    c->background = safe_dup(background);
    proj->character_count++;
}

void project_clear_characters(novel_project_t *proj)
{
    if (!proj) return;
    for (int i = 0; i < proj->character_count; i++) {
        character_t *c = &proj->characters[i];
        if (c->name) free(c->name);
        if (c->role) free(c->role);
        if (c->description) free(c->description);
        if (c->traits) free(c->traits);
        if (c->background) free(c->background);
        memset(c, 0, sizeof(character_t));
    }
    /* 保留已分配数组供复用，仅重置数量；project_destroy 仍会安全释放数组本体 */
    proj->character_count = 0;
}

/* ============== 世界观管理 ============== */

/* 整体写入世界观字段；某字段传 NULL 表示保留原值不动 */
void project_set_world(novel_project_t *proj, const char *world_type,
                       const char *setting, const char *power_system)
{
    if (!proj) return;
    if (world_type) {
        free(proj->world.world_type);
        proj->world.world_type = safe_dup(world_type);
    }
    if (setting) {
        free(proj->world.setting);
        proj->world.setting = safe_dup(setting);
    }
    if (power_system) {
        free(proj->world.power_system);
        proj->world.power_system = safe_dup(power_system);
    }
}

/* ============== 章节管理 ============== */

/* 清空全部章节（数组保留复用）；细化大纲前必须调用，否则多次细化会追加导致章节数翻倍 */
void project_clear_chapters(novel_project_t *proj)
{
    if (!proj) return;
    for (int i = 0; i < proj->chapter_count; i++) {
        chapter_t *ch = &proj->chapters[i];
        if (ch->title) free(ch->title);
        if (ch->summary) free(ch->summary);
        if (ch->content) free(ch->content);
        memset(ch, 0, sizeof(chapter_t));
    }
    /* 保留已分配数组供复用，仅重置数量；project_destroy 仍会安全释放数组本体 */
    proj->chapter_count = 0;
}

void project_add_chapter(novel_project_t *proj, const char *title, const char *summary)
{
    if (!proj) return;
    if (proj->chapter_count >= proj->chapter_capacity) {
        int new_cap = proj->chapter_capacity == 0 ? 32 : proj->chapter_capacity * 2;
        chapter_t *new_arr = (chapter_t*)realloc(proj->chapters, new_cap * sizeof(chapter_t));
        if (!new_arr) return;
        memset(new_arr + proj->chapter_count, 0,
               (new_cap - proj->chapter_count) * sizeof(chapter_t));
        proj->chapters = new_arr;
        proj->chapter_capacity = new_cap;
    }
    chapter_t *ch = &proj->chapters[proj->chapter_count];
    ch->index = proj->chapter_count;
    ch->title = safe_dup(title);
    ch->summary = safe_dup(summary);
    ch->content = str_dup("");
    ch->word_count = 0;
    ch->status = 0;
    ch->iteration_count = 0;
    proj->chapter_count++;
}

chapter_t* project_get_chapter(novel_project_t *proj, int index)
{
    if (!proj || index < 0 || index >= proj->chapter_count) return NULL;
    return &proj->chapters[index];
}

void project_update_chapter_content(novel_project_t *proj, int index, const char *content)
{
    chapter_t *ch = project_get_chapter(proj, index);
    if (!ch) return;
    if (ch->content) free(ch->content);
    ch->content = safe_dup(content);
    /* 字数应统计字符数而非字节数：中文每字 3 字节，按字节统计会虚高 3 倍 */
    ch->word_count = (int)utf8_char_count(content);
    ch->status = 2;
    project_calc_total_words(proj);
}

/* ============== 一致性检查 ============== */

void project_add_check(novel_project_t *proj, int chapter_index, double score,
                       const char *issues, const char *suggestions)
{
    if (!proj) return;
    if (proj->check_count >= proj->check_capacity) {
        int new_cap = proj->check_capacity == 0 ? 16 : proj->check_capacity * 2;
        consistency_check_t *new_arr = (consistency_check_t*)realloc(proj->checks, 
                                            new_cap * sizeof(consistency_check_t));
        if (!new_arr) return;
        memset(new_arr + proj->check_count, 0,
               (new_cap - proj->check_count) * sizeof(consistency_check_t));
        proj->checks = new_arr;
        proj->check_capacity = new_cap;
    }
    consistency_check_t *c = &proj->checks[proj->check_count];
    c->timestamp = get_timestamp_str();
    c->chapter_index = chapter_index;
    c->score = score;
    c->issues = safe_dup(issues);
    c->suggestions = safe_dup(suggestions);
    proj->check_count++;
}

/* ============== 字数统计 ============== */

void project_calc_total_words(novel_project_t *proj)
{
    if (!proj) return;
    int total = 0;
    for (int i = 0; i < proj->chapter_count; i++) {
        total += proj->chapters[i].word_count;
    }
    proj->total_words = total;
}

/* ============== 生成背景摘要 ============== */

char* project_build_context_summary(novel_project_t *proj)
{
    if (!proj) return str_dup("");
    
    strbuf_t *buf = strbuf_create(4096);
    
    strbuf_append(buf, "【小说项目】");
    strbuf_append(buf, proj->name ? proj->name : "未命名");
    strbuf_append(buf, "\n\n");
    
    if (proj->seed_idea && *proj->seed_idea) {
        strbuf_append(buf, "【原始想法】\n");
        strbuf_append(buf, proj->seed_idea);
        strbuf_append(buf, "\n\n");
    }
    
    if (proj->selected_direction) {
        strbuf_append(buf, "【选定方向】\n");
        strbuf_append(buf, proj->selected_direction->title);
        strbuf_append(buf, "\n");
        strbuf_append(buf, proj->selected_direction->description);
        strbuf_append(buf, "\n\n");
    }
    
    if (proj->selected_plan) {
        strbuf_append(buf, "【小说题目】");
        strbuf_append(buf, proj->selected_plan->title);
        strbuf_append(buf, "\n\n");
        strbuf_append(buf, "【内容简介】\n");
        strbuf_append(buf, proj->selected_plan->synopsis);
        strbuf_append(buf, "\n\n");
    }
    
    if (proj->world.world_type && *proj->world.world_type) {
        strbuf_append(buf, "【世界观设定】\n");
        strbuf_append(buf, "世界类型：");
        strbuf_append(buf, proj->world.world_type);
        strbuf_append(buf, "\n");
        if (proj->world.setting && *proj->world.setting) {
            strbuf_append(buf, "背景设定：");
            strbuf_append(buf, proj->world.setting);
            strbuf_append(buf, "\n");
        }
        if (proj->world.power_system && *proj->world.power_system) {
            strbuf_append(buf, "力量体系：");
            strbuf_append(buf, proj->world.power_system);
            strbuf_append(buf, "\n");
        }
        strbuf_append(buf, "\n");
    }
    
    if (proj->character_count > 0) {
        strbuf_append(buf, "【主要人物】\n");
        for (int i = 0; i < proj->character_count; i++) {
            character_t *c = &proj->characters[i];
            strbuf_append(buf, "- ");
            strbuf_append(buf, c->name);
            if (c->role && *c->role) {
                strbuf_append(buf, "（");
                strbuf_append(buf, c->role);
                strbuf_append(buf, "）");
            }
            if (c->description && *c->description) {
                strbuf_append(buf, "：");
                strbuf_append(buf, c->description);
            }
            if (c->traits && *c->traits) {
                strbuf_append(buf, " [性格：");
                strbuf_append(buf, c->traits);
                strbuf_append(buf, "]");
            }
            strbuf_append(buf, "\n");
        }
        strbuf_append(buf, "\n");
    }
    
    if (proj->selected_plan && proj->selected_plan->outline && *proj->selected_plan->outline) {
        strbuf_append(buf, "【故事大纲】\n");
        strbuf_append(buf, proj->selected_plan->outline);
        strbuf_append(buf, "\n");
    }
    
    char *result = strbuf_dup(buf);
    strbuf_destroy(buf);
    return result;
}

/* ============== 伏笔登记 ============== */

static void foreshadow_clear(foreshadow_t *f)
{
    if (f->title) free(f->title);
    if (f->description) free(f->description);
    f->title = NULL;
    f->description = NULL;
}

/* 原始追加（不去重）：返回新条目指针，失败返回 NULL。
   load 与 add 共用，保证「加完的那条」身份明确——绝不写 foreshadows[count-1] 猜身份 */
static foreshadow_t *foreshadow_push(novel_project_t *proj, const char *title,
                                     const char *description, int planted_chapter)
{
    if (!proj) return NULL;
    if (proj->foreshadow_count >= proj->foreshadow_capacity) {
        int cap = proj->foreshadow_capacity ? proj->foreshadow_capacity * 2 : 16;
        foreshadow_t *n = (foreshadow_t*)realloc(proj->foreshadows,
                                                 cap * sizeof(foreshadow_t));
        if (!n) return NULL;
        proj->foreshadows = n;
        proj->foreshadow_capacity = cap;
    }

    foreshadow_t *f = &proj->foreshadows[proj->foreshadow_count++];
    memset(f, 0, sizeof(*f));   /* 新区清零：加字段后不读未初始化内存 */
    f->title = str_dup(title ? title : "");
    f->description = str_dup(description ? description : "");
    f->planted_chapter = planted_chapter;
    f->resolved_chapter = -1;
    return f;
}

void project_add_foreshadow(novel_project_t *proj, const char *title,
                            const char *description, int planted_chapter)
{
    if (!proj || !title || !*title) return;

    /* 去重：同一章埋的同名伏笔不重复登记 */
    for (int i = 0; i < proj->foreshadow_count; i++) {
        foreshadow_t *f = &proj->foreshadows[i];
        if (f->planted_chapter == planted_chapter && f->title &&
            strcmp(f->title, title) == 0) return;
    }

    foreshadow_push(proj, title, description, planted_chapter);
}

int project_resolve_foreshadow(novel_project_t *proj, const char *title, int chapter)
{
    if (!proj || !title || !*title) return -1;
    for (int i = 0; i < proj->foreshadow_count; i++) {
        foreshadow_t *f = &proj->foreshadows[i];
        if (f->resolved_chapter < 0 && f->title && strcmp(f->title, title) == 0) {
            f->resolved_chapter = chapter;
            return 0;
        }
    }
    return -1;
}

/* 前文记忆块：逐章概要 + 活跃伏笔。upto_chapter 为当前要写的章节序号，
   只统计它之前的章节。 */
char* project_build_memory_block(novel_project_t *proj, int upto_chapter)
{
    if (!proj) return str_dup("");

    strbuf_t *buf = strbuf_create(4096);
    int any = 0;

    for (int i = 0; i < proj->chapter_count && i < upto_chapter; i++) {
        chapter_t *c = &proj->chapters[i];
        if (!c->summary || !*c->summary) continue;
        if (!any) {
            strbuf_append(buf, "【前文梗概】\n");
            any = 1;
        }
        char head[128];
        snprintf(head, sizeof(head), "第%d章《%s》：", i + 1,
                 (c->title && *c->title) ? c->title : "");
        strbuf_append(buf, head);
        strbuf_append(buf, c->summary);
        strbuf_append(buf, "\n");
    }

    /* 活跃伏笔 = 已埋未收 */
    int open = 0;
    for (int i = 0; i < proj->foreshadow_count; i++) {
        foreshadow_t *f = &proj->foreshadows[i];
        if (f->resolved_chapter >= 0 || f->planted_chapter >= upto_chapter) continue;
        if (!open) {
            strbuf_append(buf, "\n【尚未回收的伏笔（务必呼应或继续铺垫，但不要强行揭底）】\n");
            open = 1;
        }
        strbuf_append(buf, "- ");
        strbuf_append(buf, f->title);
        if (f->description && *f->description) {
            strbuf_append(buf, "：");
            strbuf_append(buf, f->description);
        }
        char tag[64];
        snprintf(tag, sizeof(tag), "（第%d章埋）", f->planted_chapter + 1);
        strbuf_append(buf, tag);
        strbuf_append(buf, "\n");
    }

    if (!any && !open) {
        strbuf_destroy(buf);
        return str_dup("");
    }
    char *result = strbuf_dup(buf);
    strbuf_destroy(buf);
    return result;
}

/* ============== 生成全文（用于整体分析） ============== */

/* 拼接全部章节正文，供 AI 做世界观总结/人物提取等整体分析。
   超过 max_bytes 时在 UTF-8 字符边界截断，不会切出半个字。
   章节无正文时回退到章节概要；一个字都没有时回退到大纲。 */
char* project_build_full_text(novel_project_t *proj, size_t max_bytes)
{
    if (!proj) return str_dup("");

    strbuf_t *buf = strbuf_create(8192);
    size_t used = 0;
    char head[256];
    int i;

    for (i = 0; i < proj->chapter_count; i++) {
        chapter_t *ch = &proj->chapters[i];
        const char *body = (ch->content && *ch->content) ? ch->content : ch->summary;
        size_t body_len, budget, head_len;

        if (!body || !*body) continue;
        if (used >= max_bytes) break;

        snprintf(head, sizeof(head), "\n【第%d章 %s】\n", i + 1,
                 (ch->title && *ch->title) ? ch->title : "");
        head_len = strlen(head);
        if (used + head_len < max_bytes) {
            strbuf_append(buf, head);
            used += head_len;
        }

        budget = (used < max_bytes) ? (max_bytes - used) : 0;
        body_len = strlen(body);
        if (body_len > budget) body_len = utf8_safe_len(body, budget);
        if (body_len > 0) {
            strbuf_append_len(buf, body, body_len);
            used += body_len;
        }
    }

    /* 没有任何章节内容时回退到大纲与简介，保证分析有素材 */
    if (used == 0) {
        if (proj->selected_plan) {
            if (proj->selected_plan->synopsis && *proj->selected_plan->synopsis) {
                size_t n = strlen(proj->selected_plan->synopsis);
                if (n > max_bytes) n = utf8_safe_len(proj->selected_plan->synopsis, max_bytes);
                strbuf_append_len(buf, proj->selected_plan->synopsis, n);
                used += n;
            }
            if (used < max_bytes && proj->selected_plan->outline && *proj->selected_plan->outline) {
                size_t budget = max_bytes - used;
                size_t n = strlen(proj->selected_plan->outline);
                if (n > budget) n = utf8_safe_len(proj->selected_plan->outline, budget);
                strbuf_append_len(buf, proj->selected_plan->outline, n);
            }
        }
        if (used == 0 && proj->seed_idea && *proj->seed_idea) {
            size_t n = strlen(proj->seed_idea);
            if (n > max_bytes) n = utf8_safe_len(proj->seed_idea, max_bytes);
            strbuf_append_len(buf, proj->seed_idea, n);
        }
    }

    char *result = strbuf_dup(buf);
    strbuf_destroy(buf);
    return result;
}

/* ============== 生成系统提示词 ============== */

char* project_build_system_prompt(novel_project_t *proj, const char *extra_context)
{
    strbuf_t *buf = strbuf_create(8192);
    
    strbuf_append(buf, "你是一位专业的小说创作助手。请根据以下小说背景设定进行创作，严格保持设定的一致性。\n\n");
    
    char *summary = project_build_context_summary(proj);
    strbuf_append(buf, summary);
    free(summary);
    
    if (extra_context && *extra_context) {
        strbuf_append(buf, "\n【补充说明】\n");
        strbuf_append(buf, extra_context);
        strbuf_append(buf, "\n");
    }
    
    strbuf_append(buf, "\n创作要求：\n");
    strbuf_append(buf, "1. 严格遵守上述世界观设定和人物设定\n");
    strbuf_append(buf, "2. 人物性格和行为要前后一致，避免OOC（脱离角色）\n");
    strbuf_append(buf, "3. 故事情节要符合逻辑，符合世界观规则\n");
    strbuf_append(buf, "4. 保持叙事风格的统一性\n");
    strbuf_append(buf, "5. 如果发现设定有矛盾或不足，请在回复中指出\n");
    strbuf_append(buf, "6. 禁止重复套路：不要反复用同一个身体动作表达情绪（如\"掐进掌心\"\"攥拳\"\"抿唇\"），同一动作或道具每章出现不超过3次，多用对话、神态、环境细节的差异来呈现情绪\n");
    strbuf_append(buf, "7. 控制比喻密度：不要连续堆砌\"像……\"式的比喻句，同一段落至多一处比喻，能用白描直陈的就不比\n");
    strbuf_append(buf, "8. 控制句子长度：避免超长定语层层堆叠的句子，单句尽量不超过50字，长短句交替\n");
    strbuf_append(buf, "9. 术语统一：同一人物、事物、道具全文只用一个名称，严禁同物异名（如\"密钥\"与\"密匙\"混用），前文出现过的名字后续必须沿用\n");
    
    char *result = strbuf_dup(buf);
    strbuf_destroy(buf);
    return result;
}

/* ============== 序列化/反序列化 ============== */

static void add_string_to_json(cJSON *obj, const char *key, const char *val)
{
    cJSON_AddStringToObject(obj, key, val ? val : "");
}

static const char* get_json_string(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return "";
}

/* 项目 id 会被直接拼进文件系统路径（project_save: path_join(dir, proj->id)），
   因此只允许 ASCII 标识符字符，且长度受限，杜绝路径穿越/非法字符 */
static int is_safe_id(const char *id)
{
    size_t n = 0;
    if (!id || !*id) return 0;
    for (const char *p = id; *p; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return 0;
        if (++n > 64) return 0;
    }
    /* Windows 保留设备名：con/prn/aux/nul/com1-9/lpt1-9 做目录名会打到设备上 */
    {
        static const char *reserved[] = {
            "con","prn","aux","nul",
            "com1","com2","com3","com4","com5","com6","com7","com8","com9",
            "lpt1","lpt2","lpt3","lpt4","lpt5","lpt6","lpt7","lpt8","lpt9", NULL };
        for (int i = 0; reserved[i]; i++) {
            if (_stricmp(id, reserved[i]) == 0) return 0;
        }
    }
    return 1;
}

/* 生成一个安全的替代 id（时间戳 + 进程内序号）。
   同一秒连续创建多个项目会拿到相同时间戳，序号保证本次运行内不撞车，
   否则以 id 命名的文件/目录会互相覆盖 */
static char* make_safe_id(void)
{
    static unsigned seq = 0;
    char *ts = get_timestamp_str();
    char buf[64];
    if (!ts) return str_dup("project");
    snprintf(buf, sizeof(buf), "%s_%u", ts, (unsigned)++seq);
    free(ts);
    return str_dup(buf);
}

int project_save(novel_project_t *proj, const char *dir)
{
    if (!proj || !dir) return -1;
    if (!is_safe_id(proj->id)) {
        free(proj->id);
        proj->id = make_safe_id();
    }
    
    /* 每次落盘刷新更新时间：自动保存上线后 updated_at 若停在创建时刻，
       项目列表的"最后修改"就永远是新建时间 */
    {
        char *ts = get_timestamp_str();
        if (ts) { free(proj->updated_at); proj->updated_at = ts; }
    }
    
    char *project_dir = path_join(dir, proj->id);
    ensure_dir(project_dir);
    
    cJSON *root = cJSON_CreateObject();
    add_string_to_json(root, "id", proj->id);
    add_string_to_json(root, "name", proj->name);
    add_string_to_json(root, "created_at", proj->created_at);
    add_string_to_json(root, "updated_at", proj->updated_at);
    add_string_to_json(root, "seed_idea", proj->seed_idea);
    add_string_to_json(root, "current_stage", proj->current_stage);
    add_string_to_json(root, "finale_report", proj->finale_report);
    {
        /* galgame 分支剧情状态：整块 JSON 文本，内嵌为对象。
           解析失败绝不能静默清空（那等于销毁用户剧情存档）——
           原文以 "gal_raw" 字符串键保底落盘，load 时优先还原 */
        cJSON *gal = cJSON_Parse(proj->gal_json ? proj->gal_json : "{}");
        if (gal) {
            cJSON_AddItemToObject(root, "gal", gal);
        } else {
            cJSON_AddStringToObject(root, "gal_raw", proj->gal_json ? proj->gal_json : "");
        }
    }
    cJSON_AddNumberToObject(root, "total_words", proj->total_words);
    cJSON_AddNumberToObject(root, "iteration_round", proj->iteration_round);
    
    /* 方向 */
    cJSON *dirs = cJSON_CreateArray();
    for (int i = 0; i < proj->direction_count; i++) {
        cJSON *d = cJSON_CreateObject();
        add_string_to_json(d, "title", proj->directions[i].title);
        add_string_to_json(d, "description", proj->directions[i].description);
        cJSON_AddBoolToObject(d, "selected", proj->directions[i].selected);
        cJSON_AddItemToArray(dirs, d);
    }
    cJSON_AddItemToObject(root, "directions", dirs);
    cJSON_AddNumberToObject(root, "selected_direction_index", 
        proj->selected_direction ? (int)(proj->selected_direction - proj->directions) : -1);
    
    /* 方案 */
    cJSON *plans = cJSON_CreateArray();
    for (int i = 0; i < proj->plan_count; i++) {
        cJSON *p = cJSON_CreateObject();
        add_string_to_json(p, "title", proj->plans[i].title);
        add_string_to_json(p, "synopsis", proj->plans[i].synopsis);
        add_string_to_json(p, "outline", proj->plans[i].outline);
        cJSON_AddBoolToObject(p, "selected", proj->plans[i].selected);
        cJSON_AddItemToArray(plans, p);
    }
    cJSON_AddItemToObject(root, "plans", plans);
    cJSON_AddNumberToObject(root, "selected_plan_index",
        proj->selected_plan ? (int)(proj->selected_plan - proj->plans) : -1);
    
    /* 世界观 */
    cJSON *world = cJSON_CreateObject();
    add_string_to_json(world, "world_type", proj->world.world_type);
    add_string_to_json(world, "setting", proj->world.setting);
    add_string_to_json(world, "power_system", proj->world.power_system);
    add_string_to_json(world, "geography", proj->world.geography);
    add_string_to_json(world, "history", proj->world.history);
    cJSON_AddItemToObject(root, "world", world);
    
    /* 角色 */
    cJSON *chars = cJSON_CreateArray();
    for (int i = 0; i < proj->character_count; i++) {
        cJSON *c = cJSON_CreateObject();
        add_string_to_json(c, "name", proj->characters[i].name);
        add_string_to_json(c, "role", proj->characters[i].role);
        add_string_to_json(c, "description", proj->characters[i].description);
        add_string_to_json(c, "traits", proj->characters[i].traits);
        add_string_to_json(c, "background", proj->characters[i].background);
        cJSON_AddItemToArray(chars, c);
    }
    cJSON_AddItemToObject(root, "characters", chars);
    
    /* 章节 */
    cJSON *chaps = cJSON_CreateArray();
    for (int i = 0; i < proj->chapter_count; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "index", proj->chapters[i].index);
        add_string_to_json(c, "title", proj->chapters[i].title);
        add_string_to_json(c, "summary", proj->chapters[i].summary);
        add_string_to_json(c, "content", proj->chapters[i].content);
        cJSON_AddNumberToObject(c, "word_count", proj->chapters[i].word_count);
        cJSON_AddNumberToObject(c, "status", proj->chapters[i].status);
        cJSON_AddNumberToObject(c, "iteration_count", proj->chapters[i].iteration_count);
        cJSON_AddItemToArray(chaps, c);
    }
    cJSON_AddItemToObject(root, "chapters", chaps);
    
    /* 检查记录 */
    cJSON *checks = cJSON_CreateArray();
    for (int i = 0; i < proj->check_count; i++) {
        cJSON *c = cJSON_CreateObject();
        add_string_to_json(c, "timestamp", proj->checks[i].timestamp);
        cJSON_AddNumberToObject(c, "chapter_index", proj->checks[i].chapter_index);
        cJSON_AddNumberToObject(c, "score", proj->checks[i].score);
        add_string_to_json(c, "issues", proj->checks[i].issues);
        add_string_to_json(c, "suggestions", proj->checks[i].suggestions);
        cJSON_AddItemToArray(checks, c);
    }
    cJSON_AddItemToObject(root, "consistency_checks", checks);

    /* 伏笔 */
    cJSON *fos = cJSON_CreateArray();
    for (int i = 0; i < proj->foreshadow_count; i++) {
        cJSON *f = cJSON_CreateObject();
        add_string_to_json(f, "title", proj->foreshadows[i].title);
        add_string_to_json(f, "description", proj->foreshadows[i].description);
        cJSON_AddNumberToObject(f, "planted_chapter", proj->foreshadows[i].planted_chapter);
        cJSON_AddNumberToObject(f, "resolved_chapter", proj->foreshadows[i].resolved_chapter);
        cJSON_AddItemToArray(fos, f);
    }
    cJSON_AddItemToObject(root, "foreshadows", fos);
    
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) {
        free(project_dir);
        return -1;
    }
    
    char *proj_file = path_join(project_dir, "project.json");
    /* 原子写：直接覆盖唯一副本会在崩溃/断电/磁盘满时留下截断的 project.json */
    int ret = proj_file ? write_file_atomic(proj_file, json_str) : -1;
    
    free(json_str);
    free(proj_file);
    free(project_dir);
    
    return ret;
}

novel_project_t* project_load(const char *path)
{
    if (!path || !file_exists(path)) return NULL;
    
    char *json_str = read_file(path);
    if (!json_str) return NULL;
    
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) return NULL;
    
    novel_project_t *proj = (novel_project_t*)calloc(1, sizeof(novel_project_t));
    if (!proj) { cJSON_Delete(root); return NULL; }
    
    proj->id = safe_dup(get_json_string(root, "id"));
    /* id 直接被 project_save 用作目录名：来自 JSON 的值必须校验，
       否则 "..\..\\x" / 盘符 / 通配符会让保存写到项目树之外 */
    if (!is_safe_id(proj->id)) {
        free(proj->id);
        proj->id = make_safe_id();
    }
    proj->name = safe_dup(get_json_string(root, "name"));
    proj->created_at = safe_dup(get_json_string(root, "created_at"));
    proj->updated_at = safe_dup(get_json_string(root, "updated_at"));
    proj->seed_idea = safe_dup(get_json_string(root, "seed_idea"));
    proj->current_stage = safe_dup(get_json_string(root, "current_stage"));
    proj->finale_report = safe_dup(get_json_string(root, "finale_report"));
    {
        /* galgame 分支剧情状态（"gal" 对象 <-> gal_json 文本）；
           "gal_raw" 是保存时解析失败的保底原文，优先级高于空对象 */
        cJSON *gal = cJSON_GetObjectItem(root, "gal");
        cJSON *gal_raw = cJSON_GetObjectItem(root, "gal_raw");
        char *gs = NULL;
        if (gal) gs = cJSON_PrintUnformatted(gal);
        else if (gal_raw && cJSON_IsString(gal_raw) && gal_raw->valuestring && *gal_raw->valuestring)
            gs = str_dup(gal_raw->valuestring);
        proj->gal_json = gs ? gs : str_dup("{}");
    }
    
    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "total_words")) && cJSON_IsNumber(item))
        proj->total_words = item->valueint;
    if ((item = cJSON_GetObjectItem(root, "iteration_round")) && cJSON_IsNumber(item))
        proj->iteration_round = item->valueint;
    
    /* 方向 */
    cJSON *dirs = cJSON_GetObjectItem(root, "directions");
    if (dirs && cJSON_IsArray(dirs)) {
        int count = cJSON_GetArraySize(dirs);
        for (int i = 0; i < count; i++) {
            cJSON *d = cJSON_GetArrayItem(dirs, i);
            project_add_direction(proj,
                get_json_string(d, "title"),
                get_json_string(d, "description"));
            cJSON *sel = cJSON_GetObjectItem(d, "selected");
            if (sel && sel->type & cJSON_True) proj->directions[i].selected = 1;
        }
    }
    int sel_dir_idx = -1;
    if ((item = cJSON_GetObjectItem(root, "selected_direction_index")) && cJSON_IsNumber(item))
        sel_dir_idx = item->valueint;
    if (sel_dir_idx >= 0 && sel_dir_idx < proj->direction_count)
        proj->selected_direction = &proj->directions[sel_dir_idx];
    
    /* 方案 */
    cJSON *plans = cJSON_GetObjectItem(root, "plans");
    if (plans && cJSON_IsArray(plans)) {
        int count = cJSON_GetArraySize(plans);
        for (int i = 0; i < count; i++) {
            cJSON *p = cJSON_GetArrayItem(plans, i);
            project_add_plan(proj,
                get_json_string(p, "title"),
                get_json_string(p, "synopsis"),
                get_json_string(p, "outline"));
            cJSON *sel = cJSON_GetObjectItem(p, "selected");
            if (sel && sel->type & cJSON_True) proj->plans[i].selected = 1;
        }
    }
    int sel_plan_idx = -1;
    if ((item = cJSON_GetObjectItem(root, "selected_plan_index")) && cJSON_IsNumber(item))
        sel_plan_idx = item->valueint;
    if (sel_plan_idx >= 0 && sel_plan_idx < proj->plan_count)
        proj->selected_plan = &proj->plans[sel_plan_idx];
    
    /* 世界观 */
    cJSON *world = cJSON_GetObjectItem(root, "world");
    if (world) {
        proj->world.world_type = safe_dup(get_json_string(world, "world_type"));
        proj->world.setting = safe_dup(get_json_string(world, "setting"));
        proj->world.power_system = safe_dup(get_json_string(world, "power_system"));
        proj->world.geography = safe_dup(get_json_string(world, "geography"));
        proj->world.history = safe_dup(get_json_string(world, "history"));
    } else {
        proj->world.world_type = str_dup("");
        proj->world.setting = str_dup("");
        proj->world.power_system = str_dup("");
        proj->world.geography = str_dup("");
        proj->world.history = str_dup("");
    }
    
    /* 角色 */
    cJSON *chars = cJSON_GetObjectItem(root, "characters");
    if (chars && cJSON_IsArray(chars)) {
        int count = cJSON_GetArraySize(chars);
        for (int i = 0; i < count; i++) {
            cJSON *c = cJSON_GetArrayItem(chars, i);
            project_add_character(proj,
                get_json_string(c, "name"),
                get_json_string(c, "role"),
                get_json_string(c, "description"),
                get_json_string(c, "traits"),
                get_json_string(c, "background"));
        }
    }
    
    /* 章节 */
    cJSON *chaps = cJSON_GetObjectItem(root, "chapters");
    if (chaps && cJSON_IsArray(chaps)) {
        int count = cJSON_GetArraySize(chaps);
        for (int i = 0; i < count; i++) {
            cJSON *c = cJSON_GetArrayItem(chaps, i);
            project_add_chapter(proj,
                get_json_string(c, "title"),
                get_json_string(c, "summary"));
            chapter_t *ch = &proj->chapters[i];
            if ((item = cJSON_GetObjectItem(c, "index")) && cJSON_IsNumber(item))
                ch->index = item->valueint;
            if ((item = cJSON_GetObjectItem(c, "content")) && cJSON_IsString(item)) {
                free(ch->content);
                ch->content = safe_dup(item->valuestring);
            }
            if ((item = cJSON_GetObjectItem(c, "word_count")) && cJSON_IsNumber(item))
                ch->word_count = item->valueint;
            if ((item = cJSON_GetObjectItem(c, "status")) && cJSON_IsNumber(item))
                ch->status = item->valueint;
            if ((item = cJSON_GetObjectItem(c, "iteration_count")) && cJSON_IsNumber(item))
                ch->iteration_count = item->valueint;
        }
    }
    
    /* 检查记录 */
    cJSON *checks = cJSON_GetObjectItem(root, "consistency_checks");
    if (checks && cJSON_IsArray(checks)) {
        int count = cJSON_GetArraySize(checks);
        for (int i = 0; i < count; i++) {
            cJSON *c = cJSON_GetArrayItem(checks, i);
            int ci = 0; double sc = 0;
            if ((item = cJSON_GetObjectItem(c, "chapter_index")) && cJSON_IsNumber(item))
                ci = item->valueint;
            if ((item = cJSON_GetObjectItem(c, "score")) && cJSON_IsNumber(item))
                sc = item->valuedouble;
            project_add_check(proj, ci, sc,
                get_json_string(c, "issues"),
                get_json_string(c, "suggestions"));
        }
    }

    /* 伏笔 */
    cJSON *fos = cJSON_GetObjectItem(root, "foreshadows");
    if (fos && cJSON_IsArray(fos)) {
        int count = cJSON_GetArraySize(fos);
        for (int i = 0; i < count; i++) {
            cJSON *c = cJSON_GetArrayItem(fos, i);
            const char *ft = get_json_string(c, "title");
            int planted = 0, resolved = -1;
            if ((item = cJSON_GetObjectItem(c, "planted_chapter")) && cJSON_IsNumber(item))
                planted = item->valueint;
            if ((item = cJSON_GetObjectItem(c, "resolved_chapter")) && cJSON_IsNumber(item))
                resolved = item->valueint;
            /* project_add_foreshadow 有去重会吞条目；load 必须原样恢复（含同名/空标题），
            所以走 foreshadow_push，并把回收状态写到「刚加的那条」身上 */
            foreshadow_t *f = foreshadow_push(proj, ft, get_json_string(c, "description"), planted);
            if (f && resolved >= 0) f->resolved_chapter = resolved;
        }
    }
    
    cJSON_Delete(root);
    project_calc_total_words(proj);
    return proj;
}
