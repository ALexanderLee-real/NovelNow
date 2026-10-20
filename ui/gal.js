/* gal.js - galgame branch engine (state G + prompt pipeline) (split from app.js 2026-10-18, zero behavior change; loaded in original order) */
/* ============================================================
 * 11.5 剧情分支（galgame 实时定制）
 * 多提示词管线：L0 高概念 → L1 企划 → L2 路线大纲 → L3 场景（流式）→ L4 选项
 * C 侧只提供通用 ai_call（流式 + task_done 回全文），编排全在这一层。
 * ============================================================ */

const G = {
  idea: '', plan: null, outline: null,
  nodes: {},          /* id -> {id, title, text, hook, choices, chosen, from, toOutline} */
  current: '',        /* 当前节点 id（自有顺序 id：n1、n2…） */
  outlinePos: 0,      /* 大纲游标：当前场景对应大纲第几个节点（跳线点用 to 推进） */
  log: [],            /* [{from, choice, to}] 分支历史 */
  flags: { favor: 0, trust: 0, sanity: 0, shards: 0 },
  bools: {},          /* 布尔旗标（≤12） */
  awaiting: null,     /* 'plan' | 'outline' | 'scene' —— 决定 text_delta 改道 */
  cb: null            /* aiCall 的 {resolve, reject} */
};

const GAL_MARKER = '<<<SCENE_JSON>>>';

function aiCall(system, user) {
  return new Promise((resolve, reject) => {
    /* 单槽 cb + token：上一次还没兑现就被新请求顶掉时，先把旧 Promise 收掉，
       否则 await 它的地方永远挂起；token 保证过期回调只清自己的槽 */
    if (G.cb) {
      const prev = G.cb; G.cb = null;
      prev.reject(new Error('AI 调用被新的请求取代'));
    }
    const my = { resolve, reject };
    G.cb = my;
    /* 先回 accepted 立刻 resolve，真正的结果在 task_done（d.text）里。
       600s 超时是防真死锁的兜底，不能用默认 30s 误杀慢响应 */
    send('ai_call', { system, user }, 600000).catch(e => {
      if (G.cb === my) G.cb = null;
      G.awaiting = null;   /* 失败后别让残留 'scene' 把 delta 灌进舞台 */
      reject(e);
    });
  });
}

function galOnDone(d) {
  const cb = G.cb; G.cb = null;
  G.awaiting = null;
  if (!cb) return;
  if (d.ok) cb.resolve(d.text || '');
  else cb.reject(new Error(d.message || 'AI 调用失败'));
}

/* ============ 对话演出引擎 ============
   剧本行格式：【说话人】台词 /【旁白】描写（行首无【】也按旁白）。
   演出：逐句打字机 + 说话人名牌 + 点击/空格推进，整节演完才出选项。
   不再像小说一样整篇流式铺开（用户点名要 galgame 对话体）。 */
const GAL_PLAY = { lines: [], idx: 0, shown: 0, typing: false, timer: null, onEnd: null, active: false };

function galParseScript(text) {
  const out = [];
  String(text || '').split(/\r?\n/).forEach(raw => {
    const s = raw.replace(/^\s+|\s+$/g, '');
    if (!s) return;
    const m = s.match(/^[【\[]([^】\]]{1,12})[】\]]\s*(.*)$/);
    out.push(m ? { who: m[1].replace(/^\s+|\s+$/g, ''), text: m[2] } : { who: '', text: s });
  });
  return out;
}

function galPlayStop() {
  if (GAL_PLAY.timer) { clearInterval(GAL_PLAY.timer); GAL_PLAY.timer = null; }
  GAL_PLAY.typing = false;
  GAL_PLAY.active = false;
}

