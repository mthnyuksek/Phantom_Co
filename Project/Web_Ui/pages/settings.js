import { api } from '../api.js';
import { S, isOwner, refreshState } from '../state.js';
import { $, esc, guard, toast, copyText, twoStep } from '../ui.js';
import { mcpCardHTML, bindMcp } from './settings_mcp.js';
import { notifyCardHTML, backupCardHTML, bindNotify, bindBackup } from './settings_backup.js';
import { personalHTML, bindPersonal } from './settings_personal.js';
import { t } from '../i18n.js';

let view, net = null, tun = null, tunTimer = null;
let acc = null, accTimer = null, accUntil = 0;  // sign-in states (Secrets), polled for a while after a sign-in window opened
let about = null;  // version and update check (/api/about)
let section = 'personal', renderId = 0;

// One tab per part of the page, address #/settings/<id>. Everybody has Personal; the rest is the owner's.
const SECTIONS = [
  { id: 'personal', label: 'Personal', icon: '🎨' },
  { id: 'company', label: 'Company', icon: '🏢' },
  { id: 'sharing', label: 'Sharing', icon: '🌐' },
  { id: 'secrets', label: 'Secrets', icon: '🔑' },
  { id: 'mcp', label: 'MCP', icon: '🧩' },
  { id: 'notifications', label: 'Notifications', icon: '🔔' },
  { id: 'backup', label: 'Backup', icon: '💾' },
  { id: 'about', label: 'About', icon: 'ℹ️' },
];

function sectionHTML(s, c, set) {
  if (section === 'personal') return personalHTML();
  if (section === 'company') return `
    <div class="card"><h3>Company</h3>
      <label>Name</label><input type="text" id="s-name" maxlength="60" value="${esc(c.name)}">
      <label>Goal <span class="hint-inline">every ghost reads this</span></label><textarea id="s-goal" maxlength="2000">${esc(c.goal)}</textarea>
      <div class="modal-foot"><button class="btn primary" data-act="save-company">Save</button></div></div>
    <div class="card"><h3>Limits</h3>
      <div class="two"><div><label>Runs at the same time (per brain)</label><input type="number" id="s-conc" min="1" max="4" value="${set.max_concurrent}"></div>
        <div><label>Waiting runs per person</label><input type="number" id="s-queue" min="0" max="100" value="${set.max_queue_per_user}"></div></div>
      <div class="two"><div><label>Hand-overs between ghosts (safety net)</label><input type="number" id="s-hops" min="1" max="20" value="${set.max_hops}"></div>
        <div><label>Default Claude model <span class="hint-inline">empty = Claude Code default</span></label><input type="text" id="s-model" maxlength="80" value="${esc(set.default_model)}" placeholder="e.g. haiku"></div></div>
      <div class="modal-foot"><button class="btn primary" data-act="save-limits">Save</button></div></div>
    <div class="card"><h3>Vault folder</h3>
      <p class="hint" style="margin-top:0">Everything lives here as plain files: people, ghosts, skills, rules, projects, chats, runs. Pointing to a new empty folder creates a fresh Vault there (old files are not moved).</p>
      <div class="inline"><input type="text" id="s-vault" value="${esc(s.vault_path)}"><button class="btn" data-act="save-vault">Use this folder</button><button class="btn" data-act="open-vault">Open</button></div></div>`;
  if (section === 'sharing') return `
    <div class="card"><h3>Sharing with friends</h3>
      <p class="hint" style="margin-top:0">Friends use their own Phantom Co. program and connect to this PC. Their ghost work runs here, with <b>your</b> Claude / Codex sign-in; nobody ever gets your login. It only works while this PC is on.</p>
      <label class="check"><input type="checkbox" id="s-share" ${s.share ? 'checked' : ''}> Let people on my network connect</label>
      ${s.share && net ? `<p class="hint">Friends connect to: ${net.addresses.map((a) => `<code>http://${esc(a.address)}:${net.port}</code> <span class="meta">(${esc(a.name)})</span>`).join(' &nbsp; ') || 'no network address found'}</p>` : ''}
      <p class="hint">Windows may ask once to allow the program through the firewall: allow it for <b>private</b> networks. The connection is not encrypted: use a home network or a private network app such as Tailscale, never the open internet. Sign-in is always required.</p></div>
    <div class="card" id="s-tunnel"><h3>Internet access</h3>
      <p class="hint" style="margin-top:0">For friends who are <b>not</b> on your network. Phantom Co. opens a free, encrypted Cloudflare tunnel (no account) and gives you a <code>https://…trycloudflare.com</code> address. It is off every time the program starts and the address changes each time. Sign-in is always required.</p>
      ${tunnelHTML()}</div>`;
  if (section === 'secrets') return `
    <div class="card" id="s-secrets"><h3>Secrets</h3>
      <p class="hint" style="margin-top:0">The Claude Code, Codex, Gemini CLI and OpenCode sign-ins your ghosts can use. <b>Auto</b> is the sign-in the command-line tool already has on this PC. Add more accounts (another subscription or an API key), then pick one as a ghost's brain. Sign-ins stay in this PC's settings folder, never in the Vault; API keys are stored encrypted for this Windows user and are typed only on this PC.</p>
      <p class="hint">Gemini CLI has no Auto: it works only with an API key added here.</p>
      <div id="s-acc-list">${secretsHTML()}</div>
      <div class="inline acc-add"><p class="acc-add-title">Add another sign-in or API key</p>
        <select id="s-acc-prov"><option value="claude">Claude Code</option><option value="codex">Codex</option><option value="gemini">Gemini CLI</option><option value="opencode">OpenCode (NVIDIA, OpenRouter, Groq…)</option></select>
        <select id="s-acc-service" hidden>${ocServices().map((v) => `<option value="${esc(v.id)}">${esc(v.label)}</option>`).join('')}</select>
        <select id="s-acc-kind"><option value="subscription">Subscription</option><option value="api_key">API key</option></select>
        <input type="text" id="s-acc-name" maxlength="40" placeholder="Account name, e.g. Work">
        <input type="password" id="s-acc-key" maxlength="400" placeholder="Paste the API key" autocomplete="off" hidden>
        <button class="btn" data-act="acc-add">Add account</button></div></div>`;
  if (section === 'mcp') return mcpCardHTML();
  if (section === 'notifications') return notifyCardHTML();
  if (section === 'backup') return backupCardHTML();
  return `<div class="card" id="s-about"><h3>About</h3>${aboutHTML()}</div>`;
}

