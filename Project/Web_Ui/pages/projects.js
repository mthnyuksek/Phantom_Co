import { api } from '../api.js';
import { S, projectById, agentById, agentGhost, canEdit, isOwner } from '../state.js';
import { $, esc, guard, modal, closeModal, twoStep, toast, ago } from '../ui.js';
import { openNewChatModal } from './chats.js';
import { openTaskModal } from './tasks.js';

let view, selected = '';

function listHTML() {
  return S.state.projects.map((p) => `<a class="chat-item ${p.id === selected ? 'on' : ''}" href="#/projects/${esc(p.id)}"><div class="mini big-emoji">📁</div>
    <div class="ci-body"><div class="ci-title">${esc(p.name)}</div><div class="ci-prev">${esc(p.summary || 'No summary')}</div></div>
    <div class="ci-time">${p.notes.length} note${p.notes.length === 1 ? '' : 's'}</div></a>`).join('') || '<div class="empty small">No projects yet.</div>';
}

function detail(p) {
  if (!p) return `<div class="empty big"><p>Projects give agents a shared folder and shared memory.<br>Pick one on the left${canEdit() ? ' or create a new one' : ''}.</p></div>`;
  const ro = !canEdit();
  return `<div class="detail-pad">
    <div class="page-head"><div><h1>📁 ${esc(p.name)}</h1><p>Work in this project happens in <code>Projects/${esc(p.id)}</code> inside the Vault.</p></div><div class="spacer"></div>
      <a class="btn" href="#/files/project:${esc(p.id)}">Files</a>
      ${canEdit() ? `<button class="btn" data-act="chat">Chat here</button><button class="btn" data-act="task">New task</button>` : ''}</div>
    <div class="card"><h3>About</h3>
      <label>Name</label><input type="text" id="pj-name" maxlength="60" value="${esc(p.name)}" ${ro ? 'disabled' : ''}>
      <label>Summary <span class="hint-inline">every ghost working here reads this</span></label><textarea id="pj-summary" maxlength="4000" ${ro ? 'disabled' : ''}>${esc(p.summary)}</textarea>
      ${ro ? '' : `<div class="modal-foot"><button class="btn danger ${isOwner() ? '' : 'hide'}" data-act="del" style="margin-right:auto">Remove project</button><button class="btn primary" data-act="save">Save</button></div>`}</div>
    <div class="card wide"><div class="card-head"><h3>Notes — shared memory</h3>${ro ? '' : '<button class="btn small" data-act="note-new">+ Add note</button>'}</div>
      <p class="hint">Notes that are switched on are sent to every ghost working in this project. Ghosts with the Project notes skill can write notes too.</p>
      ${p.notes.map((n) => `<div class="note ${n.enabled ? '' : 'off'}"><div class="note-top"><b>${esc(n.name)}</b>
        ${ro ? '' : `<label class="check"><input type="checkbox" data-act="note-toggle" data-id="${esc(n.id)}" ${n.enabled ? 'checked' : ''}> on</label>
        <button class="btn small" data-act="note-edit" data-id="${esc(n.id)}">Edit</button>`}</div><div class="note-body">${esc(n.body)}</div></div>`).join('') || '<div class="empty small">No notes yet.</div>'}</div>
    ${rolesCard(p, ro)}</div>`;
}

// What each ghost does in this project: added to that ghost's prompt only while it works here.
function rolesCard(p, ro) {
  const roles = p.roles || {};
  const ids = Object.keys(roles).filter((id) => agentById(id));
  return `<div class="card wide"><div class="card-head"><h3>Ghost jobs in this project</h3>${ro ? '' : '<button class="btn small" data-act="role-new">+ Set a job</button>'}</div>
    <p class="hint">A ghost keeps its persona everywhere. The job written here is added to its instructions only while it works in this project, and its colleagues see the first line.</p>
    ${ids.map((id) => { const a = agentById(id); return `<div class="note"><div class="note-top"><div class="mini">${agentGhost(a, { size: 28 })}</div><b>${esc(a.name)}</b>
      ${ro ? '' : `<button class="btn small" data-act="role-edit" data-id="${esc(id)}">Edit</button>`}</div><div class="note-body">${esc(roles[id])}</div></div>`; }).join('') || '<div class="empty small">No jobs yet. Every ghost works here with its usual role.</div>'}</div>`;
}

function roleModal(p, agentId) {
  const agents = S.state.agents.filter((a) => a.id === agentId || !(p.roles || {})[a.id]);
  if (!agents.length) return toast('Every ghost already has a job here.', true);
  modal(`<h2>Ghost job in ${esc(p.name)}</h2>
    <label>Ghost</label><select id="r-agent" ${agentId ? 'disabled' : ''}>${agents.map((a) => `<option value="${esc(a.id)}" ${a.id === agentId ? 'selected' : ''}>${esc(a.name)} — ${esc(a.role)}</option>`).join('')}</select>
    <label>Job in this project <span class="hint-inline">the first line is shown to colleagues</span></label>
    <textarea id="r-text" style="min-height:140px" maxlength="4000" placeholder="e.g. Writes the scripts of the videos. Hands finished scripts to the editor.">${esc((p.roles || {})[agentId] || '')}</textarea>
    <div class="modal-foot">${agentId ? '<button class="btn danger" id="r-del" style="margin-right:auto">Remove</button>' : ''}<button class="btn" id="r-cancel">Cancel</button><button class="btn primary" id="r-save">Save</button></div>`,
  (m) => m.addEventListener('click', guard(async (e) => {
    if (e.target.id === 'r-cancel') return closeModal();
    const id = $('#r-agent', m).value;
    if (e.target.id === 'r-del') return twoStep(e.target, 'Really remove?', async () => { await api('PUT', `/api/projects/${p.id}/roles/${id}`, { text: '' }); closeModal(); window.dispatchEvent(new Event('pc2:changed')); });
    if (e.target.id !== 'r-save') return;
    await api('PUT', `/api/projects/${p.id}/roles/${id}`, { text: $('#r-text', m).value });
    closeModal(); toast('Saved'); window.dispatchEvent(new Event('pc2:changed'));
  })));
}

