import { api } from '../api.js';
import { S, isOwner } from '../state.js';
import { $, esc, guard, twoStep, toast } from '../ui.js';

let view, selected = '';
const rule = (id) => S.state.rules.find((r) => r.id === id);
const tokens = (r) => Math.ceil((r.body || '').length / 4);

const listHTML = () => S.state.rules.map((r) => `<a class="chat-item ${r.id === selected ? 'on' : ''}" href="#/rules/${esc(r.id)}"><div class="mini big-emoji">📜</div>
  <div class="ci-body"><div class="ci-title">${esc(r.name)}${r.enabled ? '' : ' <span class="badge">off</span>'}</div><div class="ci-prev">${esc(r.body)}</div></div><div class="ci-time">~${tokens(r)} tok</div></a>`).join('') || '<div class="empty small">No rules yet.</div>';

function detail(r) {
  const total = S.state.rules.filter((x) => x.enabled).reduce((n, x) => n + tokens(x), 0);
  if (!r) return `<div class="empty big"><p>Company rules are added to the prompt of <b>every</b> ghost on <b>every</b> run.<br>Keep them few and short — now about ${total} tokens per run.</p></div>`;
  const ro = !isOwner();
  return `<div class="detail-pad"><div class="page-head"><div><h1>📜 ${esc(r.name)}</h1><p>About ${tokens(r)} tokens, paid on every run while switched on.</p></div></div>
    <div class="card wide"><label>Title</label><input type="text" id="r-name" maxlength="60" value="${esc(r.name)}" ${ro ? 'disabled' : ''}>
      <label>Rule</label><textarea id="r-body" maxlength="2000" style="min-height:140px" ${ro ? 'disabled' : ''}>${esc(r.body)}</textarea>
      ${ro ? '' : `<label class="check"><input type="checkbox" id="r-on" ${r.enabled ? 'checked' : ''}> Switched on</label>
      <div class="modal-foot"><button class="btn danger" data-act="del" style="margin-right:auto">Delete rule</button><button class="btn primary" data-act="save">Save</button></div>`}</div></div>`;
}
const paint = () => { $('#md-list', view).innerHTML = listHTML(); $('#md-detail', view).innerHTML = detail(rule(selected)); };

export async function render(v, arg) {
  view = v; selected = arg || '';
  view.innerHTML = `<div class="chats-layout"><div class="chat-side"><div class="chat-side-head"><h1>Rules</h1>${isOwner() ? '<button class="btn small primary" data-act="new">+ New</button>' : ''}</div>
    <div class="chat-list" id="md-list"></div></div><div class="chat-pane scroll" id="md-detail"></div></div>`;
  paint();
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    if (el.dataset.act === 'new') { const r = await api('POST', '/api/rules', { name: 'New rule', body: 'Write the rule here.' }); location.hash = '#/rules/' + r.id; window.dispatchEvent(new Event('pc2:changed')); }
    else if (el.dataset.act === 'save') { await api('PUT', '/api/rules/' + selected, { name: $('#r-name', view).value, body: $('#r-body', view).value, enabled: $('#r-on', view).checked }); toast('Saved'); window.dispatchEvent(new Event('pc2:changed')); }
    else if (el.dataset.act === 'del') twoStep(el, 'Really delete?', async () => { await api('DELETE', '/api/rules/' + selected); location.hash = '#/rules'; window.dispatchEvent(new Event('pc2:changed')); });
  });
}