function draw() {
  const s = S.state, owner = isOwner();
  const tabs = owner ? `<div class="tabs set-tabs">${SECTIONS.map((x) => `<a class="tab ${x.id === section ? 'on' : ''}" href="#/settings/${x.id}"><i>${x.icon}</i> ${x.label}</a>`).join('')}</div>` : '';
  view.innerHTML = `
    <div class="page-head"><div><h1>Settings</h1><p>${owner ? 'Your own preferences, and everything about your company.' : 'Your own preferences.'}</p></div></div>
    ${tabs}${sectionHTML(s, s.company, s.settings)}`;
}

function aboutHTML() {
  if (!about) return '<p class="hint" style="margin-top:0">Loading…</p>';
  const u = about.update;
  let state = '';
  if (u?.newer) state = `<p class="hint"><a href="${esc(u.url)}" target="_blank" rel="noopener">✨ Version ${esc(u.latest)} is available</a></p>
    <p class="hint">Download the new phantom_co.exe, quit this one from the tray icon and put the new file in its place. Your Vault and settings stay as they are.</p>`;
  else if (u?.checking && !u.checked) state = '<p class="hint">Looking for a newer version…</p>';
  else if (u?.error && !u.latest) state = `<p class="hint">Could not look for a newer version: ${esc(u.error)}</p>`;
  else if (u?.checked) state = '<p class="hint">This is the newest version.</p>';
  const local = 'check_updates' in about;  // the update part only reaches the owner at this PC
  return `<p class="hint" style="margin-top:0">Phantom Co. <b data-no-i18n>${esc(about.version)}</b></p>
    ${local ? `<label class="check"><input type="checkbox" id="s-updates" ${about.check_updates ? 'checked' : ''}> Look for new versions (asks GitHub twice a day, nothing is installed by itself)</label>
    ${about.check_updates ? `${state}<button class="btn small" data-act="update-check">Check now</button>` : ''}` : ''}`;
}
async function loadAbout(check = false) {
  about = await api('GET', '/api/about' + (check ? '?check=1' : '')).catch(() => about);
  if (!view || !view.isConnected || !$('#s-about', view)) return;  // left the page
  $('#s-about', view).innerHTML = $('#s-about h3', view).outerHTML + aboutHTML();
  if (about?.update?.checking) setTimeout(() => loadAbout(), 2000);
}

