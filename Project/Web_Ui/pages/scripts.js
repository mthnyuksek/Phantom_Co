// Scripts: visual scripts of ghost prompts (Development/Docs/visual_scripts.md). List on the left, the canvas and the
// properties of the selected block on the right. Changes are saved by themselves a moment after the last edit; the
// server's check (problems per block) comes back with every save.
import { api } from '../api.js';
import { S, me, isOwner, canEdit, agentById, projectById } from '../state.js';
import { $, esc, guard, toast, twoStep } from '../ui.js';
import { createCanvas, BLOCKS } from '../script_canvas.js';
import { renderRun, runPill } from './script_run.js';

const TYPES = ['Number', 'Time', 'Date', 'Bool', 'Text'];
const TYPE_HINT = { Number: '0.00', Time: '16:00', Date: '2026-11-16', Bool: 'false', Text: 'text' };
const BUILTINS = [
  ['now_time', 'Time', 'the time now'], ['today', 'Date', "today's date"], ['claude_left_5h', 'Number', "% left of Claude's 5-hour limit"],
  ['claude_left_week', 'Number', "% left of Claude's weekly limit"], ['codex_left', 'Number', "% left of Codex's limit"], ['tokens_today', 'Number', 'tokens all ghosts used today'],
];

let view, ctx, list = [], cur = null, editable = false, canvas = null, sel = null;
let check = { errors: [], prompt_count: 0 }, saveTimer = 0, saving = false, dirty = false;

const short = (s, n) => { s = String(s || ''); return s.length > n ? s.slice(0, n - 1) + '…' : s; };

// the text inside a block on the canvas: user data in data-no-i18n spans, the small hints are translated
const raw = (html) => `<span data-no-i18n>${html}</span>`;
function summary(n) {
  const p = n.props || {};
  if (n.type === 'start') return '';
  if (n.type === 'prompt') {
    const who = p.agent_id ? raw(`<b>${esc(agentById(p.agent_id)?.name || p.agent_id)}</b>`) : '<span class="sc-miss">no ghost</span>';
    return `${who} ${p.text ? raw(esc(short(p.text, 70))) : '<span class="sc-miss">no prompt</span>'}${p.target ? ` <span class="sc-var">→ ${raw(esc(p.target))}</span>` : ''}`;
  }
  if (n.type === 'set') return `<code>${esc(p.var || '?')} = ${esc(p.expr || '?')}</code>`;
  if (n.type === 'if') return `<code>${esc(p.expr || '?')}</code>`;
  if (n.type === 'delay') return p.mode === 'until' ? `<span>until</span> <code>${esc(p.expr || '?')}</code>` : `<code>${esc(p.expr || '?')}</code> <span>min</span>`;
  return '';
}

// ---------------------------------------------------------------- list
const listHTML = () => list.map((s) => `<a class="chat-item ${cur && s.id === cur.id ? 'on' : ''}" href="#/scripts/${esc(s.id)}"><div class="mini big-emoji">🧩</div>
  <div class="ci-body"><div class="ci-title">${esc(s.name)}${s.draft ? ' <span class="badge">draft</span>' : ''}${s.error_count ? ` <span class="badge sc-bad">⚠ ${s.error_count}</span>` : ''}</div>
  <div class="ci-prev">${s.prompt_count === 1 ? '1 prompt block' : `${s.prompt_count} prompt blocks`}</div></div></a>`).join('')
  || '<div class="empty small">No scripts yet.</div>';

async function loadList() {
  list = (await api('GET', '/api/scripts')).scripts;
  const el = $('#sc-list', view);
  if (el) el.innerHTML = listHTML();
}

// the page frame: the list on the left, an empty pane on the right
async function listShell(v, openId) {
  view = v;
  cur = openId ? { id: openId } : null;  // marks the open script in the list until the real one is loaded
  view.innerHTML = `<div class="chats-layout sc-layout ${openId ? 'has-chat' : ''}"><div class="chat-side"><div class="chat-side-head"><h1>Scripts</h1>${canEdit() ? '<button class="btn small primary" data-act="new">+ New</button>' : ''}</div>
    <div class="chat-list" id="sc-list"></div></div><div class="chat-pane" id="sc-detail"></div></div>`;
  await loadList();
  cur = null;
}

