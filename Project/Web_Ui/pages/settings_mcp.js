// Settings > MCP: tools from outside for the ghosts (a browser, YouTube, Telegram, your own MCP servers).
// The card lives inside the Settings page; it uses data-mcp attributes so it never meets Settings' own data-act clicks.
import { api } from '../api.js';
import { refreshState } from '../state.js';
import { $, esc, guard, toast, twoStep } from '../ui.js';

let box = null, data = null, open = '', adding = false;
const checks = {};  // name -> {ok, text} (built-in servers: bot name / channel name)

const READY = {
  browser: { icon: '🌐', text: 'A real browser (Chrome) the ghosts can click and type in. Needs Node.js and Google Chrome on this PC.' },
  youtube: { icon: '▶️', text: 'Upload videos to your YouTube channel. Needs a free Google Cloud OAuth client.' },
  telegram: { icon: '✈️', text: 'Send messages and files through your own Telegram bot.' },
};

export const mcpCardHTML = () => `<div class="card" id="s-mcp"><h3>MCP: tools from outside</h3>
  <p class="hint" style="margin-top:0">MCP servers give ghosts new tools: a web browser, your YouTube channel, a Telegram bot or servers you build yourself. A ghost gets them through a skill (the ready-made ones add their skill to the Skills page). They run only on this PC, never on a friend's, and can only be added or changed here.</p>
  <div id="mcp-box"><p class="hint">Loading…</p></div></div>`;

function statusChip(s) {
  if (!s.enabled) return '<span class="chip">Off</span>';
  if (s.kind === 'command') return s.found ? '<span class="chip ok">Ready</span>' : `<span class="chip">Not found on this PC: <code>${esc(s.command)}</code></span>`;
  const c = checks[s.name];
  if (!c) return '<span class="chip">Checking…</span>';
  return c.ok ? `<span class="chip ok">Connected</span> <span class="acc-email">${esc(c.text)}</span>` : `<span class="chip">${esc(c.text)}</span>`;
}

let tg = null;  // the Telegram bridge: {state: off|on|error, text, bot}
function listenChip(s) {
  if (s.builtin !== 'telegram' || !s.options?.listen || !s.enabled) return '';
  if (!tg || tg.state === 'off') return ' <span class="chip">Starting to listen…</span>';
  return tg.state === 'on' ? ' <span class="chip ok">Listening</span>' : ` <span class="chip">${esc(tg.text)}</span>`;
}

const secretInput = (s, key, label) => `<label>${label}</label><input type="password" data-secret="${key}" maxlength="4000" autocomplete="off"
  placeholder="${(s.secrets_set || []).includes(key) ? 'Saved (leave empty to keep it)' : 'Paste it here'}">`;

