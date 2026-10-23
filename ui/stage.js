/* ============================================================
 * stage.js — 舞台演出引擎（形似 WebGAL，自研实现，不借用其代码）
 *
 * 演出指令流（由 gal.js 的剧本解析产生，格式见 galParseScript）：
 *   {t:'say',   who, text}        角色台词（打字机 + 名字牌）
 *   {t:'narr',  text}             旁白（无名字牌，斜体）
 *   {t:'intro', text}             黑幕字幕（| 分行，逐行淡入）
 *   {t:'bg',    key, desc}        换背景（key=舞台图片\<key>.png；desc=占位描述）
 *   {t:'fig',   who, pos, on}     立绘上/退场（pos: left|center|right）
 *   {t:'bgm',   key}              音乐（舞台图片\bgm\<key>；''=停）
 *
 * 资源契约（C 侧 stage_asset，host_cmds.c）：
 *   stage_asset {kind:'bg'|'figures'|'bgm', name} → {data: 'data:...'}
 *   资源缺失是常态（用户还没生成图）：一律静默降级——渐变底/名字占位/静音，
 *   绝不打断演出（红线：没有图也要能看）。
 * ============================================================ */

const STAGE = {
  script: [], idx: 0, typing: false, timer: null,
  onEnd: null, active: false,
  auto: false, autoTimer: null,
  assets: {},          /* 'kind:name' -> data URL | null（null=确认未就绪） */
  bgSeq: 0,            /* 背景指令序号：异步回包按序判废 */
  speakSeq: 0,         /* 换装指令序号：说话图异步回包防插队 */
  bgTop: null,         /* crossfade 用：当前最上层 bg layer */
  bgKey: '',           /* 当前背景资源键（comfyApplyBg 对账用） */
  who: '', audio: null, bgmKey: ''
};

const STAGE_TYPE_MS = 30;      /* 打字机节奏，与旧 GAL_PLAY 一致 */
const STAGE_AUTO_MS = 1200;    /* 自动模式：每句演完停留 */

/* ---------- 资源（stage_asset 前端缓存层） ---------- */
function stageAsset(kind, name) {
  const k = kind + ':' + name;
  if (Object.prototype.hasOwnProperty.call(STAGE.assets, k)) {
    return Promise.resolve(STAGE.assets[k]);
  }
  return send('stage_asset', { kind, name }, 60000).then(d => {
    const url = (d && d.data) ? d.data : null;
    STAGE.assets[k] = url;
    return url;
  }).catch(() => {
    STAGE.assets[k] = null;   /* 未就绪也记账，别每次都白拉一轮 */
    return null;
  });
}

/* 资源刚生成好（comfy_done 等）：作废缓存让下次重拉 */
function stageAssetInvalidate(kind, name) {
  delete STAGE.assets[kind + ':' + name];
}

/* ---------- 背景（双层 crossfade） ---------- */
function stageSetBg(key, desc) {
  /* STAGE.bgKey 记账：comfyApplyBg 要拿它判断「是不是舞台当前背景」 */
  STAGE.bgKey = key || '';
  const bgs = $('stageBgs');
  if (!bgs) return;
  /* latest-wins：资源是异步拉取的，连发两条背景指令时慢回来的那条不许插队 */
  const seq = ++STAGE.bgSeq;
  if (!key) {
    stagePushBgLayer(null, desc || '');   /* 无图：渐变占位 + 场景描述 */
    return;
  }
  stageAsset('bg', key + '.png').then(url => {
    if (seq !== STAGE.bgSeq) return;      /* 期间又切了背景，本次直接作废 */
    stagePushBgLayer(url || null, desc || '');
    uiLog('[stage] bg ' + key + (url ? ' 已铺' : ' 未就绪，用占位'));
  });
}