// ---------------------------------------------------------------- saving
function changed() {
  if (!editable) return;
  dirty = true;
  setSaveState();
  clearTimeout(saveTimer);
  saveTimer = setTimeout(save, 700);
}
async function save() {
  clearTimeout(saveTimer);
  if (!dirty || !cur || saving) return;
  saving = true;
  dirty = false;
  setSaveState();
  try {
    const r = await api('PUT', '/api/scripts/' + cur.id, bodyOf(cur));
    if (!ctx.isCurrent()) return;
    check = r.check;
    canvas?.setErrors(check.errors);
    paintProblems();
    const item = list.find((s) => s.id === cur.id);
    if (item) { item.name = r.name; item.error_count = check.errors.length; item.prompt_count = check.prompt_count; $('#sc-list', view).innerHTML = listHTML(); }
  } catch (e) {
    dirty = true;
    toast(e.message, true);
  } finally {
    saving = false;
    setSaveState();
    if (dirty) saveTimer = setTimeout(save, 700);
  }
}
// The window closes or reloads before the timer: send what is not saved yet anyway (keepalive outlives the page).
const bodyOf = (s) => ({ name: s.name, description: s.description, project_id: s.project_id, vars: s.vars, nodes: s.nodes, edges: s.edges });
window.addEventListener('pagehide', () => {
  if (!dirty || !cur || !editable) return;
  dirty = false;
  fetch('/api/scripts/' + cur.id, { method: 'PUT', keepalive: true, credentials: 'same-origin',
    headers: { 'X-PC2': '1', 'Content-Type': 'application/json' }, body: JSON.stringify(bodyOf(cur)) }).catch(() => {});
});

function setSaveState() {
  const el = $('#sc-saved', view);
  if (!el) return;
  el.textContent = !editable ? 'Read only' : saving ? 'Saving…' : dirty ? 'Not saved yet' : 'Saved';
  el.className = 'sc-saved' + (dirty || saving ? ' busy' : '');
}

// ---------------------------------------------------------------- panel
const dis = () => (editable ? '' : 'disabled');
const varOptions = (selected, { none = false } = {}) =>
  (none ? `<option value="">— no variable —</option>` : (selected ? '' : '<option value="">— pick —</option>'))
  + cur.vars.map((v) => `<option value="${esc(v.name)}" ${v.name === selected ? 'selected' : ''}>${esc(v.name)} (${esc(v.type)})</option>`).join('');
const agentOptions = (selected) => `<option value="">— pick a ghost —</option>` + S.state.agents.map((a) => `<option value="${esc(a.id)}" ${a.id === selected ? 'selected' : ''}>${esc(a.name)}</option>`).join('');
const projectOptions = (selected, emptyLabel) => `<option value="">${esc(emptyLabel)}</option>` + S.state.projects.map((p) => `<option value="${esc(p.id)}" ${p.id === selected ? 'selected' : ''}>${esc(p.name)}</option>`).join('');
const problemsOf = (pred) => check.errors.filter(pred);
const problemList = (errs) => errs.length ? `<ul class="sc-problems">${errs.map((e) => `<li ${e.node ? `data-goto="${esc(e.node)}"` : ''}>${esc(e.message)}</li>`).join('')}</ul>` : '';

function scriptPanel() {
  const varRows = cur.vars.map((v, i) => `<div class="sc-var-row" data-var="${i}">
      <input type="text" data-vf="name" value="${esc(v.name)}" placeholder="name" maxlength="32" ${dis()} data-no-i18n>
      <select data-vf="type" ${dis()} data-no-i18n>${TYPES.map((t) => `<option ${t === v.type ? 'selected' : ''}>${t}</option>`).join('')}</select>
      <input type="text" data-vf="init" value="${esc(v.init)}" placeholder="${TYPE_HINT[v.type] || ''}" ${dis()} data-no-i18n>
      ${editable ? `<button class="btn small" data-act="var-del" title="Remove">✕</button>` : ''}</div>`).join('');
  return `<h3>Script</h3>
    <label>Description</label><textarea id="sc-desc" maxlength="2000" style="min-height:60px" ${dis()}>${esc(cur.description || '')}</textarea>
    <label>Default project for prompts</label><select id="sc-project" ${dis()}>${projectOptions(cur.project_id, '— none (shared files) —')}</select>
    <h3 class="sc-sub">Variables</h3>
    <div id="sc-vars">${varRows || '<p class="muted small">No variables yet.</p>'}</div>
    ${editable ? '<button class="btn small" data-act="var-add">+ Variable</button>' : ''}
    <details class="sc-help"><summary>Built-in variables</summary>${BUILTINS.map(([n, t, d]) => `<div><code>${n}</code> <span class="muted" data-no-i18n>${t}</span> <span class="muted">${d}</span></div>`).join('')}</details>
    <div id="sc-problems-all"></div>
    <h3 class="sc-sub">Runs</h3><div id="sc-runs"><p class="muted small">Loading…</p></div>`;
}

