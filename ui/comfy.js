/* ============================================================
 * comfy.js — 配图工坊（ComfyUI 本地生图接口）
 *
 * 后端命令（host_cmds.c）：
 *   comfy_test {url?}         探活 GET /system_stats（异步，回执走 comfy_done）
 *   comfy_gen  {node,prompt}  提交工作流生图，落盘 projects/<id>/舞台图片/<node>.png
 *   read_image {node}         读 PNG → base64 data URL（同步回 {data}）
 * 事件：comfy_done {ok, cmd, message, text}（text = JSON {node,path}）
 * 约定：只发起出站 HTTP，不开放任何端口；不占 AI 忙闲位，可与剧情生成并行。
 * ============================================================ */

/* 当前舞台节点的配图 key（用大纲节点 id，跳线也认得出） */
function comfyCurKey() {
  const out = (G.outline && G.outline.nodes) ? G.outline.nodes : [];
  const n = out[G.outlinePos];
  return (n && n.id) ? n.id : ('p' + (G.outlinePos + 1));
}

/* 下一节的配图 key */
function comfyNextKey() {
  const out = (G.outline && G.outline.nodes) ? G.outline.nodes : [];
  const idx = G.outlinePos + 1;
  const n = out[idx];
  return (n && n.id) ? n.id : ('p' + (idx + 1));
}

/* 把已生成的图铺成舞台背景。key 不是当前节点就跳过——
   预生成完成时用户可能已经推进到别的节点，别把下一节的图提前糊上。 */
function comfyApplyBg(key) {
  if (!key || key !== comfyCurKey()) return;
  send('read_image', { node: key }, 60000).then(d => {
    if (!d || !d.data) return;
    const st = $('galStage');
    if (!st) return;
    st.style.backgroundImage = 'url(' + d.data + ')';
    st.classList.add('has-bg');
    uiLog('[comfy] 背景已铺 ' + key);
  }).catch(() => {});   /* 没图就算了，别打扰剧情 */
}

/* 预生成下一节背景：只在「自动生成背景」开着时跑。
   失败只记日志/toast，绝不打断剧情主线。 */
function comfyPreGen() {
  if (!(S.cfg && S.cfg.comfyAutoBg)) return;
  if (!S.cfg.comfyUrl || !S.cfg.comfyWorkflowPath) return;
  const out = (G.outline && G.outline.nodes) ? G.outline.nodes : [];
  const nxt = out[G.outlinePos + 1];
  const key = comfyNextKey();
  const prompt = nxt
    ? ((nxt.title || '') + '。' + (nxt.summary || nxt.desc || nxt.beat || ''))
    : '下一节剧情背景插画';
  if (!prompt.replace(/[。\s]/g, '')) return;
  fire('comfy_gen', { node: key, prompt });
  uiLog('[comfy] 预生成背景 ' + key);
}

/* comfy_done 事件回执（core.js 转来） */
function comfyOnDone(d) {
  if (d.cmd === 'comfy_test') {
    const el = $('comfyStatus');
    if (el) el.textContent = d.ok ? '连接成功' : '连接失败';
    toast(d.ok ? 'success' : 'error', d.message || (d.ok ? 'ComfyUI 连接成功' : 'ComfyUI 连接失败'));
    return;
  }
  if (d.cmd === 'comfy_gen') {
    if (!d.ok) { toast('warn', d.message || '配图生成失败'); return; }
    let info = {};
    try { info = JSON.parse(d.text || '{}'); } catch (e) { info = {}; }
    uiLog('[comfy] 生成完成 ' + (info.node || '?') + ' → ' + (info.path || '?'));
    comfyApplyBg(info.node || '');
    return;
  }
}

/* ---- 配图工坊页绑定 ---- */
$('btnComfyTest').onclick = () => {
  const url = $('comfyUrl').value.trim();
  if (!url) { toast('warn', '先填 ComfyUI 地址'); return; }
  $('comfyStatus').textContent = '测试中…';
  fire('comfy_test', { url });
};

$('btnComfyPickWf').onclick = () => {
  send('pick_text_file', {}, 600000).then(d => {
    if (d && d.path) {
      $('comfyWfPath').value = d.path;
      fire('save_config', collectCfg());
      toast('success', '工作流已选定');
    }
  }).catch(e => toast('error', e.message));
};

$('btnComfySave').onclick = () => {
  send('save_config', collectCfg()).then(c => {
    S.cfg = c;
    toast('success', '配图设置已保存');
  }).catch(e => toast('error', e.message));
};

$('comfyAutoBg').addEventListener('change', () => fire('save_config', collectCfg()));

$('btnComfyGen').onclick = () => {
  /* S.proj 初值就是空对象，裸判恒真（P1）——要看有没有项目得判 hasProject */
  if (!S.proj || !S.proj.hasProject) { toast('warn', '先打开项目'); return; }
  const out = (G.outline && G.outline.nodes) ? G.outline.nodes : [];
  const cur = out[G.outlinePos];
  const n = G.nodes[G.current];
  const prompt = (cur && (cur.title || cur.summary))
    ? ((cur.title || '') + '。' + (cur.summary || ''))
    : ((n && n.text) ? n.text.slice(0, 200) : '剧情背景插画');
  fire('comfy_gen', { node: comfyCurKey(), prompt });
  toast('success', '已排队生成，图好自动铺上舞台背景');
};
