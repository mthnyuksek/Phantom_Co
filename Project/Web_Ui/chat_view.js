import { api } from './api.js';
import { S, me, agentById, senderInfo, agentGhost } from './state.js';
import { $, esc, md, ago, sleep, guard } from './ui.js';

// A conversation: messages, the live "is working" bubble and the composer.
// Used by the Chats page and by the task page. Returns { destroy, data }.
export function conversationView(el, id, { placeholder = 'Write a message…', onData } = {}) {
  let alive = true, data = null, sig = '', sending = false;
  el.innerHTML = `<div class="conv"><div class="transcript" id="cv-msgs"><div class="empty">Loading…</div></div>
    <div class="composer"><textarea id="cv-input" rows="2" placeholder="${esc(placeholder)}"></textarea>
    <button class="btn primary" id="cv-send">Send</button></div><div class="composer-note" id="cv-note"></div></div>`;
  const msgsEl = $('#cv-msgs', el), input = $('#cv-input', el), sendBtn = $('#cv-send', el), note = $('#cv-note', el);

  const renderApproval = (m) => {
    const a = agentById(m.agent), p = (data.approvals || []).find((x) => x.id === m.approval);
    const forUser = p && !p.can_decide ? (S.state?.users || []).find((u) => u.id === p.user_id) : null;
    return `<div class="approval ${p ? 'waiting' : ''}"><div class="approval-head">🔐 <b>${esc(a?.name || 'A ghost')}</b> <span>asks to use</span> <b>${esc(m.server)} › ${esc(m.tool)}</b></div>
      <details ${p ? 'open' : ''}><summary>Details</summary><pre>${esc(m.args || '')}</pre></details>
      ${p && p.can_decide ? `<div class="approval-btns"><button class="btn small primary" data-approve="${esc(m.approval)}">Approve</button><button class="btn small danger" data-decline="${esc(m.approval)}">Decline</button></div>`
        : p ? `<div class="hint">Waiting for ${esc(forUser?.name || 'the person')} to answer.</div>` : ''}</div>`;
  };
  const renderMsg = (m) => {
    if (m.kind === 'approval') return renderApproval(m);
    const who = senderInfo(m.from || 'system');
    if (who.kind === 'system') return `<div class="sys">${esc(m.text)}</div>`;
    const mine = who.kind === 'user' && who.id === me().id;
    return `<div class="msg ${mine ? 'mine' : ''}" style="--gc:${esc(who.color || '#888')}"><div class="mini">${who.svg}</div>
      <div class="msg-body"><div class="who">${esc(who.name)} <span>${esc(ago(m.ts))}</span></div><div class="bubble">${md(m.text)}</div></div></div>`;
  };
  const renderLive = (d) => d.live.map((l) => {
    const a = agentById(l.agent_id);
    const acts = (l.activity || []).map((x) => `<div>${esc(x)}</div>`).join('');
    return `<div class="msg" style="--gc:${esc(a?.color || '#888')}"><div class="mini">${agentGhost(a, { mood: l.status === 'running' ? 'working' : 'queued', size: 34 })}</div>
      <div class="msg-body"><div class="who">${esc(a?.name || l.agent_id)}</div>
      <div class="bubble typing"><span class="dots"><i></i><i></i><i></i></span> ${l.status === 'running' ? 'is working…' : 'is waiting in line…'}
      ${acts ? `<div class="activity">${acts}</div>` : ''}
      ${d.can_write || me().role === 'owner' ? `<button class="btn small" data-stop="1">Stop</button>` : ''}</div></div></div>`;
  }).join('') || (d.busy ? `<div class="sys">Waiting for a colleague…</div>` : '');

  function paint() {
    const d = data;
    const near = msgsEl.scrollHeight - msgsEl.scrollTop - msgsEl.clientHeight < 90;
    const retry = d.can_retry ? `<div class="sys retry-row"><button class="btn small" data-retry="1">↻ Try again</button></div>` : '';
    msgsEl.innerHTML = (d.messages.map(renderMsg).join('') || '<div class="empty">No messages yet.</div>') + (d.busy ? renderLive(d) : retry);
    if (near || sending) msgsEl.scrollTop = msgsEl.scrollHeight;
    const can = d.can_write && !d.busy;
    input.disabled = !can;
    sendBtn.disabled = !can;
    note.textContent = !d.can_write ? (me().role === 'viewer' ? 'You can read this conversation but not write in it.' : 'This conversation is read-only for you.')
      : d.busy ? 'The agent is working… you can write again when it is done.' : 'Enter sends, Shift+Enter adds a line.';
  }

  function apply(d) {
    data = d;
    const s = JSON.stringify([d.messages.length, d.busy, d.live, d.meta.title, d.can_write, d.can_retry, d.meta.shared, (S.state?.users || []).map((u) => u.online), (d.approvals || []).map((x) => x.id)]);
    onData?.(d);
    if (s === sig) return;
    sig = s;
    paint();
  }

  async function load() {
    try { apply(await api('GET', '/api/chats/' + id)); }
    catch (e) { if (alive) msgsEl.innerHTML = `<div class="empty">${esc(e.message)}</div>`; alive = false; }
  }
  (async () => { while (alive) { await load(); await sleep(data?.busy ? 800 : 3000); } })();

  const send = guard(async () => {
    const text = input.value.trim();
    if (!text || !data?.can_write || data.busy) return;
    sending = true;
    sendBtn.disabled = true;
    try {
      const d = await api('POST', `/api/chats/${id}/send`, { text });
      input.value = '';
      apply(d);
    } finally { sending = false; }
  });
  sendBtn.onclick = send;
  input.onkeydown = (e) => { if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); send(); } };
  msgsEl.onclick = guard(async (e) => {
    if (e.target.closest('[data-stop]')) { await api('POST', `/api/chats/${id}/stop`, {}); load(); }
    else if (e.target.closest('[data-approve], [data-decline]')) {
      const b = e.target.closest('[data-approve], [data-decline]');
      b.parentElement.querySelectorAll('button').forEach((x) => { x.disabled = true; });
      await api('POST', '/api/mcp/approvals/' + (b.dataset.approve || b.dataset.decline), { approve: !!b.dataset.approve });
      load();
    } else if (e.target.closest('[data-retry]')) { e.target.closest('[data-retry]').disabled = true; sending = true; try { apply(await api('POST', `/api/chats/${id}/retry`, {})); } finally { sending = false; } }
  });

  return { destroy: () => { alive = false; }, reload: load, get data() { return data; } };
}
