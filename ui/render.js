/* render.js - widgets (toast/modal/stream) + page rendering (split from app.js 2026-10-18, zero behavior change; loaded in original order) */
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
  setLink('linkEpub', S.cfg.lastEpub, 'EPUB');
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

let chapterReq = 0;   /* selectChapter 请求序号：声明在此，防隐式全局（严格模式/多脚本会炸） */
function selectChapter(idx) {
  S.selChapter = idx;
  document.querySelectorAll('#chList .item').forEach(x =>
    x.classList.toggle('is-sel', parseInt(x.dataset.ch, 10) === idx));
  $('btnExpandCh').disabled = idx < 0;
  fire('set_selected_chapter', { index: idx });
  if (idx < 0) return;
  /* 请求序号：点了 B 又快速点 C，B 的迟到响应不能盖掉 C 的内容 */
  const myReq = ++chapterReq;
  send('get_chapter', { index: idx }).then(d => {
    if (myReq !== chapterReq) return;
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
    if (S.proj) S.proj.characters = clean;   /* 本地快照同步，防下次渲染跳回旧数据 */
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
  /* 配图工坊（ComfyUI）三件套 */
  set('comfyUrl', c.comfyUrl || 'http://127.0.0.1:8188');
  set('comfyWfPath', c.comfyWorkflowPath || '');
  if ($('comfyAutoBg') && (!document.activeElement || document.activeElement.id !== 'comfyAutoBg')) {
    $('comfyAutoBg').checked = !!c.comfyAutoBg;
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

