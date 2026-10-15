/* ============================================================
 * novelnow WebUI — 前端逻辑
 *
 * 与 C 侧（host.c）的协议：
 *   JS -> C : {cmd, id, params}
 *   C -> JS : {type:"result", id, cmd, ok, data|error}
 *             {type:"event",  name, data}
 *
 * 事件名：status / toast / progress / busy / project / projects /
 *         task_done / chapter_written / ask
 * ============================================================ */
'use strict';

/* ============================================================
 * 1. 桥接
 * ============================================================ */
const bridge = (window.chrome && window.chrome.webview) ? window.chrome.webview : null;

let seq = 0;
const pending = new Map();

function post(obj) {
  if (!bridge) { console.warn('非 WebView2 环境，消息被丢弃', obj); return; }
  bridge.postMessage(JSON.stringify(obj));
}

/** 发命令并等待结果（C 侧同步命令会立刻回；异步命令回 {accepted:true}） */
function send(cmd, params, timeoutMs) {
  return new Promise((resolve, reject) => {
    const id = ++seq;
    /* 超时兜底：C 侧若因某种原因不回 result，Promise 会永远挂起，
       pending 里的 resolve/reject 也永远不释放。这里到点主动清理并报错。 */
    const timer = setTimeout(() => {
      if (pending.has(id)) {
        pending.delete(id);
        reject(new Error('命令超时：' + cmd));
      }
    }, timeoutMs || 30000);
    pending.set(id, {
      resolve: (v) => { clearTimeout(timer); resolve(v); },
      reject: (e) => { clearTimeout(timer); reject(e); },
      cmd
    });
    post({ cmd, id, params: params || {} });
  });
}

/** 发命令但不关心返回值，失败时弹 toast */
function fire(cmd, params) {
  return send(cmd, params).catch(e => toast('error', e.message || String(e)));
}

/* ---- UI 自检通道 ----
 * WebView2 的 DevTools 控制台在本项目的运行环境里拿不到，白屏时无从下手。
 * 这里把未捕获异常与关键节点写进宿主日志（C 侧 cmd=log -> webui.log 的 [UI] 行），
 * 是排查前端问题唯一可观测的路径。 */
function uiLog(msg) {
  if (!bridge) { console.warn('[uiLog]', msg); return; }
  bridge.postMessage(JSON.stringify({ cmd: 'log', id: 0, params: { msg: String(msg).slice(0, 480) } }));
}

window.addEventListener('error', e => {
  uiLog('[error] ' + (e.message || '?') +
        ' @ ' + String(e.filename || '?').split('/').pop() + ':' + (e.lineno || 0));
});

window.addEventListener('unhandledrejection', e => {
  const r = e && e.reason;
  uiLog('[reject] ' + (r && r.message ? r.message : String(r)));
});

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

/* ============================================================
 * 3. 小工具
 * ============================================================ */
const $ = id => document.getElementById(id);

