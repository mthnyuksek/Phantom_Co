import { api } from '../api.js';
import { S, FORMS, agentById, agentGhost, canEdit, providerLabel } from '../state.js';
import { $, esc, guard, ago, sleep, pill, moodOfStatus } from '../ui.js';
import { openAgentModal } from '../agent_editor.js';
import { openTaskModal } from './tasks.js';
import { canHireGuide, offerGuide } from '../guide.js';

let view, tasks = [];

// agents in company order: bosses first, their reports indented below them
function ordered() {
  const all = S.state.agents, byBoss = {};
  all.forEach((a) => { const b = all.some((x) => x.id === a.reports_to) ? a.reports_to : ''; (byBoss[b] ||= []).push(a); });
  const out = [], walk = (boss, depth) => (byBoss[boss] || []).forEach((a) => { out.push({ a, depth }); walk(a.id, depth + 1); });
  walk('', 0);
  return out;
}

// 842 -> "842", 12345 -> "12.3k", 2400000 -> "2.4M"
const compact = (n) => n < 1000 ? String(n) : n < 1e6 ? (n / 1000).toFixed(n < 1e4 ? 1 : 0).replace(/\.0$/, '') + 'k' : (n / 1e6).toFixed(1).replace(/\.0$/, '') + 'M';

function card({ a, depth }) {
  const act = a.activity;
  const mood = act?.state === 'working' ? 'working' : act?.state === 'queued' ? 'queued' : 'idle';
  const boss = agentById(a.reports_to);
  const status = !a.enabled ? '⏸ Switched off' : act ? (act.state === 'working' ? '⚡ Working…' : '💤 Waiting in line') : 'Idle — ready for work';
  const prov = S.state.providers.find((p) => p.id === a.provider.split('#')[0]);
  return `<div class="gcard ${mood === 'working' ? 'working' : ''} ${a.enabled ? '' : 'off'}" style="--gc:${esc(a.color)}" data-card="${esc(a.id)}">
    <div class="stage"><div class="shadow"></div>${agentGhost(a, { mood, size: 112 })}</div>
    <div class="gname">${esc(a.name)}</div><div class="grole">${esc(a.role)}</div>
    ${boss ? `<div class="gboss">↳ reports to ${esc(boss.name)}</div>` : ''}
    <span class="badge" title="${prov && !prov.available ? 'Not installed on this PC' : ''}">${esc(providerLabel(a.provider))}${prov && !prov.available ? ' ⚠' : ''}</span>
    <div class="gskills">${a.skills.length ? a.skills.length + ' skill' + (a.skills.length > 1 ? 's' : '') : 'no skills'}</div>
    <div class="gtokens" title="Tokens used today (read + written), from the finished runs of this PC's calendar day">🪙 ${esc(compact(a.tokens_today || 0))} tokens today</div>
    <div class="gstatus ${act?.state === 'working' ? 'working' : ''}">${status}</div>
    ${canEdit() ? `<div class="gactions">
      <button class="btn small primary" data-act="chat" data-id="${esc(a.id)}">Chat</button>
      <button class="btn small" data-act="give" data-id="${esc(a.id)}">Task</button>
      <button class="btn small" data-act="shift" data-id="${esc(a.id)}" title="Change form">✨</button>
      <button class="btn small" data-act="edit" data-id="${esc(a.id)}">Edit</button></div>` : ''}</div>`;
}

function draw() {
  const c = S.state.company, list = ordered();
  view.innerHTML = `
    <div class="page-head"><div><h1>The Haunt</h1><p>Your ghost company. Chat with ghosts, give them tasks and watch them work together.</p></div>
      <div class="spacer"></div>
      ${canHireGuide() ? '<button class="btn" data-act="guide">🧭 <span>Hire the guide</span></button>' : ''}${canEdit() ? `<button class="btn" data-act="new-task">+ New task</button><button class="btn primary" data-act="hire">+ Hire a ghost</button>` : ''}</div>
    <div class="company-box"><b>${esc(c.name)}</b><div class="goal">${esc(c.goal) || 'No company goal yet.'}</div></div>
    ${list.length ? '' : `<div class="empty">👻 Your haunt is empty. ${canEdit() ? 'Hire your first ghost to get started.' : 'Ask an editor or the owner to hire ghosts.'}</div>`}
    <div class="grid">${list.map(card).join('')}${canEdit() ? `<button class="add-card" data-act="hire"><span class="plus">＋</span>Hire a ghost</button>` : ''}</div>
    <h3 class="section-title">Recent tasks</h3>
    <div class="table">${tasks.length ? tasks.slice(0, 6).map((t) => {
      const a = agentById(t.agent_id);
      return `<a class="row" href="#/tasks/${esc(t.id)}"><div class="mini">${agentGhost(a, { mood: moodOfStatus(t.status), size: 34 })}</div>
        <div><div class="title">${esc(t.title)}</div><div class="meta">${esc(a ? a.name : 'removed ghost')}</div></div>
        <div class="meta hide-s">${esc(ago(t.created))}</div><div>${pill(t.status)}</div><div class="hide-s"></div></a>`;
    }).join('') : '<div class="empty">No tasks yet.</div>'}</div>`;
}

export async function render(v) {
  view = v;
  tasks = (await api('GET', '/api/tasks')).tasks;
  draw();
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    const act = el.dataset.act, id = el.dataset.id;
    if (act === 'hire') openAgentModal(null);
    else if (act === 'guide') offerGuide();
    else if (act === 'new-task') openTaskModal();
    else if (act === 'give') openTaskModal(id);
    else if (act === 'edit') openAgentModal(agentById(id));
    else if (act === 'chat') { const c = await api('POST', '/api/chats', { agent_id: id }); location.hash = '#/chats/' + c.meta.id; }
    else if (act === 'shift') {
      const a = agentById(id), next = FORMS[(FORMS.findIndex((f) => f.id === a.form) + 1) % FORMS.length].id;
      $(`[data-card="${id}"] .ghost`, view)?.classList.add('poof');
      await sleep(260);
      await api('PUT', '/api/agents/' + id, { form: next });
      window.dispatchEvent(new Event('pc2:changed'));
    }
  });
}

export async function tick(changed) {
  const t = (await api('GET', '/api/tasks')).tasks;
  if (changed || JSON.stringify(t) !== JSON.stringify(tasks)) { tasks = t; draw(); }
}
