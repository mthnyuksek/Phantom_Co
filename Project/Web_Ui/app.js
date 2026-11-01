import { api } from './api.js';
import { ghostSVG } from './ghosts.js';
import { $, $$, esc, guard, modal, closeModal, toast, ROLE_TEXT } from './ui.js';
import { S, refreshState, forgetState, me, isOwner, canEdit, userGhost, providerLabel } from './state.js';
import { ghostPickerHTML, bindGhostPicker } from './pickers.js';

const view = $('#view');
const NAV = [
  { id: 'haunt', label: 'The Haunt', icon: '🏚️' },
  { id: 'chats', label: 'Chats', icon: '💬' },
  { id: 'projects', label: 'Projects', icon: '📁' },
  { id: 'tasks', label: 'Tasks', icon: '📋' },
  { id: 'skills', label: 'Skills', icon: '🧰' },
  { id: 'rules', label: 'Rules', icon: '📜' },
  { id: 'runs', label: 'Runs', icon: '⏱️' },
  { id: 'files', label: 'Files', icon: '🗂️' },
  { id: 'team', label: 'Team', icon: '👥' },
  { id: 'settings', label: 'Settings', icon: '⚙️', owner: true },
];
const PAGES = ['haunt', 'chats', 'projects', 'tasks', 'skills', 'rules', 'runs', 'files', 'team', 'settings'];

let token = 0, current = null, currentCtx = null, timer = null, running = false;

