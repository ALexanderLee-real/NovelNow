/* host_cmds.c - file dialogs + export/import + JSON command dispatch (split from host.c 2026-10-18, zero behavior change) */
#include "host_internal.h"
#include "comfy_api.h"
#include "epub_export.h"


/* ============================================================
 * 13. 文件对话框
 * ============================================================ */

int CALLBACK browse_cb(HWND h, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED && data)
        SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

/* 弹出文件夹选择框；返回 UTF-8 路径（需 free），取消返回 NULL */
char *pick_folder_dialog(const char *initial)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    WCHAR title[] = L"请选择输出文件夹";
    WCHAR initW[MAX_PATH] = L"";
    char *result = NULL;

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g.hwnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_cb;
    if (initial && *initial) {
        MultiByteToWideChar(CP_UTF8, 0, initial, -1, initW, MAX_PATH);
        bi.lParam = (LPARAM)initW;
    }

    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return NULL;
    if (SHGetPathFromIDListW(pidl, initW)) result = w_to_u8_alloc(initW);
    CoTaskMemFree(pidl);
    return result;
}

/* 选择 project.json；返回 UTF-8 路径（需 free），取消返回 NULL */
char *pick_project_file_dialog(const char *initial_dir)
{
    OPENFILENAMEW ofn;
    WCHAR szFileW[1024] = L"";
    WCHAR filterW[256];
    WCHAR initDirW[MAX_PATH] = L"";
    const char *segs[4] = { "项目文件 (project.json)", "project.json", "所有文件 (*.*)", "*.*" };
    int si, k = 0;
    char *result = NULL;

    for (si = 0; si < 4; si++) {
        WCHAR *sg = u8_to_w(segs[si]);
        if (sg) {
            int n = lstrlenW(sg);
            if (k + n + 1 < 256) {
                memcpy(filterW + k, sg, (size_t)n * sizeof(WCHAR));
                k += n;
                filterW[k++] = L'\0';
            }
            free(sg);
        }
    }
    filterW[k] = L'\0';

    if (initial_dir && *initial_dir)
        MultiByteToWideChar(CP_UTF8, 0, initial_dir, -1, initDirW, MAX_PATH);

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFile = szFileW;
    ofn.nMaxFile = 1024;
    ofn.lpstrFilter = filterW;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = initDirW;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) result = w_to_u8_alloc(szFileW);
    return result;
}

/* 选择导入的 txt；返回 UTF-8 路径（需 free），取消返回 NULL */
char *pick_text_file_dialog(void)
{
    OPENFILENAMEW ofn;
    WCHAR szFileW[1024] = L"";
    WCHAR filterW[] = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";
    char *result = NULL;

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFile = szFileW;
    ofn.nMaxFile = 1024;
    ofn.lpstrFilter = filterW;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) result = w_to_u8_alloc(szFileW);
    return result;
}

/* ============================================================
 * 14. 导出（DOC/RTF + TXT）—— 移植自旧版 main.c，逻辑未改
 * ============================================================ */

int write_utf8_bom_file(const char *path, const char *content)
{
    static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fwrite(bom, 1, 3, fp);
    if (content) fwrite(content, 1, strlen(content), fp);
    fclose(fp);
    return 0;
}

void rtf_append_text(strbuf_t *buf, const char *utf8)
{
    const unsigned char *p = (const unsigned char *)utf8;
    if (!utf8) return;
    while (*p) {
        if (*p < 0x80) {
            if (*p == '\\' || *p == '{' || *p == '}') strbuf_append_char(buf, '\\');
            strbuf_append_char(buf, (char)*p);
            p++;
        } else {
            unsigned int cp = 0;
            int len = 0, i;
            if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
            else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
            else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
            else { p++; continue; }
            for (i = 1; i < len; i++) {
                if (p[i] == '\0' || (p[i] & 0xC0) != 0x80) break;
                cp = (cp << 6) | (p[i] & 0x3F);
            }
            if (i < len) { p++; continue; }
            if (cp <= 0xFFFF) {
                char tmp[40];
                sprintf(tmp, "\\u%u?", cp);
                strbuf_append(buf, tmp);
            } else {
                unsigned int hi, lo;
                char tmp[48];
                cp -= 0x10000;
                hi = 0xD800 + (cp >> 10);
                lo = 0xDC00 + (cp & 0x3FF);
                sprintf(tmp, "\\u%u?\\u%u?", hi, lo);
                strbuf_append(buf, tmp);
            }
            p += len;
        }
    }
}

int export_novel_txt(novel_project_t *proj, const char *path)
{
    strbuf_t *buf;
    int i, ret;
    if (!proj || !path) return -1;
    buf = strbuf_create(8192);
    if (!buf) return -1;
    if (proj->name) { strbuf_append(buf, proj->name); strbuf_append(buf, "\r\n\r\n"); }
    for (i = 0; i < proj->chapter_count; i++) {
        chapter_t *ch = &proj->chapters[i];
        strbuf_append(buf, ch->title ? ch->title : "");
        strbuf_append(buf, "\r\n\r\n");
        if (ch->content && *ch->content) {
            strbuf_append(buf, ch->content);
            strbuf_append(buf, "\r\n\r\n");
        }
    }
    ret = write_utf8_bom_file(path, strbuf_text(buf));
    strbuf_destroy(buf);
    return ret;
}

int export_novel_doc(novel_project_t *proj, const char *path)
{
    strbuf_t *buf;
    int i, ret;
    if (!proj || !path) return -1;
    buf = strbuf_create(8192);
    if (!buf) return -1;
    strbuf_append(buf, "{\\rtf1\\ansi\\ansicpg936\\deff0{\\fonttbl{\\f0\\fnil\\fcharset134 Microsoft YaHei;}}\\f0\\fs24\n");
    if (proj->name) {
        strbuf_append(buf, "\\b\\fs36 ");
        rtf_append_text(buf, proj->name);
        strbuf_append(buf, "\\b0\\fs24\\par\\par\n");
    }
    for (i = 0; i < proj->chapter_count; i++) {
        chapter_t *ch = &proj->chapters[i];
        strbuf_append(buf, "\\b\\fs28 ");
        rtf_append_text(buf, ch->title ? ch->title : "");
        strbuf_append(buf, "\\b0\\fs24\\par\n");
        if (ch->content && *ch->content) {
            rtf_append_text(buf, ch->content);
            strbuf_append(buf, "\\par\\par\n");
        }
    }
    strbuf_append(buf, "}\n");
    ret = write_file(path, strbuf_text(buf));
    strbuf_destroy(buf);
    return ret;
}