function galPlayLine() {
  const line = GAL_PLAY.lines[GAL_PLAY.idx];
  $('galNodeInfo').textContent = line.who || '旁白';
  const box = $('galText');
  box.classList.toggle('is-narration', !line.who);
  box.innerHTML = '<div class="gal-line"></div><div class="gal-hint"></div>';
  const el = box.querySelector('.gal-line');
  const hint = box.querySelector('.gal-hint');
  const full = String(line.text || '');
  GAL_PLAY.shown = 0;
  GAL_PLAY.typing = true;
  if (GAL_PLAY.timer) clearInterval(GAL_PLAY.timer);
  GAL_PLAY.timer = setInterval(() => {
    GAL_PLAY.shown += 1;
    el.innerHTML = esc(full.slice(0, GAL_PLAY.shown)) + '<span class="gal-caret">▍</span>';
    if (GAL_PLAY.shown >= full.length) {
      clearInterval(GAL_PLAY.timer); GAL_PLAY.timer = null;
      GAL_PLAY.typing = false;
      el.innerHTML = esc(full);
      hint.textContent = '点击继续 ▸';
    }
  }, 30);
  if (!full.length) {
    clearInterval(GAL_PLAY.timer); GAL_PLAY.timer = null;
    GAL_PLAY.typing = false;
    hint.textContent = '点击继续 ▸';
  }
}

function galAdvance() {
  if (!GAL_PLAY.active) return;
  const box = $('galText');
  if (GAL_PLAY.typing) {
    /* 第一下点击：立刻显示整句（跳过打字机） */
    if (GAL_PLAY.timer) { clearInterval(GAL_PLAY.timer); GAL_PLAY.timer = null; }
    GAL_PLAY.typing = false;
    const line = GAL_PLAY.lines[GAL_PLAY.idx];
    const el = box.querySelector('.gal-line');
    if (el) el.innerHTML = esc(line.text || '');
    const hint = box.querySelector('.gal-hint');
    if (hint) hint.textContent = '点击继续 ▸';
    return;
  }
  if (GAL_PLAY.idx < GAL_PLAY.lines.length - 1) {
    GAL_PLAY.idx += 1;
    galPlayLine();
  } else {
    const cb = GAL_PLAY.onEnd;
    galPlayStop();
    if (cb) cb();
  }
}

function galPlayNode(node, onEnd) {
  galPlayStop();
  $('galChoices').innerHTML = '';
  $('galBtnNext').disabled = true;
  const lines = galParseScript(node && node.text);
  if (!lines.length) {
    galRenderNodeText(node && node.text);   /* 空/异常文本走静态兜底 */
    if (onEnd) onEnd();
    return;
  }
  GAL_PLAY.lines = lines;
  GAL_PLAY.idx = 0;
  GAL_PLAY.onEnd = onEnd || null;
  GAL_PLAY.active = true;
  galPlayLine();
}

/* 静态兜底渲染（空文本/异常时） */
function galRenderNodeText(text) {
  $('galText').innerHTML = '<div class="gal-para">' + esc(String(text || '')).replace(/\n{2,}/g, '</div><div class="gal-para">').replace(/\n/g, '<br>') + '</div>';
}

/* 生成中：只提示进度，正文等剧本到手后逐句播出（不整篇流铺） */
function galStream(delta) {
  if (!delta) return;
  /* 演出进行中：新 delta 不许盖舞台（P1） */
  if (GAL_PLAY && GAL_PLAY.active) return;
  const box = $('galText');
  if (box && !box.querySelector('.gal-typing')) {
    box.innerHTML = '<div class="gal-typing">剧本生成中…</div>';
  }
}

function galSetPhase(t) { $('galPhase').textContent = t; }

function galRenderFlags() {
  $('galFavor').textContent = G.flags.favor;
  $('galTrust').textContent = G.flags.trust;
  $('galSanity').textContent = G.flags.sanity;
  $('galShards').textContent = G.flags.shards;
  const keys = Object.keys(G.bools).filter(k => G.bools[k]);
  $('galBools').innerHTML = keys.length
    ? keys.slice(0, 12).map(k => '<span class="gal-chip">' + esc(k) + '</span>').join('')
    : '<span class="hint">尚无旗标</span>';
}

