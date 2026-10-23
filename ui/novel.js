/* novel.js - SinkNow 小说演出管线（galgame 玩法退役后的主演）（2026-10-22）
 * ============================================================
 * 用户流程（m02819）：丢 TXT 小说进来 → AI 总结剧情+人物 → 逐章扒演出脚本
 * → 人物立绘生成（要说话的生成说话图，动作不大的用站立图）→ 左右对话+场景按需
 * → 纯净式看小说（把小说变成故事）。
 * 分层：C 侧只给原料命令（import_novel/set_play_script/get_novel/set_novel/
 * comfy_gen(kind=figures|bg)），编排全在这一层；舞台执行在 stage.js。
 * ============================================================ */

const N = {
  chapters: [],      /* [{index,title,content,playScript}]（正文只在按章取时拿） */
  analysis: null,    /* {summary, chars:[{name,look,pose,prompt}]} */
  cur: 0,            /* 当前章下标 */
  awaiting: null,    /* 'analyze' | 'extract' —— aiCall 结果路由 */
  cb: null,          /* aiCall 单槽 */
  playing: false,
  auto: false        /* 自动连播（一章演完自动下一章） */
};

/* ---- 通用 AI 单槽调用（原 gal.js aiCall 模式，换主人） ---- */
function novelAiCall(system, user) {
  return new Promise((resolve, reject) => {
    if (N.cb) {
      const prev = N.cb; N.cb = null;
      prev.reject(new Error('AI 调用被新的请求取代'));
    }
    const my = { resolve, reject };
    N.cb = my;
    send('ai_call', { system, user }, 600000).catch(e => {
      if (N.cb === my) { N.cb = null; reject(e); }
    });
  });
}

function novelOnDone(d) {
  const my = N.cb;
  N.cb = null;
  if (!my) return;
  if (d.ok && d.text) my.resolve(d.text);
  else my.reject(new Error(d.error || 'AI 调用失败'));
}

function novelAiCancel() {
  if (N.cb) {
    const prev = N.cb; N.cb = null;
    prev.reject(new Error('已取消'));
  }
  N.awaiting = null;
}

/* ---- 第一步：丢 TXT 进来（C 侧切章建档） ---- */
async function novelImport() {
  let picked;
  /* 文件对话框是模态的、等人点的：默认 30s 超时会把还在挑文件的用户判死
     （实测 32s 回来发现回执被丢、静默断链）——与 actions.js/comfy.js 同规给 600s */
  try { picked = await send('pick_text_file', {}, 600000); }
  catch (e) { toast('warn', '取消选择文件'); return; }
  const path = picked && picked.path;
  if (!path) return;
  setBusy(true);
  setStatus('正在导入小说…');
  try {
    const d = await send('import_novel', { path: path });
    /* send 解析给的是回执 data 本体（错已 reject）：{chapters, project} */
    const chs = (d && d.project && d.project.chapters) || [];
    N.chapters = chs.map((c, i) => ({ index: i, title: c.title, content: '', playScript: '' }));
    N.cur = 0;
    N.analysis = null;
    renderNovel();
    toast('success', '小说已导入（' + (d.chapters || chs.length) + ' 章）');
    setStatus('已导入：' + (d.project && d.project.name || '') + ' —— 点「AI 扒剧情+人物」继续');
  } catch (e) {
    toast('error', '导入失败：' + e.message);
  } finally {
    setBusy(false);
    renderSidebar();
  }
}

/* ---- 第二步：AI 总结剧情 + 人物定妆（形象描述它自己写） ---- */
const NOVEL_SYS_ANALYZE =
  '你是小说改编导演。用户给你一篇小说的开头，你要输出严格 JSON（不要任何多余文字、不要 markdown 代码块）：\n' +
  '{"summary":"全书剧情走向概括（200字内）","chars":[{"name":"人物名","look":"外貌定妆描述（发型/服装/气质，50字内）","pose":"stand|talk","prompt":"适合画成全身立绘的英文绘画提示词（发型/服装/气质/表情，纯色背景；只写形象定妆，不要写动作姿态）"}]}\n' +
  '规则：出场且有台词的人物全列，最多 8 个；pose 有台词的用 talk（会生成站立+说话两态立绘），只被提到/动作少的用 stand（只生成站立图）；prompt 尽量具体统一（同一人物多章形象要固定）。';

