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

/* 当前舞台背景键（SinkNow：剧本【@背景】的 key，舞台层记账 STAGE.bgKey）。
   galgame 大纲节点键那套（G.outline/G.outlinePos）已随 gal.js 退役（2026-10-22）。 */
function comfyCurKey() {
  return (typeof STAGE !== 'undefined' && STAGE.bgKey) ? STAGE.bgKey : '';
}

/* 把已生成的图铺成舞台背景。key 不是舞台当前背景就跳过——
   预生成完成时演出可能已经切到别的场景，别把别的场景的图提前糊上。
   统一走舞台资源层 stageSetBg（stage_asset，与剧本【@背景】指令同一目录同一契约）。 */
function comfyApplyBg(key) {
  if (!key || key !== comfyCurKey()) return;
  stageSetBg(key, '');
  uiLog('[comfy] 背景请求 ' + key);
}

/* 预生成：SinkNow 的场景图由 novelGenScenes 在开演前按需批量发（kind=bg），
   这里不再单独预生成（原 galgame 大纲逐节点预生成已退役）。 */

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
    /* 落盘文件名才是舞台资源键（SinkNow 用 name 落盘，node 只是 ASCII 事件键）：
       从 path 认领 basename；kind 看目录（figures\ 下是立绘，其它是背景）。 */
    const base = (info.path || '').split(/[\\/]/).pop().replace(/\.[^.]+$/, '') || (info.node || '');
    const kind = /figures/i.test(info.path || '') ? 'figures' : 'bg';
    /* 图刚落盘：作废舞台资源缓存再铺，否则 stageAsset 的「未就绪」记账会挡住新图 */
    stageAssetInvalidate(kind, base + '.png');
    if (kind === 'bg') comfyApplyBg(base);
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
  /* SinkNow：手动补生成当前舞台背景（剧本没图/图不满意时的补救按钮） */
  const key = comfyCurKey();
  if (!key) { toast('warn', '舞台还没铺背景（先开演到有场景的段落）'); return; }
  fire('comfy_gen', {
    node: key.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 40) || 'scene',
    name: key,
    prompt: key,
    kind: 'bg'
  });
  toast('success', '已排队生成，图好自动铺上舞台背景');
};
