/* i18n.js - i18n engine (walk/apply/observer/init) (split from app.js 2026-10-18, zero behavior change; loaded in original order) */
let I18N_LANG = 'en';
let i18nWalking = false;

function i18nMapText(s) {
  if (I18N_LANG === 'zh-CN') return s;
  const P = I18N_PATTERNS[I18N_LANG];
  if (P) for (const p of P) { if (p[0].test(s)) return s.replace(p[0], p[1]); }
  const m = I18N_TEXT[I18N_LANG];
  if (m && Object.prototype.hasOwnProperty.call(m, s)) return m[s];
  return s;
}

function i18nRestoreText(s) {
  const m = I18N_TEXT[I18N_LANG];
  if (m) for (const k in m) { if (m[k] === s) return k; }
  const B = I18N_BACK[I18N_LANG];
  if (B) for (const p of B) { if (p[0].test(s)) return s.replace(p[0], p[1]); }
  return s;
}

/* 对单个节点做翻译/还原。code、pre、textarea、script、style 保留原文 */
function i18nWalkNode(node, mode) {
  if (!node) return;
  if (node.nodeType === 3) {
    const raw = node.nodeValue;
    if (!raw || !raw.trim()) return;
    const trimmed = raw.trim();
    const nt = mode === 'restore' ? i18nRestoreText(trimmed) : i18nMapText(trimmed);
    if (nt !== trimmed) node.nodeValue = raw.replace(trimmed, nt);
    return;
  }
  if (node.nodeType !== 1) return;
  const tag = node.tagName;
  if (tag === 'SCRIPT' || tag === 'STYLE' || tag === 'TEXTAREA' || tag === 'CODE' || tag === 'PRE') return;
  if (tag !== 'SELECT' && node.hasAttribute && node.hasAttribute('placeholder')) {
    const v = node.getAttribute('placeholder');
    if (v && v.trim()) {
      const nt = mode === 'restore' ? i18nRestoreText(v.trim()) : i18nMapText(v.trim());
      if (nt !== v.trim()) node.setAttribute('placeholder', v.replace(v.trim(), nt));
    }
  }
  const kids = node.childNodes;
  for (let i = 0; i < kids.length; i++) i18nWalkNode(kids[i], mode);
}

function i18nApplyAll(mode) {
  i18nWalking = true;
  try { i18nWalkNode(document.body, mode); } catch (_) {}
  i18nWalking = false;
}

function applyLanguage(l) {
  const prev = I18N_LANG;
  if (prev !== 'zh-CN') i18nApplyAll('restore');   /* 先还原成中文基础串 */
  I18N_LANG = l;
  if (l !== 'zh-CN') i18nApplyAll('translate');
  try { store.set('novelnow-lang', l); } catch (_) {}
  document.documentElement.lang = l;
  document.title = I18N_TITLE[l] || I18N_TITLE['zh-CN'];
  const sel = document.getElementById('langSel');
  if (sel && sel.value !== l) sel.value = l;
}

/* DOM 变化监听：新增节点 / 文本变化自动翻译（toast、模态、列表重渲染、C 侧消息全覆盖） */
const i18nObserver = new MutationObserver(muts => {
  if (I18N_LANG === 'zh-CN' || i18nWalking) return;
  for (const mu of muts) {
    if (mu.type === 'characterData') {
      try { i18nWalkNode(mu.target, 'translate'); } catch (_) {}
      continue;
    }
    for (const n of mu.addedNodes) {
      try { i18nWalkNode(n, 'translate'); } catch (_) {}
    }
  }
});

(function initI18n() {
  let saved = 'en';
  try { saved = localStorage.getItem('novelnow-lang') || 'en'; } catch (_) {}
  if (!I18N_LANGS[saved]) saved = 'en';
  I18N_LANG = saved;
  if (saved !== 'zh-CN') i18nApplyAll('translate');
  document.documentElement.lang = saved;
  document.title = I18N_TITLE[saved] || I18N_TITLE['zh-CN'];
  const sel = document.getElementById('langSel');
  if (sel) {
    sel.value = saved;
    sel.addEventListener('change', () => applyLanguage(sel.value));
  }
  i18nObserver.observe(document.body, { childList: true, subtree: true, characterData: true });
})();