async function novelAnalyze() {
  if (!N.chapters.length) { toast('warn', '先导入小说'); return; }
  /* 剧情概括需要上下文：取前 3 章正文（每章截 2500 字）+ 最后 1 章开头 */
  const parts = [];
  for (let i = 0; i < Math.min(3, N.chapters.length); i++) {
    const d = await send('get_chapter', { index: i }).catch(() => null);
    if (d && d.content) parts.push('【' + (d.title || ('第' + (i + 1) + '章')) + '】\n' +
      d.content.slice(0, 2500));
    N.chapters[i].content = (d && d.content) || '';
    N.chapters[i].playScript = (d && d.playScript) || '';
  }
  const tail = await send('get_chapter', { index: N.chapters.length - 1 }).catch(() => null);
  if (tail && tail.content && N.chapters.length > 3) {
    parts.push('【结尾一章开头】\n' + tail.content.slice(0, 1200));
    N.chapters[N.chapters.length - 1].content = tail.content;
    N.chapters[N.chapters.length - 1].playScript = tail.playScript || '';
  }
  if (!parts.length) { toast('error', '读不到章节正文'); return; }
  setBusy(true);
  setStatus('AI 正在扒剧情和人物…');
  N.awaiting = 'analyze';
  try {
    const text = await novelAiCall(NOVEL_SYS_ANALYZE, parts.join('\n\n'));
    N.awaiting = null;
    const obj = parseLooseJson(text);
    if (!obj || !obj.chars) throw new Error('AI 没回出合法 JSON');
    N.analysis = {
      summary: String(obj.summary || ''),
      chars: (obj.chars || []).slice(0, 8).map(c => ({
        name: String(c.name || '').trim(),
        look: String(c.look || ''),
        pose: c.pose === 'talk' ? 'talk' : 'stand',
        prompt: String(c.prompt || c.look || '')
      })).filter(c => c.name)
    };
    await send('set_novel', { json: JSON.stringify(N.analysis) }).catch(() => {});
    renderNovel();
    toast('success', '剧情与人物扒好（' + N.analysis.chars.length + ' 人）');
    setStatus('人物定妆完成 —— 点「生成人物立绘」或「扒演出并开演」');
  } catch (e) {
    N.awaiting = null;
    toast('error', '分析失败：' + e.message);
  } finally {
    setBusy(false);
  }
}

/* 宽容 JSON 解析：剥代码块围栏/前后杂音（模型爱包 markdown） */
function parseLooseJson(text) {
  if (!text) return null;
  let s = String(text).trim();
  const fence = s.match(/```(?:json)?\s*([\s\S]*?)```/);
  if (fence) s = fence[1];
  const a = s.indexOf('{'), b = s.lastIndexOf('}');
  if (a < 0 || b <= a) return null;
  try { return JSON.parse(s.slice(a, b + 1)); }
  catch (e) {
    try { return JSON.parse(s.slice(a, b + 1).replace(/,\s*([}\]])/g, '$1')); }
    catch (e2) { return null; }
  }
}

/* ---- 第三步：人物立绘生成（ComfyUI；没配置就跳过，舞台有剪影兜底） ---- */
async function novelGenFigures() {
  if (!N.analysis || !N.analysis.chars.length) { toast('warn', '先「AI 扒剧情+人物」'); return; }
  /* ComfyUI 要配齐地址+工作流才算真能出图；缺一样就整步跳过（舞台剪影兜底），
     别对着空工作流发请求吃一排「请先选择工作流文件」（P1） */
  const url = (S.cfg && S.cfg.comfyUrl) || '';
  const wf = (S.cfg && S.cfg.comfyWorkflowPath) || '';
  if (!url || !wf) { toast('warn', 'ComfyUI 未配齐（地址/工作流，配图工坊页），立绘跳过（舞台用剪影）'); return; }
  let n = 0;
  for (const c of N.analysis.chars) {
    /* 站立图全员都有：<name>.png。姿态后缀拼在定妆 prompt 后面（形象由 prompt 锁定，两态只差动作） */
    try {
      await send('comfy_gen', {
        node: 'fig_' + (n + 1),
        name: c.name,
        kind: 'figures',
        prompt: ((c.prompt || c.look || '') + ', standing naturally, full body, relaxed, neutral expression').trim()
      });
      n++;
    } catch (e) { uiLog('[novel] 立绘请求失败 ' + c.name + '：' + e.message); }
    /* 说话两态：有台词的人补一张 <name>_talk.png，舞台在他开口时自动换上（stageHighlight 联动） */
    if (c.pose === 'talk') {
      try {
        await send('comfy_gen', {
          node: 'fig_' + (n + 1),
          name: c.name + '_talk',
          kind: 'figures',
          prompt: ((c.prompt || c.look || '') + ', talking, mouth open, expressive gesture, looking at viewer').trim()
        });
        n++;
      } catch (e) { uiLog('[novel] 说话图请求失败 ' + c.name + '：' + e.message); }
    }
  }
  toast('success', '已发 ' + n + ' 个立绘生成请求（后台出图，出来自动上舞台）');
}