function stagePushBgLayer(url, desc) {
  const bgs = $('stageBgs');
  if (!bgs) return;
  const layer = document.createElement('div');
  layer.className = 'stage-bg-layer' + (url ? ' has-img' : ' is-placeholder');
  if (url) layer.style.backgroundImage = 'url(' + url + ')';
  else layer.textContent = desc || '';
  bgs.appendChild(layer);
  /* 下一帧把旧层淡出、新层淡入；旧层动画完就摘掉，最多同时两层 */
  requestAnimationFrame(() => { layer.classList.add('is-on'); });
  const olds = bgs.querySelectorAll('.stage-bg-layer');
  for (let i = 0; i < olds.length - 1; i++) {
    const old = olds[i];
    old.classList.remove('is-on');
    setTimeout(() => { if (old.parentNode) old.parentNode.removeChild(old); }, 700);
  }
  STAGE.bgTop = layer;
}

/* ---------- 立绘（三槽位：左/中/右） ----------
 * key=资源名（DSL 的 :图名），缺省用人物名；on=false 时按人物名从任意槽位撤下 */
function stageSetFigure(who, pos, key, on) {
  const name = String(who || '');
  if (!on) {
    document.querySelectorAll('#stageFigs .stage-fig-slot').forEach(s => {
      if (s.dataset.who === name) {
        s.classList.remove('is-on');
        setTimeout(() => { if (!s.classList.contains('is-on')) s.innerHTML = ''; }, 400);
      }
    });
    return;
  }
  const slot = document.querySelector('#stageFigs .stage-fig-slot[data-pos="' + pos + '"]');
  if (!slot) return;
  stageAsset('figures', (key || name) + '.png').then(url => {
    slot.innerHTML = '';
    const holder = document.createElement('div');
    holder.className = 'stage-fig-body';
    if (url) {
      const img = document.createElement('img');
      img.alt = name;
      img.src = url;
      holder.appendChild(img);
    } else {
      /* 没立绘资源：名字首字剪影占位，槽位照样亮（没图也要能演） */
      holder.classList.add('is-ph');
      const glyph = document.createElement('span');
      glyph.className = 'stage-fig-glyph';
      glyph.textContent = name.slice(0, 1) || '?';
      holder.appendChild(glyph);
    }
    slot.appendChild(holder);
    slot.dataset.who = name;
    /* 说话两态换装记账：key=图名（说话图找 <key>_talk.png），standSrc=站立图回程票 */
    slot.dataset.key = key || name;
    delete slot.dataset.talking;
    if (url) slot.dataset.standSrc = url; else delete slot.dataset.standSrc;
    requestAnimationFrame(() => slot.classList.add('is-on'));
    uiLog('[stage] fig ' + name + ' @' + pos + (url ? '' : '（占位）'));
  });
}

/* 说话人高亮 + 两态换装：当前说话人的槽位亮、换「说话图」（<key>_talk.png），
   其他压暗、回「站立图」。没说话图就保持站图（静默降级，红线不打断演出）。 */
function stageHighlight(who) {
  STAGE.who = who || '';
  const seq = ++STAGE.speakSeq;
  document.querySelectorAll('#stageFigs .stage-fig-slot').forEach(s => {
    s.classList.toggle('is-dim', !!(who && s.dataset.who && s.dataset.who !== who));
    const img = s.querySelector('img');
    const key = s.dataset.key || s.dataset.who || '';
    if (!img || !key) return;   /* 剪影占位没图可换 */
    if (who && s.dataset.who === who) {
      stageAsset('figures', key + '_talk.png').then(url => {
        /* 迟到的换装不许插队：序号过期/人已换岗就作废 */
        if (seq !== STAGE.speakSeq || s.dataset.who !== who) return;
        if (url) {
          if (!s.dataset.standSrc) s.dataset.standSrc = img.src;
          img.src = url;
          s.dataset.talking = '1';
          uiLog('[stage] speak ' + who + ' → ' + key + '_talk');
        }
      });
    } else if (s.dataset.talking) {
      delete s.dataset.talking;
      img.src = s.dataset.standSrc || img.src;
    }
  });
}