function galRenderTree() {
  const ids = Object.keys(G.nodes);
  $('galTreeInfo').textContent = ids.length + ' 节点';
  if (!ids.length) { $('galTree').innerHTML = '<span class="hint">尚无节点</span>'; return; }
  $('galTree').innerHTML = ids.map(id => {
    const n = G.nodes[id];
    const cur = id === G.current ? ' is-cur' : '';
    return '<div class="gal-node' + cur + '" data-node="' + esc(id) + '">' +
      '<b>' + esc(n.title || id) + '</b>' +
      (n.chosen ? '<span class="gal-chip">' + esc(n.chosen) + '</span>' : '') +
      '</div>';
  }).join('');
  $('galTree').querySelectorAll('.gal-node').forEach(el => {
    el.onclick = () => {
      const n = G.nodes[el.dataset.node];
      if (!n) return;
      /* 树上点看旧节点也走对话演出；只有看的正是当前节点才收尾出选项 */
      const isCur = (n.id === G.current);
      galPlayNode(n, () => {
        if (isCur) {
          if (n.choices && n.choices.length) galRenderChoices(n);
          else $('galBtnNext').disabled = false;
        } else {
          /* 看的是旧节点：按钮状态还原成当前节点的 */
          const cur = G.nodes[G.current];
          $('galBtnNext').disabled = !!(cur && cur.choices && cur.choices.length);
        }
      });
    };
  });
}

function galRenderChoices(node) {
  const box = $('galChoices');
  box.innerHTML = '';
  if (!node || !node.choices || !node.choices.length) return;
  node.choices.forEach((c, i) => {
    const b = document.createElement('button');
    b.className = 'btn ghost gal-choice';
    b.textContent = c.label || ('选项 ' + (i + 1));
    b.onclick = (e) => { e.stopPropagation(); galChoose(node, i); };   /* 别冒泡到舞台点击推进 */
    box.appendChild(b);
  });
}

function galApplyFx(fx) {
  if (!fx) return;
  ['favor', 'trust', 'sanity', 'shards'].forEach(k => {
    if (typeof fx[k] === 'number') G.flags[k] = (G.flags[k] || 0) + fx[k];
  });
  (fx.flags || []).forEach(f => { G.bools[f] = true; });
  (fx.clear || []).forEach(f => { delete G.bools[f]; });
}

function galSave() {
  /* 全量状态落盘到 project.json 的 gal 键；存完顺手 save_project——
     gal 状态只活在 project.json 里，不自动存就只剩内存一份 */
  const json = JSON.stringify({
    idea: G.idea, plan: G.plan, outline: G.outline,
    nodes: G.nodes, current: G.current, outlinePos: G.outlinePos, log: G.log,
    flags: G.flags, bools: G.bools
  });
  send('set_gal', { json }).then(() => fire('save_project'))
    .catch(e => toast('error', e.message || String(e)));
}

function galLoad() {
  /* 切项目：先斩断在飞的旧 ai_call，否则旧节点兑付后会 galSave 进新项目（跨项目污染） */
  if (G.cb) { const prev = G.cb; G.cb = null; prev.reject(new Error('项目已切换，剧情生成已中断')); }
  G.awaiting = null;
  send('get_gal').then(r => {
    let s = null;
    try { s = typeof r.gal === 'string' ? JSON.parse(r.gal) : r.gal; } catch (e) { s = null; }
    if (!s || typeof s !== 'object') return;
    G.idea = s.idea || '';
    G.plan = s.plan || null;
    G.outline = s.outline || null;
    G.nodes = s.nodes || {};
    G.current = s.current || '';
    G.outlinePos = typeof s.outlinePos === 'number' ? s.outlinePos : 0;
    G.log = s.log || [];
    G.flags = Object.assign({ favor: 0, trust: 0, sanity: 0, shards: 0 }, s.flags || {});
    G.bools = s.bools || {};
    if (G.idea) $('galIdea').value = G.idea;
    galRenderFlags();
    galRenderTree();
    const n = G.nodes[G.current];
    if (n) {
      $('galNodeInfo').textContent = n.title || n.id;
      galPlayNode(n, () => {
        /* 有选项时禁「下一节」防重复节点 */
        if (!n.choices || !n.choices.length) $('galBtnNext').disabled = false;
        else { $('galBtnNext').disabled = true; galRenderChoices(n); }
      });
      galSetPhase('已恢复：' + (n.title || n.id));
      comfyApplyBg(comfyCurKey());   /* 恢复舞台背景（若已生成） */
    } else if (G.plan) {
      galSetPhase('企划已就绪');
      $('galBtnNext').disabled = false;
    }
  }).catch(() => {});
}