/* ---- 第四步：逐章扒演出脚本（DSL 喂 stage.js） ---- */
const NOVEL_SYS_EXTRACT =
  '你是舞台演出编剧。给你小说的一章正文和人物表，把这一章改写成舞台演出脚本。\n' +
  '输出纯文本（不要 JSON、不要解释），每行一条指令：\n' +
  '【@背景】场景键（一句场景描述）——场景键用拼音或英文小写下划线，全章 2-4 个；\n' +
  '【@立绘】人物名:左 或 人物名:右 ——人物上台站位（对话一边一个）；人物名:退 下台；\n' +
  '【人物名】台词原文（保留原文对白，不要改写）；\n' +
  '【旁白】叙述/动作描写（压缩成短句，一行一条）；\n' +
  '【@黑幕】过场字幕（章节跳跃时才用，多行用 | 分隔）。\n' +
  '节奏：一节 2-5 条演出指令；对话与旁白交替；场景键首次出现时先给【@背景】再给台词。';

async function novelExtract(idx, silent) {
  if (!N.chapters.length) return null;
  const meta = N.chapters[idx];
  if (!meta) return null;
  let content = meta.content;
  if (!content) {
    const d = await send('get_chapter', { index: idx }).catch(() => null);
    if (d) {
      content = d.content || '';
      meta.content = content;
      meta.playScript = d.playScript || '';
    }
  }
  if (!content) return null;
  if (meta.playScript && !silent) return meta.playScript;   /* 扒过就不重扒 */
  if (meta.playScript && silent) return meta.playScript;
  const chars = (N.analysis && N.analysis.chars || [])
    .map(c => c.name + '（' + c.look + '）').join('；');
  const user = '【人物表】' + (chars || '（未分析，自行从正文推断人物）') +
    '\n\n【本章正文】\n' + content.slice(0, 6000);
  setBusy(true);
  if (!silent) setStatus('AI 正在扒第 ' + (idx + 1) + ' 章的演出…');
  N.awaiting = 'extract';
  try {
    const script = await novelAiCall(NOVEL_SYS_EXTRACT, user);
    N.awaiting = null;
    meta.playScript = script;
    await send('set_play_script', { index: idx, script: script }).catch(() => {});
    return script;
  } catch (e) {
    N.awaiting = null;
    if (!silent) toast('error', '扒演出失败：' + e.message);
    return null;
  } finally {
    setBusy(false);
  }
}

/* ---- 场景按需生成：剧本里的【@背景】键没图就发 comfy_gen（kind=bg） ---- */
function novelGenScenes(script) {
  /* 同 novelGenFigures：地址+工作流齐了才发；缺配舞台渐变占位照样演 */
  const url = (S.cfg && S.cfg.comfyUrl) || '';
  const wf = (S.cfg && S.cfg.comfyWorkflowPath) || '';
  if (!url || !wf) return;
  const seen = {};
  (script || []).forEach(ins => {
    if (ins.t !== 'bg' || !ins.key || seen[ins.key]) return;
    seen[ins.key] = 1;
    const fname = ins.key + '.png';
    /* 没图才发：图在盘上就别重画（真 ComfyUI 一张几十秒，回放一遍全重画太奢侈）。
       探测前先作废缓存，防「首次探测没图」的空缓存毒化后来的判定。 */
    stageAssetInvalidate('bg', fname);
    stageAsset('bg', fname).then(got => {
      if (got) { uiLog('[novel] 场景图已有，跳过生成 ' + ins.key); return; }
      /* node=ASCII 事件键（comfy_done 对账）；name=落盘文件名（须与舞台 stageAsset('bg', key+'.png') 对得上） */
      send('comfy_gen', {
        node: ins.key.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 40) || 'scene',
        name: ins.key,
        prompt: ins.desc || ins.key,
        kind: 'bg'
      }).catch(e => uiLog('[novel] 场景图请求失败 ' + ins.key + '：' + (e.message || e)));
    });
  });
}

/* ---- 播放：纯净式看小说 ---- */
async function novelPlay(idx) {
  if (!N.chapters.length) { toast('warn', '先导入小说'); return; }
  if (idx == null) idx = N.cur;
  if (idx < 0 || idx >= N.chapters.length) return;
  N.cur = idx;
  renderNovel();
  const script = await novelExtract(idx);
  if (!script) {
    const meta = N.chapters[idx];
    /* 扒不出演出也要能看原文——静态旁白兜底 */
    const text = meta.content || '';
    if (text) {
      const lines = text.split(/\r?\n/).filter(l => l.trim()).slice(0, 40)
        .map(l => ({ t: 'narr', text: l }));
      stagePlay(lines, novelOnChapterEnd);
      N.playing = true;
    } else {
      toast('warn', '本章没有内容');
    }
    return;
  }
  const ins = stageParseScript(script);
  if (!ins.length) {
    toast('warn', '演出脚本为空，直接读原文');
    return;
  }
  novelGenScenes(ins);   /* 场景按需生成：有键没图先占位，出图自动铺（comfyOnDone→stageSetBg） */
  N.playing = true;
  stagePlay(ins, novelOnChapterEnd);
}