function nodePanel(n) {
  const b = BLOCKS[n.type];
  const p = n.props || {};
  let fields = '';
  if (n.type === 'start') fields = '<p class="muted small">The run begins here when someone presses Run.</p>';
  else if (n.type === 'end') fields = '<p class="muted small">Nothing comes after this block. A run also ends by itself when no block is left.</p>';
  else if (n.type === 'prompt') fields = `
    <label>Ghost</label><select data-pf="agent_id" ${dis()}>${agentOptions(p.agent_id)}</select>
    <label>Project</label><select data-pf="project_id" ${dis()}>${projectOptions(p.project_id, '— the script\'s default —')}</select>
    <label>Prompt</label><textarea data-pf="text" maxlength="20000" style="min-height:120px" placeholder="What should the ghost do? Use {variable} to put a value in." ${dis()}>${esc(p.text)}</textarea>
    <label>Save the answer's value into</label><select data-pf="target" ${dis()}>${varOptions(p.target, { none: true })}</select>
    <p class="muted small">With a variable the ghost is asked to end its answer with &lt;value&gt;…&lt;/value&gt;.</p>`;
  else if (n.type === 'set') fields = `
    <label>Variable</label><select data-pf="var" ${dis()}>${varOptions(p.var)}</select>
    <label>New value</label><input type="text" data-pf="expr" value="${esc(p.expr)}" placeholder="score + 1" ${dis()} data-no-i18n>`;
  else if (n.type === 'if') fields = `
    <label>Condition</label><input type="text" data-pf="expr" value="${esc(p.expr)}" placeholder="claude_left_5h > 50 and now_time < 16:00" ${dis()} data-no-i18n>
    <p class="muted small">Compare with &lt; &lt;= &gt; &gt;= == !=, join with and / or / not. "yes" goes left, "no" goes right.</p>`;
  else if (n.type === 'delay') fields = `
    <label>Wait</label><select data-pf="mode" ${dis()}><option value="for" ${p.mode !== 'until' ? 'selected' : ''}>for some minutes</option><option value="until" ${p.mode === 'until' ? 'selected' : ''}>until a time of day</option></select>
    <label>${p.mode === 'until' ? 'Time' : 'Minutes'}</label><input type="text" data-pf="expr" value="${esc(p.expr)}" placeholder="${p.mode === 'until' ? '16:00' : '30'}" ${dis()} data-no-i18n>`;
  return `<h3><span>${b.icon}</span> <span data-no-i18n>${b.label}</span></h3>
    <label>Name on the canvas</label><input type="text" data-nf="label" value="${esc(n.label || '')}" placeholder="${b.label}" maxlength="80" ${dis()}>
    ${fields}
    <div id="sc-node-problems">${problemList(problemsOf((e) => e.node === n.id))}</div>
    ${editable ? '<div class="modal-foot"><button class="btn danger small" data-act="remove">Delete block</button></div>' : ''}`;
}

function edgePanel(i) {
  const e = cur.edges[i];
  if (!e) return '';
  const name = (id) => { const n = cur.nodes.find((x) => x.id === id); return n ? (n.label || BLOCKS[n.type]?.label || id) : id; };
  return `<h3>Arrow</h3><p data-no-i18n>${esc(name(e.from))}${e.port === 'true' ? ' (yes)' : e.port === 'false' ? ' (no)' : ''} → ${esc(name(e.to))}</p>
    ${editable ? '<div class="modal-foot"><button class="btn danger small" data-act="remove">Delete arrow</button></div>' : ''}`;
}

