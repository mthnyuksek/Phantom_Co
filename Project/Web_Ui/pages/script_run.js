// The live view of one script run (#/scripts/<script>/run/<run>): the script's canvas read-only, every block coloured by
// its state, the running block pulsing; the side panel shows the selected block's output (or the variables and the event
// log), with Pause / Step / Resume / Stop for the person who started the run (or the owner). Polls while the run lives.
import { api } from '../api.js';
import { lang } from '../i18n.js';
import { me, isOwner, canEdit, agentById } from '../state.js';
import { $, esc, guard, toast, md, sleep, modal, closeModal, duration } from '../ui.js';
import { createCanvas, BLOCKS } from '../script_canvas.js';
import { conversationView } from '../chat_view.js';
import { openRunModal } from './runs.js';

const RUN_TEXT = { running: 'Running', paused: 'Paused', done: 'Done', failed: 'Failed', stopped: 'Stopped' };
const RUN_PILL = { running: 'running', paused: 'queued', done: 'done', failed: 'failed', stopped: 'cancelled' };
export const runPill = (s) => `<span class="pill ${RUN_PILL[s] || 'idle'}">${esc(RUN_TEXT[s] || s)}</span>`;
const NODE_TEXT = { waiting: 'Waiting', ready: 'Ready to start', running: 'Running', done: 'Done', skipped: 'Skipped', failed: 'Failed', stopped: 'Stopped' };
const live = (s) => s === 'running' || s === 'paused';

// a value from the run state ({type, value}) as people read it
export function showValue(v) {
  if (!v) return '';
  if (v.type === 'Number') {
    const n = BigInt(v.value), neg = n < 0n, a = neg ? -n : n;
    return (neg ? '-' : '') + (a / 100n).toString() + (lang === 'tr' ? ',' : '.') + (a % 100n).toString().padStart(2, '0');
  }
  if (v.type === 'Time') return String(Math.floor(v.value / 60)).padStart(2, '0') + ':' + String(v.value % 60).padStart(2, '0');
  if (v.type === 'Date') return new Date(v.value * 86400000).toISOString().slice(0, 10);
  if (v.type === 'Bool') return v.value ? 'true' : 'false';
  return String(v.value ?? '');
}
const clock = (ms) => new Date(ms).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
const left = (ms) => { const s = Math.max(0, Math.round((ms - Date.now()) / 1000)); return s < 60 ? `${s} s` : `${Math.floor(s / 60)} min ${s % 60} s`; };

let view, ctx, run = null, canvas = null, sel = null, lastPanel = '';

const nodeOf = (id) => run.script.nodes.find((n) => n.id === id);
const nodeName = (id) => { const n = nodeOf(id); return n ? (n.label || BLOCKS[n.type]?.label || id) : id; };
const canControl = () => canEdit() && run && (run.user_id === me().id || isOwner());

// the text inside a block: what it does (user data, not translated), then how it is doing
const raw = (html) => `<span data-no-i18n>${html}</span>`;
function summary(n) {
  const p = n.props || {}, ns = run.nodes[n.id] || {};
  let what = '';
  if (n.type === 'prompt') what = raw(`<b>${esc(agentById(p.agent_id)?.name || p.agent_id)}</b> ${esc(String(p.text || '').slice(0, 60))}`);
  else if (n.type === 'set') what = `<code>${esc(p.var)} = ${esc(p.expr)}</code>`;
  else if (n.type === 'if') what = `<code>${esc(p.expr)}</code>`;
  else if (n.type === 'delay') what = p.mode === 'until' ? `<span>until</span> <code>${esc(p.expr)}</code>` : `<code>${esc(p.expr)}</code> <span>min</span>`;
  let how = '';
  if (ns.state === 'running' && n.type === 'delay' && ns.wake_at) how = `⏳ ${raw(esc(left(ns.wake_at)))}`;
  else if (ns.state === 'running' && n.type === 'prompt') how = '💭 <span>working…</span>';
  else if (ns.state === 'running' && ns.usage_wait) how = '📊 <span>reading the usage limits…</span>';
  else if (ns.state === 'done' && n.type === 'if') how = `→ <span>${ns.output === 'true' ? 'yes' : 'no'}</span>`;
  else if (ns.state === 'done' && n.type === 'prompt' && ns.output) how = '✓ ' + raw(esc(String(ns.output).slice(0, 50)));
  else if (ns.state === 'failed') how = '✗ <span>' + esc(String(ns.error || '').slice(0, 80)) + '</span>';
  return what + (how ? `<div class="sc-how">${how}</div>` : '');
}