const PROV = { claude: 'Claude Code', codex: 'Codex', gemini: 'Gemini CLI', opencode: 'OpenCode' };
const KEY_ONLY = ['gemini', 'opencode'];  // no subscription sign-in: API keys only
// The model services an OpenCode key can belong to (from the server: id, label, key prefix, default model).
const ocServices = () => S.state.providers.find((p) => p.id === 'opencode')?.services || [];
const keyPlaceholder = () => {
  if ($('#s-acc-prov', view).value !== 'opencode') return t('Paste the API key');
  const sv = ocServices().find((v) => v.id === $('#s-acc-service', view).value);
  return t('Paste the {} key').replace('{}', sv?.label || '') + (sv?.prefix ? ` (${sv.prefix}…)` : '');
};
function keyStatusHTML(a) {
  const st = a.status, hint = a.key_hint ? ` <code class="acc-key">${esc(a.key_hint)}</code>` : '';
  if (!st) return '<span class="chip">Checking…</span>' + hint;
  if (st.signed_in) return '<span class="chip ok">Key works</span>' + hint;
  const why = { key_rejected: 'Key rejected', key_unreadable: 'Key cannot be read by this Windows user', unreachable: 'Could not check the key' }[st.error] || 'Could not check the key';
  return `<span class="chip">${why}</span>${hint}`;
}
function statusHTML(st, available) {
  if (available === false || st?.error === 'not_installed') return '<span class="chip">Not installed</span>';
  if (!st) return '<span class="chip">Checking…</span>';
  if (st.error === 'google_unsupported') return '<span class="chip">Google sign-in no longer works: add an API key</span>' + (st.email ? ` <span class="acc-email">${esc(st.email)}</span>` : '');
  if (!st.signed_in) return '<span class="chip">Not signed in</span>';
  return `<span class="chip ok">Signed in</span>${st.email ? ` <span class="acc-email">${esc(st.email)}</span>` : ''}${st.plan ? ` <span class="badge">${esc(st.plan)}</span>` : ''}`;
}
function usedHTML(names) {
  return names?.length ? `<div class="meta"><span>Used by:</span> ${names.map(esc).join(', ')}</div>` : '';
}
function secretsHTML() {
  const autos = acc ? acc.auto : S.state.providers.filter((p) => p.id !== 'demo' && p.id !== 'gemini').map((p) => ({ provider: p.id, label: p.label, available: p.available }));
  const list = acc ? acc.accounts : (S.state.accounts || []);
  const rows = autos.map((a) => `<div class="acc-row"><div class="acc-main"><div><b>${esc(PROV[a.provider] || a.label)} · Auto</b></div>
      <div class="acc-state">${statusHTML(a.status, a.available)}</div>${usedHTML(a.agents)}</div>
      <div class="acc-btns">${a.available && a.status && !a.status.signed_in && !KEY_ONLY.includes(a.provider) ? `<button class="btn small" data-act="acc-login" data-provider="${esc(a.provider)}">Sign in</button>` : ''}</div></div>`);
  for (const a of list) {
    const api = a.kind === 'api_key';
    rows.push(`<div class="acc-row"><div class="acc-main"><div class="acc-title"><b>${esc(PROV[a.provider])} ·</b> <input type="text" class="acc-name" data-id="${esc(a.id)}" maxlength="40" value="${esc(a.name)}" title="Rename">${api ? ' <span class="badge">API key</span>' : ''}${a.service ? ` <span class="badge">${esc(a.service.toUpperCase())}</span>` : ''}</div>
      <div class="acc-state">${api ? keyStatusHTML(a) : statusHTML(a.status)}</div>${usedHTML(a.agents)}
      <div class="inline acc-keyedit" data-for="${esc(a.id)}" hidden><input type="password" class="acc-newkey" maxlength="400" placeholder="Paste the new API key" autocomplete="off"><button class="btn small" data-act="acc-key-save" data-id="${esc(a.id)}">Save key</button></div></div>
      <div class="acc-btns">${api ? `<button class="btn small" data-act="acc-key" data-id="${esc(a.id)}">Replace key</button>`
        : KEY_ONLY.includes(a.provider) ? '' : `<button class="btn small" data-act="acc-login" data-id="${esc(a.id)}">${a.status?.signed_in ? 'Sign in again' : 'Sign in'}</button>`}
      <button class="btn small danger" data-act="acc-del" data-id="${esc(a.id)}">Remove</button></div></div>`);
  }
  return rows.join('');
}
async function loadAccounts() {
  clearTimeout(accTimer);
  const r = await api('GET', '/api/accounts/status').catch(() => null);
  if (!view || !view.isConnected || !$('#s-acc-list', view)) return;  // left the page
  if (r) acc = r;
  const box = $('#s-acc-list', view);
  if (!box.contains(document.activeElement)) box.innerHTML = secretsHTML();  // do not disturb somebody renaming
  if (Date.now() < accUntil) accTimer = setTimeout(loadAccounts, 5000);
}
async function openLogin(body) {
  await api('POST', '/api/accounts/login', body);
  toast('A sign-in window opened. Sign in there, then come back here.');
  accUntil = Date.now() + 180000;
  clearTimeout(accTimer);
  accTimer = setTimeout(loadAccounts, 5000);
}