function formHTML(s) {
  const isNew = !s.name;
  let body = '';
  if (s.kind === 'builtin' && s.builtin === 'telegram') {
    body = `<p class="hint">In Telegram, talk to @BotFather, send /newbot and copy the token it gives. Then send /start to your new bot: a ghost can read your chat id with "read messages", or write it below as the default chat.</p>
      ${secretInput(s, 'bot_token', 'Bot token')}
      <label>Default chat id <span class="hint-inline">where messages go when the ghost names no chat</span></label><input type="text" data-f="chat_id" maxlength="64" value="${esc(s.options?.chat_id || '')}" placeholder="e.g. 123456789">
      <label class="check"><input type="checkbox" data-f="listen" ${s.options?.listen ? 'checked' : ''}> Let people talk to ghosts from Telegram (/agents, /&lt;ghost&gt; message, /new)</label>
      <p class="hint">Each person connects their own Telegram account in their profile (Profile &gt; Telegram); the ghosts then work with that person's rights. Answers and approval requests come back to Telegram. Only one Phantom Co. window can listen to a bot.</p>`;
  } else if (s.kind === 'builtin' && s.builtin === 'youtube') {
    body = `<ol class="hint mcp-steps"><li>Open console.cloud.google.com, make a project and switch on YouTube Data API v3.</li>
      <li>OAuth consent screen: type External, add your own Google account as a test user.</li>
      <li>Credentials &gt; Create OAuth client ID &gt; Desktop app. Copy the client id and secret here and save.</li>
      <li>Press "Sign in with Google" and allow access to your channel.</li></ol>
      <p class="hint">Google keeps videos uploaded by apps it has not reviewed private; you make them public in YouTube Studio. Uploads use the daily YouTube quota of your project (a few videos a day).</p>
      ${secretInput(s, 'client_id', 'OAuth client id')}${secretInput(s, 'client_secret', 'OAuth client secret')}`;
  } else {
    const envs = (s.env_names || []).join('\n');
    body = `${isNew ? `<label>Name <span class="hint-inline">a-z, 0-9 and _; the ghosts see tools as mcp__name__tool</span></label><input type="text" data-f="name" maxlength="24" placeholder="e.g. discord">` : ''}
      <div class="two"><div><label>Command</label><input type="text" data-f="command" maxlength="260" value="${esc(s.command || '')}" placeholder="npx, python, uv or a full path"></div>
        <div><label>Folder <span class="hint-inline">optional: Phantom Co. copies it and runs the copy</span></label><input type="text" data-f="folder" maxlength="400" value="${esc(s.folder || '')}" placeholder="C:\\…\\My_Server"></div></div>
      <label>Arguments <span class="hint-inline">one per line</span></label><textarea data-f="args" style="min-height:70px">${esc((s.args || []).join('\n'))}</textarea>
      <label>Secrets <span class="hint-inline">one per line: NAME=value sets it, NAME alone keeps the saved value; handed to the server as environment variables</span></label>
      <textarea data-f="env" style="min-height:60px" placeholder="API_KEY=…" autocomplete="off">${esc(envs)}</textarea>
      ${(s.secrets_set || []).length ? `<p class="hint">Saved: ${s.secrets_set.map(esc).join(', ')} (values are never shown)</p>` : ''}`;
  }
  return `<div class="mcp-form" data-for="${esc(s.name || '')}">${body}
    <label class="check"><input type="checkbox" data-f="approve" ${s.approve !== false ? 'checked' : ''}> Ask me before it acts (approve or decline each call in the chat)</label>
    <label>Only these tools ask <span class="hint-inline">comma-separated; empty = every tool asks</span></label>
    <input type="text" data-f="approve_tools" maxlength="400" value="${esc((s.approve_tools || []).join(', '))}">
    ${isNew ? '' : `<label class="check"><input type="checkbox" data-f="enabled" ${s.enabled ? 'checked' : ''}> Switched on</label>`}
    <div class="modal-foot">${isNew ? '<button class="btn" data-mcp="cancel-new">Cancel</button>' : ''}<button class="btn primary" data-mcp="save" data-name="${esc(s.name || '')}">Save</button></div></div>`;
}

function rowHTML(s) {
  return `<div class="acc-row mcp-row"><div class="acc-main"><div class="acc-title"><b>${esc(READY[s.name]?.icon || '🧩')} <span>${esc(s.label)}</span></b> <code>${esc(s.name)}</code>
      <span class="badge">${s.kind === 'builtin' ? 'built-in' : 'command'}</span>${s.approve ? ' <span class="badge">asks first</span>' : ''}</div>
    <div class="acc-state">${statusChip(s)}${listenChip(s)}</div>
    <div class="meta">${s.skills.length ? `Skills: ${s.skills.map(esc).join(', ')}` : 'No skill uses it yet: tick it in a skill on the Skills page.'}</div>
    ${open === s.name ? formHTML(s) : ''}</div>
    <div class="acc-btns">${s.builtin === 'youtube' ? `<button class="btn small" data-mcp="google" data-name="${esc(s.name)}">Sign in with Google</button>` : ''}
      ${s.folder ? `<button class="btn small" data-mcp="refresh" data-name="${esc(s.name)}" title="Copy the folder again after you changed the server">Update copy</button>` : ''}
      <button class="btn small" data-mcp="edit" data-name="${esc(s.name)}">${open === s.name ? 'Close' : 'Edit'}</button>
      <button class="btn small danger" data-mcp="del" data-name="${esc(s.name)}">Remove</button></div></div>`;
}