/* ---- L1 企划 ---- */
const GAL_SYS_PLAN =
  '你是资深 galgame 企划师。根据用户的高概念输出企划 JSON（不要任何多余文字，只输出 JSON）：\n' +
  '{"highConcept":"一句话高概念","world":"世界观 200 字内","characters":[{"name":"","role":"","trait":""}],\n' +
  '"routes":[{"id":"r1","title":"","tone":"","endings":{"good":"","bad":"","true":""}}],\n' +
  '"foreshadow":[{"id":"f1","desc":"","plantAt":"","resolveAt":""}],\n' +
  '"truth":{"shardsNeeded":3,"desc":""}}\n' +
  '硬性规则：3-5 条路线，每线好/坏/真 3 结局 + 1 个总真结局（真相碎片集齐解锁）；\n' +
  '伏笔每条必须标注埋设与回收位置；数值 flag 只用好感/信任/理智三个 + 真相碎片计数器；\n' +
  '软分支重汇率 ≥70%，硬分支每章 ≤2 个、深度 ≤2 层，伪选择占 30-40%。';

/* ---- L2 路线大纲 ---- */
function galSysOutline() {
  return '你是 galgame 路线设计师。根据企划为「主干 + 跳线点」结构输出大纲 JSON（只输出 JSON）：\n' +
  '{"nodes":[{"id":"c01s01","title":"","beat":"开场钩子|日常|伏笔|爆发|结局分支","desc":"50 字内","branchTo":{"选项效果示例":"c01s03a"}}]}\n' +
  '规则：10-15 个节点；节拍按 开场钩子(0-500字悬念)→日常 3-5 节→伏笔每节≥1→爆发点在 35% 与 70%→结局分支 85-95%；\n' +
  '节点 id 用 cNNsNN 格式；分支是跳转不是复制文本；每节末尾留 1 个悬念钩子。';
}

/* ---- L3 场景 + L4 选项 ---- */
function galSysScene() {
  return '你是 galgame 场景写手。写出可直接逐句演出的对话剧本（' + (parseInt($('galWords').value, 10) || 1000) + ' 字左右）。\n' +
    '格式：每行一句台词，句首用【说话人】标注名字；旁白/动作/景物描写用【旁白】开头。\n' +
    '示例：\n【旁白】夜色沉了下来，走廊的灯忽明忽暗。\n【小雨】你……怎么会在这里？\n【我】我只是路过。\n' +
    '要求：台词口语化、有情绪、有潜台词；对话:旁白≈70:30；节末留悬念钩子。\n' +
    '剧本写完后，另起一行输出 ' + GAL_MARKER + ' 然后输出选项 JSON（只输出 JSON）：\n' +
    '{"hook":"节末悬念句","title":"本节短标题","choices":[{"label":"","kind":"plot|affection|fake","fx":{"favor":0,"trust":0,"sanity":0,"shards":0,"flags":[""]},"cond":{"favor>=2":"","flag":""},"to":"下一节点 id"}],\n' +
    '"foreshadow":["f1 埋设/回收于 cN"]}\n' +
    '规则：选项 2-4 个，其中伪选择（kind=fake，怎么选都重汇）占 30-40%；解锁条件只用阈值+布尔两段式；\n' +
    'fx 数值增量很小（±1~2）；to 填大纲里顺理成章的下一个节点 id。';
}