function tunnelHTML() {
  if (!tun) return '<p class="hint">Not available.</p>';
  const st = tun.state;
  if (st === 'installing') return `<p class="hint">⏳ ${esc(tun.message)}</p>`;
  if (!tun.installed) return `${st === 'error' ? `<div class="errbox" style="margin-left:0">${esc(tun.message)}</div>` : ''}<p class="hint">It needs Cloudflare's small helper program <b>cloudflared</b> (about 40 MB, downloaded from Cloudflare's GitHub page once).</p><button class="btn primary" data-act="tun-install">Download cloudflared</button>`;
  if (st === 'starting') return `<p class="hint">⏳ ${esc(tun.message || 'Opening the tunnel...')}</p>`;
  if (st === 'on') return `<div class="linkbox"><input type="text" readonly value="${esc(tun.url)}"><button class="btn small" data-act="tun-copy">Copy</button></div>
    <p class="hint">Friends paste this address into "I was invited to a friend's company", or you send them an invitation link from the People page (it will use this address). Switch it off when you do not need it.</p>
    <button class="btn danger" data-act="tun-stop">Switch off</button>`;
  return `${st === 'error' ? `<div class="errbox" style="margin-left:0">${esc(tun.message)}</div>` : ''}<button class="btn primary" data-act="tun-start">Switch on</button>`;
}
async function pollTunnel() {
  clearTimeout(tunTimer);
  tun = await api('GET', '/api/tunnel').catch(() => null);
  if (!view || !view.isConnected || !$('#s-tunnel', view)) return;  // left the page
  const card = $('#s-tunnel', view);  // not found by its title: the title is translated in the Turkish interface
  if (card) card.innerHTML = card.querySelector('h3').outerHTML + card.querySelector('p.hint').outerHTML + tunnelHTML();
  if (tun && (tun.state === 'installing' || tun.state === 'starting')) tunTimer = setTimeout(pollTunnel, 1000);
}