function esc(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

function num(n) {
  if (n == null || isNaN(n)) return '0';
  return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

function toast(kind, text) {
  const box = $('toasts');
  const el = document.createElement('div');
  el.className = 'toast ' + (kind || 'info');
  el.innerHTML = '<i></i><span>' + esc(text) + '</span>';
  box.appendChild(el);
  requestAnimationFrame(() => el.classList.add('is-in'));
  setTimeout(() => {
    el.classList.remove('is-in');
    el.classList.add('is-out');
    setTimeout(() => el.remove(), 260);
  }, kind === 'error' ? 5200 : 3000);
}

function setStatus(text, isErr) {
  $('statusText').textContent = text || '就绪';
  const dot = $('busyDot');
  dot.classList.toggle('is-err', !!isErr && !S.busy);
}

function setBusy(b) {
  S.busy = !!b;
  $('busyDot').classList.toggle('is-busy', S.busy);
  $('btnCancel').disabled = !S.busy;
  if (!S.busy) {
    $('progressWrap').classList.remove('is-on', 'is-indet');
    $('progressBar').style.width = '0%';
    $('progressText').textContent = '';
  }
}

function setProgress(cur, total, text) {
  const wrap = $('progressWrap');
  const bar = $('progressBar');
  wrap.classList.add('is-on');
  if (total > 0) {
    wrap.classList.remove('is-indet');
    bar.style.width = Math.max(0, Math.min(100, (cur / total) * 100)) + '%';
    $('progressText').textContent = cur + ' / ' + total;
  } else {
    wrap.classList.add('is-indet');
    $('progressText').textContent = '';
  }
  if (text) $('statusText').textContent = text;
}

/* ---------- 弹层 ---------- */
let modalClose = null;
/* 阻塞式弹层专用状态。C 的工作线程会卡在 WaitForSingleObject 上等 JS 回 answer，
   所以这类弹层（pinned）不能被 Esc / 点遮罩无声关掉——关掉了 JS 就永远不会回话，
   任务会永久停在确认框上，连「取消任务」都没用。
   处理办法：pinned 弹层必须走「等价动作」收尾（当前把 Esc / 遮罩等价为「停止」），
   没有等价动作时干脆不放行，强制用户点按钮。 */
let modalPinned = false;
let modalDismiss = null;

/* ---- 通用流式面板：方向/方案/大纲/一致性检查等阶段逐字输出 ---- */
let streamBox = null, streamPre = null;
function streamEnsure() {
  if (streamBox) return;
  streamBox = document.createElement('div');
  streamBox.style.cssText = 'position:fixed;right:18px;bottom:18px;width:380px;max-height:45vh;' +
    'display:none;flex-direction:column;background:var(--bg-card,#1d1f24);border:1px solid rgba(128,128,128,.35);' +
    'border-radius:10px;box-shadow:0 8px 24px rgba(0,0,0,.35);z-index:9999;overflow:hidden;';
  streamBox.innerHTML = '<div style="padding:8px 12px;font-size:12px;opacity:.75;display:flex;' +
    'justify-content:space-between"><span>AI 输出中…</span><span id="streamClose" style="cursor:pointer">✕</span></div>';
  streamPre = document.createElement('pre');
  streamPre.style.cssText = 'margin:0;padding:0 12px 12px;overflow:auto;white-space:pre-wrap;' +
    'word-break:break-word;font:12px/1.6 ui-monospace,Consolas,monospace;flex:1;';
  streamBox.appendChild(streamPre);
  document.body.appendChild(streamBox);
  streamBox.querySelector('#streamClose').onclick = () => streamHide();
}
function streamFeed(delta) {
  if (!delta) return;
  streamEnsure();
  streamBox.style.display = 'flex';
  /* 只保留尾部 ~8KB，避免超长大纲把 DOM 撑爆 */
  const MAX = 8 * 1024;
  streamPre.textContent = (streamPre.textContent + delta).slice(-MAX);
  streamPre.scrollTop = streamPre.scrollHeight;
}
function streamHide() {
  if (!streamBox) return;
  streamBox.style.display = 'none';
  streamPre.textContent = '';
}

function showModal(title, bodyHtml, actions, opts) {
  $('mTitle').textContent = title;
  $('mText').innerHTML = bodyHtml;
  const box = $('mActions');
  box.innerHTML = '';
  (actions || []).forEach(a => {
    const b = document.createElement('button');
    b.className = 'btn ' + (a.kind || 'ghost');
    b.textContent = a.label;
    b.onclick = () => {
      /* force=true：点按钮是合法收尾路径，可以无条件关闭 */
      if (!a.keepOpen) hideModal(true);
      if (a.onClick) a.onClick();
    };
    box.appendChild(b);
  });
  $('overlay').classList.add('is-on');
  modalClose = null;
  modalPinned = !!(opts && opts.pinned);
  modalDismiss = (opts && opts.onDismiss) || null;
}

/* force 缺省/false 时，pinned 弹层不会被无声关闭。
   注意：pinned 弹层自身不做「关闭时回话」——等价动作里已经包含回话，
   否则会出现「关了但没回话」这个正是要修的 bug。 */
function hideModal(force) {
  if (modalPinned && !force) {
    const f = modalDismiss;
    if (!f) return;                       /* 没有等价动作：不放行 */
    modalPinned = false;                  /* 先解锁，防重入 */
    modalDismiss = null;
    try { f(); } catch (e) { uiLog('[modal] 等价动作异常 ' + (e && e.message)); }
    $('overlay').classList.remove('is-on');
    return;
  }
  $('overlay').classList.remove('is-on');
  modalPinned = false;
  modalDismiss = null;
  if (modalClose) { const f = modalClose; modalClose = null; f(); }
}

$('overlay').addEventListener('click', e => {
  if (e.target === $('overlay')) { hideModal(); }
});

/* ============================================================
 * 4. 页面切换
 * ============================================================ */
let page = 'idea';

function goPage(name) {
  if (name === page) return;
  page = name;
  document.querySelectorAll('.nav-item').forEach(b =>
    b.classList.toggle('is-active', b.dataset.page === name));
  document.querySelectorAll('.page').forEach(p =>
    p.classList.toggle('is-active', p.dataset.page === name));
  const body = document.querySelector('.page[data-page="' + name + '"]');
  if (body) body.scrollTop = 0;
  closeMenu();
}

document.getElementById('nav').addEventListener('click', e => {
  const b = e.target.closest('.nav-item');
  if (b) goPage(b.dataset.page);
});

/* ============================================================
 * 5. 渲染：侧栏 / 项目列表
 * ============================================================ */
function renderSidebar() {
  const p = S.proj;
  $('brandSub').textContent = p.hasProject
    ? (p.name || '未命名项目')
    : '未打开项目';

  $('statChapters').textContent = p.hasProject ? (p.chapters || []).length : '—';
  $('statWords').textContent = p.hasProject ? num(p.totalWords) : '—';

  const badge = {
    idea: (p.directions || []).length,
    plan: (p.plans || []).length,
    outline: (p.chapters || []).length
  };
  document.querySelectorAll('.nav-badge').forEach(el => {
    const n = badge[el.dataset.badge] || 0;
    el.textContent = n ? String(n) : '';
    el.classList.toggle('has', n > 0);
  });

  $('projStage').textContent = p.hasProject && p.stage ? stageName(p.stage) : '';
  $('projName').value = p.hasProject ? (p.name || '') : '';

  $('projDir').textContent = S.cfg.projectsDir || '—';
  $('outDir').textContent = outDir() || '—';
  setLink('linkDoc', S.cfg.lastDoc, 'RTF');
  setLink('linkTxt', S.cfg.lastTxt, 'TXT');
  $('cfgPath').textContent = S.cfg.configPath ? 'config.json' : '';
}

function stageName(s) {
  return ({ idea: '创意中', direction: '已选方向', plan: '已选方案', outline: '大纲中', writing: '写作中', review: '审阅中' })[s] || s;
}

function setLink(id, path, label) {
  const a = $(id);
  if (path) {
    a.textContent = label;
    a.classList.remove('is-off');
    a.dataset.path = path;
    a.title = path;
  } else {
    a.textContent = label;
    a.classList.add('is-off');
    a.dataset.path = '';
    a.title = '尚未导出';
  }
}

function renderProjectList() {
  const box = $('projList');
  const list = S.projects || [];
  $('projListHint').textContent = list.length ? list.length + ' 个' : '—';
  if (!list.length) {
    box.innerHTML = '<div class="empty small"><p>暂无项目</p><span>新建项目并保存后会出现在这里</span></div>';
    return;
  }
  box.innerHTML = list.map(p => `
    <div class="item" data-pid="${esc(p.id)}">
      <div class="item-title">${esc(p.name || p.id)}</div>
      <div class="item-meta">
        <span>${esc((p.updatedAt || '').replace('T', ' ').slice(0, 16) || '未知时间')}</span>
        <span class="chip dim">${p.chapterCount || 0} 章</span>
        ${p.stage ? `<span class="chip dim">${esc(stageName(p.stage))}</span>` : ''}
      </div>
    </div>`).join('');
}

$('projList').addEventListener('click', e => {
  const it = e.target.closest('.item');
  if (!it) return;
  /* 走 loadProject 而不是 fire：需要收到 C 侧的 needConfirm 才能弹「未保存」确认 */
  loadProject({ id: it.dataset.pid });
});

/* ============================================================
 * 6. 渲染：创意页
 * ============================================================ */
function renderIdea() {
  const p = S.proj, dirs = p.directions || [];
  $('dirCountHint').textContent = dirs.length ? dirs.length + ' 个方向' : '尚未生成';

  const box = $('dirList');
  if (!dirs.length) {
    box.innerHTML = `<div class="empty">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.3"><path d="M12 3v18M3 12h18"/></svg>
      <p>还没有方向</p><span>先在左侧写下想法，然后点「生成创作方向」</span></div>`;
  } else {
    box.innerHTML = dirs.map(d => `
      <div class="item ${d.index === S.selDirection || d.selected ? 'is-sel' : ''}" data-dir="${d.index}">
        <div class="item-title"><span class="idx">${String(d.index + 1).padStart(2, '0')}</span>${esc(d.title || '（无标题）')}</div>
        <div class="item-desc">${esc(d.description || '')}</div>
      </div>`).join('');
  }

  $('btnSelDir').disabled = S.selDirection < 0;
  $('btnGenPlans').disabled = S.selDirection < 0;
  if (!$('ideaText').matches(':focus')) $('ideaText').value = p.seedIdea || '';
}

$('dirList').addEventListener('click', e => {
  const it = e.target.closest('.item');
  if (!it) return;
  const idx = parseInt(it.dataset.dir, 10);
  S.selDirection = idx;
  document.querySelectorAll('#dirList .item').forEach(x => x.classList.toggle('is-sel', x === it));
  $('btnSelDir').disabled = false;
  $('btnGenPlans').disabled = false;
});

/* ============================================================
 * 7. 渲染：方案页
 * ============================================================ */
function renderPlan() {
  const p = S.proj, plans = p.plans || [];
  $('planCountHint').textContent = plans.length + ' 套';

  const box = $('planList');
  if (!plans.length) {
    box.innerHTML = `<div class="empty">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.3"><path d="M4 4h16v16H4z"/><path d="M8 9h8M8 13h6"/></svg>
      <p>还没有方案</p><span>在「创意」页选定方向后生成</span></div>`;
  } else {
    box.innerHTML = plans.map(pl => `
      <div class="item ${pl.index === S.selPlan || pl.selected ? 'is-sel' : ''}" data-plan="${pl.index}">
        <div class="item-title"><span class="idx">${String(pl.index + 1).padStart(2, '0')}</span>${esc(pl.title || '（无标题）')}</div>
        <div class="item-desc">${esc(pl.synopsis || '')}</div>
      </div>`).join('');
  }

  $('btnSelPlan').disabled = S.selPlan < 0;
  $('btnExpandOutline').disabled = S.selPlan < 0;
}

$('planList').addEventListener('click', e => {
  const it = e.target.closest('.item');
  if (!it) return;
  const idx = parseInt(it.dataset.plan, 10);
  S.selPlan = idx;
  document.querySelectorAll('#planList .item').forEach(x => x.classList.toggle('is-sel', x === it));
  $('btnSelPlan').disabled = false;
  $('btnExpandOutline').disabled = false;
  loadPlan(idx);
});

function loadPlan(idx) {
  const pl = (S.proj.plans || [])[idx];
  S.openPlan = idx;
  if (!pl) {
    $('planTitle').value = ''; $('planSynopsis').value = ''; $('planOutline').value = '';
    return;
  }
  $('planTitle').value = pl.title || '';
  $('planSynopsis').value = pl.synopsis || '';
  $('planOutline').value = pl.outline || '';
}

/* ============================================================
 * 8. 渲染：大纲 · 写作
 * ============================================================ */
function statusChip(st) {
  if (st === 3) return '<span class="chip ok">已审核</span>';
  if (st === 2) return '<span class="chip ok">已完成</span>';
  if (st === 1) return '<span class="chip warn">写作中</span>';
  return '<span class="chip dim">未写</span>';
}

function renderOutline() {
  const p = S.proj, chs = p.chapters || [];
  $('chHint').textContent = chs.length ? chs.length + ' 章' : '0 章';

  const box = $('chList');
  if (!chs.length) {
    box.innerHTML = `<div class="empty">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.3"><circle cx="12" cy="12" r="9"/><path d="M12 8v8M8 12h8"/></svg>
      <p>还没有章节</p><span>到「方案」页细化大纲</span></div>`;
  } else {
    box.innerHTML = chs.map(c => `
      <div class="item ${c.index === S.selChapter ? 'is-sel' : ''}" data-ch="${c.index}">
        <div class="item-title"><span class="idx">${String(c.index + 1).padStart(2, '0')}</span>${esc(c.title || '（无标题）')}</div>
        <div class="item-meta">${statusChip(c.status)}<span>${num(c.wordCount)} 字</span></div>
      </div>`).join('');
  }

  $('btnExpandCh').disabled = S.selChapter < 0;
  $('chWc').textContent = (S.selChapter >= 0 && chs[S.selChapter])
    ? num(chs[S.selChapter].wordCount) + ' 字' : '—';

  renderWorld();
  renderChars();
}

$('chList').addEventListener('click', e => {
  const it = e.target.closest('.item');
  if (!it) return;
  const idx = parseInt(it.dataset.ch, 10);
  selectChapter(idx);
});

function selectChapter(idx) {
  S.selChapter = idx;
  document.querySelectorAll('#chList .item').forEach(x =>
    x.classList.toggle('is-sel', parseInt(x.dataset.ch, 10) === idx));
  $('btnExpandCh').disabled = idx < 0;
  fire('set_selected_chapter', { index: idx });
  if (idx < 0) return;
  send('get_chapter', { index: idx }).then(d => {
    S.openChapter = idx;
    $('chTitle').value = d.title || '';
    $('chSummary').value = d.summary || '';
    $('chContent').value = d.content || '';
    $('chWc').textContent = num(d.wordCount) + ' 字';
  }).catch(() => {});
}

function renderWorld() {
  const w = S.proj.world || {};
  if (!$('wType').matches(':focus')) $('wType').value = w.worldType || '';
  if (!$('wSetting').matches(':focus')) $('wSetting').value = w.setting || '';
  if (!$('wPower').matches(':focus')) $('wPower').value = w.powerSystem || '';
}

function renderChars() {
  const box = $('charList');
  const cs = S.proj.characters || [];
  if (!cs.length) {
    box.innerHTML = '<div class="empty small"><p>暂无人物</p><span>可点右上「生成」让 AI 起草</span></div>';
    return;
  }
  box.innerHTML = cs.map((c, i) => `
    <div class="item" data-char="${i}">
      <div class="item-title">${esc(c.name || '（无名）')}</div>
      <div class="item-meta">
        ${c.role ? `<span class="chip dim">${esc(c.role)}</span>` : ''}
      </div>
      ${c.description ? `<div class="item-desc">${esc(c.description)}</div>` : ''}
    </div>`).join('');
}

/* 伏笔登记表：已埋未收=醒目提示，已回收=淡色 */
function renderForeshadows() {
  const box = $('fsList');
  if (!box) return;
  const fs = S.proj.foreshadows || [];
  const openN = fs.filter(f => f.resolved == null || f.resolved < 0).length;
  const hint = $('fsHint');
  if (hint) hint.textContent = fs.length ? `${fs.length} 条 · ${openN} 未回收` : '0 条';
  if (!fs.length) {
    box.innerHTML = '<div class="empty small"><p>写完章节后自动登记</p></div>';
    return;
  }
  box.innerHTML = fs.map(f => {
    const done = f.resolved != null && f.resolved >= 0;
    return `<div class="item" style="${done ? 'opacity:.55' : ''}">
      <div class="item-title">${done ? '✅' : '🪝'} ${esc(f.title || '（未命名）')}</div>
      <div class="item-meta"><span class="chip dim">第${(f.planted ?? 0) + 1}章埋</span>${done ? `<span class="chip dim">第${f.resolved + 1}章收</span>` : '<span class="chip dim">未回收</span>'}</div>
      ${f.description ? `<div class="item-desc">${esc(f.description)}</div>` : ''}
    </div>`;
  }).join('');
}

/* 人物编辑弹层：整体替换提交（对应 C 侧 set_characters） */
let charDraft = null;

function openCharEditor() {
  charDraft = JSON.parse(JSON.stringify(S.proj.characters || []));
  if (!charDraft.length) charDraft.push({ name: '', role: '', traits: '', description: '', background: '' });
  renderCharEditor();
}

function renderCharEditor() {
  const body = `
    <div class="form" style="gap:10px">
      <div class="list list-tight" style="max-height:46vh" id="charEditList">
        ${charDraft.map((c, i) => `
          <div class="item" style="cursor:default">
            <div class="item-title" style="justify-content:space-between">
              <span>人物 ${i + 1}</span>
              <button class="mini-btn" data-del="${i}" type="button">删除</button>
            </div>
            <div class="form" style="gap:7px;margin-top:8px">
              <div class="inline-fields">
                <label class="field sm" style="width:47%"><span>姓名</span><input type="text" data-f="name" data-i="${i}" value="${esc(c.name)}"></label>
                <label class="field sm" style="width:47%"><span>定位</span><input type="text" data-f="role" data-i="${i}" value="${esc(c.role)}"></label>
              </div>
              <label class="field"><span>性格特点</span><input type="text" data-f="traits" data-i="${i}" value="${esc(c.traits)}"></label>
              <label class="field"><span>描述</span><textarea class="ta" rows="2" data-f="description" data-i="${i}">${esc(c.description)}</textarea></label>
              <label class="field"><span>背景故事</span><textarea class="ta" rows="2" data-f="background" data-i="${i}">${esc(c.background)}</textarea></label>
            </div>
          </div>`).join('')}
      </div>
      <button class="btn ghost" id="charAdd" type="button">＋ 添加人物</button>
    </div>`;

  showModal('编辑人物', body, [
    { label: '取消' },
    { label: '保存', kind: 'primary', keepOpen: true, onClick: saveCharEditor }
  ]);

  const list = $('charEditList');
  list.addEventListener('input', e => {
    const t = e.target;
    if (t.dataset && t.dataset.f) charDraft[+t.dataset.i][t.dataset.f] = t.value;
  });
  list.addEventListener('click', e => {
    const b = e.target.closest('[data-del]');
    if (!b) return;
    charDraft.splice(+b.dataset.del, 1);
    renderCharEditor();
  });
  $('charAdd').onclick = () => {
    charDraft.push({ name: '', role: '', traits: '', description: '', background: '' });
    renderCharEditor();
  };
}

function saveCharEditor() {
  const clean = charDraft.filter(c => (c.name || '').trim());
  send('set_characters', { characters: clean }).then(() => {
    hideModal();
    toast('success', '人物已保存');
  }).catch(e => toast('error', e.message));
}

/* ============================================================
 * 9. 渲染：设置页
 * ============================================================ */

/* ---------- 服务商预设 ----------
 * 预设表由 C 侧（host.c 的 g_presets）下发，这里只负责渲染与回填。
 * 端点是否命中某个预设，用于高亮对应按钮。 */
let presetsBuilt = false;

function presetList() {
  return (S.cfg && Array.isArray(S.cfg.presets)) ? S.cfg.presets : [];
}

/* 归一化端点：容忍结尾斜杠，以及用户直接填了完整 chat/completions 地址 */
function normUrl(u) {
  return String(u || '').trim()
    .replace(/\/+$/, '')
    .replace(/\/chat\/completions$/, '')
    .replace(/\/+$/, '');
}

function currentPreset() {
  const el = $('apiUrl');
  const u = normUrl(el ? el.value : '');
  if (!u) return null;
  return presetList().find(p => normUrl(p.url) === u) || null;
}

function fillModelList(models) {
  const dl = $('modelList');
  if (!dl) return;
  dl.innerHTML = (models || []).map(m => `<option value="${esc(m)}"></option>`).join('');
}

function markActivePreset() {
  const cur = currentPreset();
  document.querySelectorAll('#presetRow .preset-chip').forEach(b => {
    b.classList.toggle('on', !!cur && b.dataset.id === cur.id);
  });
  const hint = $('presetHint');
  if (hint) {
    hint.textContent = cur
      ? `已匹配「${cur.name}」，点「保存设置」生效。模型可手改，或点右侧「获取」拉真实列表。`
      : '自定义端点（未匹配预设）。模型名可手填，或点「获取」拉取。';
  }
}

function renderPresets() {
  const row = $('presetRow');
  if (!row) return;
  const list = presetList();
  if (!presetsBuilt && list.length) {
    row.innerHTML = list.map(p =>
      `<button type="button" class="preset-chip" data-id="${esc(p.id)}">${esc(p.name)}</button>`
    ).join('');
    presetsBuilt = true;
  }
  /* 若启动时端点已命中某预设，顺手把该预设的模型灌进 datalist */
  const cur = currentPreset();
  const dl = $('modelList');
  if (cur && dl && !dl.children.length) fillModelList(cur.models);
  markActivePreset();
}

/* 点预设：填端点 → 填模型 → 立即保存 */
function applyPreset(id) {
  const p = presetList().find(x => x.id === id);
  if (!p) return;

  $('apiUrl').value = p.url || '';
  fillModelList(p.models);

  /* 只覆盖「空」或「本来就是其它预设的模型」，不冲掉用户手填的自定义模型 */
  const now = ($('model').value || '').trim();
  const fromPreset = presetList().some(x => (x.models || []).indexOf(now) >= 0);
  if (!now || fromPreset) $('model').value = (p.models || [])[0] || '';

  markActivePreset();

  send('save_config', collectCfg()).then(c => {
    S.cfg = c;
    renderSidebar();
    markActivePreset();
    toast('success', '已切换到 ' + p.name);
  }).catch(e => toast('error', e.message));
}

function renderSettings() {
  const c = S.cfg;
  const set = (id, v) => { const el = $(id); if (el && !el.matches(':focus')) el.value = v == null ? '' : v; };
  set('apiUrl', c.apiUrl);
  set('apiKey', c.apiKey);
  set('model', c.model);
  set('temperature', c.temperature);
  set('maxTokens', c.maxTokens);
  set('timeout', c.timeout);
  set('rateLimitMs', c.rateLimitMs);
  set('maxRetries', c.maxRetries);
  if (!document.activeElement || document.activeElement.id !== 'consistency') {
    $('consistency').checked = !!c.enableConsistencyCheck;
  }
  renderPresets();
}

/* ============================================================
 * 10. 全量渲染
 * ============================================================ */
function renderAll(reloadEditors) {
  renderSidebar();
  renderIdea();
  renderPlan();
  renderOutline();
  renderSettings();
  renderProjectList();
  renderForeshadows();

  /* 选中态：优先用 C 侧给的索引，其次用本地记录 */
  const sd = S.proj.selectedDirection;
  if (sd != null && sd >= 0) S.selDirection = sd;
  const sp = S.proj.selectedPlan;
  if (sp != null && sp >= 0) { S.selPlan = sp; if (reloadEditors) loadPlan(sp); }
  if (S.selDirection >= 0) $('btnSelDir').disabled = false;

  if (S.openChapter >= 0 && S.openChapter < (S.proj.chapters || []).length) {
    if (reloadEditors) selectChapter(S.openChapter);
  }
}

/* 拉取最新项目快照并全量刷新：任务完成 / 选定方向方案等场景的公共刷新入口，
   消除各处重复的 send('get_project').then(...renderAll) */
function refreshProject(reloadEditors) {
  return send('get_project').then(p => {
    S.proj = p;
    renderAll(reloadEditors);
  }).catch(() => {});
}

/* ============================================================
 * 11. 事件处理
 * ============================================================ */
function onMessage(m) {
  if (!m) return;
  if (m.type === 'result') {
    const p = pending.get(m.id);
    if (!p) return;
    pending.delete(m.id);
    if (m.ok) p.resolve(m.data || {});
    else p.reject(new Error(m.error || '操作失败'));
    return;
  }
  if (m.type !== 'event') return;

  const d = m.data || {};
  switch (m.name) {
    case 'status':
      setStatus(d.text);
      break;

    case 'toast':
      toast(d.kind, d.text);
      break;

    case 'busy':
      setBusy(d.busy);
      break;

    case 'progress':
      setProgress(d.current, d.total, d.message || d.stage);
      break;

    case 'project':
      S.proj = d;
      renderSidebar();
      renderIdea();
      renderPlan();
      renderOutline();
      renderSettings();
      renderForeshadows();
      {
        const sd = d.selectedDirection, sp = d.selectedPlan;
        if (sd != null && sd >= 0) { S.selDirection = sd; $('btnSelDir').disabled = false; $('btnGenPlans').disabled = false; }
        if (sp != null && sp >= 0) { S.selPlan = sp; $('btnSelPlan').disabled = false; $('btnExpandOutline').disabled = false; if (S.openPlan !== sp) loadPlan(sp); }
        document.querySelectorAll('#dirList .item').forEach(x =>
          x.classList.toggle('is-sel', parseInt(x.dataset.dir, 10) === S.selDirection));
        document.querySelectorAll('#planList .item').forEach(x =>
          x.classList.toggle('is-sel', parseInt(x.dataset.plan, 10) === S.selPlan));
      }
      break;

    case 'projects':
      S.projects = d.projects || [];
      renderProjectList();
      break;

    case 'chapter_delta':
      /* 流式增量：实时回填当前打开的章节编辑器（其它章节等 task_done 刷新） */
      if (d.index === S.openChapter && d.delta) {
        const ta = $('chContent');
        ta.value += d.delta;
        ta.scrollTop = ta.scrollHeight;
        const wc = ta.value.replace(/\s/g, '').length;
        $('chWc').textContent = num(wc) + ' 字 · 生成中…';
      }
      break;

    case 'text_delta':
      /* 通用流式增量（方向/方案/大纲/一致性检查）：浮动面板逐字刷 */
      streamFeed(d.delta);
      break;

    case 'chapter_written':
      /* 逐章生成：某章写完了 */
      if (d.index === S.openChapter) {
        $('chContent').value = d.content || '';
        $('chWc').textContent = num(d.wordCount) + ' 字';
      }
      break;

    case 'task_done':
      setBusy(false);
      streamHide();
      /* 任务已结束，收掉可能还挂着的阻塞式确认框（例如用户是用「取消任务」
         在确认框挡着时中断的——那种情况下 C 侧已经代为收场，前端还留着弹层）。
         只处理 pinned，避免顺手关掉用户正在填的其它弹层。 */
      if (modalPinned) hideModal(true);
      handleTaskDone(d);
      break;

    case 'ask':
      askChapterReview(d);
      break;

    case 'shutdown':
      break;
  }
}

function handleTaskDone(d) {
  /* 项目结构变化的命令：刷新章节列表等 */
  if (d.cmd === 'fetch_models' && d.ok && d.models) {
    const dl = $('modelList');
    dl.innerHTML = d.models.map(m => `<option value="${esc(m)}">`).join('');
    toast('success', '已获取 ' + d.models.length + ' 个模型，点「模型」输入框可见下拉');
    return;
  }

  /* 章节/方案数量变了，重取一次完整快照最稳妥 */
  if (['gen_directions', 'gen_plans', 'expand_outline', 'load_project',
       'generate_characters', 'summarize_worldview', 'analyze_text', 'import_txt',
       'check_finale'].includes(d.cmd)) {
    refreshProject(true);
    send('get_config').then(c => { S.cfg = c; renderSidebar(); }).catch(() => {});
  }

  /* 选定方向/方案后留在本页操作，任务完成再翻到下一页 */
  if (d.cmd === 'gen_plans' && d.ok) goPage('plan');
  if (d.cmd === 'expand_outline' && d.ok) goPage('outline');

  /* 伏笔回收校验：完成后弹出报告全文 */
  if (d.cmd === 'check_finale' && d.ok) {
    send('get_project').then(p => {
      S.proj = p;
      const rep = (p && p.finaleReport) ? String(p.finaleReport).trim() : '';
      if (rep) showModal('伏笔回收校验报告',
        `<div style="white-space:pre-wrap;line-height:1.7">${esc(rep)}</div>`,
        [{ label: '知道了', kind: 'primary' }]);
    }).catch(() => {});
  }

  if (d.cmd === 'expand_chapter' && S.selChapter >= 0) {
    refreshProject(false);
    selectChapter(S.selChapter);
  }

  if (d.cmd === 'auto_write' || d.cmd === 'expand_all') {
    send('get_project').then(p => { S.proj = p; renderAll(true); }).catch(() => {});
    send('get_config').then(c => { S.cfg = c; renderSidebar(); }).catch(() => {});
  }
}

/* 逐章生成的「是否满意」询问。
   这个弹层对应 C 侧 bridge_ask 的阻塞等待，必须保证一定回 answer，
   所以注册成 pinned：Esc / 点遮罩等价于点「停止」，不会出现「关了但没回话」。 */
function askChapterReview(d) {
  const stop = () => fire('answer', { askId: d.askId, answer: 'cancel' });
  showModal('逐章生成 · 请确认',
    `<div style="white-space:pre-wrap;line-height:1.7">${esc(d.text || '')}\n\n满意就继续下一章；不满意可重新生成本章。</div>`, [
    { label: '停止', onClick: stop },
    { label: '重新生成本章', kind: 'ghost', onClick: () => fire('answer', { askId: d.askId, answer: 'no' }) },
    { label: '满意，继续', kind: 'primary', onClick: () => fire('answer', { askId: d.askId, answer: 'yes' }) }
  ], { pinned: true, onDismiss: stop });
}

/* ============================================================
 * 12. 交互绑定
 * ============================================================ */

/* ---- 创意页 ---- */
$('btnGenDirs').onclick = () => {
  const idea = $('ideaText').value.trim();
  if (!idea) { toast('warn', '请先写下你的想法'); $('ideaText').focus(); return; }
  if (!S.proj.hasProject) { toast('warn', '请先在「设置」页新建项目'); return; }
  ghost('#dirList', 3);
  fire('gen_directions', {
    idea,
    count: parseInt($('dirCount').value, 10) || 5,
    iterations: parseInt($('dirIter').value, 10) || 2
  });
};

$('btnSelDir').onclick = () => {
  if (S.selDirection < 0) return;
  fire('select_direction', { index: S.selDirection }).then(() => {
    refreshProject(false);
    toast('success', '方向已选定，点「生成小说方案」继续');
  }).catch(() => {});
};

$('btnGenPlans').onclick = () => {
  ghost('#planList', 3);
  fire('gen_plans', { count: parseInt($('planCount').value, 10) || 3 });
};

$('ideaText').addEventListener('blur', () => {
  if (!S.proj.hasProject) return;
  const v = $('ideaText').value;
  if (v !== (S.proj.seedIdea || '')) fire('set_project_idea', { idea: v });
});

/* 生成中的占位骨架（一次性动画，任务结束即移除） */
function ghost(sel, n) {
  const box = document.querySelector(sel);
  if (!box) return;
  box.innerHTML = Array.from({ length: n }).map(() =>
    '<div class="item is-ghost"><div class="bar"></div><div class="bar"></div></div>').join('');
}

/* ---- 方案页 ---- */
$('btnSelPlan').onclick = () => {
  if (S.selPlan < 0) return;
  fire('select_plan', { index: S.selPlan }).then(() => {
    refreshProject(false);
    toast('success', '方案已选定，点「细化大纲」继续');
  }).catch(() => {});
};

$('btnExpandOutline').onclick = () => {
  ghost('#chList', 4);
  fire('expand_outline', { count: parseInt($('chCount').value, 10) || 20 });
};

['planTitle', 'planSynopsis', 'planOutline'].forEach(id => {
  $(id).addEventListener('blur', () => {
    if (S.openPlan < 0) return;
    fire('set_plan', {
      index: S.openPlan,
      title: $('planTitle').value,
      synopsis: $('planSynopsis').value,
      outline: $('planOutline').value
    });
  });
});

/* ---- 大纲 · 写作 ---- */
$('btnExpandCh').onclick = () => {
  if (S.selChapter < 0) { toast('warn', '请先选择一个章节'); return; }
  fire('expand_chapter', { index: S.selChapter, words: parseInt($('words').value, 10) || 2000 });
};

$('btnExpandAll').onclick = () => {
  if (!(S.proj.chapters || []).length) { toast('warn', '还没有章节'); return; }
  showModal('批量扩写', '将依次扩写全部章节。已完成的章节会被覆盖。<br><br>任务期间可以点右下角「取消任务」中断。', [
    { label: '取消' },
    { label: '开始', kind: 'primary', onClick: () => fire('expand_all', { words: parseInt($('words').value, 10) || 2000 }) }
  ]);
};

$('btnStepWrite').onclick = () => {
  if (!(S.proj.chapters || []).length) { toast('warn', '还没有章节'); return; }
  fire('step_write', { words: parseInt($('words').value, 10) || 2000 });
};

$('btnAutoWrite').onclick = () => {
  if (!S.cfg.apiKey) { toast('warn', '请先在「设置」页填写 API Key'); goPage('settings'); return; }
  const contents = (S.proj.chapters || []).filter(c => c.hasContent).length;
  const resumeHint = contents && contents < (S.proj.chapters || []).length
    ? '检测到已写 ' + contents + '/' + (S.proj.chapters || []).length + ' 章，可在下方填起始章序号续写（0 = 从头）。'
    : '从想法出发，自动完成：创意发散 → 方案生成 → 大纲细化 → 正文扩写。<br>填起始章序号 &gt; 0 可跳过前三个阶段续写（要求项目已有方向/方案/大纲）。';
  showModal('全自动写作',
    resumeHint + '<br><br>' +
    '参数取自各页面输入框（方向数 / 方案数 / 章节数 / 每章字数）。<br>' +
    '这会消耗大量 token，且过程较长，可随时取消。', [
    { label: '取消' },
    {
      /* keepOpen：缺项目等前置不满足时只 toast，弹层留着让用户补完再点 */
      label: '开始', kind: 'primary', keepOpen: true, onClick: () => {
        if (!S.proj.hasProject) { toast('warn', '请先在「设置」页新建项目'); goPage('settings'); return; }
        const startCh = parseInt($('resumeFrom').value, 10) || 0;
        hideModal(true);
        fire('auto_write', {
          dirCount: parseInt($('dirCount').value, 10) || 5,
          planCount: parseInt($('planCount').value, 10) || 3,
          chapters: parseInt($('chCount').value, 10) || 20,
          words: parseInt($('words').value, 10) || 3000,
          startChapter: startCh
        });
      }
    }
  ]);
  /* 弹层已渲染，补一个续写输入框（挂在正文区 mText 末尾） */
  const box = $('mText');
  if (box && !$('resumeFrom')) {
    const wrap = document.createElement('label');
    wrap.className = 'field';
    wrap.style.marginTop = '10px';
    wrap.innerHTML = '<span>起始章序号（0 = 完整流程）</span>';
    const inp = document.createElement('input');
    inp.type = 'number'; inp.id = 'resumeFrom'; inp.min = '0'; inp.value = '0';
    wrap.appendChild(inp);
    box.appendChild(wrap);
  }
};

$('btnCheck').onclick = () => {
  if (S.selChapter < 0) { toast('warn', '请先选择一个章节'); return; }
  fire('check_consistency', { index: S.selChapter });
};

/* 伏笔回收校验：全书正文完成后跑一次，报告弹层展示 */
$('btnFinale').onclick = () => {
  if (!S.proj.hasProject) { toast('warn', '请先在「设置」页新建项目'); return; }
  const n = (S.proj.chapters || []).filter(c => c.hasContent).length;
  if (!n) { toast('warn', '还没有任何章节正文，先写作再校验'); return; }
  fire('check_finale', {});
};

$('btnExport').onclick = () => {
  const dir = outDir();
  if (!dir) { toast('warn', '还没有设置输出目录，先点「选择目录」'); return; }
  /* 三个键名一起带，C 侧认哪个用哪个 */
  send('export', { dir: dir, path: dir, exportDir: dir }).then(d => {
    S.cfg.lastDoc = d.doc || '';
    S.cfg.lastTxt = d.txt || '';
    renderSidebar();
  }).catch(e => toast('error', e.message));
};

$('btnSumWorld').onclick = () => fire('summarize_worldview', {});
$('btnGenChars').onclick = () => fire('generate_characters', {});

['chTitle', 'chSummary', 'chContent'].forEach(id => {
  $(id).addEventListener('blur', () => {
    if (S.openChapter < 0) return;
    fire('set_chapter', {
      index: S.openChapter,
      title: $('chTitle').value,
      summary: $('chSummary').value,
      content: $('chContent').value
    });
  });
});

['wType', 'wSetting', 'wPower'].forEach(id => {
  $(id).addEventListener('blur', () => {
    if (!S.proj.hasProject) return;
    fire('set_world', {
      worldType: $('wType').value,
      setting: $('wSetting').value,
      powerSystem: $('wPower').value
    });
  });
});

/* 人物编辑：点列表项打开弹层 */
$('charList').addEventListener('click', () => openCharEditor());

/* ---- 设置页 ---- */
$('presetRow').addEventListener('click', e => {
  const b = e.target.closest('.preset-chip');
  if (b) applyPreset(b.dataset.id);
});

/* 手改端点时实时更新高亮/提示（不触发保存） */
$('apiUrl').addEventListener('input', markActivePreset);

$('btnEye').onclick = () => {
  const i = $('apiKey');
  const show = i.type === 'password';
  i.type = show ? 'text' : 'password';
  $('btnEye').textContent = show ? '隐藏' : '显示';
};

$('btnFetchModels').onclick = () => fire('fetch_models', {
  apiUrl: $('apiUrl').value.trim(),
  apiKey: $('apiKey').value
});

$('btnSaveCfg').onclick = () => {
  send('save_config', collectCfg()).then(c => {
    S.cfg = c;
    renderSidebar();
  }).catch(e => toast('error', e.message));
};

function collectCfg() {
  /* 目录键用驼峰：C 侧 apply_config_json 只读 projectsDir / exportDir。
     exportDir 现已真正持久化（config.c 支持 export_dir），这里回传当前导出目录。 */
  const projDir = S.cfg.projectsDir || '';
  const expDir = outDir();
  /* 温度允许 0：parseFloat("0") 是 falsy，|| 0.8 会把用户明确设置的 0 吞掉 */
  const temp = parseFloat($('temperature').value);
  return {
    apiUrl: $('apiUrl').value.trim(),
    apiKey: $('apiKey').value,
    model: $('model').value.trim(),
    temperature: Number.isFinite(temp) ? temp : (S.cfg.temperature ?? 0.8),
    maxTokens: parseInt($('maxTokens').value, 10) || S.cfg.maxTokens || 8192,
    timeout: parseInt($('timeout').value, 10) || S.cfg.timeout || 120,
    rateLimitMs: Math.max(0, parseInt($('rateLimitMs').value, 10) || 0),
    maxRetries: Math.max(0, parseInt($('maxRetries').value, 10) || 0),
    enableConsistencyCheck: $('consistency').checked,
    projectsDir: projDir,
    exportDir: expDir
  };
}

$('consistency').addEventListener('change', () => {
  if ($('consistency').checked) {
    showModal('一致性检查 · 高 token 消耗', 
      '开启后，自动写作阶段 5 会对<strong>每一章</strong>调用一次 AI 做一致性检查。<br><br>' +
      '以 15 章为例，等于额外 15 次请求，token 消耗显著增加。确认开启？', [
      { label: '不开', onClick: () => { $('consistency').checked = false; } },
      { label: '确认开启', kind: 'primary', onClick: () => fire('save_config', collectCfg()) }
    ]);
  } else {
    fire('save_config', collectCfg());
  }
});

/* ---------- 未保存改动的确认 ----------
   新建 / 加载项目都会销毁当前项目。C 侧在 g.dirty 时不再直接执行，
   而是回一个 needConfirm，交给这里弹层问清楚；用户选完带 force 重发。
   刻意不用 window.confirm()：一是样式要和其它弹层一致，二是它在 WebView2 里
   是宿主级模态，容易和消息泵互相卡住。 */
function confirmDiscard(text, proceed) {
  showModal('有未保存的改动', text, [
    { label: '取消' },
    { label: '不保存', kind: 'ghost', onClick: () => proceed() },
    {
      label: '保存并继续', kind: 'primary', onClick: () => {
        send('save_project', {
          name: ($('projName').value || '').trim(),
          idea: $('ideaText').value
        })
          .then(() => proceed())
          .catch(e => toast('error', '保存失败：' + e.message));
      }
    }
  ]);
}

/* 新建项目。C 侧可能回 needConfirm，确认后带 force 重发。 */
function createProject(nm, force) {
  send('new_project', { name: nm, idea: $('ideaText').value, force: force ? 1 : 0 })
    .then(d => {
      if (d && d.needConfirm) {
        confirmDiscard('当前项目「' + esc(d.projectName || '未命名') +
          '」有还没保存的改动。新建项目会丢弃这些改动，是否先保存？',
          () => createProject(nm, true));
        return;
      }
      hideModal(true);
      toast('success', '项目已创建，去「创意」页开始吧');
      goPage('idea');
    })
    .catch(e => toast('error', e.message));
}

/* 加载项目。同样可能收到 needConfirm。 */
function loadProject(params, force) {
  const p = Object.assign({}, params, { force: force ? 1 : 0 });
  send('load_project', p).then(d => {
    if (d && d.needConfirm) {
      confirmDiscard('当前项目「' + esc(d.projectName || '未命名') +
        '」有还没保存的改动。切换项目会丢弃这些改动，是否先保存？',
        () => loadProject(params, true));
      return;
    }
    toast('info', '正在加载项目…');
  }).catch(e => toast('error', e.message));
}

$('btnNewProj').onclick = () => {
  const name = ($('projName').value || '').trim();
  showModal('新建项目',
    `<div class="form"><label class="field"><span>项目名称</span><input type="text" id="npName" value="${esc(name || '未命名项目')}"></label></div>`,
    [
      { label: '取消' },
      {
        label: '创建', kind: 'primary', keepOpen: true, onClick: () => {
          const nm = ($('npName') ? $('npName').value : '').trim();
          if (!nm) { toast('warn', '请输入项目名称'); return; }
          createProject(nm, false);
        }
      }
    ]);
};

$('btnSaveProj').onclick = () => {
  if (!S.proj.hasProject) { toast('warn', '还没有项目'); return; }
  fire('save_project', { name: $('projName').value.trim() });
};

$('btnLoadFile').onclick = () => {
  send('pick_project_file', {}, 600000).then(d => {
    if (d.path) loadProject({ path: d.path });
  }).catch(() => {});
};

$('btnPickOut').onclick = () => {
  /* 原生目录选择框可能开着很久：30 秒默认超时会在用户挑选时误报「命令超时」，
     且真实结果回来时 pending 已删被丢弃。给足 10 分钟。 */
  send('pick_folder', { initial: outDir() }, 600000).then(d => {
    if (!d || !d.path) { toast('warn', '没有选择目录'); return; }
    OUT_DIR = d.path;
    S.cfg.exportDir = d.path;
    renderSidebar();
    toast('success', '输出目录已设置');
    /* 回传 C 侧持久化（collectCfg 已包含 exportDir / export_dir / projectsDir） */
    send('save_config', collectCfg()).then(c => {
      if (c) S.cfg = c;
      OUT_DIR = d.path;
      renderSidebar();
    }).catch(() => {});
  }).catch(e => toast('error', e.message));
};

$('linkDoc').onclick = e => {
  e.preventDefault();
  const p = e.target.dataset.path;
  if (p) fire('open_path', { path: p });
};
$('linkTxt').onclick = e => {
  e.preventDefault();
  const p = e.target.dataset.path;
  if (p) fire('open_path', { path: p });
};

/* ---- 更多功能菜单 ---- */
function openMenu() {
  const btn = $('btnMore');
  const r = btn.getBoundingClientRect();
  const m = $('moreMenu');
  m.style.left = Math.max(8, r.right - 214) + 'px';
  m.style.top = (r.bottom + 7) + 'px';
  m.classList.add('is-on');
}
function closeMenu() { $('moreMenu').classList.remove('is-on'); }

$('btnMore').onclick = e => {
  e.stopPropagation();
  if ($('moreMenu').classList.contains('is-on')) closeMenu(); else openMenu();
};
document.addEventListener('click', e => {
  if (!e.target.closest('#moreMenu') && e.target.id !== 'btnMore') closeMenu();
});

$('moreMenu').addEventListener('click', e => {
  const b = e.target.closest('button');
  if (!b) return;
  closeMenu();
  const act = b.dataset.act;
  if (act === 'genChars') fire('generate_characters', {});
  else if (act === 'sumWorld') fire('summarize_worldview', {});
  else if (act === 'check') $('btnCheck').click();
  else if (act === 'finale') $('btnFinale').click();
  else if (act === 'pickOut') $('btnPickOut').click();
  else if (act === 'openOut') fire('open_path', { path: outDir() });
  else if (act === 'reveal') fire('reveal_path', { path: S.cfg.projectsDir });
  else if (act === 'importTxt') {
    send('pick_text_file', {}, 600000).then(d => {
      if (d.path) fire('import_txt', { path: d.path });
    }).catch(() => {});
  }
});

/* ---- 状态栏 ---- */
$('btnCancel').onclick = () => {
  fire('cancel', {});
  $('btnCancel').disabled = true;
};

/* ---- 主题 ---- */
/* localStorage 在 about:blank 源下会抛异常（NavigateToString 兜底时），统一包一层 */
const store = {
  get(k) { try { return localStorage.getItem(k); } catch (_) { return null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch (_) {} }
};

function applyThemeLabel() {
  $('themeLabel').textContent = document.documentElement.dataset.theme === 'dark' ? '浅色模式' : '深色模式';
}

(function initTheme() {
  const saved = store.get('novelnow-theme');
  if (saved) document.documentElement.dataset.theme = saved;
  applyThemeLabel();
})();

$('btnTheme').onclick = () => {
  const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
  document.documentElement.dataset.theme = next;
  store.set('novelnow-theme', next);
  applyThemeLabel();
};

/* ---- 快捷键 ---- */
document.addEventListener('keydown', e => {
  if (e.key === 'Escape') { closeMenu(); if ($('overlay').classList.contains('is-on')) hideModal(); }
  if (e.ctrlKey && e.key.toLowerCase() === 's') {
    e.preventDefault();
    if (S.proj.hasProject) $('btnSaveProj').click();
  }
  if (e.ctrlKey && e.key >= '1' && e.key <= '4') {
    e.preventDefault();
    goPage(['idea', 'plan', 'outline', 'settings'][+e.key - 1]);
  }
});

/* ============================================================
 * 13. 启动
 * ============================================================ */
if (bridge) {
  /* 握手期的前几条报文写进宿主日志：白屏时先看这几行，能立刻判断是
     「C -> JS 没投递」还是「前端逻辑抛错」。稳定后自动闭嘴，避免长任务刷屏。
     ★ 只记类型与 id，不记正文——hello 回执里含 API Key。 */
  let recvLogged = 0;
  bridge.addEventListener('message', ev => {
    let m = ev.data;
    if (typeof m === 'string') { try { m = JSON.parse(m); } catch (_) { uiLog('[recv] JSON 解析失败'); return; } }
    if (recvLogged < 8 && m) {
      recvLogged++;
      uiLog('[recv] ' + (m.type === 'event'
        ? ('event ' + m.name)
        : ('result ' + m.cmd + ' id=' + m.id + ' ok=' + m.ok)));
    }
    /* 宿主确认视图可见（可见性自愈完成）：现在发报文才不会被丢，开始握手。 */
    if (m && m.type === 'event' && m.name === 'view_ready') startHandshake();
    try { onMessage(m); } catch (err) { uiLog('[recv] onMessage 抛异常 ' + (err && err.message)); }
  });

  /* ★ 握手时机是这套程序最脆的一环。
     宿主的 WebView2 controller 若在窗口 show 之前创建，IsVisible 会被锁死在
     FALSE；这时渲染器把整个页面当成不可见，于是
       - 宿主发给网页的报文被静默丢弃：PostWebMessageAsJson 返回 S_OK，
         网页却什么都收不到（既不 resolve 也不 reject）；
       - 网页里的 setTimeout / setInterval 全部冻结。
     症状就是「界面起来了，但所有按钮都没反应」，而且时好时坏。
     宿主已改成无条件做可见性自愈，就绪后推一条 view_ready。
     这里等它再握手；另加超时兜底与回执重发，任何一环失灵都不会让界面
     永远卡在「未连接」。 */
  let handshaked = false;
  let helloTries = 0;
  function startHandshake() {
    if (handshaked) return;
    helloTries++;
    send('hello', {}, 10000).then(d => {
      handshaked = true;
      S.cfg = d.config || {};
      S.proj = d.project || S.proj;
      S.selDirection = (S.proj.selectedDirection != null) ? S.proj.selectedDirection : -1;
      S.selPlan = (S.proj.selectedPlan != null) ? S.proj.selectedPlan : -1;
      S.selChapter = (S.proj.selectedChapter != null) ? S.proj.selectedChapter : -1;
      renderAll(true);
      setBusy(!!d.busy);
      setStatus('就绪 · 桥接已连通');
      uiLog('桥接就绪：预设 ' + presetList().length + ' 组，当前匹配 '
            + (currentPreset() ? currentPreset().id : '自定义')
            + '，章节 ' + ((S.proj.chapters || []).length));
      /* 握手自检：hello 走的是 JS -> C，这里补一次 ping 证明 C -> JS 也通。
         C 侧收到 ping 会打一条「闭环确认」日志，排查白屏时先看它。 */
      send('ping', {}).catch(() => {});
      if (!S.cfg.apiKey) {
        toast('info', '还没有配置 API Key，去「设置」页填一下就能用了');
      }
    }).catch(e => {
      if (handshaked) return;  /* 并发重发可能各自超时，只报一次 */
      handshaked = true;   /* 明确失败就不再重发，交给错误提示 */
      uiLog('[hello 失败] ' + (e && e.stack ? e.stack : String(e)));
      setStatus('桥接失败：' + e.message, true);
      toast('error', '与宿主通信失败：' + e.message);
    });
    /* 回执还可能没送到（可见性刚就绪、报文仍在路上）：隔一会儿重发。
       hello 是幂等的，重复发没有副作用；最多 6 次。 */
    if (helloTries < 6) setTimeout(() => { if (!handshaked) startHandshake(); }, 800);
  }
  /* 兜底：万一 view_ready 没来（自愈失败或宿主版本变化），也必须尝试一次 */
  setTimeout(startHandshake, 1500);
} else {
  /* 直接在浏览器里打开时，给一份演示数据，方便单独调样式 */
  setStatus('未连接到宿主（浏览器预览模式）', false);
  S.proj = {
    hasProject: true, name: '示例项目', stage: 'writing', totalWords: 18420,
    selectedDirection: 0, selectedPlan: 0,
    directions: [
      { index: 0, title: '记忆的回声', description: '一个能听见他人记忆的少女，在不停重启的城市里寻找丢失的那一天。', selected: true },
      { index: 1, title: '第九次黎明', description: '每次黎明城市都会重置，只有她记得前八次发生过什么。' }
    ],
    plans: [
      { index: 0, title: '回声之城', synopsis: '少女林晚在第七次重启的清晨醒来，发现自己的记忆里多了一个从未存在过的人。', outline: '第一章 重启的清晨\n第二章 不存在的名字\n第三章 钟楼的守夜人', selected: true }
    ],
    chapters: [
      { index: 0, title: '重启的清晨', summary: '林晚在熟悉的闹钟声里醒来。', wordCount: 3120, status: 2 },
      { index: 1, title: '不存在的名字', summary: '记忆里浮现一个从未见过的人。', wordCount: 2860, status: 1 },
      { index: 2, title: '钟楼的守夜人', summary: '她第一次爬上钟楼。', wordCount: 0, status: 0 }
    ],
    characters: [
      { name: '林晚', role: '主角', description: '十七岁，能听见他人记忆。', traits: '沉默、执拗', background: '孤儿院长大。' }
    ],
    world: { worldType: '都市奇幻', setting: '一座每七天重启一次的城市', powerSystem: '记忆感知' },
    checks: [],
    foreshadows: []
  };
  /* 浏览器直开（无桥接）时的演示数据。预设以 C 侧 g_presets 为准，此处仅作镜像 */
  S.cfg = { apiUrl: 'https://api.deepseek.com', apiKey: '', model: 'deepseek-flash',
            temperature: 0.8, maxTokens: 8192, timeout: 120, projectsDir: 'D:\\...\\projects',
            presets: [
              { id: 'deepseek', name: 'DeepSeek', url: 'https://api.deepseek.com',
                models: ['deepseek-flash', 'deepseek-v4-pro', 'deepseek-v4-flash'] },
              { id: 'gpt', name: 'OpenAI · GPT', url: 'https://api.openai.com/v1',
                models: ['gpt-5.6', 'gpt-5.2', 'gpt-5.1', 'gpt-5', 'gpt-5-mini', 'gpt-4.1', 'gpt-4o-mini'] },
              { id: 'glm', name: '智谱 · GLM', url: 'https://open.bigmodel.cn/api/paas/v4',
                models: ['glm-5', 'glm-4.7', 'glm-4.6', 'glm-4.5-air', 'glm-4.7-flashx'] },
              { id: 'seed', name: '字节 · 豆包 Seed', url: 'https://ark.cn-beijing.volces.com/api/v3',
                models: ['doubao-seed-1.6', 'doubao-seed-1.6-thinking', 'doubao-seed-2-1-pro-260628'] },
              { id: 'mimo', name: '小米 · MiMo', url: 'https://api.xiaomimimo.com/v1',
                models: ['mimo-v2-flash', 'mimo-v2-pro', 'mimo-v2-omni'] },
              { id: 'qwen', name: '阿里 · 通义千问', url: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
                models: ['qwen3.7-plus', 'qwen3.8-max', 'qwen-plus', 'qwen3.5-plus', 'qwen3.7-flash', 'qwen-flash'] }
            ] };
  renderAll(false);
}
