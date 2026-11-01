import { api } from '../api.js';
import { S, me, isOwner, userGhost } from '../state.js';
import { $, esc, guard, modal, closeModal, twoStep, toast, copyText, ROLE_TEXT } from '../ui.js';

let view, users = [];
const ROLE_INFO = { owner: 'Everything, including people, skills, settings and commands.', editor: 'Chats with ghosts, creates tasks and projects, hires ghosts.', viewer: 'Looks around, reads shared chats and tasks. Cannot change anything.' };

async function inviteModal(name, token) {
  let net = { share: false, port: location.port, addresses: [] }, tun = null;
  try { net = await api('GET', '/api/network'); tun = await api('GET', '/api/tunnel'); } catch { /* owner only */ }
  const links = [];
  if (tun?.state === 'on') links.push(`${tun.url}/#/join/${token}`);  // reachable from anywhere, encrypted
  if (net.share) links.push(...net.addresses.map((a) => `http://${a.address}:${net.port}/#/join/${token}`));
  modal(`<h2>Invite ${esc(name)}</h2>
    <p class="sub">Send this link to ${esc(name)}. They open it in their Phantom Co. (or paste it into "Join a friend's company"), choose a password and are in. It works once and expires in 3 days.</p>
    ${links.length ? links.map((l, i) => `<div class="linkbox"><input type="text" readonly value="${esc(l)}"><button class="btn small" data-copy="${esc(l)}">Copy</button></div>${i === 0 ? '' : ''}`).join('')
      : `<div class="errbox" style="margin-left:0">Nobody can reach this PC yet. In <a href="#/settings" id="to-settings">Settings</a> switch on <b>Internet access</b> (friends anywhere) or <b>Let people on my network connect</b> (same home network), then invite again.</div>
         <div class="linkbox"><input type="text" readonly value="${esc(location.origin + '/#/join/' + token)}"><button class="btn small" data-copy="${esc(location.origin + '/#/join/' + token)}">Copy</button></div>`}
    <p class="hint">Links starting with <code>https://</code> (Internet access) are encrypted and work from anywhere. Links starting with <code>http://</code> are for the same network only and are not encrypted. Several addresses are listed when this PC has several network cards: pick the one your friend can reach.</p>
    <div class="modal-foot"><button class="btn primary" id="iv-close">Done</button></div>`,
  (m) => m.addEventListener('click', guard(async (e) => {
    if (e.target.id === 'iv-close' || e.target.id === 'to-settings') return closeModal();
    const c = e.target.closest('[data-copy]');
    if (c) copyText(c.dataset.copy);
  })), true);
}

function addModal() {
  modal(`<h2>Add a person</h2><p class="sub">They get an invitation link and choose their own password.</p>
    <label>Name</label><input type="text" id="u-name" maxlength="40" autofocus>
    <label>Role</label><select id="u-role">${Object.keys(ROLE_TEXT).map((r) => `<option value="${r}" ${r === 'editor' ? 'selected' : ''}>${ROLE_TEXT[r]}</option>`).join('')}</select>
    <p class="hint" id="u-info">${esc(ROLE_INFO.editor)}</p>
    <div class="modal-foot"><button class="btn" id="u-cancel">Cancel</button><button class="btn primary" id="u-go">Create invitation</button></div>`,
  (m) => {
    $('#u-role', m).onchange = (e) => { $('#u-info', m).textContent = ROLE_INFO[e.target.value]; };
    m.addEventListener('click', guard(async (e) => {
      if (e.target.id === 'u-cancel') return closeModal();
      if (e.target.id !== 'u-go') return;
      const r = await api('POST', '/api/users', { name: $('#u-name', m).value.trim(), role: $('#u-role', m).value });
      closeModal(); await load(); draw(); inviteModal(r.user.name, r.invite_token);
    }));
  });
}

const load = async () => { users = isOwner() ? (await api('GET', '/api/users')).users : S.state.users; };

function draw() {
  view.innerHTML = `<div class="page-head"><div><h1>Team</h1><p>The people who work in this ghost company. Ghosts act with the rights of the person who asks them.</p></div><div class="spacer"></div>
    ${isOwner() ? '<button class="btn primary" data-act="add">+ Add a person</button>' : ''}</div>
    <div class="grid">${users.map((u) => `<div class="gcard" style="--gc:${esc(u.color)}"><div class="stage"><div class="shadow"></div>${userGhost(u, 96)}</div>
      <div class="gname">${esc(u.name)}${u.id === me().id ? ' <span class="badge">you</span>' : ''}</div>
      <div class="grole">${esc(ROLE_TEXT[u.role] || u.role)}</div>
      ${isOwner() ? `<div class="gstatus">${u.has_password ? 'Signed up' : '⏳ Invitation not used yet'}</div>
        <div class="gactions"><select data-role="${esc(u.id)}" style="width:auto">${Object.keys(ROLE_TEXT).map((r) => `<option value="${r}" ${r === u.role ? 'selected' : ''}>${ROLE_TEXT[r]}</option>`).join('')}</select>
        <button class="btn small" data-act="invite" data-id="${esc(u.id)}" title="New invitation link (also resets a forgotten password)">Link</button>
        ${u.id === me().id ? '' : `<button class="btn small danger" data-act="del" data-id="${esc(u.id)}">Remove</button>`}</div>` : '<div class="gstatus"></div>'}</div>`).join('')}</div>
    <div class="card" style="margin-top:26px"><h3>Roles</h3>${Object.keys(ROLE_INFO).map((r) => `<p><b>${ROLE_TEXT[r]}</b> — ${esc(ROLE_INFO[r])}</p>`).join('')}
      <p class="hint">Chats are private to the person who started them unless shared. The owner can see everything in the Vault.</p></div>`;
}

export async function render(v) {
  view = v;
  await load();
  draw();
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    const u = users.find((x) => x.id === el.dataset.id);
    if (el.dataset.act === 'add') addModal();
    else if (el.dataset.act === 'invite') { const r = await api('POST', `/api/users/${u.id}/invite`, {}); inviteModal(u.name, r.invite_token); }
    else if (el.dataset.act === 'del') twoStep(el, 'Really remove?', async () => { await api('DELETE', '/api/users/' + u.id); toast(u.name + ' was removed'); window.dispatchEvent(new Event('pc2:changed')); });
  });
  view.onchange = guard(async (e) => {
    if (!e.target.dataset.role) return;
    await api('PUT', '/api/users/' + e.target.dataset.role, { role: e.target.value });
    toast('Role changed'); window.dispatchEvent(new Event('pc2:changed'));
  });
}