function paintPanel() {
  const el = $('#sc-panel', view);
  if (!el) return;
  if (sel?.kind === 'node') { const n = cur.nodes.find((x) => x.id === sel.id); el.innerHTML = n ? nodePanel(n) : scriptPanel(); }
  else if (sel?.kind === 'edge') el.innerHTML = edgePanel(sel.index);
  else { el.innerHTML = scriptPanel(); paintRuns(); }
  paintProblems();
}
// the last runs of this script (in the script panel)
async function paintRuns() {
  if (!cur) return;
  const id = cur.id;
  const runs = (await api('GET', '/api/script_runs?script=' + encodeURIComponent(id)).catch(() => ({ runs: [] }))).runs;
  const el = $('#sc-runs', view);
  if (!el || !cur || cur.id !== id) return;
  el.innerHTML = runs.length ? runs.slice(0, 10).map((r) => `<a class="sc-run-row" href="#/scripts/${esc(id)}/run/${esc(r.id)}">${runPill(r.status)}
      <span class="muted small" data-no-i18n>${esc(new Date(r.created).toLocaleString())}</span></a>`).join('')
    : '<p class="muted small">Not run yet.</p>';
}

function paintProblems() {
  const mine = $('#sc-node-problems', view);  // the selected block's own problems (the inputs around it stay as they are)
  if (mine && sel?.kind === 'node') mine.innerHTML = problemList(problemsOf((e) => e.node === sel.id));
  const all = $('#sc-problems-all', view);
  if (all) all.innerHTML = check.errors.length
    ? `<h3 class="sc-sub">Problems (${check.errors.length})</h3><p class="muted small">The script can run once these are fixed.</p>${problemList(check.errors)}`
    : `<p class="sc-ok">✓ Ready to run${check.prompt_count ? ` · ${check.prompt_count === 1 ? '1 ghost run' : `${check.prompt_count} ghost runs`} per run` : ''}</p>`;
  const badge = $('#sc-check', view);
  if (badge) badge.innerHTML = check.errors.length ? `<span class="badge sc-bad">⚠ ${check.errors.length}</span>` : '<span class="badge">✓</span>';
}

// ---------------------------------------------------------------- page
function detailHTML() {
  if (!cur) return `<div class="empty big"><p>🧩 A script joins ghost prompts like code blocks: one after another, side by side, with conditions, delays and variables.<br>Pick a script on the left or make a new one.</p></div>`;
  const ro = !editable;
  return `<div class="sc-head">
      <a class="btn small back-btn" href="#/scripts" aria-label="Back to scripts">←</a>
      <input type="text" id="sc-name" value="${esc(cur.name)}" maxlength="120" ${ro ? 'disabled' : ''}>
      <span id="sc-check"></span><span id="sc-saved" class="sc-saved"></span>
      ${canEdit() ? '<button class="btn primary small" data-act="run">▶ Run</button>' : ''}
      ${editable ? '<button class="btn small danger" data-act="del">Delete</button>' : ''}
    </div>
    ${cur.draft ? '<div class="sc-draft">📝 A ghost wrote this script. It never runs by itself: check it, then press Run.</div>' : ''}
    <div class="sc-body"><div class="sc-host" id="sc-host"></div><aside class="sc-panel" id="sc-panel"></aside></div>`;
}