function paint() {
  $('#md-list', view).innerHTML = listHTML();
  $('#md-detail', view).innerHTML = detail(projectById(selected));
}

function noteModal(p, note) {
  modal(`<h2>${note ? 'Edit note' : 'New note'}</h2>
    <label>Title</label><input type="text" id="n-name" maxlength="60" value="${esc(note?.name || '')}">
    <label>Note</label><textarea id="n-body" style="min-height:140px" maxlength="4000">${esc(note?.body || '')}</textarea>
    <div class="modal-foot">${note ? '<button class="btn danger" id="n-del" style="margin-right:auto">Delete</button>' : ''}<button class="btn" id="n-cancel">Cancel</button><button class="btn primary" id="n-save">Save</button></div>`,
  (m) => m.addEventListener('click', guard(async (e) => {
    if (e.target.id === 'n-cancel') return closeModal();
    if (e.target.id === 'n-del') return twoStep(e.target, 'Really delete?', async () => { await api('DELETE', `/api/projects/${p.id}/notes/${note.id}`); closeModal(); window.dispatchEvent(new Event('pc2:changed')); });
    if (e.target.id !== 'n-save') return;
    const body = { name: $('#n-name', m).value.trim(), body: $('#n-body', m).value };
    if (note) await api('PUT', `/api/projects/${p.id}/notes/${note.id}`, body); else await api('POST', `/api/projects/${p.id}/notes`, body);
    closeModal(); window.dispatchEvent(new Event('pc2:changed'));
  })));
}

// HTML list of what removing project p deletes (shown while hovering "Really remove?")
async function removalList(p) {
  const tasks = ((await api('GET', '/api/tasks')).tasks || []).filter((t) => t.project_id === p.id);
  const roles = Object.keys(p.roles || {});
  const sec = (title, items) => items.length ? `<b>${title}</b><ul>${items.map((x) => `<li>${esc(x)}</li>`).join('')}</ul>` : '';
  return sec('Project', [p.name])
    + sec(`Tasks (${tasks.length})`, tasks.map((t) => t.title || 'Untitled task'))
    + sec(`Notes (${p.notes.length})`, p.notes.map((n) => n.name || n.id))
    + sec(`Ghost jobs (${roles.length})`, roles.map((id) => agentById(id)?.name || id))
    + '<p class="keep">The task chats go too. The files in the project folder stay on disk.</p>';
}

function newProjectModal() {
  modal(`<h2>New project</h2><p class="sub">A project is a folder for the work plus shared notes the ghosts remember.</p>
    <label>Name</label><input type="text" id="np-name" maxlength="60" autofocus><label>Summary</label><textarea id="np-sum" maxlength="4000" placeholder="What is this project about?"></textarea>
    <div class="modal-foot"><button class="btn" id="np-cancel">Cancel</button><button class="btn primary" id="np-go">Create</button></div>`,
  (m) => m.addEventListener('click', guard(async (e) => {
    if (e.target.id === 'np-cancel') return closeModal();
    if (e.target.id !== 'np-go') return;
    const p = await api('POST', '/api/projects', { name: $('#np-name', m).value.trim(), summary: $('#np-sum', m).value });
    closeModal(); location.hash = '#/projects/' + p.id;
  })));
}

export async function render(v, arg) {
  view = v; selected = arg || '';
  view.innerHTML = `<div class="chats-layout"><div class="chat-side"><div class="chat-side-head"><h1>Projects</h1>${canEdit() ? '<button class="btn small primary" data-act="new">+ New</button>' : ''}</div>
    <div class="chat-list" id="md-list"></div></div><div class="chat-pane scroll" id="md-detail"></div></div>`;
  paint();
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    const p = projectById(selected), act = el.dataset.act;
    if (act === 'new') newProjectModal();
    else if (act === 'save') { await api('PUT', '/api/projects/' + p.id, { name: $('#pj-name', view).value, summary: $('#pj-summary', view).value }); toast('Saved'); window.dispatchEvent(new Event('pc2:changed')); }
    else if (act === 'del') {
      const list = el.dataset.sure ? '' : await removalList(p);
      twoStep(el, 'Really remove?', async () => {
        const r = await api('DELETE', '/api/projects/' + p.id);
        toast(r.tasks_removed ? `Project and ${r.tasks_removed} task(s) removed (its files stay on disk)` : 'Project removed (its files stay on disk)');
        location.hash = '#/projects'; window.dispatchEvent(new Event('pc2:changed'));
      }, list);
    }
    else if (act === 'chat') openNewChatModal(p.id);
    else if (act === 'task') openTaskModal(undefined, p.id);
    else if (act === 'note-new') noteModal(p, null);
    else if (act === 'note-edit') noteModal(p, p.notes.find((n) => n.id === el.dataset.id));
    else if (act === 'role-new') roleModal(p, '');
    else if (act === 'role-edit') roleModal(p, el.dataset.id);
  });
  view.onchange = guard(async (e) => {
    if (e.target.dataset.act === 'note-toggle') {
      await api('PUT', `/api/projects/${selected}/notes/${e.target.dataset.id}`, { enabled: e.target.checked });
      window.dispatchEvent(new Event('pc2:changed'));
    }
  });
}
