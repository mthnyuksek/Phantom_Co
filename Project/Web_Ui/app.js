import { lang, setLang, LANGS } from './i18n.js';  // first: it translates what the other modules put on the screen
import { api } from './api.js';
import { ghostSVG } from './ghosts.js';
import { brandColor, themePickerHTML, bindThemePicker } from './theme.js';
import { $, $$, esc, guard, modal, closeModal, toast, ago, ROLE_TEXT } from './ui.js';
import { S, refreshState, forgetState, me, isOwner, canEdit, userGhost, providerLabel } from './state.js';
import { ghostPickerHTML, bindGhostPicker } from './pickers.js';
import { offerGuide } from './guide.js';

const view = $('#view');
const NAV = [
  { id: 'haunt', label: 'The Haunt', icon: '🏚️' },
  { id: 'chats', label: 'Chats', icon: '💬' },
  { id: 'projects', label: 'Projects', icon: '📁' },
  { id: 'tasks', label: 'Tasks', icon: '📋' },
  { id: 'scripts', label: 'Scripts', icon: '🧩' },
  { id: 'search', label: 'Search', icon: '🔎' },
  { id: 'overview', label: 'Overview', icon: '📊', editor: true },
  { id: 'skills', label: 'Skills', icon: '🧰' },
  { id: 'rules', label: 'Rules', icon: '📜' },
  { id: 'runs', label: 'Runs', icon: '⏱️' },
  { id: 'files', label: 'Files', icon: '🗂️' },
  { id: 'team', label: 'Team', icon: '👥' },
  { id: 'settings', label: 'Settings', icon: '⚙️', owner: true },
];
const PAGES = ['haunt', 'chats', 'projects', 'tasks', 'scripts', 'search', 'overview', 'skills', 'rules', 'runs', 'files', 'team', 'settings'];

let token = 0, current = null, currentCtx = null, timer = null, running = false;

// ---------------------------------------------------------------- auth screens
const authRoot = $('#auth-root');
function authCard(title, sub, body, mood = 'idle', form = 'wizard') {
  $('#app').hidden = true;
  authRoot.hidden = false;
  authRoot.innerHTML = `<div class="auth-card">
    <div class="auth-ghost">${ghostSVG({ form, color: brandColor(), mood, size: 120 })}</div>
    <h1>${esc(title)}</h1><p class="sub">${esc(sub)}</p>${body}${langSwitchHTML()}</div>`;
}
const langSwitchHTML = () => `<div class="auth-alt" data-no-i18n>${LANGS.map((l) => `<a href="#" data-lang="${l.id}" ${l.id === lang ? 'style="font-weight:700;pointer-events:none"' : ''}>${l.label}</a>`).join(' · ')}</div>`;
authRoot.addEventListener('click', (e) => {
  const a = e.target.closest('[data-lang]');
  if (a) { e.preventDefault(); setLang(a.dataset.lang); }
});
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

