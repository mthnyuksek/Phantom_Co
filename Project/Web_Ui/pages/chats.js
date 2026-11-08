import { api } from '../api.js';
import { S, agentById, agentGhost, userById, projectById, canEdit, me, userGhost } from '../state.js';
import { $, esc, guard, ago, modal, closeModal, twoStep, toast } from '../ui.js';
import { conversationView } from '../chat_view.js';

let view, chats = [], filter = 'mine', query = '', selected = '', conv = null, ctxRef = null;

const mineOnly = (c) => c.user_id === me().id;
function visibleChats() {
  const q = query.toLowerCase();
  return chats.filter((c) => {
    if (filter === 'mine' && !(c.kind === 'user' && mineOnly(c))) return false;
    if (filter === 'shared' && !(c.kind === 'user' && c.shared && !mineOnly(c))) return false;
    if (filter === 'agents' && c.kind !== 'agents') return false;
    return !q || (c.title + ' ' + c.preview).toLowerCase().includes(q);
  });
}

function chatAvatar(c) {
  if (c.kind === 'agents') {
    const [x, y] = (c.participants || []).map(agentById);
    return `<div class="duo"><span class="mini">${agentGhost(x, { mood: 'idle', size: 26 })}</span><span class="mini">${agentGhost(y, { mood: 'idle', size: 26 })}</span></div>`;
  }
  return `<div class="mini">${agentGhost(agentById(c.agent_id), { mood: c.busy ? 'working' : 'idle', size: 34 })}</div>`;
}

function listHTML() {
  const rows = visibleChats();
  return rows.map((c) => {
    const by = c.kind === 'user' && !mineOnly(c) ? userById(c.user_id)?.name : '';
    return `<a class="chat-item ${c.id === selected ? 'on' : ''}" href="#/chats/${esc(c.id)}">${chatAvatar(c)}
      <div class="ci-body"><div class="ci-title">${esc(c.title)}${c.busy ? ' <span class="dot-busy"></span>' : ''}</div>
      <div class="ci-prev">${by ? '<b>' + esc(by) + '</b> · ' : ''}${esc(c.preview || 'No messages yet')}</div></div>
      <div class="ci-time">${esc(ago(c.updated))}${c.shared && c.kind === 'user' ? ' 👥' : ''}</div></a>`;
  }).join('') || '<div class="empty small">No chats here.</div>';
}

const exportBtn = (m) => `<a class="btn small" href="/api/chats/${esc(m.id)}/export" download title="Save this conversation as a Markdown file">⬇ Export</a>`;

function paintList() { const el = $('#chat-list', view); if (el) el.innerHTML = listHTML(); }

function paintHeader(d) {
  const el = $('#chat-head', view);
  if (!el || !d) return;
  const m = d.meta, own = m.user_id === me().id || me().role === 'owner';
  const back = '<a class="btn small back-btn" href="#/chats" aria-label="Back to chats">←</a>';  // shown on phones only
  if (m.kind === 'agents') {
    const [x, y] = (m.participants || []).map(agentById);
    el.innerHTML = `${back}<div class="grow"><h2>${esc(m.title)}</h2><div class="meta">${esc(x?.name || '?')} ↔ ${esc(y?.name || '?')} · agents talking to each other${m.user_id ? ' for ' + esc(userById(m.user_id)?.name || 'someone') : ''}</div></div>${exportBtn(m)}`;
    return;
  }
  const a = agentById(m.agent_id), p = projectById(m.project_id), u = userById(m.user_id);
  el.innerHTML = `${back}<div class="mini" style="--gc:${esc(a?.color || '#888')}">${agentGhost(a, { mood: d.busy ? 'working' : 'idle', size: 44 })}</div>
    <div class="grow"><h2 id="ch-title" title="${own && canEdit() ? 'Click to rename' : ''}">${esc(m.title)}</h2>
      <div class="meta">${esc(a?.name || 'removed ghost')}${a ? ' · ' + esc(a.role) : ''}${u && u.id !== me().id ? ' · chat of ' + esc(u.name) : ''}</div></div>
    ${canEdit() && own ? `<select id="ch-project" title="Project"><option value="">No project</option>${S.state.projects.map((x) => `<option value="${esc(x.id)}" ${x.id === m.project_id ? 'selected' : ''}>📁 ${esc(x.name)}</option>`).join('')}</select>
      ${m.kind === 'user' ? `<label class="check share"><input type="checkbox" id="ch-share" ${m.shared ? 'checked' : ''}> Share with team</label>` : ''}
      <button class="btn small" data-act="reset" title="Make the agent forget this talk">New session</button>
      ${exportBtn(m)}<button class="btn small danger" data-act="del">Delete</button>` : (p ? `<span class="chip ok">📁 ${esc(p.name)}</span>` : '') + exportBtn(m)}`;
}

async function open(id) {
  conv?.destroy(); conv = null;
  selected = id;
  paintList();
  const pane = $('#chat-pane', view);
  $('.chats-layout', view)?.classList.toggle('has-chat', !!id);  // phones show either the list or the open chat
  if (!id) { pane.innerHTML = `<div class="empty big"><div class="mini">${userGhost(me(), 90)}</div><p>Pick a chat on the left or start a new one.</p></div>`; return; }
  pane.innerHTML = `<div class="chat-head" id="chat-head"></div><div id="chat-conv" class="chat-conv"></div>`;
  conv = conversationView($('#chat-conv', pane), id, { onData: (d) => { paintHeader(d); } });
}