function draw() {
  if (!box || !box.isConnected) return;
  if (!data) { box.innerHTML = '<p class="hint">Not available.</p>'; return; }
  const have = new Set(data.servers.map((s) => s.name));
  box.innerHTML = `${data.servers.map(rowHTML).join('') || '<p class="hint">No MCP servers yet.</p>'}
    ${adding ? `<div class="acc-row"><div class="acc-main"><b>🧩 Your own MCP server</b><p class="hint">Any MCP server that talks over stdio. A ghost with the "MCP builder" skill can write one for you.</p>${formHTML({ kind: 'command', approve: true })}</div></div>` : ''}
    <h4 class="subhead">Add</h4><div class="mcp-ready">${Object.entries(READY).map(([id, r]) => `<button class="mcp-tile" data-mcp="add" data-id="${id}" ${have.has(id) ? 'disabled' : ''}>
      <span class="mcp-icon">${r.icon}</span><b>${esc(data.catalog.find((c) => c.name === id)?.label || id)}</b><small>${have.has(id) ? 'Added' : esc(r.text)}</small></button>`).join('')}
      <button class="mcp-tile" data-mcp="own" ${adding ? 'disabled' : ''}><span class="mcp-icon">🧩</span><b>Your own</b><small>A server you or a ghost built (Python, Node…).</small></button></div>`;
}

async function load() {
  data = await api('GET', '/api/mcp').catch(() => null);
  tg = await api('GET', '/api/telegram').catch(() => null);
  draw();
  if (data?.servers.some((s) => s.builtin === 'telegram' && s.options?.listen && s.enabled) && tg?.state !== 'on' && box?.isConnected)
    setTimeout(async () => { tg = await api('GET', '/api/telegram').catch(() => tg); draw(); }, 4000);
  for (const s of data?.servers || [])
    if (s.kind === 'builtin' && s.enabled) api('GET', `/api/mcp/${s.name}/check`).then((c) => { checks[s.name] = c; draw(); }).catch(() => {});
}

function collect(form) {
  const f = (k) => form.querySelector(`[data-f="${k}"]`);
  const body = { approve: f('approve').checked, approve_tools: f('approve_tools').value.split(',').map((x) => x.trim()).filter(Boolean), secrets: {} };
  if (f('enabled')) body.enabled = f('enabled').checked;
  form.querySelectorAll('[data-secret]').forEach((i) => { if (i.value.trim()) body.secrets[i.dataset.secret] = i.value.trim(); });
  if (f('chat_id')) body.options = { chat_id: f('chat_id').value.trim(), listen: f('listen').checked };
  if (f('command')) {
    body.command = f('command').value.trim();
    body.folder = f('folder').value.trim();
    body.args = f('args').value.split('\n').map((x) => x.trim()).filter(Boolean);
    body.env_names = [];
    for (const line of f('env').value.split('\n').map((x) => x.trim()).filter(Boolean)) {
      const eq = line.indexOf('='), name = (eq < 0 ? line : line.slice(0, eq)).trim();
      body.env_names.push(name);
      if (eq >= 0 && line.slice(eq + 1).trim()) body.secrets[name] = line.slice(eq + 1).trim();
    }
  }
  if (f('name')) { body.name = f('name').value.trim(); body.kind = 'command'; }
  return body;
}

export function bindMcp(view) {
  box = $('#mcp-box', view);
  open = ''; adding = false;
  load();
  box.addEventListener('click', guard(async (e) => {
    const el = e.target.closest('[data-mcp]');
    if (!el || !box.contains(el)) return;
    const act = el.dataset.mcp, name = el.dataset.name;
    if (act === 'edit') { open = open === name ? '' : name; draw(); }
    else if (act === 'own') { adding = true; draw(); }
    else if (act === 'cancel-new') { adding = false; draw(); }
    else if (act === 'add') {
      const r = await api('POST', '/api/mcp', { catalog: el.dataset.id });
      open = r.name;
      toast(r.skill_id ? 'Added, with its skill. Give the skill to a ghost.' : 'Added');
      await refreshState(); await load();
    } else if (act === 'save') {
      const form = el.closest('.mcp-form'), body = collect(form);
      if (name) await api('PUT', '/api/mcp/' + name, body);
      else { await api('POST', '/api/mcp', body); adding = false; }
      open = ''; delete checks[name];
      toast('Saved'); await refreshState(); await load();
    } else if (act === 'del') {
      await twoStep(el, 'Really remove?', async () => { await api('DELETE', '/api/mcp/' + name); toast('Removed'); await refreshState(); await load(); });
    } else if (act === 'refresh') { await api('POST', `/api/mcp/${name}/refresh`, {}); toast('The copy is up to date'); }
    else if (act === 'google') {
      const r = await api('POST', `/api/mcp/${name}/google_login`, {});
      window.open(r.url, '_blank', 'noopener');
      toast('Google opened in your browser. Allow access there, then come back.');
      setTimeout(async () => { delete checks[name]; await load(); }, 20000);
    }
  }));
}
