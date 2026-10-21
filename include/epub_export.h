/* epub_export.h - EPUB 3 导出（支线 B，2026-10-19）
 * 自实现 ZIP/STORE 打包，无三方库；与 export_novel_doc / export_novel_txt 并列的第三产物。 */
#ifndef EPUB_EXPORT_H
#define EPUB_EXPORT_H

#include "project.h"

/* 把整本书导出成 EPUB 3；0=成功，-1=失败（proj 为空/无章节/写文件失败）。
 * path 由调用方拼好（do_export 里 "<dir>\<safe>.epub"）。 */
int export_novel_epub(novel_project_t *proj, const char *path);

#endif /* EPUB_EXPORT_H */