// ---------------------------------------------------------------- auth screens
const authRoot = $('#auth-root');
function authCard(title, sub, body, mood = 'idle', form = 'wizard') {
  $('#app').hidden = true;
  authRoot.hidden = false;
  authRoot.innerHTML = `<div class="auth-card">
    <div class="auth-ghost">${ghostSVG({ form, color: '#b69cff', mood, size: 120 })}</div>
    <h1>${esc(title)}</h1><p class="sub">${esc(sub)}</p>${body}</div>`;
}
const bridge = window.chrome?.webview;  // present when this page runs inside the Phantom Co. program window
const isRemote = !['127.0.0.1', 'localhost'].includes(location.hostname);
const companies = () => { try { return JSON.parse(localStorage.getItem('pc2.companies') || '[]'); } catch { return []; } };
function connectTo(raw) {
  let u = raw.trim();
  if (!u) return;
  if (!/^https?:\/\//i.test(u)) u = 'http://' + u;
  try { new URL(u); } catch { return toast('That does not look like an address.', true); }
  const origin = new URL(u).origin;
  try { localStorage.setItem('pc2.companies', JSON.stringify([{ url: origin }, ...companies().filter((c) => c.url !== origin)].slice(0, 5))); } catch { /* private mode */ }
  if (bridge) bridge.postMessage({ cmd: 'connect', url: u }); else location.href = u;
}
const joinFriendHTML = () => `<div class="auth-alt"><a href="#" id="a-friend">I was invited to a friend's company →</a>
  <div id="a-friend-box" hidden><label>Invitation link or address</label><input id="a-friend-url" type="text" placeholder="http://192.168.1.20:7777/#/join/…">
  <button class="btn wide-btn" id="a-friend-go">Connect</button>
  ${companies().map((c) => `<button class="btn small" data-recent="${esc(c.url)}">${esc(c.url)}</button>`).join(' ')}</div></div>`;
function bindJoinFriend() {
  $('#a-friend').onclick = (e) => { e.preventDefault(); $('#a-friend-box').hidden = false; $('#a-friend-url').focus(); };
  $('#a-friend-go').onclick = () => connectTo($('#a-friend-url').value);
  $('#a-friend-url').onkeydown = (e) => { if (e.key === 'Enter') connectTo(e.target.value); };
  authRoot.querySelectorAll('[data-recent]').forEach((b) => { b.onclick = () => connectTo(b.dataset.recent); });
}
const field = (id, label, type = 'text', extra = '') => `<label>${label}</label><input id="${id}" type="${type}" ${extra}>`;

function showSetup() {
  authCard('Welcome to Phantom Co.', 'Create the owner account. You can invite friends afterwards.',
    `${field('a-name', 'Your name', 'text', 'maxlength="40" autofocus')}${field('a-pass', 'Password (at least 8 characters)', 'password')}
     <div class="auth-err" id="a-err"></div><button class="btn primary wide-btn" id="a-go">Create company</button>${joinFriendHTML()}`);
  bindJoinFriend();
  const go = guard(async () => {
    try {
      await api('POST', '/api/auth/setup', { name: $('#a-name').value.trim(), password: $('#a-pass').value });
      await startApp();
    } catch (e) { $('#a-err').textContent = e.message; }
  });
  $('#a-go').onclick = go;
  $('#a-pass').onkeydown = (e) => { if (e.key === 'Enter') go(); };
}

function showLogin() {
  authCard('Sign in', 'Welcome back to the haunt.',
    `${field('a-name', 'Name', 'text', 'maxlength="40" autofocus')}${field('a-pass', 'Password', 'password')}
     <div class="auth-err" id="a-err"></div><button class="btn primary wide-btn" id="a-go">Sign in</button>
     ${isRemote ? '' : '<div class="auth-alt"><a href="#" id="a-new">Create a new account</a></div>'}${joinFriendHTML()}`);
  bindJoinFriend();
  if ($('#a-new')) $('#a-new').onclick = (e) => { e.preventDefault(); showRegister(); };
  const go = async () => {
    try {
      await api('POST', '/api/auth/login', { name: $('#a-name').value.trim(), password: $('#a-pass').value });
      await startApp();
    } catch (e) { $('#a-err').textContent = e.message; }
  };
  $('#a-go').onclick = go;
  $('#a-pass').onkeydown = (e) => { if (e.key === 'Enter') go(); };
}

function showRegister() {
  authCard('New account', 'Create another owner account on this PC.',
    `${field('a-name', 'Your name', 'text', 'maxlength="40" autofocus')}${field('a-pass', 'Password (at least 8 characters)', 'password')}
     <div class="auth-err" id="a-err"></div><button class="btn primary wide-btn" id="a-go">Create account</button>
     <div class="auth-alt"><a href="#" id="a-back">← Back to sign in</a></div>`);
  $('#a-back').onclick = (e) => { e.preventDefault(); showLogin(); };
  const go = guard(async () => {
    try {
      await api('POST', '/api/auth/register', { name: $('#a-name').value.trim(), password: $('#a-pass').value });
      await startApp();
    } catch (e) { $('#a-err').textContent = e.message; }
  });
  $('#a-go').onclick = go;
  $('#a-pass').onkeydown = (e) => { if (e.key === 'Enter') go(); };
}

async function showJoin(tok) {
  let name;
  try { name = (await api('GET', '/api/auth/invite?token=' + tok)).name; } catch (e) {
    authCard('This invitation is not valid', e.message + ' Ask the owner for a new link.', '<button class="btn wide-btn" id="a-back">Go to sign in</button>', 'failed');
    $('#a-back').onclick = () => { location.hash = ''; boot(); };
    return;
  }
  authCard(`Hello ${name}!`, 'Choose a password to join Phantom Co..',
    `${field('a-pass', 'Password (at least 8 characters)', 'password', 'autofocus')}<div class="auth-err" id="a-err"></div>
     <button class="btn primary wide-btn" id="a-go">Join</button>`, 'done');
  const go = async () => {
    try {
      await api('POST', '/api/auth/join', { token: tok, password: $('#a-pass').value });
      location.hash = '#/haunt';
      await startApp();
    } catch (e) { $('#a-err').textContent = e.message; }
  };
  $('#a-go').onclick = go;
  $('#a-pass').onkeydown = (e) => { if (e.key === 'Enter') go(); };
}

async function boot() {
  stopApp();
  const join = location.hash.match(/^#\/join\/([0-9a-f]{48})$/);
  const status = await api('GET', '/api/auth/status');
  if (join) return showJoin(join[1]);
  if (status.setup_needed) return showSetup();
  if (!status.me) return showLogin();
  await startApp();
}
window.addEventListener('pc2:changed', guard(async () => { if (!running) return; await refreshState(); paintShell(); await route(); }));
window.addEventListener('pc2:signedout', () => { if (running) boot(); });

// ------------------------------------------------------------------ the shell
function paintShell() {
  const s = S.state;
  $('#brand-name').textContent = s.company.name;
  $('#brand-ghost').innerHTML = ghostSVG({ form: 'classic', color: '#b69cff', size: 34 });
  $('#provider-chips').innerHTML = s.providers.filter((p) => p.id !== 'demo')
    .map((p) => `<span class="chip ${p.available ? 'ok' : ''}" title="${p.available ? 'Found on this PC' : 'Not found on this PC'}">${esc(p.label)}</span>`).join('');
  $('#home-btn').hidden = !(bridge && isRemote);
  const m = s.me;
  $('#me-chip').innerHTML = `<span class="mini">${userGhost(m, 30)}</span><span class="me-text"><b>${esc(m.name)}</b><small>${esc(ROLE_TEXT[m.role] || m.role)}</small></span>`;
  const page = location.hash.replace(/^#\//, '').split('/')[0] || 'haunt';
  const busyChats = s.jobs.length;
  $('#nav').innerHTML = NAV.filter((n) => !n.owner || m.role === 'owner')
    .map((n) => `<a href="#/${n.id}" data-page="${n.id}" class="${n.id === page ? 'on' : ''}"><i>${n.icon}</i> ${n.label}${n.id === 'runs' && busyChats ? `<span class="count">${busyChats}</span>` : ''}</a>`).join('');
}

async function route() {
  if (!running) return;
  const [p0, ...rest] = (location.hash.replace(/^#\//, '') || 'haunt').split('/');
  const p = PAGES.includes(p0) ? p0 : 'haunt';
  const arg = decodeURIComponent(rest.join('/'));
  const my = ++token;
  view.onclick = null;
  view.oninput = null;
  view.onchange = null;
  $$('#nav a').forEach((a) => a.classList.toggle('on', a.dataset.page === p));
  try {
    const mod = await import(`./pages/${p}.js`);
    if (my !== token) return;
    currentCtx?.destroy?.();
    current = mod;
    currentCtx = { isCurrent: () => my === token };
    await mod.render(view, arg, currentCtx);
  } catch (e) {
    view.innerHTML = `<div class="empty">Something went wrong: ${esc(e.message)}</div>`;
  }
}
window.addEventListener('hashchange', () => { if (running) { paintShell(); route(); } });

async function startApp() {
  stopApp();
  await refreshState();
  authRoot.hidden = true;
  $('#app').hidden = false;
  running = true;
  paintShell();
  await route();
  timer = setInterval(guard(async () => {
    if (document.hidden || !running) return;
    const changed = await refreshState();
    if (changed) paintShell();
    await current?.tick?.(changed);
  }), 2000);
}
function stopApp() {
  running = false;
  clearInterval(timer);
  forgetState();
  closeModal();
  currentCtx?.destroy?.();
  currentCtx = null;
  token++;
}

// ------------------------------------------------- lend my Claude/Codex to this company
// Only inside the program window while it shows a friend's company: the window program does the lending.
let lend = { running: false, url: '', status: '' };
bridge?.addEventListener('message', (e) => {
  if (e.data?.type !== 'worker_status') return;
  lend = { running: !!e.data.running, url: e.data.url || '', status: e.data.status || '' };
  paintLend();
});
const lendHere = () => lend.running && lend.url === location.origin;
function paintLend() {
  const box = $('#p-lend'), txt = $('#p-lend-status');
  if (!box || !txt) return;
  box.checked = lendHere();
  txt.textContent = lend.running && !lendHere() ? 'Your AI is currently lent to another company (' + lend.url + ').' : lend.status || (lendHere() ? '' : 'Off');
}
function lendHTML() {
  if (!bridge || !isRemote || !canEdit()) return '';
  return `<h3 class="subhead">Lend my AI</h3>
    <label style="display:flex;gap:8px;align-items:center;margin:0"><input type="checkbox" id="p-lend" style="width:auto"> Let this company use the Claude Code / Codex on <b>my PC</b></label>
    <p class="sub" id="p-lend-status"></p>
    <p class="sub">The company can then give work to my AI. Only file and web tools run (no commands). The files of the project the AI works on are copied to this PC and the changes copied back. It uses my own subscriptions, so only switch it on for people I trust. It stays on when I reopen the program, until I switch it off.</p>`;
}
function bindLend(el) {
  const box = $('#p-lend', el);
  if (!box) return;
  bridge.postMessage({ cmd: 'worker_status' });
  const t = setInterval(() => { if (!document.body.contains(box)) clearInterval(t); else bridge.postMessage({ cmd: 'worker_status' }); }, 3000);
  box.onchange = guard(async () => {
    if (box.checked) {
      const { token } = await api('POST', '/api/worker/token', {});
      bridge.postMessage({ cmd: 'worker_start', url: location.origin, token });
    } else {
      bridge.postMessage({ cmd: 'worker_stop' });
      await api('DELETE', '/api/worker/token');
    }
  });
}

// ------------------------------------------------------------------- profile
$('#me-chip').onclick = () => openProfile();
$('#home-btn').onclick = () => bridge?.postMessage({ cmd: 'home' });
function openProfile() {
  const m = me();
  const draft = { form: m.form, color: m.color };
  modal(`<h2>Your profile</h2><p class="sub">${esc(ROLE_TEXT[m.role])} · this is how others see you.</p>
    <div class="editor"><div class="preview" id="pp"></div><div>
      <label>Name</label><input type="text" id="p-name" maxlength="40" value="${esc(m.name)}">
      <div id="pp-pick">${ghostPickerHTML()}</div>
      <h3 class="subhead">Change password</h3>
      <div class="two"><div><label>Current password</label><input type="password" id="p-old" autocomplete="current-password"></div>
        <div><label>New password</label><input type="password" id="p-new" autocomplete="new-password"></div></div>
      ${lendHTML()}
    </div></div>
    <div class="modal-foot"><button class="btn danger" id="p-out" style="margin-right:auto">Sign out</button>
      <button class="btn" id="p-cancel">Cancel</button><button class="btn primary" id="p-save">Save</button></div>`,
  (el) => {
    const prev = () => { $('#pp', el).style.setProperty('--gc', draft.color); $('#pp', el).innerHTML = ghostSVG({ form: draft.form, color: draft.color, size: 150 }) + `<div class="pname">${esc($('#p-name', el).value)}</div>`; };
    bindGhostPicker($('#pp-pick', el), draft, prev);
    $('#p-name', el).oninput = prev;
    prev();
    bindLend(el);
    el.addEventListener('click', guard(async (e) => {
      if (e.target.id === 'p-cancel') return closeModal();
      if (e.target.id === 'p-out') { await api('POST', '/api/auth/logout', {}); closeModal(); return boot(); }
      if (e.target.id !== 'p-save') return;
      const body = { name: $('#p-name', el).value.trim(), form: draft.form, color: draft.color };
      if ($('#p-new', el).value) { body.password = $('#p-new', el).value; body.old_password = $('#p-old', el).value; }
      await api('PUT', '/api/me', body);
      closeModal(); toast('Profile saved'); await refreshState(); paintShell(); route();
    }));
  });
}

boot().catch((e) => { document.body.innerHTML = `<div class="empty">Cannot reach the Node: ${esc(e.message)}</div>`; });