/* 导出到指定目录；返回 0 成功 */
int do_export(const char *dir_override)
{
    const char *dir;
    char doc_path[2048], txt_path[2048], epub_path[2048];
    char safe[256];
    const unsigned char *p;
    int k = 0;

    if (!g.project) return -1;
    dir = (dir_override && *dir_override) ? dir_override
        : (g.export_dir[0] ? g.export_dir
           : ((g.config && g.config->projects_dir && *g.config->projects_dir)
              ? g.config->projects_dir : "."));
    if (!dir || !*dir) dir = ".";

    p = (const unsigned char *)((g.project->name && *g.project->name) ? g.project->name : "novel");
    while (*p && k < 200) {
        unsigned char c = *p;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') { p++; continue; }
        safe[k++] = (char)c;
        p++;
    }
    safe[k] = '\0';
    /* 截断可能停在多字节 UTF-8 汉字中间：续字节(0x80..0xBF)回退到首字节，防文件名乱码 */
    while (k > 0 && ((unsigned char)safe[k - 1] & 0xC0) == 0x80) k--;
    safe[k] = '\0';
    if (k == 0) snprintf(safe, sizeof(safe), "novel");

    snprintf(doc_path, sizeof(doc_path), "%s\\%s.doc", dir, safe);
    snprintf(txt_path, sizeof(txt_path), "%s\\%s.txt", dir, safe);
    snprintf(epub_path, sizeof(epub_path), "%s\\%s.epub", dir, safe);

    g.last_doc[0] = '\0';
    g.last_txt[0] = '\0';
    g.last_epub[0] = '\0';
    if (export_novel_doc(g.project, doc_path) == 0) {
        snprintf(g.last_doc, sizeof(g.last_doc), "%s", doc_path);
    }
    if (export_novel_txt(g.project, txt_path) == 0) {
        snprintf(g.last_txt, sizeof(g.last_txt), "%s", txt_path);
    }
    /* 支线 B：EPUB 是独立产物，它失败不该拖死 doc/txt 的导出结果 */
    if (export_novel_epub(g.project, epub_path) == 0) {
        snprintf(g.last_epub, sizeof(g.last_epub), "%s", epub_path);
    }
    return (g.last_doc[0] || g.last_txt[0] || g.last_epub[0]) ? 0 : -1;
}

void open_path(const char *path)
{
    WCHAR *w;
    if (!path || !*path) return;
    w = u8_to_w(path);
    if (!w) return;
    ShellExecuteW(g.hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
}

/* 砍掉结尾处不完整的 UTF-8 序列，返回可安全解码的字节数。
   背景：导入大文件时我们按 max_bytes 硬截断，最后一个字符极可能被切成半个。
   这半个字符本身不合法，会让下面 MB_ERR_INVALID_CHARS 的严格校验**整体**失败，
   于是整篇 UTF-8 中文被误判成 GBK 再转一遍——全文乱码。
   做法：从尾部往回最多看 4 字节，找到首字节就按它的序列长度判断是否完整。 */
size_t utf8_trim_incomplete(const unsigned char *s, size_t len)
{
    size_t i, need;
    if (!s || len == 0) return 0;
    for (i = 1; i <= 4 && i <= len; i++) {
        unsigned char c = s[len - i];
        if ((c & 0x80) == 0) return len;            /* ASCII，上一字符完整 */
        if ((c & 0xC0) == 0xC0) {                   /* 找到首字节 */
            if ((c & 0xE0) == 0xC0)      need = 2;
            else if ((c & 0xF0) == 0xE0) need = 3;
            else if ((c & 0xF8) == 0xF0) need = 4;
            else return len;                        /* 非法首字节：交给严格校验去否掉 */
            return (i == need) ? len : len - i;
        }
        /* 0x80..0xBF 是续字节，继续往回找首字节 */
    }
    return len;                                     /* 往回 4 字节都没找到首字节：交给校验 */
}

/* 砍掉结尾处孤立的 GBK 首字节。
   ★ 不能只看最后一个字节：GBK 尾字节范围是 0x40-0xFE（含 0x81-0xFE），
   所以末字节落在 0x81-0xFE 时既可能是"半个汉字的首字节"，也可能是
   "完整汉字的尾字节"。必须从头部正向成对扫描才能确定边界，
   否则「中文」这种以 0xC4 结尾的合法文本会被误砍成「中?」。 */
size_t gbk_trim_incomplete(const unsigned char *s, size_t len)
{
    size_t i = 0;
    if (!s) return 0;
    while (i < len) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }               /* ASCII */
        if (c == 0x80 || c == 0xFF) { i++; continue; } /* 非法单字节，跳过交给解码器 */
        if (i + 1 >= len) return i;                    /* 尾部只剩一个首字节：砍掉 */
        i += 2;
    }
    return i;
}

/* 读取导入的文本：自动识别 UTF-8 / UTF-16LE / GBK，超长按 UTF-8 边界截断 */
char *read_text_file_smart(const char *path, size_t max_bytes)
{
    FILE *f;
    long size;
    unsigned char *raw;
    char *out = NULL;
    size_t rd;

    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }
    if ((size_t)size > max_bytes) size = (long)max_bytes;
    raw = (unsigned char *)malloc((size_t)size + 4);
    if (!raw) { fclose(f); return NULL; }
    rd = fread(raw, 1, (size_t)size, f);
    fclose(f);
    raw[rd] = 0; raw[rd + 1] = 0; raw[rd + 2] = 0;

    if (rd >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        out = str_dup((char *)raw + 3);
    } else if (rd >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        /* UTF-16LE。★ 这里**不能**用 MultiByteToWideChar(1200, ...)：
           本机实测无论参数怎么给都返回 0 + ERROR_INVALID_PARAMETER(87)，
           整条分支等于失效——导入 UTF-16 的 txt 永远报"读取失败"。
           Windows 上 WCHAR 的原生字节序就是 UTF-16LE，直接按码元拷贝即可；
           (rd-2)/2 天然按 2 字节对齐，被切半个字符时也不会越界。 */
        size_t units = (size_t)((rd - 2) / 2);
        if (units > 0) {
            WCHAR *wbuf = (WCHAR *)malloc((units + 1) * sizeof(WCHAR));
            if (wbuf) {
                memcpy(wbuf, raw + 2, units * sizeof(WCHAR));
                /* 结尾若是落单的高代理项（emoji 被切了一半），一并丢掉 */
                if (units > 0 && wbuf[units - 1] >= 0xD800 && wbuf[units - 1] <= 0xDBFF) units--;
                wbuf[units] = 0;
                out = w_to_u8_alloc(wbuf);
                free(wbuf);
            }
        }
    } else {
        /* 先按 UTF-8 试；只在**去掉结尾半个字符之后**仍然非法时，才退回 GBK。
           顺序很关键：先修尾再校验，否则大文件必然被切出半个字，
           严格校验必失败，于是所有 UTF-8 文本都会被误当 GBK 转成乱码。
           ★ 校验用 ulen 作显式长度，**不能**先 raw[ulen]=0：
             ulen < rd 时那会把 GBK 回退分支要用的字节提前抹掉
             （实测 D6 D0 CE C4 会被毁成 D6 D0 CE 00，解出「中?」）。 */
        size_t ulen = utf8_trim_incomplete(raw, rd);
        if (ulen > 0 &&
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCSTR)raw, (int)ulen, NULL, 0) > 0) {
            raw[ulen] = 0;
            out = str_dup((char *)raw);
        } else {
            size_t glen = gbk_trim_incomplete(raw, rd);
            int n;
            raw[glen] = 0;
            n = MultiByteToWideChar(936, 0, (LPCSTR)raw, (int)glen, NULL, 0);
            if (n > 0) {
                WCHAR *wbuf = (WCHAR *)malloc((size_t)(n + 1) * sizeof(WCHAR));
                if (wbuf) {
                    MultiByteToWideChar(936, 0, (LPCSTR)raw, (int)glen, wbuf, n);
                    wbuf[n] = 0;
                    out = w_to_u8_alloc(wbuf);
                    free(wbuf);
                }
            }
        }
    }
    free(raw);
    return out;
}