export async function render(v, arg) {
  view = v;
  const my = ++renderId;
  clearTimeout(tunTimer);
  clearTimeout(accTimer);
  section = isOwner() && SECTIONS.some((x) => x.id === arg) ? arg : 'personal';
  if (section === 'sharing') {
    net = await api('GET', '/api/network').catch(() => null);
    tun = await api('GET', '/api/tunnel').catch(() => null);
    if (my !== renderId) return;  // another tab was opened meanwhile
  }
  acc = null;
  draw();
  if (section === 'personal') bindPersonal(view);
  if (!isOwner()) return;
  if (section === 'mcp') bindMcp(view);
  if (section === 'notifications') bindNotify(view).catch((e) => toast(e.message, true));
  if (section === 'backup') bindBackup(view).catch((e) => toast(e.message, true));
  if (section === 'secrets') loadAccounts();
  if (section === 'about') loadAbout();
  if (section === 'sharing' && tun && (tun.state === 'installing' || tun.state === 'starting')) tunTimer = setTimeout(pollTunnel, 1000);
  view.onclick = guard(async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    const act = el.dataset.act;
    if (act === 'save-company') { await api('PUT', '/api/company', { name: $('#s-name', view).value, goal: $('#s-goal', view).value }); toast('Saved'); window.dispatchEvent(new Event('pc2:changed')); }
    else if (act === 'save-limits') {
      await api('PUT', '/api/settings', { max_concurrent: +$('#s-conc', view).value, max_queue_per_user: +$('#s-queue', view).value, max_hops: +$('#s-hops', view).value, default_model: $('#s-model', view).value.trim() });
      toast('Saved'); window.dispatchEvent(new Event('pc2:changed'));
    } else if (act === 'save-vault') { await api('PUT', '/api/settings/vault', { path: $('#s-vault', view).value.trim() }); toast('Vault changed'); window.dispatchEvent(new Event('pc2:changed')); }
    else if (act === 'open-vault') await api('POST', '/api/vault/open', {});
    else if (act === 'tun-install') { await api('POST', '/api/tunnel', { action: 'install' }); await pollTunnel(); }
    else if (act === 'tun-start') { await api('POST', '/api/tunnel', { action: 'start' }); await pollTunnel(); }
    else if (act === 'tun-stop') { await api('POST', '/api/tunnel', { action: 'stop' }); await pollTunnel(); }
    else if (act === 'tun-copy') copyText(tun.url);
    else if (act === 'update-check') { el.disabled = true; await loadAbout(true); }
    else if (act === 'acc-add') {
      const name = $('#s-acc-name', view).value.trim(), kind = $('#s-acc-kind', view).value, key = $('#s-acc-key', view).value.trim();
      if (!name) return toast('Please enter a name.');
      if (kind === 'api_key' && !key) return toast('Paste the API key.');
      const provider = $('#s-acc-prov', view).value;
      const a = await api('POST', '/api/accounts', { provider, name, kind, key: kind === 'api_key' ? key : undefined, service: provider === 'opencode' ? $('#s-acc-service', view).value : undefined });
      $('#s-acc-name', view).value = '';
      $('#s-acc-key', view).value = '';
      await refreshState();
      $('#s-acc-list', view).innerHTML = secretsHTML();
      if (kind === 'api_key') { toast('Key saved. Checking it…'); loadAccounts(); } else await openLogin({ id: a.id });
    } else if (act === 'acc-key') {
      const box = view.querySelector(`.acc-keyedit[data-for="${CSS.escape(el.dataset.id)}"]`);
      box.hidden = !box.hidden;
      if (!box.hidden) box.querySelector('input').focus();
    } else if (act === 'acc-key-save') {
      const input = el.parentElement.querySelector('input'), key = input.value.trim();
      if (!key) return toast('Paste the API key.');
      await api('PUT', '/api/accounts/' + el.dataset.id, { key });
      input.value = '';
      el.parentElement.hidden = true;
      toast('Key saved. Checking it…');
      acc = null;
      $('#s-acc-list', view).innerHTML = secretsHTML();
      loadAccounts();
    } else if (act === 'acc-login') await openLogin(el.dataset.id ? { id: el.dataset.id } : { provider: el.dataset.provider });
    else if (act === 'acc-del') {
      const a = acc?.accounts.find((x) => x.id === el.dataset.id);
      const gemini = a?.provider === 'gemini';  // Gemini has no Auto to go back to
      const list = a?.agents?.length ? `<b>${gemini ? 'These ghosts need another Gemini key:' : 'These ghosts go back to Auto:'}</b><br>${a.agents.map(esc).join('<br>')}` : '';
      await twoStep(el, 'Really remove?', async () => {
        const r = await api('DELETE', '/api/accounts/' + el.dataset.id);
        toast(r.moved.length && !gemini ? 'Account removed. Its ghosts use Auto now.' : 'Account removed');
        await refreshState();
        acc = null;
        $('#s-acc-list', view).innerHTML = secretsHTML();
        loadAccounts();
      }, list);
    }
  });
  view.onchange = guard(async (e) => {
    if (e.target.id === 's-acc-kind') { $('#s-acc-key', view).hidden = e.target.value !== 'api_key'; return; }
    if (e.target.id === 's-acc-prov') {  // Gemini CLI (Google refuses the free sign-in) and OpenCode work only with an API key
      const kind = $('#s-acc-kind', view), keyOnly = KEY_ONLY.includes(e.target.value);
      kind.querySelector('option[value="subscription"]').disabled = keyOnly;
      if (keyOnly) kind.value = 'api_key';
      $('#s-acc-key', view).hidden = kind.value !== 'api_key';
      $('#s-acc-service', view).hidden = e.target.value !== 'opencode';
      $('#s-acc-key', view).placeholder = keyPlaceholder();
      return;
    }
    if (e.target.id === 's-acc-service') {
      $('#s-acc-key', view).placeholder = keyPlaceholder();
      return;
    }
    if (e.target.classList.contains('acc-name')) {
      const name = e.target.value.trim();
      if (!name) return;
      await api('PUT', '/api/accounts/' + e.target.dataset.id, { name });
      await refreshState();
      toast('Saved');
      return;
    }
    if (e.target.id === 's-updates') {
      about = await api('PUT', '/api/about', { check_updates: e.target.checked });
      $('#s-about', view).innerHTML = $('#s-about h3', view).outerHTML + aboutHTML();
      if (about.update?.checking) setTimeout(() => loadAbout(), 2000);
      return;
    }
    if (e.target.id !== 's-share') return;
    net = await api('PUT', '/api/network', { share: e.target.checked });
    toast(e.target.checked ? 'Sharing is on' : 'Sharing is off');
    await new Promise((r) => setTimeout(r, 900));  // the Node restarts its listener
    window.dispatchEvent(new Event('pc2:changed'));
  });
}
