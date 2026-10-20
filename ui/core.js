/* core.js - bridge + send/fire + message dispatch (onMessage/handleTaskDone/askChapterReview) (split from app.js 2026-10-18, zero behavior change; loaded in original order) */
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
        /* 增量计字：每帧整篇 replace 重算对长章是 O(n²)（P1）。只数新增片段；
           基准长度对不上（换章/手工编辑/task_done 刷新过）才重算一次兜底 */
        const before = ta.value;
        if (S._wcLen !== before.length) S._wc = before.replace(/\s/g, '').length;
        S._wcLen = before.length + d.delta.length;
        S._wc += d.delta.replace(/\s/g, '').length;
        ta.value += d.delta;
        ta.scrollTop = ta.scrollHeight;
        $('chWc').textContent = num(S._wc) + ' 字 · 生成中…';
      }
      break;

    case 'text_delta':
      /* 通用流式增量（方向/方案/大纲/一致性检查）：浮动面板逐字刷。
         galgame 场景流式期间改道到剧情舞台；L1 企划/L2 大纲是 JSON 元数据，
         只静默丢弃（结果走 task_done 全文），别把裸 JSON 刷进流式面板。 */
      /* typeof G 守卫：纯小说版没有 gal.js，全按通用流式走 */
      if (typeof G !== 'undefined' && G.awaiting === 'scene') galStream(d.delta);
      else if (typeof G !== 'undefined' && G.awaiting) { /* L1/L2：静默 */ }
      else streamFeed(d.delta);
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

    case 'comfy_done':
      /* ComfyUI 配图回执（探活/生图完成）：不占忙闲位，只管铺背景与提示 */
      if (typeof comfyOnDone === 'function') comfyOnDone(d);
      break;

    case 'ask':
      askChapterReview(d);
      break;

    case 'shutdown':
      break;
  }
}

function handleTaskDone(d) {
  /* 通用 AI 调用（galgame 管线）：结果走 d.text（typeof G：纯小说版无 gal.js） */
  if (typeof G !== 'undefined' && d.cmd === 'ai_call') { galOnDone(d); return; }
  /* 兜底：单任务互斥下 aiCall 挂着时不可能有别的 task_done 到达；
     到了就说明 aiCall 已被取消/顶掉——收掉防 Promise 悬挂 */
  if (typeof G !== 'undefined' && G.cb) {
    const prev = G.cb; G.cb = null;
    G.awaiting = null;
    prev.reject(new Error('任务已结束（' + (d.cmd || '') + '）'));
  }
  /* 切换项目后恢复剧情分支状态 */
  if (d.cmd === 'load_project' && typeof galLoad === 'function') galLoad();

  /* 项目结构变化的命令：刷新章节列表等 */
  if (d.cmd === 'fetch_models' && d.ok && d.models) {
    const dl = $('modelList');
    if (!dl) return;
    dl.innerHTML = d.models.map(m => `<option value="${esc(m)}">`).join('');
    toast('success', '已获取 ' + d.models.length + ' 个模型，点「模型」输入框可见下拉');
    return;
  }

  /* 章节/方案数量变了，重取一次完整快照最稳妥 */
  if (['gen_directions', 'gen_plans', 'expand_outline', 'load_project',
       'generate_characters', 'summarize_worldview', 'analyze_text', 'import_txt',
       'check_finale', 'step_write', 'set_characters', 'answer'].includes(d.cmd)) {
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

