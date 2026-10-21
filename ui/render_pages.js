/* render_pages.js - 页面渲染：侧栏/项目列表/想法/方案/大纲/世界/人物/伏笔（T5 拆分自 render.js，零行为变化） */
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