function novelOnChapterEnd() {
  N.playing = false;
  if (N.auto && N.cur < N.chapters.length - 1) {
    setTimeout(() => novelPlay(N.cur + 1), 600);
  } else {
    setStatus('本章演完（' + (N.chapters[N.cur] && N.chapters[N.cur].title || '') + '）');
  }
}

function novelStop() {
  stageStop();
  N.playing = false;
}

/* ---- 恢复（切项目/握手后） ---- */
async function novelRestore() {
  try {
    /* 章节列表自己拉快照，别等 S.proj 投喂：load_project 的 task_done 先到，
       refreshProject 的 get_project 回包后到——novelRestore 跑在中间就是吃冷饭，
       重启后 SinkNow 章节为 0、开演拒绝干活的真凶（E2E 实测抓到） */
    const p = await send('get_project').catch(() => null);
    const proj = (p && p.chapters) ? p : S.proj;
    if (proj && proj.chapters) {
      N.chapters = proj.chapters.map((c, i) => ({
        index: i, title: c.title, content: '', playScript: ''
      }));
    } else {
      N.chapters = [];
    }
    N.cur = 0;
    N.playing = false;
    const nv = await send('get_novel').catch(() => null);
    if (nv && nv.novel && nv.novel !== '{}') {
      try { N.analysis = JSON.parse(nv.novel); } catch (e) { N.analysis = null; }
    } else {
      N.analysis = null;
    }
    renderNovel();
  } catch (e) { /* 恢复失败静默：没项目就是没项目 */ }
}

/* ---- 渲染：章节列表 + 人物卡 ---- */
function renderNovel() {
  const list = $('novelChapters');
  if (list) {
    list.innerHTML = N.chapters.map((c, i) =>
      '<div class="item' + (i === N.cur ? ' active' : '') + '" data-ch="' + i + '">' +
      esc(c.title || ('第 ' + (i + 1) + ' 章')) + '</div>').join('') ||
      '<div class="muted">尚未导入小说</div>';
  }
  /* 章节数牌：以前是写死的「0 章」占位（全库没人更新它，E2E 差点被它骗了） */
  const ph = $('novelPhase2');
  if (ph) ph.textContent = N.chapters.length + ' 章';
  const info = $('novelInfo');
  if (info) {
    const a = N.analysis;
    info.textContent = a && a.summary ? a.summary :
      (N.chapters.length ? '共 ' + N.chapters.length + ' 章 · 尚未分析剧情' : '把 TXT 小说丢进来，SinkNow 把它变成故事');
  }
  const cast = $('novelCast');
  if (cast) {
    const a = N.analysis;
    cast.innerHTML = (a && a.chars || []).map(c =>
      '<div class="item"><b>' + esc(c.name) + '</b>' +
      '<div class="muted">' + esc(c.look || '') + '</div>' +
      '<div class="muted">' + (c.pose === 'talk' ? '说话担当' : '站桩担当') + '</div></div>').join('') ||
      '<div class="muted">人物待扒</div>';
  }
  const btn = $('novelBtnAuto');
  if (btn) {
    btn.textContent = N.auto ? '自动连播：开' : '自动连播：关';
    btn.classList.toggle('on', !!N.auto);
  }
}

/* ---- 事件绑定 ---- */
(function novelBind() {
  const on = (id, fn) => { const el = $(id); if (el) el.addEventListener('click', fn); };
  on('novelBtnImport', () => novelImport());
  on('novelBtnAnalyze', () => novelAnalyze());
  on('novelBtnFigures', () => novelGenFigures());
  on('novelBtnPlay', () => novelPlay(null));
  on('novelBtnPrev', () => { novelStop(); novelPlay(Math.max(0, N.cur - 1)); });
  on('novelBtnNext', () => { novelStop(); novelPlay(Math.min(N.chapters.length - 1, N.cur + 1)); });
  on('novelBtnAuto', () => { N.auto = !N.auto; renderNovel(); });
  const list = $('novelChapters');
  if (list) list.addEventListener('click', e => {
    const it = e.target.closest('.item');
    if (!it || it.dataset.ch == null) return;
    novelStop();
    novelPlay(parseInt(it.dataset.ch, 10));
  });
  uiLog('[novel] pipeline ready');
})();
