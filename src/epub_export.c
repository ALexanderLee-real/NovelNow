/* epub_export.c - EPUB 3 导出（支线 B，2026-10-19）
 *
 * EPUB 本质是一个 zip。规范只要求 mimetype 作为第一项且不压缩（STORE），
 * 其余条目用 STORE 同样合法——这里统一 STORE + 自算 CRC32（表驱动 0xEDB88320），
 * 比拖 zlib 进来省事得多，导出全书也就几百 KB~几 MB，压缩率无所谓。
 *
 * 结构：
 *   mimetype                       "application/epub+zip"（第一项，STORE）
 *   META-INF/container.xml         指向 OEBPS/content.opf
 *   OEBPS/content.opf              包文档（EPUB 3.0，含 dcterms:modified）
 *   OEBPS/nav.xhtml                EPUB 3 目录（epub:type="toc"）
 *   OEBPS/toc.ncx                  EPUB 2 兼容目录（spine toc 属性挂它）
 *   OEBPS/style.css                简单排版
 *   OEBPS/ch1..chN.xhtml           每章一个 XHTML，正文按行拆 <p>
 */
#include "epub_export.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------- ZIP/STORE 最小实现 ---------------- */

typedef struct {
    char          name[96];   /* zip 内路径，纯 ASCII */
    unsigned long crc;        /* CRC-32 */
    unsigned int  size;       /* 数据字节数 */
    unsigned int  offset;     /* local header 在文件里的起始偏移 */
} zip_ent_t;

typedef struct {
    FILE       *f;
    zip_ent_t  *ents;
    int         count, cap;
    unsigned int dos_time, dos_date;
    int         err;
} zip_w_t;

/* CRC-32（IEEE 802.3，反射多项式 0xEDB88320） */
static unsigned long crc32_buf(const unsigned char *p, size_t n)
{
    static unsigned long tbl[256];
    static int have_tbl = 0;
    unsigned long crc;
    size_t i;
    if (!have_tbl) {
        unsigned long c;
        int k, j;
        for (k = 0; k < 256; k++) {
            c = (unsigned long)k;
            for (j = 0; j < 8; j++)
                c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
            tbl[k] = c;
        }
        have_tbl = 1;
    }
    crc = 0xFFFFFFFFUL;
    for (i = 0; i < n; i++)
        crc = tbl[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}

/* 小端写入（EPUB/zip 规定小端；手写不依赖主机字节序） */
static void w16(FILE *f, unsigned int v)
{
    unsigned char b[2];
    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 8) & 0xFF);
    fwrite(b, 1, 2, f);
}
static void w32(FILE *f, unsigned long v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 8) & 0xFF);
    b[2] = (unsigned char)((v >> 16) & 0xFF);
    b[3] = (unsigned char)((v >> 24) & 0xFF);
    fwrite(b, 1, 4, f);
}