/* ---------- 对话框（打字机） ---------- */
function stageHint(t) {
  const el = $('stageHint');
  if (el) el.textContent = t || '';
}

function stageShowName(who) {
  const el = $('stageName');
  if (!el) return;
  if (who) { el.textContent = who; el.hidden = false; }
  else { el.textContent = ''; el.hidden = true; }
}

function stageTypeLine(who, text, isNarr, onDone) {
  const box = $('galText');
  if (!box) { if (onDone) onDone(); return; }
  stageShowName(who);
  const dlg = $('stageDialog');
  if (dlg) dlg.classList.toggle('is-narration', !!isNarr);
  stageHighlight(isNarr ? '' : (who || ''));
  box.innerHTML = '';
  const line = document.createElement('div');
  line.className = 'stage-line';
  box.appendChild(line);
  const full = String(text || '');
  STAGE.typing = true;
  stageHint('');
  let shown = 0;
  let done = false;
  if (STAGE.timer) { clearInterval(STAGE.timer); STAGE.timer = null; }
  const finish = () => {
    if (done) return;          /* 防双响：打字机自然收尾 + 点击补完可能撞车 */
    done = true;
    if (STAGE.timer) { clearInterval(STAGE.timer); STAGE.timer = null; }
    STAGE.typing = false;
    line.textContent = full;
    stageHint('点击继续 ▸');
    if (onDone) onDone();
  };
  if (!full.length) { finish(); return; }
  STAGE.timer = setInterval(() => {
    shown += 1;
    line.textContent = full.slice(0, shown);
    if (shown >= full.length) finish();
  }, STAGE_TYPE_MS);
  STAGE.finishTyping = finish;   /* advance() 补完整句用 */
}

/* ---------- 黑幕字幕 ---------- */
function stageIntro(text, onDone) {
  const el = $('stageIntro');
  if (!el) { if (onDone) onDone(); return; }
  el.hidden = false;
  el.classList.add('is-on');
  el.innerHTML = '';
  const lines = String(text || '').split('|');
  lines.forEach(t => {
    const d = document.createElement('div');
    d.className = 'stage-intro-line';
    d.textContent = t;
    el.appendChild(d);
  });
  stageHint('点击继续 ▸');
  STAGE.typing = false;
  STAGE.introDone = () => {
    el.classList.remove('is-on');
    setTimeout(() => { el.hidden = true; }, 500);
    if (onDone) onDone();
  };
}

/* ---------- BGM ---------- */
function stageSetBgm(key) {
  if (STAGE.bgmKey === key) return;
  STAGE.bgmKey = key || '';
  if (STAGE.audio) { STAGE.audio.pause(); STAGE.audio = null; }
  if (!key) return;
  stageAsset('bgm', key).then(url => {
    if (!url || STAGE.bgmKey !== key) return;   /* 拉取期间又切了就不播 */
    try {
      const a = new Audio(url);
      a.loop = true;
      a.volume = 0.5;
      a.play().catch(() => { uiLog('[stage] bgm 被浏览器拦了，等下一次点击再播'); });
      STAGE.audio = a;
      uiLog('[stage] bgm ' + key + ' 播放中');
    } catch (e) { uiLog('[stage] bgm 播放失败: ' + (e && e.message)); }
  });
}

