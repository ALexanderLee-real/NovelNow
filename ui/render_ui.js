/* render_ui.js - 小部件（toast/modal/stream）+ 导航（T5 拆分自 render.js，零行为变化；加载序 = 原 render.js 位置） */
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
  /* 防覆盖：已有 pinned 弹层（逐章询问）在等回话时，新弹层会顶掉它的
     onDismiss，那个 askId 就永远没人应答 → C 侧 WaitForSingleObject 永久卡死。
     顶掉前先走等价动作（= 回 'cancel' 放行 C 侧）。 */
  if (modalPinned && modalDismiss) hideModal();
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

/* GalNow 子页切换（剧情分支 / 配图工坊）——只切子页签，不动 goPage 主导航 */
function goGalPane(name) {
  document.querySelectorAll('#galnowTabs .galnow-tab').forEach(b =>
    b.classList.toggle('is-active', b.dataset.pane === name));
  document.querySelectorAll('.page[data-page="galnow"] .galnow-pane').forEach(p =>
    p.classList.toggle('is-active', p.dataset.pane === name));
}
document.addEventListener('click', e => {
  const t = e.target.closest && e.target.closest('#galnowTabs .galnow-tab');
  if (t) goGalPane(t.dataset.pane);
});

function goPage(name) {
  if (name === page) return;
  /* 空页守卫：目标页不存在（如纯小说版没有 galnow 页时按 Ctrl+4）→ 回退 idea，防全灭空白 */
  if (!document.querySelector('.page[data-page="' + name + '"]')) name = 'idea';
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

