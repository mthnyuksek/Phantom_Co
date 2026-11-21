// Settings > Personal, the part everybody sees: colour theme, language, your own Telegram and (in a friend's company) lending your AI.
import { lang, setLang, LANGS } from '../i18n.js';
import { api } from '../api.js';
import { themePickerHTML, bindThemePicker } from '../theme.js';
import { S, canEdit, refreshState } from '../state.js';
import { $, esc, guard, toast, ago, twoStep, copyText } from '../ui.js';

const bridge = window.chrome?.webview;  // present when this page runs inside the Phantom Co. program window
const isRemote = !['127.0.0.1', 'localhost'].includes(location.hostname);

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
  return `<div class="card"><h3>Lend my AI <span class="badge experimental">Experimental</span></h3>
    <label style="display:flex;gap:8px;align-items:center;margin:0"><input type="checkbox" id="p-lend" style="width:auto"> Let this company use the Claude Code / Codex on <b>my PC</b></label>
    <p class="sub" id="p-lend-status"></p>
    <p class="sub">The company can then give work to my AI. Only file and web tools run (no commands). The files of the project the AI works on are copied to this PC and the changes copied back. It uses my own subscriptions, so only switch it on for people I trust. It stays on when I reopen the program, until I switch it off.</p>
    <p class="sub">Still experimental: files my AI deletes are not deleted in the company, very large project folders are slow, and work for my AI fails while my PC is off. Both PCs need the same Phantom Co. version.</p></div>`;
}
function bindLend(root) {
  const box = $('#p-lend', root);
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

// ------------------------------------------------- your own Telegram (the owner switches listening on in Settings > MCP)
function telegramHTML() {
  const t = S.state.telegram;
  if (!t || (!t.on && !t.linked.length)) return '';
  return `<h3>Telegram</h3>
    ${t.linked.length ? `<p class="hint">Connected: ${t.linked.map((x) => esc(x.name || '?')).join(', ')}. Write /agents to @${esc(t.bot)} to begin.</p>` : `<p class="hint">Talk to the ghosts from Telegram: /agents lists them, /&lt;ghost&gt; message writes to one, /new makes it forget the chat.</p>`}
    <div class="inline">${t.on ? '<button class="btn small" id="p-tg-link">Connect Telegram</button>' : ''}${t.linked.length ? '<button class="btn small danger" id="p-tg-unlink">Disconnect</button>' : ''}</div>
    <div id="p-tg-code"></div>`;
}
function bindTelegram(card) {
  card.addEventListener('click', guard(async (e) => {
    if (e.target.id === 'p-tg-link') {
      const r = await api('POST', '/api/telegram/link', {});
      $('#p-tg-code', card).innerHTML = `<p class="hint">Open the bot and press Start, or send this to @${esc(r.bot)} within 10 minutes:</p>
        <div class="linkbox"><input type="text" readonly value="/link ${esc(r.code)}" data-no-i18n><a class="btn small primary" href="https://t.me/${encodeURIComponent(r.bot)}?start=link_${esc(r.code)}" target="_blank" rel="noopener">Open in Telegram</a></div>`;
    } else if (e.target.id === 'p-tg-unlink') {
      await api('DELETE', '/api/telegram/link');
      toast('Telegram disconnected');
      await refreshState();
      card.innerHTML = telegramHTML();
      card.hidden = !card.innerHTML;
    }
  }));
}

// ------------------------------------------------- API keys for the person's own programs (an AI assistant)
function keysHTML(keys, max) {
  const rows = keys.map((k) => `<div class="key-row"><span><b data-no-i18n>${esc(k.name)}</b> <code data-no-i18n>pc_${esc(k.hint)}</code>
      <span class="sub">${k.used ? `Last used: <span>${ago(k.used)}</span>` : 'Never used'}</span></span>
      <button class="btn small danger" data-del-key="${esc(k.id)}">Delete</button></div>`).join('');
  return `<h3>API keys</h3>
    <p class="sub">Your own programs (for example an AI assistant) can use Phantom Co. with a key: it works with ghosts, tasks and scripts with your rights (at most an editor's). It cannot change accounts, keys, skills, rules, secrets or settings. Changing your password ends all keys.</p>
    ${rows || '<p class="hint">No keys yet.</p>'}
    <div id="p-key-new"></div>
    ${keys.length < max ? `<div class="inline"><input type="text" id="p-key-name" maxlength="40" placeholder="Name, e.g. My assistant"><button class="btn small primary" id="p-key-add">Create key</button></div>` : ''}`;
}
async function paintKeys(card, fresh) {
  const r = await api('GET', '/api/api_keys');
  card.innerHTML = keysHTML(r.keys, r.max);
  if (fresh) {
    $('#p-key-new', card).innerHTML = `<p class="hint">Copy the key now: it is shown only once.</p>
      <div class="linkbox"><input type="text" readonly value="${esc(fresh)}" data-no-i18n><button class="btn small primary" id="p-key-copy">Copy</button></div>
      <p class="sub">Address: <code data-no-i18n>${esc(location.origin)}</code> · MCP: <code data-no-i18n>${esc(location.origin)}/mcp</code></p>`;
    $('#p-key-copy', card).onclick = () => copyText(fresh);
  }
}
function bindKeys(card) {
  paintKeys(card).catch(() => { card.hidden = true; });
  card.addEventListener('click', guard(async (e) => {
    if (e.target.id === 'p-key-add') {
      const name = $('#p-key-name', card).value.trim();
      if (!name) return toast('Please give the key a name.', true);
      const r = await api('POST', '/api/api_keys', { name });
      await paintKeys(card, r.token);
    } else if (e.target.dataset.delKey) {
      const btn = e.target;
      twoStep(btn, 'Really delete?', async () => { await api('DELETE', '/api/api_keys/' + encodeURIComponent(btn.dataset.delKey)); await paintKeys(card); });
    }
  }));
}

export function personalHTML() {
  const tg = telegramHTML();
  return `<div class="card"><h3>Theme</h3><div id="p-theme">${themePickerHTML()}</div></div>
    <div class="card"><h3>Language</h3><select id="p-lang" data-no-i18n>${LANGS.map((l) => `<option value="${l.id}" ${l.id === lang ? 'selected' : ''}>${l.label}</option>`).join('')}</select></div>
    <div class="card" id="p-tg" ${tg ? '' : 'hidden'}>${tg}</div>
    <div class="card" id="p-keys"></div>
    ${lendHTML()}`;
}

export function bindPersonal(view) {
  bindThemePicker($('#p-theme', view));
  $('#p-lang', view).onchange = (e) => setLang(e.target.value);
  bindTelegram($('#p-tg', view));
  bindKeys($('#p-keys', view));
  bindLend(view);
}