export async function render(v, arg, c) {
  const [sid, mode, runId] = (arg || '').split('/');
  if (mode === 'run' && runId) return renderRun(v, sid, runId, c, () => listShell(v, sid));
  arg = sid;
  view = v; ctx = c;
  cur = null; sel = null; canvas = null; dirty = false; check = { errors: [], prompt_count: 0 };
  await listShell(view, arg);
  if (arg) {
    try { cur = await api('GET', '/api/scripts/' + encodeURIComponent(arg)); } catch (e) { toast(e.message, true); }
    if (!ctx.isCurrent()) return;
    if (cur) {
      check = cur.check || check;
      delete cur.check;
      editable = canEdit() && (cur.created_by === me().id || isOwner());
    }
  }
  $('#sc-list', view).innerHTML = listHTML();
  $('#sc-detail', view).innerHTML = detailHTML();
  if (cur) {
    canvas = createCanvas($('#sc-host', view), {
      script: cur, readOnly: !editable, summary,
      onChange: () => { changed(); if (sel?.kind === 'edge' && !cur.edges[sel.index]) sel = null; },
      onSelect: (s) => { sel = s; paintPanel(); },
      onWarn: (m) => toast(m, true),
    });
    canvas.setErrors(check.errors);
    paintPanel();
    setSaveState();
    if (!cur.nodes.length && editable) canvas.togglePalette(true);
  }
  ctx.destroy = () => { if (dirty) save(); clearTimeout(saveTimer); };

  view.oninput = (e) => {
    if (!cur || !editable) return;
    const t = e.target;
    if (t.id === 'sc-name') { cur.name = t.value; return changed(); }
    if (t.id === 'sc-desc') { cur.description = t.value; return changed(); }
    const n = sel?.kind === 'node' ? cur.nodes.find((x) => x.id === sel.id) : null;
    if (n && t.dataset.pf && t.tagName !== 'SELECT') { n.props[t.dataset.pf] = t.value; canvas.refresh(); return changed(); }
    if (n && t.dataset.nf === 'label') { n.label = t.value; canvas.refresh(); return changed(); }
    const row = t.closest('[data-var]');
    if (row && t.tagName === 'INPUT') { renameOrSetVar(+row.dataset.var, t.dataset.vf, t.value); return changed(); }
  };
  view.onchange = (e) => {
    if (!cur || !editable) return;
    const t = e.target;
    if (t.id === 'sc-project') { cur.project_id = t.value; return changed(); }
    const n = sel?.kind === 'node' ? cur.nodes.find((x) => x.id === sel.id) : null;
    if (n && t.dataset.pf && t.tagName === 'SELECT') {
      n.props[t.dataset.pf] = t.value;
      canvas.refresh();
      if (t.dataset.pf === 'mode') paintPanel();
      return changed();
    }
    const row = t.closest('[data-var]');
    if (row && t.tagName === 'SELECT') { cur.vars[+row.dataset.var].type = t.value; paintPanel(); return changed(); }
  };
  view.onkeydown = (e) => { if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); save(); } };
  view.onclick = guard(async (e) => {
    const go = e.target.closest('[data-goto]');
    if (go && canvas) { canvas.select({ kind: 'node', id: go.dataset.goto }); return; }
    const el = e.target.closest('[data-act]');
    if (!el) return;
    const act = el.dataset.act;
    if (act === 'new') {
      const s = await api('POST', '/api/scripts', { name: 'New script', nodes: [{ id: 'start', type: 'start', x: -100, y: 0 }] });
      location.hash = '#/scripts/' + s.id;
    } else if (act === 'var-add') {
      let i = cur.vars.length + 1;
      while (cur.vars.some((v) => v.name === 'var' + i)) i++;
      cur.vars.push({ name: 'var' + i, type: 'Number', init: '' });
      paintPanel(); changed();
    } else if (act === 'var-del') {
      cur.vars.splice(+el.closest('[data-var]').dataset.var, 1);
      paintPanel(); changed();
    } else if (act === 'remove') {
      canvas.remove();
    } else if (act === 'run') {
      await save();
      if (check.errors.length) { toast('Fix the problems of this script first.', true); return; }
      const r = await api('POST', `/api/scripts/${cur.id}/run`);
      location.hash = `#/scripts/${cur.id}/run/${r.id}`;
    } else if (act === 'del') {
      twoStep(el, 'Really delete?', async () => {
        clearTimeout(saveTimer); dirty = false;
        await api('DELETE', '/api/scripts/' + cur.id);
        location.hash = '#/scripts';
      });
    }
  });
}

// A renamed variable is renamed in the blocks that use it as a target / Set variable too (not inside expressions or prompts).
function renameOrSetVar(i, field, value) {
  const v = cur.vars[i];
  if (field === 'name') {
    const old = v.name;
    v.name = value;
    for (const n of cur.nodes) {
      if (n.props?.target === old) n.props.target = value;
      if (n.type === 'set' && n.props?.var === old) n.props.var = value;
    }
    canvas.refresh();
  } else {
    v[field] = value;
  }
}

export function tick() { /* the editor does not follow other people's edits while open */ }