/* ---------- 播放器主循环 ---------- */
function stageStep() {
  if (!STAGE.active) return;
  const ins = STAGE.script[STAGE.idx];
  if (!ins) {
    const cb = STAGE.onEnd;
    STAGE.active = false;
    STAGE.onEnd = null;
    if (cb) cb();
    return;
  }
  /* 指令型（bg/fig/bgm）不占用「一句」，自动滑到下一条 */
  if (ins.t === 'bg') { STAGE.idx++; stageSetBg(ins.key, ins.desc); stageStep(); return; }
  if (ins.t === 'fig') { STAGE.idx++; stageSetFigure(ins.who, ins.pos, ins.key, ins.on); stageStep(); return; }
  if (ins.t === 'bgm') { STAGE.idx++; stageSetBgm(ins.key || ''); stageStep(); return; }
  if (ins.t === 'intro') {
    STAGE.idx++;
    stageIntro(ins.text, () => {
      stageHint('点击继续 ▸');
      STAGE.pendingNext = () => { STAGE.pendingNext = null; stageStep(); };
    });
    return;
  }
  /* say / narr：占「一句」，演完等推进 */
  STAGE.idx++;
  stageTypeLine(ins.who || '', ins.text || '', ins.t === 'narr', () => {
    if (STAGE.auto) {
      if (STAGE.autoTimer) clearTimeout(STAGE.autoTimer);
      STAGE.autoTimer = setTimeout(() => {
        if (STAGE.active && !STAGE.typing) stageStep();
      }, STAGE_AUTO_MS);
    }
  });
}

/* ============ 舞台 DSL 解析器（SinkNow 演出脚本 → 指令流） ============
   每行一条：【@背景】key（描述）/【@立绘】人物:左|中|右[:图名]/【@立绘】人物:退
   /【@黑幕】文本|分行/【@音乐】音频名/【说话人】台词/【旁白】叙述。
   从 gal.js galParseScript 收编而来（galgame 玩法退役，解析器是演出层资产）。 */
function stageParseScript(text) {
  const out = [];
  String(text || '').split(/\r?\n/).forEach(raw => {
    const s = raw.replace(/^\s+|\s+$/g, '');
    if (!s) return;
    const m = s.match(/^[【\[]([^】\]]{1,12})[】\]]\s*(.*)$/);
    const head = m ? m[1].replace(/^\s+|\s+$/g, '') : '';
    const body = m ? m[2] : s;
    if (head === '@背景') {
      const mm = body.match(/^(.*?)\s*[（(](.*?)[）)]\s*$/) || [];
      out.push({ t: 'bg', key: (mm[1] || body).trim(), desc: mm[2] || '' });
      return;
    }
    if (head === '@立绘') {
      const parts = body.split(':');
      const who = (parts[0] || '').replace(/^\s+|\s+$/g, '');
      const posRaw = (parts[1] || '').replace(/^\s+|\s+$/g, '');
      if (!who || !posRaw) return;
      if (posRaw === '退' || posRaw === 'off') { out.push({ t: 'fig', who, pos: '', key: '', on: false }); return; }
      const pos = posRaw === '左' ? 'left' : posRaw === '右' ? 'right' : 'center';
      out.push({ t: 'fig', who, pos, key: (parts[2] || '').replace(/^\s+|\s+$/g, '') || who, on: true });
      return;
    }
    if (head === '@黑幕') { out.push({ t: 'intro', text: body }); return; }
    if (head === '@音乐') { out.push({ t: 'bgm', key: body.replace(/^\s+|\s+$/g, '') }); return; }
    if (head && head.charAt(0) === '@') return;   /* 未知 @ 指令：跳过，别当台词念 */
    out.push(head ? { t: 'say', who: head, text: body } : { t: 'narr', text: s });
  });
  return out;
}

function stagePlay(script, onEnd) {
  stageStopTimers();
  STAGE.script = script || [];
  STAGE.idx = 0;
  STAGE.onEnd = onEnd || null;
  STAGE.active = true;
  STAGE.pendingNext = null;
  STAGE.introDone = null;    /* 上一节黑幕没点完就换节：陈旧回调不许吞推进 */
  STAGE.finishTyping = null;
  stageStep();
}

function stageStopTimers() {
  if (STAGE.timer) { clearInterval(STAGE.timer); STAGE.timer = null; }
  if (STAGE.autoTimer) { clearTimeout(STAGE.autoTimer); STAGE.autoTimer = null; }
  STAGE.typing = false;
}