function showSetup(vaultPath) {
  // vaultPath comes only for the person at this PC: they choose where the Vault (all company files) lives.
  const vaultBox = vaultPath ? `<label>Vault folder (all company files)</label>
     <div class="vault-pick"><input id="a-vault" type="text" value="${esc(vaultPath)}" spellcheck="false">
     ${bridge ? '<button class="btn" id="a-browse" type="button">Browse…</button>' : ''}</div>
     <div class="hint vault-hint">Pick an empty folder, or any folder and a Phantom_Vault folder is made inside it.</div>` : '';
  authCard('Welcome to Phantom Co.', 'Create the owner account. You can invite friends afterwards.',
    `${field('a-name', 'Your name', 'text', 'maxlength="40" autofocus')}${field('a-pass', 'Password (at least 8 characters)', 'password')}${vaultBox}
     <div class="auth-err" id="a-err"></div><button class="btn primary wide-btn" id="a-go">Create company</button>${joinFriendHTML()}`);
  bindJoinFriend();
  if ($('#a-browse')) {
    const onPick = (e) => {
      if (e.data?.type !== 'folder_picked') return;
      bridge.removeEventListener('message', onPick);
      if (e.data.path && $('#a-vault')) $('#a-vault').value = e.data.path;
    };
    $('#a-browse').onclick = () => { bridge.addEventListener('message', onPick); bridge.postMessage({ cmd: 'pick_folder' }); };
  }
  const go = guard(async () => {
    try {
      const body = { name: $('#a-name').value.trim(), password: $('#a-pass').value };
      if ($('#a-vault')) body.vault_path = $('#a-vault').value.trim();
      await api('POST', '/api/auth/setup', body);
      await startApp();
      offerGuide();
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
  if (status.setup_needed) return showSetup(status.vault_path);
  if (!status.me) return showLogin();
  await startApp();
}
window.addEventListener('pc2:changed', guard(async () => { if (!running) return; await refreshState(); paintShell(); await route(); }));
window.addEventListener('pc2:signedout', () => { if (running) boot(); });

// ------------------------------------------------------------------ the shell
function paintShell() {
  const s = S.state;
  $('#brand-name').textContent = s.company.name;
  $('#brand-ghost').innerHTML = ghostSVG({ form: 'classic', color: brandColor(), size: 34 });
  $('#provider-chips').innerHTML = s.providers.filter((p) => p.id !== 'demo' && ((p.id !== 'gemini' && p.id !== 'opencode') || p.available))  // Gemini / OpenCode only when installed: most people do not have them
    .map((p) => `<span class="chip ${p.available ? 'ok' : ''}" title="${p.available ? 'Found on this PC' : 'Not found on this PC'}">${esc(p.label)}</span>`).join('');
  $('#home-btn').hidden = !(bridge && isRemote);
  const m = s.me;
  $('#me-chip').innerHTML = `<span class="mini">${userGhost(m, 30)}</span><span class="me-text"><b>${esc(m.name)}</b><small>${esc(ROLE_TEXT[m.role] || m.role)}</small></span>`;
  const page = location.hash.replace(/^#\//, '').split('/')[0] || 'haunt';
  const busyChats = s.jobs.length, asks = (s.approvals || []).filter((x) => x.can_decide).length;  // ghost actions waiting for this person's yes/no
  const scriptAsks = (s.asks || []).length;  // Ask blocks of scripts waiting for this person
  document.querySelectorAll('.bell-count').forEach((el) => { el.hidden = !s.unread; el.textContent = s.unread > 99 ? '99+' : s.unread; });
  $('#nav').innerHTML = NAV.filter((n) => (!n.owner || m.role === 'owner') && (!n.editor || m.role !== 'viewer'))
    .map((n) => `<a href="#/${n.id}" data-page="${n.id}" class="${n.id === page ? 'on' : ''}"><i>${n.icon}</i> ${n.label}${n.id === 'runs' && busyChats ? `<span class="count">${busyChats}</span>` : ''}${n.id === 'scripts' && scriptAsks ? `<span class="count ask" title="A script is waiting for your answer">❓ ${scriptAsks}</span>` : ''}${n.id === 'chats' && asks ? `<span class="count ask" title="Waiting for your approval">🔐 ${asks}</span>` : ''}</a>`).join('');
  // phone top bar: page name, small ghost, profile button
  $('#top-ghost').innerHTML = ghostSVG({ form: 'classic', color: brandColor(), size: 26 });
  $('#top-title').textContent = (NAV.find((n) => n.id === page) || NAV[0]).label;
  $('#top-me').innerHTML = userGhost(m, 30);
}

// The bell: what happened while this person was away. A notice opens the place it is about and counts as read.
async function openBell() {
  setMenu(false);
  const items = (await api('GET', '/api/notifications')).items;
  const text = (n, k) => (lang === 'tr' ? n[k + '_tr'] : n[k]) || '';
  const ICON = { failed: '⚠️', ask: '❓', budget: '💰', done: '✅' };
  modal(`<h2>Notifications</h2>
    <div class="nt-list">${items.map((n) => `<a class="nt-row ${n.read ? '' : 'new'}" href="${esc(n.link || '#/haunt')}" data-nt="${esc(n.id)}"><i>${ICON[n.kind] || '🔔'}</i>
      <div><b data-no-i18n>${esc(text(n, 'title'))}</b>${text(n, 'text') ? `<div class="nt-text" data-no-i18n>${esc(text(n, 'text'))}</div>` : ''}<small>${esc(ago(n.ts))}</small></div></a>`).join('') || '<div class="empty small">Nothing yet.</div>'}</div>
    <div class="modal-foot">${items.some((n) => !n.read) ? '<button class="btn" id="nt-all">Mark all as read</button>' : ''}<button class="btn" id="nt-close">Close</button></div>`,
  (el) => el.addEventListener('click', guard(async (e) => {
    const row = e.target.closest('[data-nt]');
    if (e.target.id === 'nt-close') return closeModal();
    if (e.target.id === 'nt-all') await api('POST', '/api/notifications/read', {});
    else if (row) await api('POST', '/api/notifications/read', { id: row.dataset.nt });
    else return;
    closeModal();
    await refreshState();
    paintShell();
  })));
}
document.querySelectorAll('.bell-btn').forEach((b) => { b.onclick = guard(openBell); });
document.addEventListener('keydown', (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k' && running) { e.preventDefault(); location.hash = '#/search'; }
});

// phone: the sidebar is a drawer opened from the top bar
const setMenu = (open) => { document.body.classList.toggle('nav-open', open); $('#menu-btn').setAttribute('aria-expanded', String(open)); };
$('#menu-btn').onclick = () => setMenu(!document.body.classList.contains('nav-open'));
$('#scrim').onclick = () => setMenu(false);
$('#nav').addEventListener('click', (e) => { if (e.target.closest('a')) setMenu(false); });
$('#top-me').onclick = () => openProfile();

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
window.addEventListener('hashchange', () => {
  setMenu(false);
  if (running) { closeModal(); paintShell(); route(); }  // a phone's back button must not leave a dialog over the next page
  else if (/^#\/join\//.test(location.hash)) boot();  // an invitation link opened on a page that is already loaded (same address, only the # part differs: no reload)
});

async function startApp() {
  stopApp();
  await refreshState();
  authRoot.hidden = true;
  $('#app').hidden = false;
  running = true;
  paintShell();
  await route();
  checkUpdate();
  let hiddenTicks = 0;
  timer = setInterval(guard(async () => {
    if (!running) return;
    if (document.hidden) {  // still say "I am here" every 30 s: a minimized window is not asleep
      if (++hiddenTicks % 15 === 0) await api('GET', '/api/auth/status');
      return;
    }
    hiddenTicks = 0;
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

// ------------------------------------------------- "a newer version is out" (owner at this PC only)
// The server asks GitHub in the background; the first answer may still be on its way, so look again a few times.
async function checkUpdate(tries = 4) {
  if (isRemote || !isOwner()) return;
  const r = await api('GET', '/api/about').catch(() => null);
  const u = r?.update, box = $('#update-note');
  if (!u || !box || !running) return;
  if (u.checking && !u.checked && tries > 0) { setTimeout(() => checkUpdate(tries - 1), 5000); return; }
  let dismissed = '';
  try { dismissed = localStorage.getItem('pc2.update.dismissed') || ''; } catch { /* private mode */ }
  box.hidden = !(u.newer && dismissed !== u.latest);
  if (box.hidden) return;
  box.innerHTML = `<a href="${esc(u.url)}" target="_blank" rel="noopener">✨ Version ${esc(u.latest)} is available</a>
    <button class="update-close" id="update-close" aria-label="Hide" title="Hide">×</button>`;
  $('#update-close').onclick = () => {
    try { localStorage.setItem('pc2.update.dismissed', u.latest); } catch { /* private mode */ }
    box.hidden = true;
  };
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
  return `<h3 class="subhead">Lend my AI <span class="badge experimental">Experimental</span></h3>
    <label style="display:flex;gap:8px;align-items:center;margin:0"><input type="checkbox" id="p-lend" style="width:auto"> Let this company use the Claude Code / Codex on <b>my PC</b></label>
    <p class="sub" id="p-lend-status"></p>
    <p class="sub">The company can then give work to my AI. Only file and web tools run (no commands). The files of the project the AI works on are copied to this PC and the changes copied back. It uses my own subscriptions, so only switch it on for people I trust. It stays on when I reopen the program, until I switch it off.</p>
    <p class="sub">Still experimental: files my AI deletes are not deleted in the company, very large project folders are slow, and work for my AI fails while my PC is off. Both PCs need the same Phantom Co. version.</p>`;
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
// Profile > Telegram: connect your own Telegram account to talk to the ghosts (the owner switches listening on in Settings > MCP).
function telegramHTML() {
  const t = S.state.telegram;
  if (!t || (!t.on && !t.linked.length)) return '';
  return `<h3 class="subhead">Telegram</h3><div id="p-tg">
    ${t.linked.length ? `<p class="hint">Connected: ${t.linked.map((x) => esc(x.name || '?')).join(', ')}. Write /agents to @${esc(t.bot)} to begin.</p>` : `<p class="hint">Talk to the ghosts from Telegram: /agents lists them, /&lt;ghost&gt; message writes to one, /new makes it forget the chat.</p>`}
    <div class="inline">${t.on ? '<button class="btn small" id="p-tg-link">Connect Telegram</button>' : ''}${t.linked.length ? '<button class="btn small danger" id="p-tg-unlink">Disconnect</button>' : ''}</div>
    <div id="p-tg-code"></div></div>`;
}
function bindTelegram(el) {
  el.addEventListener('click', guard(async (e) => {
    if (e.target.id === 'p-tg-link') {
      const r = await api('POST', '/api/telegram/link', {});
      $('#p-tg-code', el).innerHTML = `<p class="hint">Open the bot and press Start, or send this to @${esc(r.bot)} within 10 minutes:</p>
        <div class="linkbox"><input type="text" readonly value="/link ${esc(r.code)}" data-no-i18n><a class="btn small primary" href="https://t.me/${encodeURIComponent(r.bot)}?start=link_${esc(r.code)}" target="_blank" rel="noopener">Open in Telegram</a></div>`;
    } else if (e.target.id === 'p-tg-unlink') {
      await api('DELETE', '/api/telegram/link');
      toast('Telegram disconnected');
      await refreshState();
      $('#p-tg', el).outerHTML = telegramHTML() || '<div id="p-tg"></div>';
    }
  }));
}
// The app's own ghosts take the colour of the theme.
window.addEventListener('pc2:theme', () => { if (S.state && !$('#app').hidden) paintShell(); });
$('#me-chip').onclick = () => { setMenu(false); openProfile(); };
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
      <h3 class="subhead">Theme</h3>
      <div id="p-theme">${themePickerHTML()}</div>
      <h3 class="subhead">Language</h3>
      <select id="p-lang" data-no-i18n>${LANGS.map((l) => `<option value="${l.id}" ${l.id === lang ? 'selected' : ''}>${l.label}</option>`).join('')}</select>
      ${telegramHTML()}
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
    bindTelegram(el);
    $('#p-lang', el).onchange = (e) => setLang(e.target.value);
    bindThemePicker($('#p-theme', el));
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
