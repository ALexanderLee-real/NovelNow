/* state.js - global state S + OUT_DIR (split from app.js 2026-10-18, zero behavior change; loaded in original order) */
/* ============================================================
 * 2. 状态
 * ============================================================ */
const S = {
  cfg: {},
  proj: { hasProject: false, directions: [], plans: [], chapters: [], characters: [], checks: [], foreshadows: [] },
  projects: [],
  busy: false,
  selDirection: -1,
  selPlan: -1,
  selChapter: -1,
  openChapter: -1,     /* 当前编辑器里加载的是哪一章 */
  openPlan: -1
};

/* ---------- 输出目录 ----------
   C 侧的 export 命令用的是它自己配置里的 exportDir；前端这里选完目录
   只改了内存里的副本、从没回传过，所以 C 侧始终是空 →
   「导出失败，请检查输出目录是否可写」。这里补上回传与本地兜底。 */
let OUT_DIR = '';
function outDir() { return OUT_DIR || S.cfg.exportDir || S.cfg.projectsDir || ''; }