/* ============================================================
 * 16. 命令处理（全部在 UI 线程）
 * ============================================================ */

/* 需要独占 project 的命令在这些场景下被拒绝 */
int require_idle_reply(int id, const char *cmd, const char *what)
{
    if (!is_busy()) return 0;
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "后台任务正在进行，无法%s，请先取消或等待完成", what);
        reply_err(id, cmd, msg);
    }
    return 1;
}

/* ---- 未保存改动的确认 ----
   新建 / 加载项目会销毁当前 project，有可能丢掉还没落盘的改动，必须先征询。
   这里**刻意不用 MessageBoxW**：cmd_dispatch 的调用栈来自 WebView2 的
   WebMessageReceived 回调，在那里弹模态会开嵌套消息循环，和 WebView2 自己的
   消息泵互相卡住的风险很高（WM_CLOSE 是窗口消息，没这个顾虑）。
   改为：先回执一个 needConfirm，前端弹自绘弹层，用户选完带 force 重发。
   返回 1 表示「已回执，调用方直接 return」。 */
int need_discard_confirm(cJSON *root)
{
    if (!g.dirty || !g.project) return 0;
    return !p_bool(root, "force", 0);
}

void reply_need_discard(int id, const char *cmd)
{
    cJSON *d = cJSON_CreateObject();
    add_str(d, "needConfirm", "discard");
    add_str(d, "cmd", cmd ? cmd : "");
    add_str(d, "projectName", (g.project && g.project->name) ? g.project->name : "");
    reply_ok(id, cmd, d);
}

void cmd_hello(int id)
{
    cJSON *d = cJSON_CreateObject();
    char ver[64];
    w_to_u8(NW_VERSION, ver, sizeof(ver));
    add_str(d, "version", ver);
    cJSON_AddItemToObject(d, "config", config_json());
    /* 忙时工作线程正改写 g.project（章节数组 realloc、字符串 free/str_dup），
       深读就是 UAF（P0-C 读侧漏网的最后一个入口）。喂无项目空壳——
       project_meta_json(NULL) 返回合法 hasProject:0 壳；前端 handleTaskDone
       白名单会在任务结束后 refreshProject 补真数据，不会卡在假状态上。 */
    cJSON_AddItemToObject(d, "project", project_meta_json(is_busy() ? NULL : g.project));
    cJSON_AddBoolToObject(d, "busy", is_busy() ? 1 : 0);
    add_str(d, "exportDir", g.export_dir);
    add_str(d, "lastDoc", g.last_doc);
    add_str(d, "lastTxt", g.last_txt);
    add_str(d, "lastEpub", g.last_epub);
    log_line("配置下发：服务商预设 %d 组，当前模型 %s",
             config_presets_count(),
             (g.config && g.config->ai && g.config->ai->model) ? g.config->ai->model : "?");
    reply_ok(id, "hello", d);
    push_project_list();   /* 附带项目列表 */
}

/* --- 生成类命令的公共准入检查 --- */
int check_api_key(int id, const char *cmd)
{
    if (g.config && g.config->ai && g.config->ai->api_key && *g.config->ai->api_key) return 0;
    reply_err(id, cmd, "请先在「设置」页配置 API Key");
    return 1;
}

int check_project(int id, const char *cmd)
{
    if (g.project) return 0;
    reply_err(id, cmd, "还没有项目，请先在「项目」页新建");
    return 1;
}