function stageStop() {
  stageStopTimers();
  STAGE.active = false;
  STAGE.onEnd = null;
  STAGE.pendingNext = null;
  STAGE.introDone = null;
  STAGE.finishTyping = null;
}

/* 清台：导入新小说时清掉上一部的舞台残留（背景/立绘/黑幕/名字/BGM） */
function stageClear() {
  stageStop();
  const bgs = $('stageBgs'); if (bgs) bgs.innerHTML = '';
  const figs = $('stageFigs');
  if (figs) figs.querySelectorAll('.stage-fig-slot').forEach(s => {
    s.innerHTML = ''; s.classList.remove('is-on', 'is-dim'); delete s.dataset.who;
    delete s.dataset.key; delete s.dataset.standSrc; delete s.dataset.talking;
  });
  const intro = $('stageIntro');
  if (intro) { intro.hidden = true; intro.classList.remove('is-on'); intro.innerHTML = ''; }
  stageShowName('');
  const box = $('galText');
  if (box) box.innerHTML = '<div class="gal-empty">把 TXT 小说丢进来，SinkNow 把它变成故事。</div>';
  stageHint('');
  stageSetBgm('');
  STAGE.who = '';
}

/* 推进：打字中→补完整句；黑幕→下一行/关闭；完了→下一条 */
function stageAdvance() {
  if (!STAGE.active) return;
  if (STAGE.introDone) {
    const f = STAGE.introDone;
    STAGE.introDone = null;
    f();
    return;
  }
  if (STAGE.typing) {
    if (STAGE.finishTyping) STAGE.finishTyping();
    return;
  }
  if (STAGE.pendingNext) { const f = STAGE.pendingNext; f(); return; }
  stageStep();
}

/* ---------- 选项（WebGAL 式：舞台中央大按钮） ---------- */
function stageRenderChoices(choices, onPick) {
  const box = $('galChoices');
  if (!box) return;
  box.innerHTML = '';
  (choices || []).forEach((c, i) => {
    const b = document.createElement('button');
    b.className = 'stage-choice';
    b.type = 'button';
    b.textContent = c.label || ('选项 ' + (i + 1));
    b.onclick = (e) => {
      e.stopPropagation();      /* 别冒泡到舞台点击推进 */
      box.innerHTML = '';
      if (onPick) onPick(i);
    };
    box.appendChild(b);
  });
}

/* ---------- 生成中占位 ---------- */
function stageSetGenerating(text) {
  if (STAGE.active) return;     /* 演出中不许盖舞台 */
  const box = $('galText');
  if (box && !box.querySelector('.gal-typing')) {
    box.innerHTML = '<div class="gal-typing">' + esc(String(text || '剧本生成中…')) + '</div>';
  }
}

/* ---------- 绑定：点击/空格推进、自动开关 ---------- */
(function stageBind() {
  const view = $('stageView');
  if (view) view.addEventListener('click', stageAdvance);
  document.addEventListener('keydown', (e) => {
    if (!STAGE.active) return;
    const tag = (e.target && e.target.tagName) || '';
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
    if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); stageAdvance(); }
  });
  const auto = $('stageBtnAuto');
  if (auto) auto.onclick = (e) => {
    e.stopPropagation();
    STAGE.auto = !STAGE.auto;
    auto.classList.toggle('is-on', STAGE.auto);
    if (STAGE.auto && STAGE.active && !STAGE.typing && !STAGE.introDone && !STAGE.pendingNext) stageStep();
  };
})();

/* 启动自检 + 全局 JS 错误上报（前端报错不吞，进 webui.log 留证）。
   能打出「engine ready」说明 stage.js 解析成功；gal.js 加载后另打一行，
   两行都在才算整条演出链路的代码都活着。 */
window.addEventListener('error', (e) => {
  uiLog('[js-error] ' + (e && e.message ? e.message : '?') + ' @' + (e && e.filename ? e.filename : '?') + ':' + (e && e.lineno ? e.lineno : '?'));
});
uiLog('[stage] engine ready');