function galUserContext() {
  const p = G.plan || {};
  const out = G.outline ? (G.outline.nodes || []) : [];
  const curIdx = Math.max(0, Math.min(G.outlinePos, Math.max(0, out.length - 1)));
  const win = out.slice(Math.max(0, curIdx - 1), curIdx + 2);
  const recent = G.log.slice(-3);
  return '【企划】\n' + JSON.stringify(p).slice(0, 2000) +
    '\n【大纲当前位置及相邻节点】\n' + JSON.stringify(win) +
    '\n【flag 快照】\n' + JSON.stringify(G.flags) + ' 布尔旗标:' + JSON.stringify(G.bools) +
    '\n【最近选择】\n' + JSON.stringify(recent) +
    (G.current && G.nodes[G.current] ? '\n【上一节结尾 300 字】\n' + G.nodes[G.current].text.slice(-300) : '') +
    '\n【用户想法】\n' + G.idea;
}

function galParseJson(raw) {
  let t = String(raw).trim();
  t = t.replace(/^```(?:json)?\s*/i, '').replace(/```\s*$/, '');
  const a = t.indexOf('{');
  if (a < 0) throw new Error('模型未返回 JSON');
  /* 从右往左收缩重试：AI 常在 JSON 后多吐尾巴，或截断了最后一个对象 */
  let last = t.lastIndexOf('}');
  while (last > a) {
    try { return JSON.parse(t.slice(a, last + 1)); } catch (e) { /* 收缩重试 */ }
    last = t.lastIndexOf('}', last - 1);
  }
  throw new Error('模型未返回合法 JSON');
}

async function galGenPlan() {
  G.idea = $('galIdea').value.trim();
  if (!G.idea) { toast('warn', '请先写下你的想法'); $('galIdea').focus(); return; }
  if (!S.proj.hasProject) { toast('warn', '请先在「设置」页新建项目'); return; }
  if (!S.cfg.apiKey && !(S.cfg.ai && S.cfg.ai.apiKey)) { toast('warn', '请先在「设置」页配置 API Key'); goPage('settings'); return; }
  setBusy(true);
  galSetPhase('L1 企划生成中…');
  try {
    G.awaiting = 'plan';
    const raw = await aiCall(GAL_SYS_PLAN, '用户想法：' + G.idea + '\n路线数：' + (parseInt($('galRoutes').value, 10) || 3));
    G.plan = galParseJson(raw);
    G.outline = null; G.nodes = {}; G.current = ''; G.outlinePos = 0; G.log = [];
    G.flags = { favor: 0, trust: 0, sanity: 0, shards: 0 }; G.bools = {};
    galSetPhase('L2 路线大纲生成中…');
    G.awaiting = 'outline';
    const raw2 = await aiCall(galSysOutline(), '企划：\n' + JSON.stringify(G.plan).slice(0, 3000) + '\n用户想法：' + G.idea);
    G.outline = galParseJson(raw2);
    galRenderFlags(); galRenderTree();
    galSave();
    toast('success', '企划与大纲就绪，开始演出');
    await galNextNode(true);
  } catch (e) {
    toast('error', e.message || String(e));
    galSetPhase('生成失败');
  } finally { setBusy(false); }
}