void cmd_dispatch(cJSON *root)
{
    const char *cmd = j_str(root, "cmd", NULL);
    int id = j_int(root, "id", 0);
    cJSON *accepted;

    if (!cmd) return;

    /* ---- 任务进行中也允许的命令 ---- */
    if (strcmp(cmd, "hello") == 0) { cmd_hello(id); return; }
    if (strcmp(cmd, "get_config") == 0) { reply_ok(id, cmd, config_json()); return; }
    if (strcmp(cmd, "list_projects") == 0) { reply_project_list(id, cmd); return; }
    /* 读命令也拦忙时：工作线程（th_*）正在改写 project——章节数组 realloc、
       字符串 free/str_dup，主线程深读就是 UAF（P0-C）。任务结束后前端
       handleTaskDone 会自动 refreshProject，数据不丢。 */
    if (strcmp(cmd, "get_project") == 0) {
        if (require_idle_reply(id, cmd, "读取项目")) return;
        reply_ok(id, cmd, project_meta_json(g.project));
        return;
    }
    if (strcmp(cmd, "ping") == 0) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "tick", (double)GetTickCount());
        reply_ok(id, cmd, d);
        /* 前端在 hello 回执之后会补一次 ping：这条日志出现即证明
           JS -> C -> JS 双向通道都通（排查白屏时先看这里）。 */
        log_line("闭环确认：JS -> C -> JS 双向通畅");
        return;
    }
    /* ---- UI 自检通道 ----
       前端把诊断信息写进 webui.log。WebView2 的 DevTools 控制台在本环境下
       拿不到，这是排查白屏 / JS 异常唯一可观测的路径。 */
    if (strcmp(cmd, "log") == 0) {
        const char *m = p_str(root, "msg", "");
        log_line("[UI] %s", m ? m : "(空)");
        return;
    }
    /* 窗口标题随界面语言同步（前端 applyLanguage / initI18n 调 set_title） */
    if (strcmp(cmd, "set_title") == 0) {
        const char *t = p_str(root, "title", "");
        if (t && *t) {
            WCHAR *w = u8_to_w(t);
            if (w) {
                SetWindowTextW(g.hwnd, w);
                free(w);
            }
        }
        reply_ok(id, cmd, NULL);
        return;
    }
    if (strcmp(cmd, "answer") == 0) {
        int aid = p_int(root, "askId", -1);
        const char *ans = p_str(root, "answer", "cancel");
        ask_t *it;
        int v = 3;
        if (ans && strcmp(ans, "yes") == 0) v = 1;
        else if (ans && strcmp(ans, "no") == 0) v = 2;
        if (g_askCsReady) {
            EnterCriticalSection(&g_askCs);
            for (it = g_asks; it; it = it->next) {
                if (it->id == aid) { it->answer = v; SetEvent(it->done); break; }
            }
            LeaveCriticalSection(&g_askCs);
        }
        reply_ok(id, cmd, NULL);
        return;
    }
    if (strcmp(cmd, "cancel") == 0) {
        if (g.engine) engine_request_cancel(g.engine);
        /* 工作线程可能正卡在 bridge_ask 的确认框上，这个状态下引擎的取消标志
           不会被看到（检查点在章节之间，线程还没走到）。这里直接把待决询问
           按「取消」唤醒，保证「取消任务」在确认框挡着时同样生效。 */
        ask_release_all();
        push_status("正在取消任务，将在当前步骤结束后停止...");
        reply_ok(id, cmd, NULL);
        return;
    }
    if (strcmp(cmd, "get_chapter") == 0) {
        int idx = p_int(root, "index", -1);
        /* P0-C：忙时工作线程可能正在改写章节数组，深读会踩 UAF */
        if (require_idle_reply(id, cmd, "读取章节")) return;
        if (!g.project || idx < 0 || idx >= g.project->chapter_count) {
            reply_err(id, cmd, "章节不存在");
            return;
        }
        {
            chapter_t *c = &g.project->chapters[idx];
            cJSON *d = cJSON_CreateObject();
            cJSON_AddNumberToObject(d, "index", idx);
            add_str(d, "title", c->title);
            add_str(d, "summary", c->summary);
            add_str(d, "content", c->content);
            cJSON_AddNumberToObject(d, "wordCount", c->word_count);
            cJSON_AddNumberToObject(d, "status", c->status);
            reply_ok(id, cmd, d);
        }
        return;
    }
    if (strcmp(cmd, "get_plan") == 0) {
        int idx = p_int(root, "index", -1);
        /* P0-C：同 get_chapter，忙时禁止深读 plans 数组 */
        if (require_idle_reply(id, cmd, "读取方案")) return;
        if (!g.project || idx < 0 || idx >= g.project->plan_count) {
            reply_err(id, cmd, "方案不存在");
            return;
        }
        {
            novel_plan_t *pl = &g.project->plans[idx];
            cJSON *d = cJSON_CreateObject();
            cJSON_AddNumberToObject(d, "index", idx);
            add_str(d, "title", pl->title);
            add_str(d, "synopsis", pl->synopsis);
            add_str(d, "outline", pl->outline);
            reply_ok(id, cmd, d);
        }
        return;
    }
    /* 只写 app 结构体里的 int 游标，不碰 project；读写都在 UI 主线程，
       与工作线程无争用——不用拦忙时（P0-C 复查结论） */
    if (strcmp(cmd, "set_selected_chapter") == 0) {
        int idx = p_int(root, "index", -1);
        g.selected_chapter = idx;
        reply_ok(id, cmd, NULL);
        return;
    }

    /* 支线 C：Token 用量统计。只读 config 计数 + ai 层会话累计，不碰 project，
       任务进行中也允许（AI 任务跑着的时候才最想看用量） */
    if (strcmp(cmd, "token_stats") == 0) {
        long long sp = 0, sc = 0, st = 0;
        cJSON *d = cJSON_CreateObject();
        ai_usage_session(&sp, &sc, &st);
        cJSON_AddNumberToObject(d, "sessionPrompt", (double)sp);
        cJSON_AddNumberToObject(d, "sessionCompletion", (double)sc);
        cJSON_AddNumberToObject(d, "sessionTotal", (double)st);
        cJSON_AddNumberToObject(d, "prompt",     (double)(g.config ? g.config->tokens_prompt : 0));
        cJSON_AddNumberToObject(d, "completion", (double)(g.config ? g.config->tokens_completion : 0));
        cJSON_AddNumberToObject(d, "total",      (double)(g.config ? g.config->tokens_total : 0));
        reply_ok(id, cmd, d);
        return;
    }

    /* ---- 以下命令需要独占 project，任务运行中一律拒绝 ---- */

    if (strcmp(cmd, "save_config") == 0) {
        cJSON *p = j_params(root);
        char *cfg_path;
        if (require_idle_reply(id, cmd, "保存设置")) return;
        apply_config_json(p ? p : root);
        cfg_path = config_get_default_path();
        if (config_save(g.config, cfg_path) == 0) {
            restart_autosave();
            reply_ok(id, cmd, config_json());
            push_toast("success", "设置已保存");
        } else {
            reply_err(id, cmd, "配置文件写入失败，请检查磁盘权限");
        }
        free(cfg_path);
        return;
    }
    if (strcmp(cmd, "fetch_models") == 0) {
        cJSON *p = j_params(root);
        if (require_idle_reply(id, cmd, "获取模型列表")) return;
        /* 允许用户未点保存就取列表：先把当前输入并进 config */
        if (p) apply_config_json(p);
        if (check_api_key(id, cmd)) return;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        push_status("正在获取模型列表...");
        push_busy();
        if (!launch_thread(th_fetch_models, NULL)) {
            end_task();
            reply_err(id, cmd, "无法启动线程");
            return;
        }
        accepted = cJSON_CreateObject();
        cJSON_AddBoolToObject(accepted, "accepted", 1);
        reply_ok(id, cmd, accepted);
        return;
    }
    if (strcmp(cmd, "pick_folder") == 0) {
        char *picked = pick_folder_dialog(p_str(root, "initial", NULL));
        if (!picked) { reply_err(id, cmd, "未选择文件夹"); return; }
        snprintf(g.export_dir, sizeof(g.export_dir), "%s", picked);
        if (g.config) {
            free(g.config->export_dir);
            g.config->export_dir = str_dup(picked);
        }
        {
            cJSON *d = cJSON_CreateObject();
            add_str(d, "path", picked);
            reply_ok(id, cmd, d);
        }
        free(picked);
        return;
    }
    if (strcmp(cmd, "pick_project_file") == 0) {
        char *picked = pick_project_file_dialog(
            (g.config && g.config->projects_dir) ? g.config->projects_dir : NULL);
        if (!picked) { reply_err(id, cmd, "未选择文件"); return; }
        {
            cJSON *d = cJSON_CreateObject();
            add_str(d, "path", picked);
            reply_ok(id, cmd, d);
        }
        free(picked);
        return;
    }
    if (strcmp(cmd, "pick_text_file") == 0) {
        char *picked = pick_text_file_dialog();
        if (!picked) { reply_err(id, cmd, "未选择文件"); return; }
        {
            cJSON *d = cJSON_CreateObject();
            add_str(d, "path", picked);
            reply_ok(id, cmd, d);
        }
        free(picked);
        return;
    }
    if (strcmp(cmd, "open_path") == 0) {
        open_path(p_str(root, "path", NULL));
        reply_ok(id, cmd, NULL);
        return;
    }
    if (strcmp(cmd, "reveal_path") == 0) {
        const char *path = p_str(root, "path", NULL);
        if (path && *path) {
            WCHAR *w = u8_to_w(path);
            if (w) {
                /* 参数缓冲必须装得下 /select,"<path>"：路径超长时 swprintf 返回负数，
                   不能拿残缺参数去开 explorer（P1 截断）。装不下就退化为打开所在目录。 */
                WCHAR args[MAX_PATH * 2 + 32];
                int n = swprintf(args, sizeof(args) / sizeof(args[0]), L"/select,\"%s\"", w);
                if (n > 0) {
                    ShellExecuteW(g.hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
                } else {
                    ShellExecuteW(g.hwnd, L"open", L"explorer.exe", w, NULL, SW_SHOWNORMAL);
                }
                free(w);
            }
        }
        reply_ok(id, cmd, NULL);
        return;
    }

    /* --- 项目 --- */
    if (strcmp(cmd, "new_project") == 0) {
        const char *name = p_str(root, "name", "");
        const char *idea = p_str(root, "idea", "");
        if (require_idle_reply(id, cmd, "新建项目")) return;
        if (!name || !*name) { reply_err(id, cmd, "请输入项目名称"); return; }
        /* 这一步会销毁当前项目，先把未保存的改动问清楚 */
        if (need_discard_confirm(root)) { reply_need_discard(id, cmd); return; }
        if (g.project) { project_destroy(g.project); g.project = NULL; }
        g.project = project_create(name, idea);
        if (!g.project) { reply_err(id, cmd, "内存不足，无法创建项目"); return; }
        g.selected_chapter = -1;
        g.dirty = 1;
        g.last_doc[0] = g.last_txt[0] = g.last_epub[0] = '\0';
        reply_ok(id, cmd, project_meta_json(g.project));
        push_project();
        push_status("新项目已创建");
        return;
    }
    if (strcmp(cmd, "load_project") == 0) {
        const char *pid = p_str(root, "id", NULL);
        const char *ppath = p_str(root, "path", NULL);
        char full[4096];
        char *dup;
        if (require_idle_reply(id, cmd, "加载项目")) return;
        if (ppath && *ppath) {
            dup = str_dup(ppath);
        } else if (pid && *pid && g.config && g.config->projects_dir) {
            snprintf(full, sizeof(full), "%s\\%s\\project.json", g.config->projects_dir, pid);
            dup = str_dup(full);
        } else {
            reply_err(id, cmd, "缺少项目标识");
            return;
        }
        if (!file_exists(dup)) {
            free(dup);
            reply_err(id, cmd, "项目文件不存在");
            return;
        }
        /* 加载完成后会替换掉当前项目（在 handle_task_done 里销毁旧的），
           所以要在起线程之前先确认未保存的改动 */
        if (need_discard_confirm(root)) { free(dup); reply_need_discard(id, cmd); return; }
        if (!try_begin_task()) { free(dup); reply_err(id, cmd, "有任务正在进行"); return; }
        push_status("正在加载项目...");
        push_busy();
        if (!launch_thread(th_load_project, dup)) {
            free(dup);
            end_task();
            reply_err(id, cmd, "无法启动加载线程");
            return;
        }
        accepted = cJSON_CreateObject();
        cJSON_AddBoolToObject(accepted, "accepted", 1);
        reply_ok(id, cmd, accepted);
        return;
    }
    if (strcmp(cmd, "save_project") == 0) {
        const char *name = p_str(root, "name", NULL);
        if (require_idle_reply(id, cmd, "保存项目")) return;
        if (!g.project) { reply_err(id, cmd, "没有项目可保存"); return; }
        if (name) { free(g.project->name); g.project->name = str_dup(name); }
        if (project_save(g.project, g.config->projects_dir) == 0) {
            g.dirty = 0;
            reply_ok(id, cmd, project_meta_json(g.project));
            push_toast("success", "项目已保存");
            push_status("项目保存成功");
        } else {
            reply_err(id, cmd, "项目保存失败，请检查 projects 目录是否可写");
        }
        return;
    }
    if (strcmp(cmd, "set_project_name") == 0) {
        const char *name = p_str(root, "name", "");
        if (require_idle_reply(id, cmd, "修改项目名称")) return;
        if (!g.project) { reply_err(id, cmd, "没有项目"); return; }
        free(g.project->name);
        g.project->name = str_dup(name ? name : "");
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        return;
    }
    if (strcmp(cmd, "set_project_idea") == 0) {
        const char *idea = p_str(root, "idea", "");
        if (require_idle_reply(id, cmd, "修改创意")) return;
        if (!g.project) { reply_err(id, cmd, "没有项目"); return; }
        free(g.project->seed_idea);
        g.project->seed_idea = str_dup(idea ? idea : "");
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        return;
    }

    /* --- 编辑章节 / 方案 / 世界观 / 人物 --- */
    if (strcmp(cmd, "set_chapter") == 0) {
        int idx = p_int(root, "index", -1);
        if (require_idle_reply(id, cmd, "编辑章节")) return;
        if (!g.project || idx < 0 || idx >= g.project->chapter_count) {
            reply_err(id, cmd, "章节不存在");
            return;
        }
        {
            chapter_t *c = &g.project->chapters[idx];
            const char *t = j_str(j_params(root), "title", NULL);
            const char *s = j_str(j_params(root), "summary", NULL);
            const char *ct = j_str(j_params(root), "content", NULL);
            if (t) { free(c->title);  c->title  = str_dup(t); }
            if (s) { free(c->summary); c->summary = str_dup(s); }
            if (ct != NULL) {
                free(c->content);
                c->content = str_dup(ct);
                c->word_count = (int)utf8_char_count(ct);
            }
            project_calc_total_words(g.project);
            g.dirty = 1;
            reply_ok(id, cmd, NULL);
            push_project();
        }
        return;
    }
    if (strcmp(cmd, "set_plan") == 0) {
        int idx = p_int(root, "index", -1);
        if (require_idle_reply(id, cmd, "编辑方案")) return;
        if (!g.project || idx < 0 || idx >= g.project->plan_count) {
            reply_err(id, cmd, "方案不存在");
            return;
        }
        {
            novel_plan_t *pl = &g.project->plans[idx];
            const char *t = j_str(j_params(root), "title", NULL);
            const char *s = j_str(j_params(root), "synopsis", NULL);
            const char *o = j_str(j_params(root), "outline", NULL);
            if (t) { free(pl->title);    pl->title = str_dup(t); }
            if (s) { free(pl->synopsis); pl->synopsis = str_dup(s); }
            if (o) { free(pl->outline);  pl->outline = str_dup(o); }
            g.dirty = 1;
            reply_ok(id, cmd, NULL);
            push_project();
        }
        return;
    }
    if (strcmp(cmd, "set_world") == 0) {
        cJSON *p = j_params(root);
        if (require_idle_reply(id, cmd, "编辑世界观")) return;
        if (!g.project) { reply_err(id, cmd, "没有项目"); return; }
        {
            const char *wt = j_str(p, "worldType", NULL);
            const char *se = j_str(p, "setting", NULL);
            const char *ps = j_str(p, "powerSystem", NULL);
            const char *ge = j_str(p, "geography", NULL);
            const char *hi = j_str(p, "history", NULL);
            project_set_world(g.project, wt, se, ps);
            /* geography / history 业务层没有 setter，这里直接写公开字段 */
            if (ge) { free(g.project->world.geography); g.project->world.geography = str_dup(ge); }
            if (hi) { free(g.project->world.history);   g.project->world.history   = str_dup(hi); }
            g.dirty = 1;
            reply_ok(id, cmd, NULL);
            push_project();
        }
        return;
    }
    if (strcmp(cmd, "set_characters") == 0) {
        cJSON *p = j_params(root);
        cJSON *arr = p ? cJSON_GetObjectItemCaseSensitive(p, "characters") : NULL;
        if (require_idle_reply(id, cmd, "编辑人物")) return;
        if (!g.project) { reply_err(id, cmd, "没有项目"); return; }
        if (!arr || !cJSON_IsArray(arr)) { reply_err(id, cmd, "人物数据格式错误"); return; }
        /* 整体替换：复用业务层的 clear + add，避免自行操作容量字段 */
        project_clear_characters(g.project);
        {
            int n = cJSON_GetArraySize(arr), i;
            for (i = 0; i < n; i++) {
                cJSON *c = cJSON_GetArrayItem(arr, i);
                if (!c) continue;
                project_add_character(g.project,
                    j_str(c, "name", ""), j_str(c, "role", ""),
                    j_str(c, "description", ""), j_str(c, "traits", ""),
                    j_str(c, "background", ""));
            }
        }
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        push_project();
        return;
    }
    if (strcmp(cmd, "select_direction") == 0) {
        int idx = p_int(root, "index", -1);
        if (require_idle_reply(id, cmd, "选择方向")) return;
        if (!g.project || idx < 0 || idx >= g.project->direction_count) {
            reply_err(id, cmd, "方向不存在");
            return;
        }
        project_select_direction(g.project, idx);
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        push_project();
        push_status("已选定方向，可继续生成方案");
        return;
    }
    if (strcmp(cmd, "select_plan") == 0) {
        int idx = p_int(root, "index", -1);
        if (require_idle_reply(id, cmd, "选择方案")) return;
        if (!g.project || idx < 0 || idx >= g.project->plan_count) {
            reply_err(id, cmd, "方案不存在");
            return;
        }
        project_select_plan(g.project, idx);
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        push_project();
        push_status("已选定方案，可继续细化大纲");
        return;
    }
    if (strcmp(cmd, "export") == 0) {
        const char *dir = p_str(root, "dir", NULL);
        if (require_idle_reply(id, cmd, "导出")) return;
        if (!g.project || g.project->chapter_count == 0) {
            reply_err(id, cmd, "没有可导出的章节，请先细化大纲或写入章节内容");
            return;
        }
        if (do_export(dir) == 0) {
            cJSON *d = cJSON_CreateObject();
            add_str(d, "doc", g.last_doc);
            add_str(d, "txt", g.last_txt);
            add_str(d, "epub", g.last_epub);   /* 支线 B */
            reply_ok(id, cmd, d);
            push_toast("success", "已导出 DOC / TXT / EPUB");
            push_status("已导出 DOC/TXT/EPUB");
        } else {
            reply_err(id, cmd, "导出失败，请检查输出目录是否可写");
        }
        return;
    }

    /* --- 生成类（异步） --- */
#define ACCEPT_ASYNC                                                                \
    do {                                                                            \
        accepted = cJSON_CreateObject();                                            \
        cJSON_AddBoolToObject(accepted, "accepted", 1);                             \
        reply_ok(id, cmd, accepted);                                                \
        push_busy();                                                                \
    } while (0)

    if (strcmp(cmd, "gen_directions") == 0) {
        gen_dir_task_t *t;
        int iters = p_int(root, "iterations", 2);
        int count = p_int(root, "count", 5);
        if (require_idle_reply(id, cmd, "生成创作方向")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (iters < 1) iters = 1;
        if (iters > 6) iters = 6;
        if (count < 1) count = 5;
        if (count > 20) count = 20;
        /* 生成前把最新创意写回项目 */
        {
            const char *idea = p_str(root, "idea", NULL);
            if (idea) { free(g.project->seed_idea); g.project->seed_idea = str_dup(idea); }
        }
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        t = (gen_dir_task_t *)malloc(sizeof(gen_dir_task_t));
        if (!t) { end_task(); reply_err(id, cmd, "内存不足"); return; }
        t->iterations = iters;
        t->count = count;
        engine_clear_cancel(g.engine);
        push_status("正在生成创作方向...");
        if (!launch_thread(th_gen_directions, t)) {
            free(t); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "gen_plans") == 0) {
        int count = p_int(root, "count", 3);
        if (require_idle_reply(id, cmd, "生成方案")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!g.project->selected_direction) { reply_err(id, cmd, "请先选定一个创作方向"); return; }
        if (count < 1) count = 3;
        if (count > 10) count = 10;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在生成小说方案...");
        if (!launch_thread(th_gen_plans, (LPVOID)(INT_PTR)count)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "expand_outline") == 0) {
        int count = p_int(root, "count", 20);
        if (require_idle_reply(id, cmd, "细化大纲")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!g.project->selected_plan) { reply_err(id, cmd, "请先选定一个小说方案"); return; }
        if (count < 3) count = 20;
        if (count > 50) count = 50;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在细化大纲...");
        if (!launch_thread(th_expand_outline, (LPVOID)(INT_PTR)count)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "expand_chapter") == 0) {
        ch_task_t *t;
        int idx = p_int(root, "index", -1);
        int words = p_int(root, "words", 2000);
        if (require_idle_reply(id, cmd, "扩写章节")) return;
        if (check_api_key(id, cmd)) return;
        if (!g.project || idx < 0 || idx >= g.project->chapter_count) {
            reply_err(id, cmd, "请先选择一个章节");
            return;
        }
        if (words < 200) words = 2000;
        if (words > 20000) words = 20000;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        t = (ch_task_t *)malloc(sizeof(ch_task_t));
        if (!t) { end_task(); reply_err(id, cmd, "内存不足"); return; }
        t->index = idx; t->words = words;
        engine_clear_cancel(g.engine);
        push_status("正在扩写本章...");
        if (!launch_thread(th_expand_chapter, t)) {
            free(t); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "expand_all") == 0) {
        expand_all_task_t *t;
        int words = p_int(root, "words", 2000);
        int force = p_int(root, "force", 0);
        if (require_idle_reply(id, cmd, "批量扩写")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (g.project->chapter_count == 0) { reply_err(id, cmd, "没有章节可扩写"); return; }
        if (words < 200) words = 2000;
        if (words > 20000) words = 20000;
        t = (expand_all_task_t *)malloc(sizeof(expand_all_task_t));
        if (!t) { reply_err(id, cmd, "内存不足"); return; }
        t->words = words;
        t->force = force ? 1 : 0;
        if (!try_begin_task()) { free(t); reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在批量扩写...");
        if (!launch_thread(th_expand_all, t)) {
            free(t); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "step_write") == 0) {
        ch_task_t *t;
        int words = p_int(root, "words", 2000);
        int start = -1, i;
        if (require_idle_reply(id, cmd, "逐章生成")) return;
        if (check_api_key(id, cmd)) return;
        if (!g.project || g.project->chapter_count == 0) {
            reply_err(id, cmd, "没有章节可生成，请先在「大纲」页细化大纲");
            return;
        }
        for (i = 0; i < g.project->chapter_count; i++) {
            chapter_t *c = &g.project->chapters[i];
            if (!c->content || !*c->content) { start = i; break; }
        }
        if (start < 0) { reply_err(id, cmd, "所有章节均已写完；如需重写某一章，请选中该章后点「扩写本章」"); return; }
        if (words < 200) words = 2000;
        if (words > 20000) words = 20000;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        t = (ch_task_t *)malloc(sizeof(ch_task_t));
        if (!t) { end_task(); reply_err(id, cmd, "内存不足"); return; }
        t->index = start; t->words = words;
        engine_clear_cancel(g.engine);
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "逐章生成：正在生成第 %d 章...", start + 1);
            push_status(msg);
        }
        if (!launch_thread(th_step_write, t)) {
            free(t); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "check_consistency") == 0) {
        int idx = p_int(root, "index", -1);
        if (require_idle_reply(id, cmd, "一致性检查")) return;
        if (check_api_key(id, cmd)) return;
        if (!g.project || idx < 0 || idx >= g.project->chapter_count) {
            reply_err(id, cmd, "请先选择一个章节");
            return;
        }
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在进行一致性检查...");
        if (!launch_thread(th_check_consistency, (LPVOID)(INT_PTR)idx)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "summarize_worldview") == 0) {
        if (require_idle_reply(id, cmd, "总结世界观")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在总结世界观...");
        if (!launch_thread(th_sum_world, NULL)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "generate_characters") == 0) {
        if (require_idle_reply(id, cmd, "生成人物")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在根据小说内容生成人物草稿...");
        if (!launch_thread(th_gen_chars, NULL)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "check_finale") == 0) {
        if (require_idle_reply(id, cmd, "伏笔回收校验")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在做伏笔回收校验...");
        if (!launch_thread(th_check_finale, NULL)) {
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "analyze_text") == 0) {
        const char *text = p_str(root, "text", NULL);
        char *dup = str_dup(text ? text : "");
        if (require_idle_reply(id, cmd, "分析文本")) { free(dup); return; }
        if (check_api_key(id, cmd)) { free(dup); return; }
        if (check_project(id, cmd)) { free(dup); return; }
        if (!dup) { reply_err(id, cmd, "内存不足"); return; }
        if (!try_begin_task()) { free(dup); reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在分析文本...");
        if (!launch_thread(th_analyze_text, dup)) {
            free(dup); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
    if (strcmp(cmd, "auto_write") == 0) {
        auto_task_t *t;
        int dirs = p_int(root, "dirCount", 5);
        int plans = p_int(root, "planCount", 3);
        int chs = p_int(root, "chapters", 20);
        int words = p_int(root, "words", 3000);
        int start_ch = p_int(root, "startChapter", 0);
        if (require_idle_reply(id, cmd, "全自动写作")) return;
        if (check_api_key(id, cmd)) return;
        if (dirs < 1) dirs = 5;
        if (dirs > 20) dirs = 20;
        if (plans < 1) plans = 3;
        if (plans > 10) plans = 10;
        if (chs < 3) chs = 20;
        if (chs > 50) chs = 50;
        if (words < 200) words = 3000;
        if (words > 20000) words = 20000;
        if (start_ch < 0) start_ch = 0;
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        t = (auto_task_t *)malloc(sizeof(auto_task_t));
        if (!t) { end_task(); reply_err(id, cmd, "内存不足"); return; }
        t->dirs = dirs; t->plans = plans; t->chapters = chs; t->words = words; t->start_ch = start_ch;
        engine_clear_cancel(g.engine);
        push_status("开始全自动写作...");
        if (!launch_thread(th_auto_write, t)) {
            free(t); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        ACCEPT_ASYNC;
        return;
    }
#undef ACCEPT_ASYNC

    if (strcmp(cmd, "import_txt") == 0) {
        const char *path = p_str(root, "path", NULL);
        char *text;
        if (require_idle_reply(id, cmd, "导入文本")) return;
        if (check_api_key(id, cmd)) return;
        if (check_project(id, cmd)) return;
        if (!path || !*path) { reply_err(id, cmd, "缺少文件路径"); return; }
        text = read_text_file_smart(path, 4 * 1024 * 1024);
        if (!text || !*text) {
            free(text);
            reply_err(id, cmd, "文件读取失败或内容为空（仅支持 txt，最大 4MB）");
            return;
        }
        if (!try_begin_task()) { free(text); reply_err(id, cmd, "有任务正在进行"); return; }
        engine_clear_cancel(g.engine);
        push_status("正在分析导入的文本...");
        if (!launch_thread(th_analyze_text, text)) {
            free(text); end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        {
            cJSON *a = cJSON_CreateObject();
            cJSON_AddBoolToObject(a, "accepted", 1);
            reply_ok(id, cmd, a);
            push_busy();
        }
        return;
    }
    if (strcmp(cmd, "ai_call") == 0) {
        const char *sys = p_str(root, "system", "");
        const char *usr = p_str(root, "user", "");
        ai_call_task_t *t;
        if (require_idle_reply(id, cmd, "AI 调用")) return;
        if (check_api_key(id, cmd)) return;
        if (!usr || !*usr) { reply_err(id, cmd, "缺少 user 提示词"); return; }
        if (!try_begin_task()) { reply_err(id, cmd, "有任务正在进行"); return; }
        t = (ai_call_task_t *)malloc(sizeof(ai_call_task_t));
        if (!t) { end_task(); reply_err(id, cmd, "内存不足"); return; }
        t->system_prompt = str_dup(sys ? sys : "");
        t->user_prompt = str_dup(usr);
        if (!t->system_prompt || !t->user_prompt) {
            free(t->system_prompt); free(t->user_prompt); free(t);
            end_task(); reply_err(id, cmd, "内存不足"); return;
        }
        engine_clear_cancel(g.engine);
        if (!launch_thread(th_ai_call, t)) {
            free(t->system_prompt); free(t->user_prompt); free(t);
            end_task(); reply_err(id, cmd, "无法启动线程"); return;
        }
        /* ACCEPT_ASYNC 宏在 dispatch 尾部已 #undef，这里展开写 */
        accepted = cJSON_CreateObject();
        cJSON_AddBoolToObject(accepted, "accepted", 1);
        reply_ok(id, cmd, accepted);
        push_busy();
        return;
    }
    if (strcmp(cmd, "get_gal") == 0) {
        /* busy 时 worker 线程正在读写 gal_json（project_save 序列化 "gal" 键），
           读快照也要过门闩，否则与 set_gal 的 free/替换 交错成 UAF */
        if (require_idle_reply(id, cmd, "读取剧情分支")) return;
        cJSON *d = cJSON_CreateObject();
        add_str(d, "gal", (g.project && g.project->gal_json) ? g.project->gal_json : "{}");
        reply_ok(id, cmd, d);
        return;
    }
    if (strcmp(cmd, "set_gal") == 0) {
        const char *js = p_str(root, "json", NULL);
        if (require_idle_reply(id, cmd, "保存剧情分支")) return;
        if (check_project(id, cmd)) return;
        if (!js) { reply_err(id, cmd, "缺少 json"); return; }
        {
            /* 先 dup 判空再换指针：str_dup 失败时 gal_json 被置 NULL = 静默丢剧情存档 */
            char *copy = str_dup(js);
            if (!copy) { reply_err(id, cmd, "内存不足"); return; }
            free(g.project->gal_json);
            g.project->gal_json = copy;
        }
        g.dirty = 1;
        reply_ok(id, cmd, NULL);
        return;
    }
    if (strcmp(cmd, "comfy_test") == 0) {
        /* 连通测试：不占忙闲位（可与 AI 任务并行），结果走 comfy_done 事件 */
        const char *url = p_str(root, "url", NULL);
        char *copy;
        if (!url || !*url) url = (g.config && g.config->comfy_url) ? g.config->comfy_url : "";
        copy = str_dup(url);
        if (!copy) { reply_err(id, cmd, "内存不足"); return; }
        if (!launch_thread_untracked(th_comfy_test, copy)) {
            free(copy); reply_err(id, cmd, "无法启动线程"); return;
        }
        accepted = cJSON_CreateObject();
        cJSON_AddBoolToObject(accepted, "accepted", 1);
        reply_ok(id, cmd, accepted);
        return;
    }
    if (strcmp(cmd, "comfy_gen") == 0) {
        /* 生成配图：不占忙闲位。落盘 <exe_dir>/舞台图片/<proj_id>/<node>.png
           （目录故意在程序目录而非 projects_dir——沿用手账 10-20 决定；
           装进 Program Files 时可能没写权限，ensure_dir 失败会报「无法创建图片目录」） */
        const char *node = p_str(root, "node", "");
        const char *prompt = p_str(root, "prompt", "");
        const char *wf_path = (g.config && g.config->comfy_workflow_path)
                              ? g.config->comfy_workflow_path : NULL;
        char *wf = NULL, *proj_dir = NULL, *img_dir = NULL, *out_path = NULL;
        comfy_gen_task_t *t;
        size_t i;
        if (check_project(id, cmd)) return;
        if (!node || !*node) { reply_err(id, cmd, "缺少 node（图片键）"); return; }
        for (i = 0; node[i]; i++) {
            char c = node[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) {
                reply_err(id, cmd, "node 只允许字母/数字/_/-"); return;
            }
        }
        if (!prompt || !*prompt) { reply_err(id, cmd, "缺少 prompt（场景描述）"); return; }
        if (!g.config || !g.config->comfy_url || !*g.config->comfy_url) {
            reply_err(id, cmd, "ComfyUI 地址未配置（见「配图工坊」）"); return;
        }
        if (!wf_path || !*wf_path) {
            reply_err(id, cmd, "工作流 JSON 未配置（见「配图工坊」）"); return;
        }
        wf = read_file(wf_path);
        if (!wf || !*wf) {
            free(wf); reply_err(id, cmd, "工作流文件读取失败"); return;
        }
        if (!strstr(wf, "{{PROMPT}}")) {
            free(wf);
            reply_err(id, cmd, "工作流里没有 {{PROMPT}} 占位符（正向提示词处写上它）");
            return;
        }
        /* 图片库在程序目录的「舞台图片」下，每项目一个子文件夹防同名节点互踩 */
        proj_dir = get_exe_dir();
        img_dir = proj_dir ? path_join(proj_dir, "舞台图片") : NULL;
        free(proj_dir);
        if (!img_dir || ensure_dir(img_dir) != 0) {
            free(wf); free(img_dir); reply_err(id, cmd, "无法创建图片目录"); return;
        }
        {
            char *sub = path_join(img_dir, g.project->id);
            free(img_dir);
            img_dir = sub;
        }
        if (!img_dir || ensure_dir(img_dir) != 0) {
            free(wf); free(img_dir); reply_err(id, cmd, "无法创建图片目录"); return;
        }
        {
            char fname[320];
            snprintf(fname, sizeof(fname), "%s.png", node);
            out_path = path_join(img_dir, fname);
        }
        free(img_dir);
        t = (comfy_gen_task_t *)calloc(1, sizeof(comfy_gen_task_t));
        if (!t) {
            free(wf); free(out_path); reply_err(id, cmd, "内存不足"); return;
        }
        t->node = str_dup(node);
        t->url = str_dup(g.config->comfy_url);
        t->workflow_json = wf;            /* 所有权移交 */
        t->prompt = str_dup(prompt);
        t->out_path = out_path;           /* 所有权移交 */
        if (!t->node || !t->url || !t->prompt || !t->out_path) {
            free(t->node); free(t->url); free(t->workflow_json);
            free(t->prompt); free(t->out_path); free(t);
            reply_err(id, cmd, "内存不足"); return;
        }
        if (!launch_thread_untracked(th_comfy_gen, t)) {
            free(t->node); free(t->url); free(t->workflow_json);
            free(t->prompt); free(t->out_path); free(t);
            reply_err(id, cmd, "无法启动线程"); return;
        }
        accepted = cJSON_CreateObject();
        cJSON_AddBoolToObject(accepted, "accepted", 1);
        reply_ok(id, cmd, accepted);
        return;
    }
    if (strcmp(cmd, "read_image") == 0) {
        /* 同步读图：PNG -> data URL，前端当背景用。限 16MB。 */
        const char *node = p_str(root, "node", "");
        char *proj_dir, *img_dir, *path, *data;
        size_t i;
        if (check_project(id, cmd)) return;
        if (!node || !*node) { reply_err(id, cmd, "缺少 node（图片键）"); return; }
        for (i = 0; node[i]; i++) {
            char c = node[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) {
                reply_err(id, cmd, "node 只允许字母/数字/_/-"); return;
            }
        }
        proj_dir = get_exe_dir();
        img_dir = proj_dir ? path_join(proj_dir, "舞台图片") : NULL;
        free(proj_dir);
        {
            char *sub = img_dir ? path_join(img_dir, g.project->id) : NULL;
            free(img_dir);
            img_dir = sub;
        }
        {
            char fname[320];
            snprintf(fname, sizeof(fname), "%s.png", node);
            path = img_dir ? path_join(img_dir, fname) : NULL;
        }
        free(img_dir);
        if (!path) { reply_err(id, cmd, "内存不足"); return; }
        data = image_data_url(path);
        free(path);
        if (!data) {
            reply_err(id, cmd, "图片尚未生成或读取失败");
            return;
        }
        {
            cJSON *d = cJSON_CreateObject();
            add_str(d, "data", data);
            free(data);
            reply_ok(id, cmd, d);
        }
        return;
    }
    if (strcmp(cmd, "shutdown") == 0) {
        reply_ok(id, cmd, NULL);
        PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
        return;
    }

    reply_err(id, cmd, "未知命令");
}
