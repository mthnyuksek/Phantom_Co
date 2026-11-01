import { api } from '../api.js';
import { S, agentById, agentGhost, projectById, userById, canEdit, me, isOwner } from '../state.js';
import { $, esc, guard, ago, sleep, pill, moodOfStatus, modal, closeModal, twoStep, duration, toast } from '../ui.js';
import { conversationView } from '../chat_view.js';
import { openRunModal } from './runs.js';

let view, list = [];

// "Wakes every 30 min" / "Wakes every day at 09:00" / "" when the task has no wake-up
const wakeText = (t) => !t.wake ? '' : (t.wake_on ? '' : '⏸ ') + (t.wake === 'interval' ? `Wakes every ${t.wake_every} min` : `Wakes every day at ${t.wake_at}`);

function openWakeModal(t, done) {
  modal(`<h2>Wake-up</h2><p class="sub">The task runs again by itself. It uses the rights of the person who made it and is skipped while it is still running. It only wakes while Phantom Co. is open.</p>
    <label>Repeat</label><select id="w-kind"><option value="none">Never</option><option value="interval">Every few minutes</option><option value="daily">Every day at a time</option></select>
    <div id="w-int"><label>Every (minutes)</label><input type="number" id="w-every" min="1" max="10080" value="${esc(t.wake_every || 60)}"></div>
    <div id="w-day"><label>Time (this PC's clock)</label><input type="time" id="w-at" value="${esc(t.wake_at || '09:00')}"></div>
    <label class="check" id="w-onrow"><input type="checkbox" id="w-on" ${t.wake && !t.wake_on ? '' : 'checked'}> Wake-up is on</label>
    <div class="modal-foot"><button class="btn" id="w-cancel">Cancel</button><button class="btn primary" id="w-ok">Save</button></div>`,
  (m) => {
    const sync = () => {
      const k = $('#w-kind', m).value;
      $('#w-int', m).hidden = k !== 'interval';
      $('#w-day', m).hidden = k !== 'daily';
      $('#w-onrow', m).hidden = k === 'none';
    };
    $('#w-kind', m).value = t.wake || 'none';
    sync();
    $('#w-kind', m).onchange = sync;
    m.addEventListener('click', guard(async (e) => {
      if (e.target.id === 'w-cancel') return closeModal();
      if (e.target.id !== 'w-ok') return;
      const kind = $('#w-kind', m).value;
      const r = await api('PUT', `/api/tasks/${t.id}/wake`, { wake: kind, every_min: +$('#w-every', m).value, at: $('#w-at', m).value, on: $('#w-on', m).checked });
      closeModal(); toast('Wake-up saved'); done(r);
    }));
  });
}

function row(t) {
  const a = agentById(t.agent_id), p = projectById(t.project_id), u = userById(t.created_by);
  return `<a class="row" href="#/tasks/${esc(t.id)}"><div class="mini">${agentGhost(a, { mood: moodOfStatus(t.status), size: 34 })}</div>
    <div><div class="title">${esc(t.title)}</div><div class="meta">${esc(a ? a.name : 'removed ghost')}${p ? ' · 📁 ' + esc(p.name) : ''}${u ? ' · by ' + esc(u.name) : ''}</div>${t.wake ? `<div class="meta">⏰ ${esc(wakeText(t))}</div>` : ''}</div>
    <div class="meta hide-s">${esc(ago(t.created))}</div><div>${pill(t.status)}</div><div class="hide-s"></div></a>`;
}

export function openTaskModal(agentId, projectId) {
  const st = S.state;
  const agents = st.agents.filter((a) => a.enabled);
  if (!agents.length) return toast('Hire a ghost first.', true);
  modal(`<h2>New task</h2><p class="sub">Describe the job. The ghost works in the project folder, or in the shared files folder.</p>
    <label>Title</label><input type="text" id="t-title" maxlength="120" placeholder="e.g. Write a short project plan">
    <label>Details</label><textarea id="t-desc" style="min-height:140px" placeholder="What exactly should be done? What should the result look like?"></textarea>
    <div class="two"><div><label>Give it to</label><select id="t-agent">${agents.map((a) => `<option value="${esc(a.id)}" ${a.id === agentId ? 'selected' : ''}>${esc(a.name)} — ${esc(a.role)}</option>`).join('')}</select></div>
      <div><label>Project</label><select id="t-project"><option value="">No project (shared files)</option>${st.projects.map((p) => `<option value="${esc(p.id)}" ${p.id === projectId ? 'selected' : ''}>${esc(p.name)}</option>`).join('')}</select></div></div>
    <label class="check"><input type="checkbox" id="t-run" checked> Start right away</label>
    <div class="modal-foot"><button class="btn" id="t-cancel">Cancel</button><button class="btn primary" id="t-ok">Create task</button></div>`,
  (m) => {
    $('#t-title', m).focus();
    m.addEventListener('click', guard(async (e) => {
      if (e.target.id === 't-cancel') return closeModal();
      if (e.target.id !== 't-ok') return;
      const t = await api('POST', '/api/tasks', { title: $('#t-title', m).value.trim(), description: $('#t-desc', m).value, agent_id: $('#t-agent', m).value,
        project_id: $('#t-project', m).value, run: $('#t-run', m).checked });
      closeModal(); location.hash = '#/tasks/' + t.id;
    }));
  });
}

