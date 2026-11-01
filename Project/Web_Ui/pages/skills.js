import { api } from '../api.js';
import { S, skillById, isOwner, canEdit } from '../state.js';
import { $, esc, guard, modal, closeModal, twoStep, toast } from '../ui.js';

let view, selected = '';
const TOOL_HELP = { Read: 'read files', Glob: 'find files', Grep: 'search in files', Write: 'create files', Edit: 'change files', Bash: 'run commands', PowerShell: 'run commands', WebSearch: 'search the web', WebFetch: 'read web pages' };
const DIR_HELP = { Agents: 'ghost definitions', Skills: 'skills', Rules: 'company rules', Projects: 'projects and notes' };

const listHTML = () => S.state.skills.map((s) => `<a class="chat-item ${s.id === selected ? 'on' : ''}" href="#/skills/${esc(s.id)}"><div class="mini big-emoji">🧰</div>
  <div class="ci-body"><div class="ci-title">${esc(s.name)}${s.min_role === 'owner' ? ' <span class="badge">owner</span>' : ''}${s.enabled ? '' : ' <span class="badge">off</span>'}</div>
  <div class="ci-prev">${esc(s.description)}</div></div><div class="ci-time">${s.tools.length} tool${s.tools.length === 1 ? '' : 's'}</div></a>`).join('') || '<div class="empty small">No skills yet.</div>';

function detail(s) {
  if (!s) return `<div class="empty big"><p>Skills are the only way a ghost gets tools.<br>${isOwner() ? 'Pick one on the left, or add one from a template.' : 'Pick one on the left to read it.'}</p></div>`;
  const ro = !isOwner(), users = S.state.agents.filter((a) => a.skills.includes(s.id));
  return `<div class="detail-pad"><div class="page-head"><div><h1>🧰 ${esc(s.name)}</h1><p>Used by ${users.length ? users.map((a) => esc(a.name)).join(', ') : 'nobody yet'}.</p></div></div>
    <div class="card wide"><label>Name</label><input type="text" id="sk-name" maxlength="60" value="${esc(s.name)}" ${ro ? 'disabled' : ''}>
      <label>Short description</label><input type="text" id="sk-desc" maxlength="200" value="${esc(s.description)}" ${ro ? 'disabled' : ''}>
      <label>Tools <span class="hint-inline">each built-in tool costs about 900 tokens per run</span></label>
      <div class="skill-list">${S.state.tools.map((t) => `<label class="check"><input type="checkbox" data-tool="${t}" ${s.tools.includes(t) ? 'checked' : ''} ${ro ? 'disabled' : ''}> ${t} <span class="hint-inline">${esc(TOOL_HELP[t] || '')}</span></label>`).join('')}</div>
      <label>May touch these parts of the Vault</label>
      <div class="skill-list">${Object.keys(DIR_HELP).map((d) => `<label class="check"><input type="checkbox" data-dir="${d}" ${s.dirs.includes(d) ? 'checked' : ''} ${ro ? 'disabled' : ''}> ${d} <span class="hint-inline">${esc(DIR_HELP[d])}</span></label>`).join('')}</div>
      <label>How to use it <span class="hint-inline">sent to the ghost on every run, keep it short</span></label>
      ${canEdit() ? `<textarea id="sk-body" style="min-height:160px" maxlength="8000" ${ro ? 'disabled' : ''}>${esc(s.body || '')}</textarea>` : '<div class="hint">Only editors can read the instructions.</div>'}
      <p class="hint">Needs role: <b>${esc(s.min_role)}</b> ${s.min_role === 'owner' ? '— commands and Vault access are owner-only: a ghost only gets this skill when the owner asks it.' : '— anyone who can chat can use it through a ghost.'}</p>
      ${ro ? '' : `<label class="check"><input type="checkbox" id="sk-on" ${s.enabled ? 'checked' : ''}> Switched on</label>
      <div class="modal-foot"><button class="btn danger" data-act="del" style="margin-right:auto">Delete skill</button><button class="btn primary" data-act="save">Save</button></div>`}</div></div>`;
}

const paint = () => { $('#md-list', view).innerHTML = listHTML(); $('#md-detail', view).innerHTML = detail(skillById(selected)); };

async function templateModal() {
  const { templates } = await api('GET', '/api/skill_templates');
  modal(`<h2>Add a skill from a template</h2><p class="sub">Templates are ready-made skills. You can edit them afterwards.</p>
    <div class="tpl-list">${templates.map((t) => `<div class="tpl-item"><div><b>${esc(t.name)}</b> ${t.min_role === 'owner' ? '<span class="badge">owner</span>' : ''}<div class="meta">${esc(t.description)}</div>
      <div class="meta">Tools: ${t.tools.length ? esc(t.tools.join(', ')) : 'none'}</div></div>
      ${t.installed ? '<span class="chip ok">added</span>' : `<button class="btn small primary" data-add="${esc(t.id)}">Add</button>`}</div>`).join('')}</div>
    <div class="modal-foot"><button class="btn" id="tp-close">Close</button></div>`,
  (m) => m.addEventListener('click', guard(async (e) => {
    if (e.target.id === 'tp-close') return closeModal();
    const a = e.target.closest('[data-add]');
    if (!a) return;
    const s = await api('POST', '/api/skills/from_template', { id: a.dataset.add });
    closeModal(); toast('Skill added'); location.hash = '#/skills/' + s.id; window.dispatchEvent(new Event('pc2:changed'));
  })), true);
}

export async function render(v, arg) {
  view = v; selected = arg || '';
  view.innerHTML = `<div class="chats-layout"><div class="chat-side"><div class="chat-side-head"><h1>Skills</h1>
    ${isOwner() ? '<button class="btn small" data-act="tpl">Templates</button><button class="btn small primary" data-act="new">+ New</button>' : ''}</div>
    <div class="chat-list" id="md-list"></div></div><div class="chat-pane scroll" id="md-detail"></div></div>`;
  paint();
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    const act = el.dataset.act;
    if (act === 'tpl') return templateModal();
    if (act === 'new') {
      const s = await api('POST', '/api/skills', { name: 'New skill', description: 'What this skill is for', tools: ['Read'], body: 'Explain how to use it.' });
      location.hash = '#/skills/' + s.id; window.dispatchEvent(new Event('pc2:changed'));
    } else if (act === 'save') {
      const tools = [...view.querySelectorAll('[data-tool]:checked')].map((x) => x.dataset.tool), dirs = [...view.querySelectorAll('[data-dir]:checked')].map((x) => x.dataset.dir);
      await api('PUT', '/api/skills/' + selected, { name: $('#sk-name', view).value, description: $('#sk-desc', view).value, tools, dirs, body: $('#sk-body', view)?.value ?? '', enabled: $('#sk-on', view).checked });
      toast('Saved'); window.dispatchEvent(new Event('pc2:changed'));
    } else if (act === 'del') twoStep(el, 'Really delete?', async () => { await api('DELETE', '/api/skills/' + selected); toast('Skill deleted'); location.hash = '#/skills'; window.dispatchEvent(new Event('pc2:changed')); });
  });
}