static void zip_w_init(zip_w_t *z, FILE *f)
{
    time_t t = time(NULL);
    struct tm tmv;
    memset(z, 0, sizeof(*z));
    z->f = f;
#ifdef _MSC_VER
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    z->dos_time = (unsigned int)((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
    z->dos_date = (unsigned int)(((tmv.tm_year + 1900 - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
}

/* 追加一项（STORE）；data 在函数内直接落盘，调用方保留所有权 */
static int zip_add(zip_w_t *z, const char *name, const void *data, unsigned int size)
{
    zip_ent_t *e;
    unsigned long crc;
    long off;

    if (z->err) return -1;
    off = ftell(z->f);
    if (off < 0) { z->err = 1; return -1; }

    crc = crc32_buf((const unsigned char *)data, size);

    /* local file header */
    w32(z->f, 0x04034B50UL);
    w16(z->f, 20);          /* version needed */
    w16(z->f, 0);           /* flags（名字纯 ASCII，不用 UTF-8 标志） */
    w16(z->f, 0);           /* method: STORE */
    w16(z->f, z->dos_time);
    w16(z->f, z->dos_date);
    w32(z->f, crc);
    w32(z->f, size);
    w32(z->f, size);
    w16(z->f, (unsigned int)strlen(name));
    w16(z->f, 0);           /* extra len */
    fwrite(name, 1, strlen(name), z->f);
    if (size) fwrite(data, 1, size, z->f);
    if (ferror(z->f)) { z->err = 1; return -1; }

    /* 记进中央目录表 */
    if (z->count >= z->cap) {
        int cap = z->cap ? z->cap * 2 : 16;
        zip_ent_t *ne = (zip_ent_t *)realloc(z->ents, (size_t)cap * sizeof(zip_ent_t));
        if (!ne) { z->err = 1; return -1; }
        z->ents = ne;
        z->cap = cap;
    }
    e = &z->ents[z->count++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->crc = crc;
    e->size = size;
    e->offset = (unsigned int)off;
    return 0;
}

/* 写中央目录 + EOCD 收尾 */
static int zip_finish(zip_w_t *z)
{
    long cd_off, cd_size;
    int i;

    if (z->err) return -1;
    cd_off = ftell(z->f);
    if (cd_off < 0) return -1;

    for (i = 0; i < z->count; i++) {
        zip_ent_t *e = &z->ents[i];
        unsigned int nlen = (unsigned int)strlen(e->name);
        w32(z->f, 0x02014B50UL);
        w16(z->f, 20);      /* version made by */
        w16(z->f, 20);      /* version needed */
        w16(z->f, 0);
        w16(z->f, 0);       /* STORE */
        w16(z->f, z->dos_time);
        w16(z->f, z->dos_date);
        w32(z->f, e->crc);
        w32(z->f, e->size);
        w32(z->f, e->size);
        w16(z->f, nlen);
        w16(z->f, 0);       /* extra */
        w16(z->f, 0);       /* comment */
        w16(z->f, 0);       /* disk start */
        w16(z->f, 0);       /* internal attrs */
        w32(z->f, 0);       /* external attrs */
        w32(z->f, e->offset);
        fwrite(e->name, 1, nlen, z->f);
    }
    cd_size = ftell(z->f) - cd_off;
    if (cd_size < 0) return -1;

    w32(z->f, 0x06054B50UL);
    w16(z->f, 0);
    w16(z->f, 0);
    w16(z->f, (unsigned int)z->count);
    w16(z->f, (unsigned int)z->count);
    w32(z->f, (unsigned long)cd_size);
    w32(z->f, (unsigned long)cd_off);
    w16(z->f, 0);
    return ferror(z->f) ? -1 : 0;
}

/* ---------------- XHTML 助手 ---------------- */

/* XML 转义（文本节点只需 & < >） */
static void xml_esc(strbuf_t *b, const char *s)
{
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
        case '&':  strbuf_append(b, "&amp;");  break;
        case '<':  strbuf_append(b, "&lt;");   break;
        case '>':  strbuf_append(b, "&gt;");   break;
        default:   strbuf_append_char(b, *s);  break;
        }
    }
}

/* 正文按行拆 <p>（\r\n / \n 都认，空行跳过） */
static void xhtml_paragraphs(strbuf_t *b, const char *content)
{
    const char *p = content;
    if (!p) return;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && p[len - 1] == '\r') len--;
        if (len) {
            char *line;
            strbuf_append(b, "<p>");
            line = (char *)malloc(len + 1);
                if (line) {
                    memcpy(line, p, len);
                    line[len] = '\0';
                    xml_esc(b, line);
                    free(line);
                }
            strbuf_append(b, "</p>\n");
        }
        if (!nl) break;
        p = nl + 1;
    }
}

/* ---------------- EPUB 组装 ---------------- */

static void add_xml_decl(strbuf_t *b)
{
    strbuf_append(b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
}

int export_novel_epub(novel_project_t *proj, const char *path)
{
    FILE *f;
    zip_w_t z;
    strbuf_t *b = NULL;
    char name[128];   /* 放得下最长 manifest 行（约 78 字节）；64 会把 media-type= 那行
                         静默截断、换行符弄丢，三行黏一行（实测踩过） */
    char ident[128];
    char modified[32];
    const char *title;
    int i, ret = -1;
    time_t now;
    struct tm tmv;

    if (!proj || !path || proj->chapter_count <= 0) return -1;

    title = (proj->name && *proj->name) ? proj->name : "未命名";
    snprintf(ident, sizeof(ident), "urn:novelnow:%s",
             (proj->id && *proj->id) ? proj->id : "novel");

    now = time(NULL);
#ifdef _MSC_VER
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    snprintf(modified, sizeof(modified), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    f = fopen_utf8(path, "wb");
    if (!f) return -1;
    zip_w_init(&z, f);

    /* 1. mimetype：规范强制第一项 + STORE（我们全部 STORE，天然合规） */
    if (zip_add(&z, "mimetype", "application/epub+zip", 20) != 0) goto done;

    /* 2. META-INF/container.xml */
    b = strbuf_create(512);
    if (!b) goto done;
    add_xml_decl(b);
    strbuf_append(b, "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n");
    strbuf_append(b, "  <rootfiles>\n");
    strbuf_append(b, "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n");
    strbuf_append(b, "  </rootfiles>\n</container>\n");
    if (zip_add(&z, "META-INF/container.xml", strbuf_text(b), (unsigned int)strlen(strbuf_text(b))) != 0) goto done;
    strbuf_destroy(b); b = NULL;

    /* 3. OEBPS/content.opf */
    b = strbuf_create(4096);
    if (!b) goto done;
    add_xml_decl(b);
    strbuf_append(b, "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\">\n");
    strbuf_append(b, "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n");
    strbuf_append(b, "  <dc:identifier id=\"bookid\">");
    xml_esc(b, ident);
    strbuf_append(b, "</dc:identifier>\n  <dc:title>");
    xml_esc(b, title);
    strbuf_append(b, "</dc:title>\n  <dc:language>zh-CN</dc:language>\n");
    strbuf_append(b, "  <meta property=\"dcterms:modified\">");
    strbuf_append(b, modified);
    strbuf_append(b, "</meta>\n</metadata>\n<manifest>\n");
    strbuf_append(b, "  <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n");
    strbuf_append(b, "  <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n");
    strbuf_append(b, "  <item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n");
    for (i = 0; i < proj->chapter_count; i++) {
        snprintf(name, sizeof(name), "  <item id=\"ch%d\" href=\"ch%d.xhtml\" media-type=\"application/xhtml+xml\"/>\n", i + 1, i + 1);
        strbuf_append(b, name);
    }
    strbuf_append(b, "</manifest>\n<spine toc=\"ncx\">\n");
    for (i = 0; i < proj->chapter_count; i++) {
        snprintf(name, sizeof(name), "  <itemref idref=\"ch%d\"/>\n", i + 1);
        strbuf_append(b, name);
    }
    strbuf_append(b, "</spine>\n</package>\n");
    if (zip_add(&z, "OEBPS/content.opf", strbuf_text(b), (unsigned int)strlen(strbuf_text(b))) != 0) goto done;
    strbuf_destroy(b); b = NULL;

    /* 4. OEBPS/nav.xhtml（EPUB 3 目录） */
    b = strbuf_create(2048);
    if (!b) goto done;
    add_xml_decl(b);
    strbuf_append(b, "<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"zh-CN\" xml:lang=\"zh-CN\">\n");
    strbuf_append(b, "<head><meta charset=\"utf-8\"/><title>目录</title></head>\n<body>\n");
    strbuf_append(b, "<nav epub:type=\"toc\"><h1>目录</h1>\n<ol>\n");
    for (i = 0; i < proj->chapter_count; i++) {
        chapter_t *ch = &proj->chapters[i];
        snprintf(name, sizeof(name), "ch%d.xhtml", i + 1);
        strbuf_append(b, "<li><a href=\"");
        strbuf_append(b, name);
        strbuf_append(b, "\">");
        xml_esc(b, ch->title ? ch->title : "");
        strbuf_append(b, "</a></li>\n");
    }
    strbuf_append(b, "</ol>\n</nav>\n</body>\n</html>\n");
    if (zip_add(&z, "OEBPS/nav.xhtml", strbuf_text(b), (unsigned int)strlen(strbuf_text(b))) != 0) goto done;
    strbuf_destroy(b); b = NULL;

    /* 5. OEBPS/toc.ncx（EPUB 2 兼容目录） */
    b = strbuf_create(2048);
    if (!b) goto done;
    add_xml_decl(b);
    strbuf_append(b, "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n<head>\n");
    strbuf_append(b, "  <meta name=\"dtb:uid\" content=\"");
    xml_esc(b, ident);
    strbuf_append(b, "\"/>\n  <meta name=\"dtb:depth\" content=\"1\"/>\n</head>\n");
    strbuf_append(b, "<docTitle><text>");
    xml_esc(b, title);
    strbuf_append(b, "</text></docTitle>\n<navMap>\n");
    for (i = 0; i < proj->chapter_count; i++) {
        chapter_t *ch = &proj->chapters[i];
        strbuf_append(b, "  <navPoint id=\"navpoint-");
        snprintf(name, sizeof(name), "%d\" playOrder=\"%d\"><navLabel><text>", i + 1, i + 1);
        strbuf_append(b, name);
        xml_esc(b, ch->title ? ch->title : "");
        strbuf_append(b, "</text></navLabel><content src=\"ch");
        snprintf(name, sizeof(name), "%d.xhtml\"/></navPoint>\n", i + 1);
        strbuf_append(b, name);
    }
    strbuf_append(b, "</navMap>\n</ncx>\n");
    if (zip_add(&z, "OEBPS/toc.ncx", strbuf_text(b), (unsigned int)strlen(strbuf_text(b))) != 0) goto done;
    strbuf_destroy(b); b = NULL;

    /* 6. OEBPS/style.css */
    {
        const char *css =
            "body{font-family:\"Microsoft YaHei\",serif;line-height:1.8;margin:1em;}\n"
            "h1{font-size:1.4em;margin:1.2em 0 0.8em;}\n"
            "p{text-indent:2em;margin:0.4em 0;}\n";
        if (zip_add(&z, "OEBPS/style.css", css, (unsigned int)strlen(css)) != 0) goto done;
    }

    /* 7. 章节 XHTML：一章一文件 */
    for (i = 0; i < proj->chapter_count; i++) {
        chapter_t *ch = &proj->chapters[i];
        const char *chtitle = (ch->title && *ch->title) ? ch->title : "";
        b = strbuf_create(4096);
        if (!b) goto done;
        add_xml_decl(b);
        strbuf_append(b, "<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"zh-CN\" xml:lang=\"zh-CN\">\n<head>\n");
        strbuf_append(b, "  <meta charset=\"utf-8\"/>\n  <title>");
        xml_esc(b, chtitle);
        strbuf_append(b, "</title>\n  <link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n</head>\n<body>\n");
        strbuf_append(b, "<h1>");
        xml_esc(b, chtitle);
        strbuf_append(b, "</h1>\n");
        xhtml_paragraphs(b, ch->content);
        strbuf_append(b, "</body>\n</html>\n");
        snprintf(name, sizeof(name), "OEBPS/ch%d.xhtml", i + 1);
        if (zip_add(&z, name, strbuf_text(b), (unsigned int)strlen(strbuf_text(b))) != 0) goto done;
        strbuf_destroy(b); b = NULL;
    }

    ret = zip_finish(&z);
done:
    if (b) strbuf_destroy(b);
    free(z.ents);
    fclose(f);
    return ret;
}
