/* actions.js - event bindings + project actions + theme + startup/handshake (split from app.js 2026-10-18, zero behavior change; loaded in original order) */
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
  /* 用 send 直连：fire 吞掉失败后 .then 照跑，会红绿 toast 同现 */
  send('select_direction', { index: S.selDirection }).then(() => {
    refreshProject(false);
    toast('success', '方向已选定，点「生成小说方案」继续');
  }).catch(e => toast('error', e.message || String(e)));
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
  /* 用 send 直连：fire 吞掉失败后 .then 照跑，会红绿 toast 同现 */
  send('select_plan', { index: S.selPlan }).then(() => {
    refreshProject(false);
    toast('success', '方案已选定，点「细化大纲」继续');
  }).catch(e => toast('error', e.message || String(e)));
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
  showModal('批量扩写', '将依次扩写全部章节。<br><br>「跳过已完成」只补写空白/未达标章节，绝不碰写好的稿子；「全部重写」会覆盖所有章节。<br><br>任务期间可以点右下角「取消任务」中断。', [
    { label: '取消' },
    { label: '开始（跳过已完成）', kind: 'primary', onClick: () => fire('expand_all', { words: parseInt($('words').value, 10) || 2000, force: 0 }) },
    { label: '全部重写', kind: 'ghost', onClick: () => fire('expand_all', { words: parseInt($('words').value, 10) || 2000, force: 1 }) }
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
  /* C 侧 export 只读 "dir"（host_cmds.c:900 p_str(root,"dir",NULL)），多发废键只会
     让两边键名契约越来越糊（P1：曾经三键齐发 dir/path/exportDir） */
  send('export', { dir: dir }).then(d => {
    S.cfg.lastDoc = d.doc || '';
    S.cfg.lastTxt = d.txt || '';
    S.cfg.lastEpub = d.epub || '';
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

/* 人物编辑：点列表项打开弹层；点列表空白不许误开（P1） */
$('charList').addEventListener('click', e => {
  if (e.target.closest('.item')) openCharEditor();
});

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
    comfyUrl: $('comfyUrl') ? $('comfyUrl').value.trim() : (S.cfg.comfyUrl || ''),
    comfyWorkflowPath: $('comfyWfPath') ? $('comfyWfPath').value.trim() : (S.cfg.comfyWorkflowPath || ''),
    comfyAutoBg: $('comfyAutoBg') ? $('comfyAutoBg').checked : !!S.cfg.comfyAutoBg,
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
$('linkEpub').onclick = e => {
  e.preventDefault();
  const p = e.target.dataset.path;
  if (p) fire('open_path', { path: p });
};

/* ---- 支线 C：Token 用量展示 ---- */
function fmtTok(n) {
  n = Number(n) || 0;
  if (n >= 1e6) return (n / 1e6).toFixed(1) + 'M';
  if (n >= 1e4) return (n / 1e3).toFixed(1) + 'k';
  return String(n);
}
function refreshTokens() {
  const el = $('tokenUse');
  if (!el) return;
  send('token_stats').then(d => {
    if (!d) return;
    el.textContent = '本次 ' + fmtTok(d.sessionTotal) + ' · 累计 ' + fmtTok(d.total);
  }).catch(() => {});
}
refreshTokens();   /* 启动先来一发；之后每次 task_done 由 core.js 刷新 */

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
  /* 同步收掉在飞的 aiCall：取消后 task_done 的 cmd 不一定是 ai_call，
     不手动 reject 就永久悬挂（typeof G：纯小说版无 gal.js） */
  if (typeof G !== 'undefined') {
    if (G.cb) { const prev = G.cb; G.cb = null; prev.reject(new Error('已取消')); }
    G.awaiting = null;
  }
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
  if (e.ctrlKey && e.key >= '1' && e.key <= '5') {
    e.preventDefault();
    goPage(['idea', 'plan', 'outline', 'galnow', 'settings'][+e.key - 1]);
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
  let helloInFlight = false;   /* 并发守卫：view_ready + 兜底 + 重发可能撞车 */
  function startHandshake() {
    if (handshaked || helloInFlight) return;
    helloInFlight = true;
    helloTries++;
    send('hello', {}, 10000).then(d => {
      helloInFlight = false;
      if (handshaked) return;
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
      if (typeof galLoad === 'function') galLoad();   /* 恢复剧情分支状态（项目里存过才有） */
      if (!S.cfg.apiKey) {
        toast('info', '还没有配置 API Key，去「设置」页填一下就能用了');
      }
    }).catch(e => {
      helloInFlight = false;
      if (handshaked) return;
      /* 超时可能只是报文还在路上：失败先重试，6 次耗尽才放弃 */
      uiLog('[hello 失败 第 ' + helloTries + ' 次] ' + (e && e.stack ? e.stack : String(e)));
      if (helloTries >= 6) {
        handshaked = true;
        setStatus('桥接失败：' + e.message, true);
        toast('error', '与宿主通信失败：' + e.message);
      }
    });
    /* 回执还可能没送到（可见性刚就绪、报文仍在路上）：隔一会儿重发。
       hello 是幂等的，重复发没有副作用；最多 6 次。 */
    if (helloTries < 6) setTimeout(function helloRetry() {
      if (handshaked) return;
      if (helloInFlight) { setTimeout(helloRetry, 800); return; }  /* 没发出去要接着等 */
      startHandshake();
    }, 800);
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
