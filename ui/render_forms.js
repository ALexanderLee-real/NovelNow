/* render_forms.js - 人物编辑器/设置/预设/renderAll/refreshProject（T5 拆分自 render.js，零行为变化） */
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