// ----------------------------------------------------------------- task page
async function renderTask(id, ctx) {
  let d;
  try { d = await api('GET', '/api/tasks/' + id); } catch (e) { view.innerHTML = `<div class="empty">${esc(e.message)}</div>`; return; }
  let conv;
  const head = () => {
    const t = d.task, a = agentById(t.agent_id), p = projectById(t.project_id), u = userById(t.created_by);
    const active = t.status === 'running' || t.status === 'queued';
    $('#th', view).innerHTML = `<div class="mini" style="--gc:${esc(a?.color || '#888')}">${agentGhost(a, { mood: moodOfStatus(t.status), size: 62 })}</div>
      <div><h1>${esc(t.title)}</h1><div class="meta">${esc(a ? a.name + ' · ' + a.role : 'removed ghost')}${p ? ' · 📁 <a href="#/projects/' + esc(p.id) + '">' + esc(p.name) + '</a>' : ''}${u ? ' · by ' + esc(u.name) : ''} · ${esc(ago(t.created))}</div>${t.wake ? `<div class="meta">⏰ ${esc(wakeText(t))}</div>` : ''}</div>
      ${pill(t.status)}<div class="spacer"></div>
      ${canEdit() ? `<button class="btn" data-act="wake">⏰ Wake-up</button> ${active ? '<button class="btn" data-act="stop">Stop</button>' : '<button class="btn primary" data-act="rerun">Run again</button>'}
      <button class="btn danger" data-act="del" ${active ? 'disabled' : ''}>Delete</button>` : ''}`;
    $('#runs', view).innerHTML = d.runs.length ? `<div class="table">${d.runs.slice().reverse().map((r) => {
      const ag = agentById(r.agent_id);
      return `<div class="row run-row" data-act="run" data-id="${esc(r.id)}"><div class="mini">${agentGhost(ag, { size: 34 })}</div>
        <div><div class="title">${esc(r.kind)} · ${esc(ag?.name || r.agent_id)}</div><div class="meta">${esc(r.provider)} · ${esc(duration(r.started, r.ended))}</div></div>
        <div class="meta hide-s">${r.tokens_in || r.tokens_out ? (r.tokens_in + r.tokens_out).toLocaleString() + ' tokens' : ''}</div><div>${pill(r.status)}</div><div class="hide-s"></div></div>`;
    }).join('')}</div>` : '';
  };
  view.innerHTML = `<a class="back" href="#/tasks">← All tasks</a><div class="task-head" id="th"></div><div id="conv"></div>
    <h3 class="section-title">Runs</h3><div id="runs"></div>`;
  head();
  conv = conversationView($('#conv', view), d.task.conversation_id, { placeholder: 'Add a note — the ghost runs the task again with it…' });
  const refresh = async () => { while (ctx.isCurrent()) { await sleep(2000); try { d = await api('GET', '/api/tasks/' + id); if (ctx.isCurrent()) head(); } catch { return; } } };
  refresh();
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    if (el.dataset.act === 'run') return openRunModal(el.dataset.id);
    if (el.dataset.act === 'wake') return openWakeModal(d.task, (r) => { d.task = r; head(); });
    if (el.dataset.act === 'rerun') { await api('POST', `/api/tasks/${id}/run`, {}); conv.reload(); }
    if (el.dataset.act === 'stop') { await api('POST', `/api/chats/${d.task.conversation_id}/stop`, {}); }
    if (el.dataset.act === 'del') twoStep(el, 'Really delete?', async () => { await api('DELETE', '/api/tasks/' + id); toast('Task deleted'); location.hash = '#/tasks'; });
  });
  ctx.destroy = () => conv.destroy();
}

export async function render(v, arg, ctx) {
  view = v;
  if (arg) { await renderTask(arg, ctx); return; }
  list = (await api('GET', '/api/tasks')).tasks;
  draw();
}
function draw() {
  view.innerHTML = `<div class="page-head"><div><h1>Tasks</h1><p>Work your ghosts were asked to do. Every task keeps its own conversation.</p></div><div class="spacer"></div>
    ${canEdit() ? '<button class="btn primary" data-act="new">+ New task</button>' : ''}</div>
    <div class="table">${list.length ? list.map(row).join('') : '<div class="empty">No tasks yet. Create one and give it to a ghost.</div>'}</div>`;
  view.onclick = guard((e) => { if (e.target.closest('[data-act=new]')) openTaskModal(); });
}
export async function tick() {
  if (location.hash.split('/').length > 2) return;
  const t = (await api('GET', '/api/tasks')).tasks;
  if (JSON.stringify(t) !== JSON.stringify(list)) { list = t; draw(); }
}