export function openNewChatModal(projectId = '') {
  const agents = S.state.agents.filter((a) => a.enabled);
  if (!agents.length) return toast('Hire a ghost first.', true);
  let pick = agents[0].id;
  modal(`<h2>New chat</h2><p class="sub">Who do you want to talk to?</p>
    <div class="pick-grid">${agents.map((a) => `<button class="pick ${a.id === pick ? 'on' : ''}" data-pick="${esc(a.id)}" style="--gc:${esc(a.color)}"><div class="mini">${agentGhost(a, { mood: 'idle', size: 56 })}</div><b>${esc(a.name)}</b><small>${esc(a.role)}</small></button>`).join('')}</div>
    <label>Project (optional)</label><select id="nc-project"><option value="">No project</option>${S.state.projects.map((p) => `<option value="${esc(p.id)}" ${p.id === projectId ? 'selected' : ''}>${esc(p.name)}</option>`).join('')}</select>
    <div class="modal-foot"><button class="btn" id="nc-cancel">Cancel</button><button class="btn primary" id="nc-go">Start chat</button></div>`,
  (m) => m.addEventListener('click', guard(async (e) => {
    const p = e.target.closest('[data-pick]');
    if (p) { pick = p.dataset.pick; m.querySelectorAll('.pick').forEach((x) => x.classList.toggle('on', x === p)); return; }
    if (e.target.id === 'nc-cancel') return closeModal();
    if (e.target.id === 'nc-go') {
      const c = await api('POST', '/api/chats', { agent_id: pick, project_id: $('#nc-project', m).value });
      closeModal(); location.hash = '#/chats/' + c.meta.id;
    }
  })));
}

export async function render(v, arg, ctx) {
  view = v; ctxRef = ctx;
  chats = (await api('GET', '/api/chats')).chats;
  const owner = me().role === 'owner';
  view.innerHTML = `<div class="chats-layout">
    <div class="chat-side">
      <div class="chat-side-head"><h1>Chats</h1>${canEdit() ? '<button class="btn small primary" data-act="new">+ New</button>' : ''}</div>
      <div class="tabs"><span class="tab ${filter === 'mine' ? 'on' : ''}" data-f="mine">Mine</span><span class="tab ${filter === 'shared' ? 'on' : ''}" data-f="shared">Shared</span><span class="tab ${filter === 'agents' ? 'on' : ''}" data-f="agents">Agents</span></div>
      <input type="text" id="chat-q" placeholder="Search…" value="${esc(query)}">
      <div class="chat-list" id="chat-list"></div>
    </div>
    <div class="chat-pane" id="chat-pane"></div></div>`;
  await open(arg);
  view.onclick = guard(async (e) => {
    const f = e.target.closest('[data-f]');
    if (f) { filter = f.dataset.f; view.querySelectorAll('[data-f]').forEach((x) => x.classList.toggle('on', x === f)); paintList(); return; }
    const t = e.target.closest('#ch-title');
    if (t && canEdit() && !$('#ch-title-in', view)) {  // click the title to rename it (no browser dialog)
      const input = document.createElement('input');
      input.id = 'ch-title-in'; input.type = 'text'; input.maxLength = 80; input.value = t.textContent; input.className = 'title-edit';
      t.replaceWith(input); input.focus(); input.select();
      let done = false;
      const finish = guard(async (save) => {
        if (done) return; done = true;
        if (save && input.value.trim() && input.value.trim() !== t.textContent) await api('PUT', '/api/chats/' + selected, { title: input.value.trim() });
        conv?.reload(); paintHeader(conv?.data);
      });
      input.onkeydown = (ev) => { if (ev.key === 'Enter') finish(true); if (ev.key === 'Escape') finish(false); };
      input.onblur = () => finish(true);
      return;
    }
    const el = e.target.closest('[data-act]');
    if (!el) return;
    if (el.dataset.act === 'new') return openNewChatModal();
    if (el.dataset.act === 'reset') { await api('POST', `/api/chats/${selected}/reset`, {}); conv.reload(); toast('Fresh start'); }
    if (el.dataset.act === 'del') twoStep(el, 'Really delete?', async () => { await api('DELETE', '/api/chats/' + selected); toast('Chat deleted'); selected = ''; location.hash = '#/chats'; });
  });
  view.oninput = (e) => { if (e.target.id === 'chat-q') { query = e.target.value; paintList(); } };
  view.onchange = guard(async (e) => {
    if (e.target.id === 'ch-project') { await api('PUT', '/api/chats/' + selected, { project_id: e.target.value }); toast('Project set'); }
    if (e.target.id === 'ch-share') { await api('PUT', '/api/chats/' + selected, { shared: e.target.checked }); toast(e.target.checked ? 'Shared with the team' : 'Private again'); }
  });
  ctx.destroy = () => { conv?.destroy(); conv = null; };
}

export async function tick() {
  const c = (await api('GET', '/api/chats')).chats;
  if (JSON.stringify(c) !== JSON.stringify(chats)) { chats = c; paintList(); }
}