async function galNextNode(first, nextHint) {
  if (!G.plan) { toast('warn', '请先生成企划'); return; }
  setBusy(true);
  G.awaiting = 'scene';
  $('galChoices').innerHTML = '';
  $('galBtnNext').disabled = true;
  galSetPhase(first ? '开场演出中…' : 'L3 场景生成中…');
  try {
    const raw = await aiCall(galSysScene(), galUserContext());
    const cut = raw.indexOf(GAL_MARKER);
    const text = (cut >= 0 ? raw.slice(0, cut) : raw).replace(/^\s+|\s+$/g, '');
    let meta = { hook: '', title: '', choices: [], foreshadow: [] };
    if (cut >= 0) {
      try { meta = Object.assign(meta, galParseJson(raw.slice(cut + GAL_MARKER.length))); }
      catch (e) { uiLog('[gal] 选项 JSON 解析失败: ' + (e && e.message)); toast('error', '剧情选项解析失败，已按无选项继续'); }
    }
    /* 节点身份用自有顺序 id：选项的 to 是「跳转目标」不是身份——
       否则选了 to=c01s02 的选项，下一个节点就叫 c01s02，把上一个同名节点覆盖掉。
       to 只用来推进大纲游标（跳线点）。 */
    const seq = Object.keys(G.nodes).length + 1;
    const nextId = 'n' + seq;
    const out = (G.outline && G.outline.nodes) ? G.outline.nodes : [];
    const hintIdx = out.findIndex(n => n.id === (nextHint || ''));
    if (hintIdx >= 0) G.outlinePos = hintIdx;
    else if (!first) G.outlinePos = Math.min(G.outlinePos + 1, Math.max(0, out.length - 1));
    const node = {
      id: nextId, title: meta.title || ('节点 ' + seq),
      text, hook: meta.hook || '', choices: meta.choices || [], chosen: '',
      from: G.current,
      toOutline: nextHint || (meta.choices[0] && meta.choices[0].to) || ''
    };
    G.nodes[nextId] = node;
    G.current = nextId;
    G.awaiting = null;
    /* 对话演出：逐句播完再出选项（继续/选项都等演完） */
    galPlayNode(node, () => {
      if (node.choices.length) galRenderChoices(node);
      else $('galBtnNext').disabled = false;
    });
    galRenderTree();
    galSave();
    /* 配图工坊接线：本节若有预生成的背景图就铺上；
       「自动生成背景」开着时，提前把下一节的图排进 ComfyUI 队列 */
    comfyApplyBg(comfyCurKey());
    comfyPreGen();
    return true;
  } catch (e) {
    toast('error', e.message || String(e));
    galSetPhase('演出失败');
    $('galBtnNext').disabled = false;
    return false;
  } finally { G.awaiting = null; setBusy(false); }
}

async function galChoose(node, i) {
  const c = node.choices[i];
  if (!c) return;
  /* 先留快照：galNextNode 失败时回滚，选项才不会被白白消费（选了就没了） */
  const snap = {
    chosen: node.chosen, logLen: G.log.length,
    flags: Object.assign({}, G.flags), bools: Object.assign({}, G.bools)
  };
  galApplyFx(c.fx);
  node.chosen = c.label || ('选项 ' + (i + 1));
  G.log.push({ from: node.id, choice: node.chosen, to: c.to || '' });
  galRenderFlags();
  galRenderTree();
  $('galChoices').innerHTML = '';
  const ok = await galNextNode(false, c.to || '');
  if (!ok) {
    node.chosen = snap.chosen;
    G.log.length = snap.logLen;
    G.flags = snap.flags; G.bools = snap.bools;
    galRenderFlags();
    galRenderTree();
    if (node.choices && node.choices.length) galRenderChoices(node);
    toast('warn', '剧情生成失败，选项已还原，可重选');
  }
}

/* ---- gal 页绑定 ---- */
$('galBtnStart').onclick = galGenPlan;
$('galBtnNext').onclick = () => galNextNode(false);
/* 点击舞台推进台词（选项按钮已 stopPropagation） */
$('galStage').addEventListener('click', galAdvance);
/* 空格/回车推进台词；在输入框里打字时不劫持 */
document.addEventListener('keydown', (e) => {
  if (!GAL_PLAY.active) return;
  const tag = (e.target && e.target.tagName) || '';
  if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
  if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); galAdvance(); }
});
$('galBtnReset').onclick = () => {
  galPlayStop();
  G.plan = null; G.outline = null; G.nodes = {}; G.current = ''; G.outlinePos = 0; G.log = [];
  G.flags = { favor: 0, trust: 0, sanity: 0, shards: 0 }; G.bools = {};
  $('galText').innerHTML = '<div class="gal-empty">尚未开始。写下想法，点「生成企划并开局」。</div>';
  $('galChoices').innerHTML = '';
  const st = $('galStage');
  if (st) { st.style.backgroundImage = ''; st.classList.remove('has-bg'); }   /* 清舞台背景 */
  $('galNodeInfo').textContent = '—';
  $('galBtnNext').disabled = true;
  galRenderFlags(); galRenderTree();
  galSetPhase('未开始');
  galSave();
};

