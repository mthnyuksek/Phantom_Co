import { api } from '../api.js';
import { S, me, isOwner, userById } from '../state.js';
import { $, esc, guard, twoStep, toast } from '../ui.js';

// Prompt shortcuts: "/restaurant Kadıköy" in a chat or in Telegram sends the saved prompt, {args} = what follows the command.
// One list for the whole team; editors make them, only the maker or the owner changes one.
let view, selected = '';
const shortcut = (id) => S.state.shortcuts.find((s) => s.id === id);
const canChange = (s) => !s.id || isOwner() || s.created_by === me().id;

const listHTML = () => S.state.shortcuts.map((s) => `<a class="chat-item ${s.id === selected ? 'on' : ''}" href="#/shortcuts/${esc(s.id)}"><div class="mini big-emoji">⚡</div>
  <div class="ci-body"><div class="ci-title" data-no-i18n>/${esc(s.command)}</div><div class="ci-prev" data-no-i18n>${esc(s.title || s.body)}</div></div></a>`).join('') || '<div class="empty small">No shortcuts yet.</div>';

function detail(s) {
  if (!s) return `<div class="empty big"><p>A shortcut is a ready-made prompt. Write <b data-no-i18n>/restaurant Kadıköy</b> in a chat or in Telegram and the ghost gets the saved text, with <b data-no-i18n>{args}</b> replaced by what you wrote after the command.<br>Everybody who can chat can use all shortcuts.</p></div>`;
  const ro = !canChange(s), isNew = !s.id;
  const ghosts = S.state.agents.filter((a) => a.enabled || a.id === s.agent_id);
  const maker = userById(s.created_by);
  return `<div class="detail-pad"><a class="btn small back-btn" href="#/shortcuts" aria-label="Back to shortcuts">←</a><div class="page-head"><div><h1>⚡ ${isNew ? 'New shortcut' : `<span data-no-i18n>/${esc(s.command)}</span>`}</h1>
      <p>${isNew ? 'It is shared with the whole team.' : `<span>Made by ${esc(maker?.name || 'someone')}.</span> ${ro ? '<span>Only they and the owner can change it.</span>' : ''}`}</p></div></div>
    <div class="card wide"><label>Command <span class="hint-inline">letters and numbers, no spaces: you type / and this word</span></label>
      <input type="text" id="sc-command" maxlength="32" value="${esc(s.command)}" placeholder="restaurant" ${ro ? 'disabled' : ''}>
      <label>Short description <span class="hint-inline">shown in the list when you type /</span></label>
      <input type="text" id="sc-title" maxlength="120" value="${esc(s.title)}" ${ro ? 'disabled' : ''}>
      <label>Prompt <span class="hint-inline">write <b data-no-i18n>{args}</b> where your own words should go; without it they are added below</span></label>
      <textarea id="sc-body" maxlength="8000" style="min-height:160px" placeholder="Research restaurants near {args} and recommend three." ${ro ? 'disabled' : ''}>${esc(s.body)}</textarea>
      <label>Ghost <span class="hint-inline">in Telegram the shortcut goes to this ghost; empty = the one you are talking to. In a chat it always goes to that chat's ghost.</span></label>
      <select id="sc-agent" ${ro ? 'disabled' : ''}><option value="">The current ghost</option>${ghosts.map((a) => `<option value="${esc(a.id)}" ${a.id === s.agent_id ? 'selected' : ''}>${esc(a.name)}</option>`).join('')}</select>
      ${ro ? '' : `<div class="modal-foot">${isNew ? '' : '<button class="btn danger" data-act="del" style="margin-right:auto">Delete shortcut</button>'}<button class="btn primary" data-act="save">Save</button></div>`}</div></div>`;
}

const current = () => selected === 'new' ? { id: '', command: '', title: '', body: '', agent_id: '', created_by: me().id } : shortcut(selected);
const paint = () => { $('#md-list', view).innerHTML = listHTML(); $('#md-detail', view).innerHTML = detail(current()); };

export async function render(v, arg) {
  view = v; selected = arg || '';
  view.innerHTML = `<div class="chats-layout ${selected ? 'has-chat' : ''}"><div class="chat-side"><div class="chat-side-head"><h1>Shortcuts</h1><a class="btn small primary" href="#/shortcuts/new">+ New</a></div>
    <div class="chat-list" id="md-list"></div></div><div class="chat-pane scroll" id="md-detail"></div></div>`;
  paint();
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    const body = () => ({ command: $('#sc-command', view).value.trim(), title: $('#sc-title', view).value.trim(), body: $('#sc-body', view).value, agent_id: $('#sc-agent', view).value });
    if (el.dataset.act === 'save') {
      const r = selected === 'new' ? await api('POST', '/api/shortcuts', body()) : await api('PUT', '/api/shortcuts/' + selected, body());
      toast('Saved');
      window.dispatchEvent(new Event('pc2:changed'));
      if (selected === 'new') location.hash = '#/shortcuts/' + r.id;
    } else if (el.dataset.act === 'del') twoStep(el, 'Really delete?', async () => { await api('DELETE', '/api/shortcuts/' + selected); location.hash = '#/shortcuts'; window.dispatchEvent(new Event('pc2:changed')); });
  });
}