function headHTML() {
  const ctl = canControl() && live(run.status);
  return `<a class="btn small" href="#/scripts/${esc(run.script_id)}" title="Back to the editor">←</a>
    <h2 class="sc-run-title" data-no-i18n>${esc(run.script_name)}</h2>
    <span id="sr-pill">${runPill(run.status)}</span>
    <span class="muted small sc-run-meta" data-no-i18n>${esc(new Date(run.created).toLocaleString())} · ${esc(duration(run.created, run.ended || null))}</span>
    <span class="grow"></span>
    ${ctl && run.status === 'running' ? '<button class="btn small" data-act="pause">⏸ Pause</button>' : ''}
    ${ctl && run.status === 'paused' ? '<button class="btn small" data-act="step">⏭ Step</button><button class="btn small" data-act="resume">▶ Resume</button>' : ''}
    ${ctl ? '<button class="btn small danger" data-act="stop">■ Stop</button>' : ''}
    ${!live(run.status) && canEdit() ? '<button class="btn small primary" data-act="again">▶ Run again</button>' : ''}`;
}

function eventLine(e) {
  const t = `<span class="muted" data-no-i18n>${esc(new Date(e.ts).toLocaleTimeString())}</span>`;
  const n = e.node ? `<b data-no-i18n>${esc(nodeName(e.node))}</b>` : '';
  switch (e.kind) {
    case 'run_started': return `${t} ▶ <span>Run started</span>`;
    case 'node_started': return `${t} ${n} <span>started</span>`;
    case 'node_done': return `${t} ✓ ${n}${e.text ? ` → <span>${e.text === 'true' ? 'yes' : 'no'}</span>` : ''}`;
    case 'node_skipped': return `${t} ↷ ${n} <span>skipped</span>`;
    case 'node_failed': return `${t} ✗ ${n}: <span>${esc(e.text)}</span>`;
    case 'var_set': return `${t} <code data-no-i18n>${esc(e.var)} = ${esc(e.text)}</code>`;
    case 'run_ended': return `${t} ■ <span>Run ended</span> ${runPill(e.text)}${e.error ? ` <span>${esc(e.error)}</span>` : ''}`;
    default: return `${t} ${n} <span>${esc(e.text || '')}</span>`;
  }
}

function panelHTML() {
  if (sel?.kind === 'node') {
    const n = nodeOf(sel.id), ns = run.nodes[sel.id] || {};
    if (!n) return '';
    const b = BLOCKS[n.type];
    const runs = ns.runs || [];
    return `<h3><span>${b.icon}</span> <span data-no-i18n>${esc(n.label || b.label)}</span></h3>
      <p><span class="pill ${ns.state === 'done' ? 'done' : ns.state === 'failed' ? 'failed' : ns.state === 'running' ? 'running' : 'idle'}">${esc(NODE_TEXT[ns.state] || ns.state)}</span>
        ${ns.started ? `<span class="muted small" data-no-i18n> ${esc(duration(ns.started, ns.ended || null))}</span>` : ''}</p>
      ${n.type === 'delay' && ns.state === 'running' && ns.wake_at ? `<p class="small">Goes on at <b data-no-i18n>${esc(clock(ns.wake_at))}</b></p>` : ''}
      ${ns.restarted ? '<p class="muted small">Started again after the program was restarted.</p>' : ''}
      ${ns.error ? `<div class="errbox">⚠️ ${esc(ns.error)}</div>` : ''}
      ${ns.output && n.type === 'prompt' ? `<label>Answer</label><div class="sc-output bubble">${md(ns.output)}</div>` : ns.output ? `<label>Result</label><p data-no-i18n><code>${esc(ns.output)}</code></p>` : ''}
      ${ns.conv ? '<button class="btn small" data-act="conv">💬 Open the conversation</button> ' : ''}
      ${runs.length ? `<button class="btn small" data-act="details" data-run="${esc(runs[runs.length - 1])}">⏱️ Run details</button>` : ''}`;
  }
  const vars = Object.entries(run.vars || {});
  const events = (run.events || []).slice(-60).reverse();
  return `<h3>Variables</h3>
    ${vars.length ? `<table class="sc-vars-table">${vars.map(([k, v]) => `<tr><td data-no-i18n><code>${esc(k)}</code></td><td class="muted" data-no-i18n>${esc(v.type)}</td><td data-no-i18n><b>${esc(showValue(v))}</b></td></tr>`).join('')}</table>` : '<p class="muted small">This script has no variables.</p>'}
    ${run.error ? `<div class="errbox">⚠️ ${esc(run.error)}</div>` : ''}
    <h3 class="sc-sub">What happened</h3>
    <div class="sc-events">${events.map((e) => `<div>${eventLine(e)}</div>`).join('')}</div>`;
}

