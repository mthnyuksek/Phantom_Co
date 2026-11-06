// Interface language (English / Turkish). The pages are written in English; in Turkish this module translates
// the text the pages put on the screen (text nodes and a few attributes) with the dictionary in i18n_tr.js,
// now and whenever the page changes. No dictionary entry = the English text stays, so nothing ever breaks.
//
// Per person and per browser/window (localStorage), changed in the profile or on the sign-in screens.
// Chat messages, code and anything people typed are never translated.
import { EXACT, PATTERNS } from './i18n_tr.js';

export const LANGS = [{ id: 'en', label: 'English' }, { id: 'tr', label: 'Türkçe' }];
const KEY = 'pc2.lang';

function initial() {
  try {
    const v = localStorage.getItem(KEY);
    if (v === 'en' || v === 'tr') return v;
  } catch { /* private mode */ }
  return (navigator.language || '').toLowerCase().startsWith('tr') ? 'tr' : 'en';
}

export const lang = initial();
// The program window uses the same language for its tray menu and notices.
try { window.chrome?.webview?.postMessage({ cmd: 'lang', lang }); } catch { /* not in the program window */ }
export function setLang(id) {
  try { localStorage.setItem(KEY, id); } catch { /* private mode */ }
  location.reload();
}

// ---- translation of one string
const compiled = PATTERNS.map(([en, tr]) => {
  // {} = any text, {#} = digits only (so "{#} notes" does not turn a skill called "Project notes" into nonsense)
  const parts = en.split(/(\{\}|\{#\})/).map((p) => (p === '{}' ? '(.+?)' : p === '{#}' ? '(\\d+)' : p.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')));
  return [new RegExp('^' + parts.join('') + '$', 's'), tr];
});

export function translate(text) {
  if (lang !== 'tr' || !text) return text;
  const m = text.match(/^(\s*)([\s\S]*?)(\s*)$/);
  const core = m[2];
  if (!core || !/[A-Za-z]/.test(core)) return text;
  let out = EXACT[core];
  if (out === undefined) {
    for (const [re, tr] of compiled) {
      const r = core.match(re);
      if (r) { out = tr.replace(/\{(\d)\}/g, (_, i) => translate(r[+i + 1]) ?? r[+i + 1]); break; }
    }
  }
  if (out === undefined) return text;
  done.add(out);
  return m[1] + out + m[3];
}
const done = new Set();  // everything this module produced (so the checker below does not list it)
// for strings built in code
export const t = (en) => translate(en);

// ---- the screen
// Chat text is never translated; the live "is working…" bubble (.typing) is ours, its tool lines carry data-no-i18n.
const SKIP = 'script,style,textarea,input,pre,code,.bubble:not(.typing),.think,[data-no-i18n]';
const SKIP_ALL = 'script,style,pre,code,.bubble:not(.typing),.think,[data-no-i18n]';  // not even the attributes
const FIELDS = 'textarea,input';  // what people typed is left alone, but their placeholder/title is translated
const ATTRS = ['placeholder', 'title', 'aria-label', 'alt'];

function node(n) {
  if (n.nodeType === 3) {
    if (n.parentElement && n.parentElement.closest(SKIP)) return;
    const v = translate(n.nodeValue);
    if (v !== n.nodeValue) n.nodeValue = v;
  } else if (n.nodeType === 1) {
    if (n.closest(SKIP_ALL)) return;
    for (const a of ATTRS) if (n.hasAttribute(a)) { const v = translate(n.getAttribute(a)); if (v !== n.getAttribute(a)) n.setAttribute(a, v); }
    if (n.matches(FIELDS)) return;
    for (const c of n.childNodes) node(c);
  }
}

if (lang === 'tr') {
  document.documentElement.lang = 'tr';
  document.title = translate(document.title);
  const run = () => {
    node(document.body);
    new MutationObserver((list) => {
      for (const r of list) {
        if (r.type === 'childList') r.addedNodes.forEach(node);
        else if (r.type === 'characterData') node(r.target);
        else if (r.type === 'attributes') node(r.target);
      }
    }).observe(document.body, { childList: true, subtree: true, characterData: true, attributes: true, attributeFilter: ATTRS });
  };
  if (document.body) run(); else document.addEventListener('DOMContentLoaded', run);
}

// development helper: window.__untranslated() lists visible English text that has no Turkish yet
window.__untranslated = () => {
  const seen = new Set();
  const walk = (n) => {
    if (n.nodeType === 3) {
      const s = n.nodeValue.trim();
      if (s && /[A-Za-z]{3}/.test(s) && !n.parentElement.closest(SKIP) && !done.has(s) && translate(s) === s) seen.add(s);
    } else if (n.nodeType === 1 && !n.matches(SKIP_ALL)) {
      for (const a of ATTRS) { const v = n.getAttribute(a); if (v && /[A-Za-z]{3}/.test(v) && !done.has(v) && translate(v) === v) seen.add('[' + a + '] ' + v); }
      if (!n.matches(FIELDS)) n.childNodes.forEach(walk);
    }
  };
  walk(document.body);
  return [...seen];
};
