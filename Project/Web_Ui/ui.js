// Small helpers shared by all pages.
export const $ = (s, el = document) => el.querySelector(s);
export const $$ = (s, el = document) => [...el.querySelectorAll(s)];
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
export const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

export function toast(msg, isErr = false) {
  const el = document.createElement('div');
  el.className = 'toast' + (isErr ? ' err' : '');
  el.textContent = msg;
  $('#toasts').append(el);
  setTimeout(() => el.remove(), isErr ? 6000 : 3200);
}
export const guard = (fn) => async (...a) => { try { return await fn(...a); } catch (e) { toast(e.message, true); } };

export function ago(iso) {
  if (!iso) return '';
  const s = Math.max(0, (Date.now() - new Date(iso).getTime()) / 1000);
  if (s < 60) return 'just now';
  if (s < 3600) return Math.floor(s / 60) + ' min ago';
  if (s < 86400) return Math.floor(s / 3600) + ' h ago';
  return Math.floor(s / 86400) + ' d ago';
}
export function duration(a, b) {
  if (!a) return '';
  const s = Math.round(((b ? new Date(b) : new Date()) - new Date(a)) / 1000);
  return s < 60 ? s + ' s' : Math.floor(s / 60) + ' min ' + (s % 60) + ' s';
}

// minimal, safe markdown: code fences, `code`, **bold**, line breaks
export function md(text) {
  return String(text ?? '').split('```').map((part, i) => {
    if (i % 2) return '<pre><code>' + esc(part.replace(/^[\w-]*\n/, '')) + '</code></pre>';
    return esc(part).replace(/`([^`\n]+)`/g, '<code>$1</code>').replace(/\*\*([^*\n]+)\*\*/g, '<b>$1</b>').replace(/\n/g, '<br>');
  }).join('');
}

export const STATUS_TEXT = { done: 'Done', running: 'Running', queued: 'Queued', idle: 'Not started', failed: 'Failed', cancelled: 'Stopped', timeout: 'Timed out', interrupted: 'Interrupted' };
export const pill = (s) => `<span class="pill ${esc(s)}">${esc(STATUS_TEXT[s] || s)}</span>`;
export const moodOfStatus = (s) => ({ running: 'working', queued: 'queued', done: 'done', failed: 'failed', timeout: 'failed', interrupted: 'failed' }[s] || 'idle');
export const ROLE_TEXT = { owner: 'Owner', editor: 'Editor', viewer: 'Viewer' };

// modal dialog: modal(html, onMount(el)) ; closeModal()
const root = () => $('#modal-root');
export function modal(html, onMount, wide = false) {
  root().innerHTML = `<div class="modal-bg"><div class="modal ${wide ? 'wide' : ''}">${html}</div></div>`;
  const bg = root().firstChild;
  bg.addEventListener('mousedown', (e) => { if (e.target === bg) closeModal(); });
  onMount?.(bg);
  return bg;
}
export const closeModal = () => { root().innerHTML = ''; };
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeModal(); });

// a button that asks twice ("Delete" -> "Really?") instead of a browser dialog
// First click asks, second click acts. `list` (optional, HTML string) is shown in a popup
// while the pointer is over the asking button: what the action will remove.
export function twoStep(btn, ask, action, list) {
  if (btn.dataset.sure) { btn._twoStepReset?.(); return action(); }
  const old = btn.textContent;
  btn.dataset.sure = 1;
  btn.textContent = ask;
  let pop = null, timer = 0;
  const hide = () => { pop?.remove(); pop = null; };
  const show = () => {
    if (!list || pop) return;
    pop = document.createElement('div');
    pop.className = 'twostep-pop';
    pop.innerHTML = list;
    document.body.appendChild(pop);
    const r = btn.getBoundingClientRect(), w = pop.offsetWidth, h = pop.offsetHeight;
    pop.style.left = Math.max(8, Math.min(r.left, innerWidth - w - 8)) + 'px';
    pop.style.top = (r.top - h - 8 >= 8 ? r.top - h - 8 : r.bottom + 8) + 'px';
  };
  const arm = (ms) => { clearTimeout(timer); timer = setTimeout(reset, ms); };
  const enter = () => { show(); clearTimeout(timer); };
  const leave = () => { hide(); arm(3000); };
  function reset() {
    clearTimeout(timer); hide();
    btn.removeEventListener('mouseenter', enter); btn.removeEventListener('mouseleave', leave);
    btn.textContent = old; delete btn.dataset.sure; delete btn._twoStepReset;
  }
  btn._twoStepReset = reset;
  btn.addEventListener('mouseenter', enter);
  btn.addEventListener('mouseleave', leave);
  if (btn.matches(':hover')) enter(); else arm(3000);
}

export async function copyText(text) {
  try { await navigator.clipboard.writeText(text); toast('Copied'); } catch { toast('Copy failed - select the text and copy it by hand.', true); }
}

// one event in a run's log (used by the run details)
export function renderEvent(ev) {
  switch (ev.kind) {
    case 'message': return `<div class="bubble">${md(ev.text)}</div>`;
    case 'tool_use': return `<div class="tool">🔧 <b>${esc(ev.name)}</b> ${esc(ev.text)}</div>`;
    case 'tool_result': return ev.text ? `<details class="result ${ev.is_error ? 'err' : ''}"><summary>${ev.is_error ? 'Tool error' : 'Tool result'}</summary><pre>${esc(ev.text)}</pre></details>` : '';
    case 'thinking': return `<div class="think">💭 ${esc(ev.text)}</div>`;
    case 'error': return `<div class="errbox">⚠️ ${esc(ev.text)}</div>`;
    case 'done': return `<div class="sys">— ${esc(STATUS_TEXT[ev.status] || ev.status)} —</div>`;
    default: return `<div class="sys">${esc(ev.text)}</div>`;
  }
}
