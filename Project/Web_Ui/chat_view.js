import { api } from './api.js';
import { S, me, agentById, senderInfo, agentGhost } from './state.js';
import { $, esc, md, ago, sleep, guard } from './ui.js';

// A conversation: messages, the live "is working" bubble and the composer.
// Used by the Chats page and by the task page. Returns { destroy, data }.
export function conversationView(el, id, { placeholder = 'Write a message…', onData } = {}) {
  let alive = true, data = null, sig = '', sending = false;
  el.innerHTML = `<div class="conv"><div class="transcript" id="cv-msgs"><div class="empty">Loading…</div></div>
    <div class="composer"><div class="slash-pop" id="cv-slash" hidden></div><textarea id="cv-input" rows="2" placeholder="${esc(placeholder)}"></textarea>
    <button class="btn primary" id="cv-send">Send</button></div><div class="composer-note" id="cv-note"></div></div>`;
  const msgsEl = $('#cv-msgs', el), input = $('#cv-input', el), sendBtn = $('#cv-send', el), note = $('#cv-note', el), slashEl = $('#cv-slash', el);

  // Prompt shortcuts (Shortcuts page): "/restaurant Kadıköy" is sent as the saved prompt with {args} filled in.
  const shortcuts = () => S.state?.shortcuts || [];
  const expandShortcut = (text) => {
    const m = /^\/([^\s]+)\s*([\s\S]*)$/.exec(text);
    const sc = m && shortcuts().find((s) => s.command === m[1].toLowerCase());
    if (!sc) return text;
    const args = m[2].trim();
    return sc.body.includes('{args}') ? sc.body.split('{args}').join(args) : args ? `${sc.body}\n\n${args}` : sc.body;
  };
  let slashList = [], slashAt = 0;
  const slashClose = () => { slashList = []; slashEl.hidden = true; };
  function slashUpdate() {
    const v = input.value;
    slashList = /^\/[^\s]*$/.test(v) && !input.disabled ? shortcuts().filter((s) => s.command.startsWith(v.slice(1).toLowerCase())) : [];
    if (!slashList.length) return slashClose();
    slashAt = Math.min(slashAt, slashList.length - 1);
    slashEl.hidden = false;
    slashEl.innerHTML = slashList.map((s, i) => `<div class="slash-item ${i === slashAt ? 'on' : ''}" data-i="${i}"><b data-no-i18n>/${esc(s.command)}</b> <span data-no-i18n>${esc(s.title || s.body)}</span></div>`).join('');
  }
  const slashPick = (i) => { input.value = `/${slashList[i].command} `; slashClose(); input.focus(); };
  input.oninput = slashUpdate;
  input.onblur = () => setTimeout(slashClose, 150);
  slashEl.onmousedown = (e) => { const it = e.target.closest('.slash-item'); if (it) { e.preventDefault(); slashPick(+it.dataset.i); } };

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
      <div class="bubble typing"><span class="dots"><i></i><i></i><i></i></span> <span class="live-status">${l.status === 'running' ? 'is working…' : 'is waiting in line…'}</span>
      ${l.slow ? `<div class="hint">The model service has not answered for a while. Free services have a request limit; it tries again by itself.</div>` : ''}
      ${acts ? `<div class="activity" data-no-i18n>${acts}</div>` : ''}
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
      : d.busy ? 'The agent is working… you can write again when it is done.'
        : shortcuts().length ? 'Enter sends, Shift+Enter adds a line. Type / for shortcuts.' : 'Enter sends, Shift+Enter adds a line.';
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
    const text = expandShortcut(input.value.trim());
    if (!text || !data?.can_write || data.busy) return;
    sending = true;
    sendBtn.disabled = true;
    slashClose();
    try {
      const d = await api('POST', `/api/chats/${id}/send`, { text });
      input.value = '';
      apply(d);
    } finally { sending = false; }
  });
  sendBtn.onclick = send;
  input.onkeydown = (e) => {
    if (slashList.length && !slashEl.hidden) {  // the shortcut list is open
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); slashAt = (slashAt + (e.key === 'ArrowDown' ? 1 : slashList.length - 1)) % slashList.length; slashUpdate(); return; }
      if (e.key === 'Escape') { e.preventDefault(); slashClose(); return; }
      const exact = input.value.trim() === `/${slashList[slashAt].command}`;
      if (e.key === 'Tab' || (e.key === 'Enter' && !e.shiftKey && !exact)) { e.preventDefault(); slashPick(slashAt); return; }
    }
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); send(); }
  };
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