function paint() {
  const states = Object.fromEntries(Object.entries(run.nodes).map(([k, v]) => [k, v.state]));
  canvas.setStates(states, run.edges);
  $('#sr-head', view).innerHTML = headHTML();
  const html = panelHTML();
  if (html !== lastPanel) { $('#sr-panel', view).innerHTML = html; lastPanel = html; }
}

// `my` is this page's own context: after "Run again" the module's `ctx` already belongs to the next page,
// and a loop that looked at it would keep painting the old run over the new one.
async function poll(id, my) {
  while (my.isCurrent()) {
    await sleep(live(run.status) ? 1000 : 4000);
    if (!my.isCurrent()) return;
    try {
      const r = await api('GET', '/api/script_runs/' + id);
      if (!my.isCurrent() || r.id !== id) return;
      const was = run.status;
      run = r;
      paint();
      if (!live(run.status) && live(was)) toast(run.status === 'done' ? 'The script finished.' : run.status === 'failed' ? 'The script failed.' : 'The script was stopped.', run.status === 'failed');
    } catch { /* the next look */ }
  }
}

export async function renderRun(v, scriptId, runId, c, shell) {
  view = v; ctx = c; sel = null; lastPanel = '';
  await shell();
  if (!ctx.isCurrent()) return;
  try { run = await api('GET', '/api/script_runs/' + encodeURIComponent(runId)); } catch (e) { toast(e.message, true); return; }
  if (!ctx.isCurrent()) return;
  $('#sc-detail', view).innerHTML = `<div class="sc-head" id="sr-head"></div>
    <div class="sc-body"><div class="sc-host" id="sr-host"></div><aside class="sc-panel" id="sr-panel"></aside></div>`;
  canvas = createCanvas($('#sr-host', view), {
    script: run.script, readOnly: true, summary,
    onSelect: (s) => { sel = s?.kind === 'node' ? s : null; lastPanel = ''; paint(); },
  });
  paint();
  // follow the running block's pulse without asking the server: the countdowns move every second
  const tickTimer = setInterval(() => { if (run && live(run.status)) canvas.refresh(); }, 1000);
  let convView = null;
  ctx.destroy = () => { clearInterval(tickTimer); convView?.destroy(); };
  poll(runId, c);

  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    const act = el.dataset.act;
    if (act === 'new') {
      const s = await api('POST', '/api/scripts', { name: 'New script', nodes: [{ id: 'start', type: 'start', x: -100, y: 0 }] });
      location.hash = '#/scripts/' + s.id;
    } else if (['pause', 'resume', 'step', 'stop'].includes(act)) {
      run = await api('POST', `/api/script_runs/${run.id}/${act}`);
      paint();
    } else if (act === 'again') {
      const r = await api('POST', `/api/scripts/${run.script_id}/run`);
      location.hash = `#/scripts/${run.script_id}/run/${r.id}`;
    } else if (act === 'conv') {
      const ns = run.nodes[sel.id];
      modal(`<h2 data-no-i18n>${esc(nodeName(sel.id))}</h2><div class="sc-conv-modal" id="sr-conv"></div>
        <div class="modal-foot"><button class="btn" data-close="1">Close</button></div>`, (m) => {
        convView = conversationView($('#sr-conv', m), ns.conv, {});
        m.addEventListener('click', (ev) => { if (ev.target.closest('[data-close]')) { convView?.destroy(); convView = null; closeModal(); } });
      }, true);
    } else if (act === 'details') {
      await openRunModal(el.dataset.run);
    }
  });
}
